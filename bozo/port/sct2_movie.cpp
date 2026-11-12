// SCTPT2's movies. A movie either streams from the disc, an MPEG program stream the game reads into a ring through its
// loader (0x17DB40 opens the stream: the file's sector and size), or sits whole in RAM, handed to libmpeg with
// sceMpegAddBs and looped (the main menu's). The game feeds the IPU and asks libmpeg for each picture
// (sceMpegGetPicture). Here the stream is read from the disc image or from RAM, its video demuxed when it is a program
// stream and decoded by BozoEngine's video decoder, and each picture written where the IPU would have put it: RGBA, alpha
// 0x80, in 16-pixel-wide strips of macroblocks, each the picture's height. The game then draws it as any other image.
// sceMpegIsEnd answers 1 after the last picture of a streamed movie; a movie in RAM loops.
#include "sct2_movie.h"

#include "bozo/context.h"
#include "bozo/libs/cd.h"
#include "bozo/libs/libraries.h"
#include "bozo/libs/mpeg.h"
#include "bozo/port.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <vector>

namespace sct2::movie
{
    namespace
    {
        constexpr uint32_t kGetPicture = 0x00186068u;
        constexpr uint32_t kIsEnd = 0x00186188u;
        constexpr uint32_t kAddBs = 0x00186028u;

        // The stream open's stores of the file's first sector and its size.
        // bozo:native kStreamOpen
        constexpr uint32_t kStreamOpen[] = {0x0017DB7Cu, 0x0017DB9Cu};

        struct Movie
        {
            uint32_t lsn = 0, size = 0, ram = 0;
            bool loops = false;
            std::vector<uint8_t> video;
            std::vector<size_t> cuts;
            size_t next = 0;
            std::unique_ptr<bozo::libs::VideoDecoder> decoder;
            std::deque<bozo::libs::YuvPicture> ready;
            uint32_t pictures = 0;
            bool ended = false;
        };
        Movie g_movie;
        uint32_t g_openLsn = 0, g_openSize = 0, g_bs = 0, g_bsSize = 0;
        bool g_fromRam = false;

        // The video elementary stream of a program stream (PES packets of stream 0xE0).
        std::vector<uint8_t> demuxVideo(const std::vector<uint8_t> &ps)
        {
            std::vector<uint8_t> out;
            size_t i = 0;
            while (i + 4 <= ps.size())
            {
                if (ps[i] || ps[i + 1] || ps[i + 2] != 1)
                {
                    ++i;
                    continue;
                }
                const uint8_t id = ps[i + 3];
                if (id == 0xB9)
                    break;
                if (id == 0xBA)
                {
                    if (i + 14 > ps.size())
                        break;
                    i += 14 + (ps[i + 13] & 7);
                    continue;
                }
                if (i + 6 > ps.size())
                    break;
                const size_t length = (size_t(ps[i + 4]) << 8) | ps[i + 5];
                const size_t body = i + 6, end = std::min(ps.size(), body + length);
                if (id == 0xE0 && body + 3 <= end)
                {
                    const size_t payload = body + 3 + ps[body + 2];
                    if (payload <= end)
                        out.insert(out.end(), ps.begin() + payload, ps.begin() + end);
                }
                i = end;
            }
            return out;
        }

        // Where each coded picture begins: at the sequence or GOP header before it, else at its picture header.
        std::vector<size_t> cutsOf(const std::vector<uint8_t> &s)
        {
            std::vector<size_t> cuts;
            size_t header = SIZE_MAX;
            for (size_t i = 0; i + 4 <= s.size(); ++i)
            {
                if (s[i] || s[i + 1] || s[i + 2] != 1)
                    continue;
                const uint8_t code = s[i + 3];
                if ((code == 0xB3 || code == 0xB8) && header == SIZE_MAX)
                    header = i;
                else if (code == 0x00)
                {
                    cuts.push_back(header != SIZE_MAX ? header : i);
                    header = SIZE_MAX;
                }
            }
            return cuts;
        }

        bool programStream(const std::vector<uint8_t> &s) { return s.size() >= 4 && !s[0] && !s[1] && s[2] == 1 && s[3] == 0xBA; }

        void open(uint8_t *m, Runtime *rt)
        {
            Movie mv;
            std::vector<uint8_t> bytes;
            if (g_fromRam)
            {
                mv.ram = g_bs;
                mv.size = g_bsSize;
                mv.loops = true;
                bytes.assign(m + (g_bs & 0x1FFFFFFu), m + ((g_bs + g_bsSize) & 0x1FFFFFFu));
            }
            else
            {
                mv.lsn = g_openLsn;
                mv.size = g_openSize;
                std::ifstream disc(bozo::port::libraries(rt).cd.imagePath, std::ios::binary);
                bytes.resize(mv.size);
                disc.seekg(std::streamoff(mv.lsn) * 2048);
                disc.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size()));
                bytes.resize(size_t(disc.gcount()));
            }
            mv.video = programStream(bytes) ? demuxVideo(bytes) : std::move(bytes);
            mv.cuts = cutsOf(mv.video);
            mv.decoder = bozo::libs::makeVideoDecoder();
            g_movie = std::move(mv);
        }

        bool nextPicture(bozo::libs::YuvPicture &out)
        {
            Movie &mv = g_movie;
            while (mv.ready.empty() && mv.next < mv.cuts.size())
            {
                const size_t from = mv.cuts[mv.next], to = mv.next + 1 < mv.cuts.size() ? mv.cuts[mv.next + 1] : mv.video.size();
                mv.decoder->picture(mv.video.data() + from, to - from);
                if (++mv.next == mv.cuts.size())
                    mv.decoder->flush();
                bozo::libs::YuvPicture p;
                while (mv.decoder->next(p))
                    mv.ready.push_back(std::move(p));
            }
            if (mv.ready.empty() && mv.loops && !mv.cuts.empty())
            {
                mv.decoder = bozo::libs::makeVideoDecoder();
                mv.next = 0;
                return nextPicture(out);
            }
            if (mv.ready.empty())
                return false;
            out = std::move(mv.ready.front());
            mv.ready.pop_front();
            return true;
        }

        uint8_t clamp(int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); }

        void writePicture(uint8_t *m, uint32_t dest, const bozo::libs::YuvPicture &p)
        {
            const uint32_t w = (p.width + 15) & ~15, h = (p.height + 15) & ~15;
            for (uint32_t mbx = 0; mbx < w / 16; ++mbx)
                for (uint32_t y = 0; y < h; ++y)
                {
                    uint8_t *row = m + ((dest + (mbx * h + y) * 64) & 0x1FFFFFFu);
                    for (uint32_t x = 0; x < 16; ++x)
                    {
                        const uint32_t sx = std::min<uint32_t>(mbx * 16 + x, p.width - 1), sy = std::min<uint32_t>(y, p.height - 1);
                        const int Y = p.y[sy * p.width + sx] - 16;
                        const size_t c = (sy / 2) * (p.width / 2) + sx / 2;
                        const int U = p.u[c] - 128, V = p.v[c] - 128;
                        row[x * 4 + 0] = clamp((298 * Y + 409 * V + 128) >> 8);
                        row[x * 4 + 1] = clamp((298 * Y - 100 * U - 208 * V + 128) >> 8);
                        row[x * 4 + 2] = clamp((298 * Y + 516 * U + 128) >> 8);
                        row[x * 4 + 3] = 0x80;
                    }
                }
        }

        void getPicture(uint8_t *m, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t mp = getRegU32(ctx, 4), dest = getRegU32(ctx, 5);
            const bool changed = g_fromRam ? (g_movie.ram != g_bs || g_movie.size != g_bsSize)
                                           : (g_movie.lsn != g_openLsn || g_movie.ram);
            if ((g_fromRam ? g_bs != 0 : g_openLsn != 0) && (changed || !g_movie.decoder))
                open(m, rt);
            bozo::libs::YuvPicture p;
            if (g_movie.decoder && nextPicture(p) && p.width > 0 && p.height > 0)
            {
                writePicture(m, dest, p);
                ++g_movie.pictures;
                std::memcpy(m + (mp & 0x1FFFFFFu), &p.width, 4);
                std::memcpy(m + ((mp + 4) & 0x1FFFFFFu), &p.height, 4);
                std::memcpy(m + ((mp + 8) & 0x1FFFFFFu), &g_movie.pictures, 4);
            }
            else if (g_movie.decoder)
                g_movie.ended = true;
            setReturnS32(ctx, 0);
            ctx->pc = getRegU32(ctx, 31);
        }

        void addBs(uint8_t *, R5900Context *ctx, Runtime *)
        {
            g_bs = getRegU32(ctx, 5);
            g_bsSize = getRegU32(ctx, 6);
            g_fromRam = true;
            setReturnS32(ctx, 0);
            ctx->pc = getRegU32(ctx, 31);
        }

        void isEnd(uint8_t *, R5900Context *ctx, Runtime *)
        {
            setReturnS32(ctx, g_movie.ended ? 1 : 0);
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    bool observe(R5900Context *ctx, uint32_t pc)
    {
        if (pc == kStreamOpen[0])
            g_openLsn = getRegU32(ctx, 3), g_fromRam = false;
        else if (pc == kStreamOpen[1])
            g_openSize = getRegU32(ctx, 3);
        else
            return false;
        return true;
    }

    void install(bozo::port::Registry &runtime)
    {
        if (!bozo::libs::haveVideoDecoder())
            return;
        runtime.registerFunction(kGetPicture, &getPicture);
        runtime.registerFunction(kIsEnd, &isEnd);
        runtime.registerFunction(kAddBs, &addBs);
    }
}
