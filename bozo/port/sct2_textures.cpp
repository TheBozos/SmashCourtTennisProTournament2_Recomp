#include "sct2_textures.h"

#include "sct2_gs_layout.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <unordered_map>
#include <vector>

namespace sct2::textures
{
    namespace
    {
        constexpr uint32_t kWords = 1u << 20;
        constexpr uint32_t kPageWords = 2048;

        std::vector<uint32_t> g_memory(kWords);
        std::vector<uint32_t> g_generation(kWords / kPageWords);
        uint64_t g_version = 0;

        struct Cached
        {
            std::shared_ptr<bozo::render::Texture> texture;
            std::vector<std::pair<uint32_t, uint32_t>> pages;
        };
        std::unordered_map<uint64_t, Cached> g_cache;

        void touch(uint32_t word, std::set<uint32_t> &pages) { pages.insert((word & (kWords - 1)) / kPageWords); }

        void writeWord(uint32_t word, uint32_t value, uint32_t mask)
        {
            uint32_t &w = g_memory[word & (kWords - 1)];
            w = (w & ~mask) | (value & mask);
        }

        uint32_t readWord(uint32_t word) { return g_memory[word & (kWords - 1)]; }

        uint32_t bitsOf(uint32_t psm)
        {
            switch (psm)
            {
            case gs::kCt32: return 32;
            case gs::kCt24: return 24;
            case gs::kT8: case gs::kT8h: return 8;
            case gs::kT4: case gs::kT4hl: case gs::kT4hh: return 4;
            default: return 0;
            }
        }

        uint32_t scaleAlpha(uint32_t a) { return std::min<uint32_t>(255, a * 255 / 128); }

        uint32_t colour(uint32_t c) { return (c & 0x00FFFFFFu) | (scaleAlpha(c >> 24) << 24); }
    }

    void upload(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, const uint8_t *data, size_t bytes)
    {
        const uint32_t dbp = (bitbltbuf >> 32) & 0x3FFF, dbw = (bitbltbuf >> 48) & 0x3F, psm = (bitbltbuf >> 56) & 0x3F;
        const uint32_t dx = (trxpos >> 32) & 0x7FF, dy = (trxpos >> 48) & 0x7FF;
        const uint32_t w = trxreg & 0xFFF, h = (trxreg >> 32) & 0xFFF;
        const uint32_t bits = bitsOf(psm);
        if (!bits)
        {
            static std::set<uint32_t> told;
            if (told.insert(psm).second)
                std::fprintf(stderr, "[sct2:textures] upload format 0x%x is not decoded\n", psm);
            return;
        }
        std::set<uint32_t> pages;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const size_t bit = (size_t(y) * w + x) * bits;
                if ((bit + bits + 7) / 8 > bytes)
                    goto done;
                const uint32_t px = dx + x, py = dy + y;
                uint32_t v = 0;
                for (uint32_t k = 0; k < (bits + 7) / 8; ++k)
                    v |= uint32_t(data[bit / 8 + k]) << (8 * k);
                if (bits == 4)
                    v = (v >> (bit & 4)) & 0xF;
                switch (psm)
                {
                case gs::kCt32: { const uint32_t a = gs::wordCt32(dbp, dbw, px, py); writeWord(a, v, ~0u); touch(a, pages); break; }
                case gs::kCt24: { const uint32_t a = gs::wordCt32(dbp, dbw, px, py); writeWord(a, v, 0xFFFFFFu); touch(a, pages); break; }
                case gs::kT8h: { const uint32_t a = gs::wordCt32(dbp, dbw, px, py); writeWord(a, v << 24, 0xFF000000u); touch(a, pages); break; }
                case gs::kT4hl: { const uint32_t a = gs::wordCt32(dbp, dbw, px, py); writeWord(a, v << 24, 0x0F000000u); touch(a, pages); break; }
                case gs::kT4hh: { const uint32_t a = gs::wordCt32(dbp, dbw, px, py); writeWord(a, v << 28, 0xF0000000u); touch(a, pages); break; }
                case gs::kT8:
                {
                    const uint32_t b = gs::byteT8(dbp, dbw, px, py);
                    writeWord(b >> 2, v << (8 * (b & 3)), 0xFFu << (8 * (b & 3)));
                    touch(b >> 2, pages);
                    break;
                }
                case gs::kT4:
                {
                    const uint32_t n = gs::nibbleT4(dbp, dbw, px, py);
                    writeWord(n >> 3, v << (4 * (n & 7)), 0xFu << (4 * (n & 7)));
                    touch(n >> 3, pages);
                    break;
                }
                }
            }
    done:
        for (uint32_t p : pages)
            ++g_generation[p];
    }

    std::shared_ptr<const bozo::render::Texture> texture(uint64_t tex0, uint64_t texa)
    {
        const uint32_t tbp = tex0 & 0x3FFF, tbw = (tex0 >> 14) & 0x3F, psm = (tex0 >> 20) & 0x3F;
        const uint32_t tw = 1u << std::min<uint32_t>((tex0 >> 26) & 0xF, 10), th = 1u << std::min<uint32_t>((tex0 >> 30) & 0xF, 10);
        const uint32_t cbp = (tex0 >> 37) & 0x3FFF, cpsm = (tex0 >> 51) & 0xF, csm = (tex0 >> 55) & 1;
        if (!bitsOf(psm) || (bitsOf(psm) <= 8 && cpsm != gs::kCt32))
        {
            static std::set<uint64_t> told;
            if (told.insert(psm | (uint64_t(cpsm) << 8)).second)
                std::fprintf(stderr, "[sct2:textures] texture format 0x%x (palette 0x%x) is not decoded\n", psm, cpsm);
            return nullptr;
        }
        const uint64_t key = (tex0 & 0x1FFFFFFFFFFFFFFFull) ^ (psm == gs::kCt24 ? texa * 0x9E3779B97F4A7C15ull : 0);
        auto found = g_cache.find(key);
        if (found != g_cache.end())
        {
            bool fresh = true;
            for (auto [page, generation] : found->second.pages)
                fresh = fresh && g_generation[page] == generation;
            if (fresh)
                return found->second.texture;
        }
        auto t = std::make_shared<bozo::render::Texture>();
        t->width = tw;
        t->height = th;
        t->rgba.resize(size_t(tw) * th);
        t->key = key;
        t->version = ++g_version;
        t->asset = true;
        std::set<uint32_t> pages;
        uint32_t palette[256] = {};
        if (bitsOf(psm) <= 8)
        {
            const uint32_t entries = bitsOf(psm) == 4 ? 16 : 256;
            for (uint32_t i = 0; i < entries; ++i)
            {
                uint32_t x = i & 7, y = i >> 3;
                if (entries == 256 && csm == 0)
                    bozo::render::csm1Position(i, x, y);
                const uint32_t a = gs::wordCt32(cbp, 1, x, y);
                touch(a, pages);
                palette[i] = colour(readWord(a));
            }
        }
        const uint32_t ta0 = texa & 0xFF, aem = (texa >> 15) & 1;
        for (uint32_t y = 0; y < th; ++y)
            for (uint32_t x = 0; x < tw; ++x)
            {
                uint32_t out = 0;
                switch (psm)
                {
                case gs::kCt32: { const uint32_t a = gs::wordCt32(tbp, tbw, x, y); touch(a, pages); out = colour(readWord(a)); break; }
                case gs::kCt24:
                {
                    const uint32_t a = gs::wordCt32(tbp, tbw, x, y);
                    touch(a, pages);
                    const uint32_t rgb = readWord(a) & 0xFFFFFF;
                    out = rgb | (aem && !rgb ? 0 : scaleAlpha(ta0) << 24);
                    break;
                }
                case gs::kT8h: { const uint32_t a = gs::wordCt32(tbp, tbw, x, y); touch(a, pages); out = palette[readWord(a) >> 24]; break; }
                case gs::kT4hl: { const uint32_t a = gs::wordCt32(tbp, tbw, x, y); touch(a, pages); out = palette[(readWord(a) >> 24) & 0xF]; break; }
                case gs::kT4hh: { const uint32_t a = gs::wordCt32(tbp, tbw, x, y); touch(a, pages); out = palette[readWord(a) >> 28]; break; }
                case gs::kT8:
                {
                    const uint32_t b = gs::byteT8(tbp, tbw, x, y);
                    touch(b >> 2, pages);
                    out = palette[(readWord(b >> 2) >> (8 * (b & 3))) & 0xFF];
                    break;
                }
                case gs::kT4:
                {
                    const uint32_t n = gs::nibbleT4(tbp, tbw, x, y);
                    touch(n >> 3, pages);
                    out = palette[(readWord(n >> 3) >> (4 * (n & 7))) & 0xF];
                    break;
                }
                }
                t->rgba[size_t(y) * tw + x] = out;
            }
        Cached c;
        c.texture = t;
        for (uint32_t p : pages)
            c.pages.emplace_back(p, g_generation[p]);
        t->pages = c.pages;
        g_cache[key] = std::move(c);
        return t;
    }
}
