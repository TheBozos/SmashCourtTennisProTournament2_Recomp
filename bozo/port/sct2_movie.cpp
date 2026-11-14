// SCTPT2's movies. A movie either streams from the disc, an MPEG program stream the game reads into a ring through its
// loader (0x17DB40 opens the stream: the file's sector and size), or sits whole in RAM, handed to libmpeg with
// sceMpegAddBs and looped (the main menu's). The game feeds the IPU and asks libmpeg for each picture
// (sceMpegGetPicture). Here the stream is read from the disc image or from RAM, its video demuxed when it is a program
// stream and decoded by BozoEngine's video decoder, and each picture written where the IPU would have put it: RGBA, alpha
// 0x80, in 16-pixel-wide strips of macroblocks, each the picture's height. The game then draws it as any other image.
// sceMpegIsEnd answers 1 after the last picture of a streamed movie; a movie in RAM loops. A streamed movie's sound (its
// private stream 1: SPU ADPCM after an SShd / SSbd header, the channels interleaved in blocks) is decoded with it and
// fed to the port's sound stream a little ahead while its pictures are taken.
#include "sct2_movie.h"

#include "bozo/audio/port_stream.h"
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
        constexpr uint32_t kOutRate = 48000;
        constexpr double kMovieGain = 0.49;

        // The stream open's stores of the file's first sector and its size.
        // bozo:native kStreamOpen
        constexpr uint32_t kStreamOpen[] = {0x0017DB7Cu, 0x0017DB9Cu};

        struct Movie
        {
            uint32_t lsn = 0, size = 0, ram = 0, open = 0;
            bool loops = false;
            std::vector<uint8_t> video;
            std::vector<size_t> cuts;
            size_t next = 0;
            std::unique_ptr<bozo::libs::VideoDecoder> decoder;
            std::deque<bozo::libs::YuvPicture> ready;
            uint32_t pictures = 0;
            bool ended = false;
            std::vector<int16_t> sound;
            size_t soundAt = 0;
        };
        Movie g_movie;
        uint32_t g_openLsn = 0, g_openSize = 0, g_bs = 0, g_bsSize = 0, g_opens = 0;
        bool g_fromRam = false;

        // An elementary stream of a program stream: the payloads of its PES packets (`skip` bytes of substream header
        // dropped from each).
        std::vector<uint8_t> demux(const std::vector<uint8_t> &ps, uint8_t stream, size_t skip = 0)
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
                if (id == stream && body + 3 <= end)
                {
                    const size_t payload = body + 3 + ps[body + 2] + skip;
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

        uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

        // A PSS movie's sound as 48 kHz stereo: its SPU ADPCM decoded per channel, then resampled.
        std::vector<int16_t> decodeSound(const std::vector<uint8_t> &a)
        {
            if (a.size() < 0x28 || std::memcmp(a.data(), "SShd", 4) != 0 || std::memcmp(a.data() + 0x20, "SSbd", 4) != 0)
                return {};
            const uint32_t rate = le32(a.data() + 0x0C), channels = le32(a.data() + 0x10), interleave = le32(a.data() + 0x14);
            if (le32(a.data() + 0x08) != 0x10 || !rate || channels < 1 || channels > 2 || !interleave || interleave % 16)
                return {};
            static constexpr int kF0[5] = {0, 60, 115, 98, 122}, kF1[5] = {0, 0, -52, -55, -60};
            std::vector<int16_t> pcm[2];
            int h1[2] = {}, h2[2] = {};
            const size_t data = 0x28, stride = size_t(interleave) * channels;
            for (size_t at = data; at + stride <= a.size(); at += stride)
                for (uint32_t c = 0; c < channels; ++c)
                    for (size_t b = at + c * interleave; b < at + (c + 1) * interleave; b += 16)
                    {
                        const int shift = a[b] & 0xF, filter = std::min((a[b] >> 4) & 7, 4);
                        for (int k = 0; k < 28; ++k)
                        {
                            const int nibble = (a[b + 2 + k / 2] >> ((k & 1) * 4)) & 0xF;
                            int v = int(int16_t(uint16_t(nibble << 12))) >> shift;
                            v += (h1[c] * kF0[filter] + h2[c] * kF1[filter] + 32) >> 6;
                            v = std::clamp(v, -32768, 32767);
                            h2[c] = h1[c], h1[c] = v;
                            pcm[c].push_back(int16_t(v));
                        }
                    }
            if (channels == 1)
                pcm[1] = pcm[0];
            const size_t in = std::min(pcm[0].size(), pcm[1].size());
            const size_t out = in * kOutRate / rate;
            std::vector<int16_t> sound(out * 2);
            for (size_t k = 0; k < out && in; ++k)
            {
                const double t = double(k) * rate / kOutRate;
                const size_t i = std::min(size_t(t), in - 1), j = std::min(i + 1, in - 1);
                const double f = t - double(i);
                for (int c = 0; c < 2; ++c)
                    sound[k * 2 + c] = int16_t((pcm[c][i] + (pcm[c][j] - pcm[c][i]) * f) * kMovieGain);
            }
            return sound;
        }

        // The movie's sound up to a fifth of a second ahead of the mixer.
        void feedSound(Runtime *rt)
        {
            Movie &mv = g_movie;
            bozo::audio::PortStream &stream = bozo::port::audioStream(rt);
            constexpr size_t kLead = kOutRate / 5;
            while (mv.soundAt * 2 < mv.sound.size() && stream.queued() < kLead)
            {
                const size_t n = std::min(kLead - stream.queued(), mv.sound.size() / 2 - mv.soundAt);
                const size_t took = stream.push(mv.sound.data() + mv.soundAt * 2, n);
                mv.soundAt += took;
                if (!took)
                    break;
            }
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
                mv.open = g_opens;
                mv.size = g_openSize;
                std::ifstream disc(bozo::port::libraries(rt).cd.imagePath, std::ios::binary);
                bytes.resize(mv.size);
                disc.seekg(std::streamoff(mv.lsn) * 2048);
                disc.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size()));
                bytes.resize(size_t(disc.gcount()));
            }
            if (programStream(bytes))
            {
                if (!mv.loops)
                    mv.sound = decodeSound(demux(bytes, 0xBD, 4));
                mv.video = demux(bytes, 0xE0);
            }
            else
                mv.video = std::move(bytes);
            bozo::port::audioStream(rt).clear();
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
                                           : (g_movie.open != g_opens || g_movie.ram);
            if ((g_fromRam ? g_bs != 0 : g_openLsn != 0) && (changed || !g_movie.decoder))
                open(m, rt);
            bozo::libs::YuvPicture p;
            if (g_movie.decoder && nextPicture(p) && p.width > 0 && p.height > 0)
            {
                writePicture(m, dest, p);
                ++g_movie.pictures;
                feedSound(rt);
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
            const bool reopened = !g_fromRam && g_movie.open != g_opens;
            setReturnS32(ctx, g_movie.ended && !reopened ? 1 : 0);
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    bool observe(R5900Context *ctx, uint32_t pc)
    {
        if (pc == kStreamOpen[0])
            g_openLsn = getRegU32(ctx, 3), g_fromRam = false, ++g_opens;
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
