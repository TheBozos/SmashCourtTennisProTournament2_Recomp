// SCTPT2's 2D drawn natively. The game's menus, HUD and text go through its 2D library (0x179AB0-0x17A3A8): cached setters
// (ALPHA, TEX0, TEX1, TEST, PRIM, RGBAQ), vertex writes (UV, XYZF2, XYZF3) and register writes, collected into packets
// linked into the frame's ordering table by bucket; a few screens (the movies, the upload lists) link packets of their own
// data there. Here the library builds no packet: its register writes are kept per packet, the game's own packets by
// where they are, and at the frame's end both are played in the order the ordering table plays them (buckets in order,
// the newest packet of a bucket first) into primitives drawn through Renderer::drawOverlay, and images kept in
// sct2::textures, where the textures drawn are decoded from.
#include "sct2_2d.h"
#include "sct2_textures.h"

#include "bozo/context.h"
#include "bozo/port.h"
#include "bozo/render/renderer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace sct2::draw2d
{
    namespace
    {
        using bozo::render::Material;
        using bozo::render::Vertex;

        constexpr uint32_t kContext = 0x00463370u;
        constexpr uint32_t kOpenTag = 0x00179AB0u;
        constexpr uint32_t kCloseTag = 0x00179AF8u;
        constexpr uint32_t kOpenPacket = 0x00179B70u;
        constexpr uint32_t kClosePacket = 0x00179BA8u;
        constexpr uint32_t kMode = 0x00179C20u;
        constexpr uint32_t kWrite = 0x00179C98u;
        constexpr uint32_t kLink = 0x00179D88u;
        constexpr uint32_t kFrameEnd = 0x00179E48u;
        constexpr uint32_t kBucket = 0x00179EB8u;
        constexpr uint32_t kPrim = 0x0017A070u;
        constexpr uint32_t kCallData = 0x0017A2A0u;
        constexpr uint32_t kCopyData = 0x0017A318u;

        struct Draw
        {
            Material material;
            std::shared_ptr<const bozo::render::Texture> texture;
            std::vector<Vertex> vertices;
        };

        // What a packet does when the frame is played: a register write, the game data a DMA call sends, or a packet the
        // game linked itself (its tags from `addr` to its last one, `stop`).
        struct Command
        {
            enum Kind : uint8_t
            {
                Write,
                Call,
                Linked,
                Copy,
            } kind;
            uint32_t reg, addr, stop;
            uint64_t value;
        };
        using Packet = std::vector<Command>;

        struct Gs
        {
            uint64_t prim = 0, rgbaq = 0x3F80000000000000ull, st = 0, uv = 0, texa = 0, bitbltbuf = 0, trxpos = 0, trxreg = 0;
            std::array<uint64_t, 2> tex0{}, tex1{}, clamp{}, alpha{}, test{}, frame{}, xyoffset{};
            struct Point
            {
                float x, y, z, u, v, q;
                uint32_t rgba;
            };
            std::array<Point, 3> queue{};
            uint32_t queued = 0, total = 0;
        };

        Gs g_gs;
        uint8_t g_mode = 0;
        Packet g_packet;
        std::vector<Draw> g_draws;
        std::map<uint32_t, std::vector<Packet>> g_buckets;
        bozo::render::Screen g_screen;

        template <class T> T rd(uint8_t *m, uint32_t a) { T v; std::memcpy(&v, m + (a & 0x1FFFFFFu), sizeof v); return v; }
        template <class T> void wr(uint8_t *m, uint32_t a, T v) { std::memcpy(m + (a & 0x1FFFFFFu), &v, sizeof v); }

        void ret(R5900Context *ctx) { ctx->pc = getRegU32(ctx, 31); }

        Material material(uint32_t c, std::shared_ptr<const bozo::render::Texture> &texture)
        {
            const uint64_t prim = g_gs.prim;
            Material m;
            m.depthTest = false;
            m.depthWrite = false;
            m.textured = (prim >> 4) & 1;
            if (m.textured)
            {
                const uint64_t tex0 = g_gs.tex0[c];
                texture = textures::texture(tex0, g_gs.texa);
                m.textured = texture != nullptr;
                if (texture)
                    m.tex0 = texture->key;
                m.textureFunction = static_cast<Material::TextureFunction>((tex0 >> 35) & 3);
                m.textureAlpha = (tex0 >> 34) & 1;
                m.clampU = (g_gs.clamp[c] & 3) == 1;
                m.clampV = ((g_gs.clamp[c] >> 2) & 3) == 1;
                m.nearest = !((g_gs.tex1[c] >> 5) & 1);
            }
            if ((prim >> 6) & 1)
            {
                const uint64_t a = g_gs.alpha[c];
                m.gsBlend = true;
                m.blendA = a & 3;
                m.blendB = (a >> 2) & 3;
                m.blendC = (a >> 4) & 3;
                m.blendD = (a >> 6) & 3;
                m.blendFix = (a >> 32) & 0xFF;
            }
            else
                m.blend = Material::Blend::Opaque;
            const uint64_t test = g_gs.test[c];
            if (test & 1)
            {
                m.alphaTest = true;
                m.alphaFunc = static_cast<Material::AlphaFunc>((test >> 1) & 7);
                m.alphaRef = (test >> 4) & 0xFF;
            }
            const uint32_t fbmsk = uint32_t(g_gs.frame[c] >> 32);
            m.colourMask = static_cast<uint8_t>(((fbmsk & 0xFFu) != 0xFFu) | (((fbmsk >> 8) & 0xFFu) != 0xFFu) << 1 |
                                                (((fbmsk >> 16) & 0xFFu) != 0xFFu) << 2 | ((fbmsk >> 24) != 0xFFu) << 3);
            return m;
        }

        void emitTriangles(const Gs::Point *const *points, uint32_t count, bool flatLast)
        {
            const uint32_t c = (g_gs.prim >> 9) & 1;
            std::shared_ptr<const bozo::render::Texture> texture;
            Material m = material(c, texture);
            if (!m.colourMask)
                return;
            const bool gouraud = (g_gs.prim >> 3) & 1;
            const float tw = texture ? float(texture->width) : 1.0f, th = texture ? float(texture->height) : 1.0f;
            const bool fst = (g_gs.prim >> 8) & 1;
            if (!fst)
                m.useQ = true;
            if (g_draws.empty() || g_draws.back().texture != texture || std::memcmp(&g_draws.back().material, &m, sizeof m) != 0)
                g_draws.push_back({m, texture, {}});
            auto &out = g_draws.back().vertices;
            for (uint32_t k = 0; k < count; ++k)
            {
                const Gs::Point &p = *points[k];
                Vertex v;
                v.x = p.x;
                v.y = p.y;
                v.z = p.z;
                v.u = fst ? p.u / tw : (p.q != 0.0f ? p.u / p.q : p.u);
                v.v = fst ? p.v / th : (p.q != 0.0f ? p.v / p.q : p.v);
                v.q = p.q;
                v.rgba = (gouraud && !flatLast) ? p.rgba : points[count - 1]->rgba;
                out.push_back(v);
            }
        }

        void kick()
        {
            const uint32_t type = g_gs.prim & 7;
            const auto &q = g_gs.queue;
            const uint32_t n = g_gs.total;
            auto at = [&](uint32_t back) -> const Gs::Point & { return q[(n - 1 - back) % 3]; };
            switch (type)
            {
            case 3:
                if (n % 3 == 0)
                {
                    const Gs::Point *p[3] = {&at(2), &at(1), &at(0)};
                    emitTriangles(p, 3, false);
                }
                break;
            case 4:
                if (n >= 3)
                {
                    const Gs::Point *p[3] = {&at(2), &at(1), &at(0)};
                    emitTriangles(p, 3, false);
                }
                break;
            case 5:
                if (n >= 3)
                {
                    const Gs::Point *p[3] = {&q[0], &at(1), &at(0)};
                    emitTriangles(p, 3, false);
                }
                break;
            case 6:
                if (n % 2 == 0)
                {
                    const Gs::Point &a = at(1), &b = at(0);
                    Gs::Point c0 = a, c1 = b, c2 = a, c3 = b;
                    c2.x = b.x, c2.u = b.u;
                    c3.x = a.x, c3.u = a.u;
                    c1.rgba = c0.rgba = c2.rgba = c3.rgba = b.rgba;
                    const Gs::Point *t1[3] = {&c0, &c2, &c1}, *t2[3] = {&c0, &c1, &c3};
                    emitTriangles(t1, 3, true);
                    emitTriangles(t2, 3, true);
                }
                break;
            default:
                break;
            }
        }

        void vertex(uint64_t v, bool f, bool draw)
        {
            Gs::Point p;
            p.x = float(v & 0xFFFF) / 16.0f;
            p.y = float((v >> 16) & 0xFFFF) / 16.0f;
            p.z = float(f ? (v >> 32) & 0xFFFFFF : v >> 32);
            p.rgba = uint32_t(g_gs.rgbaq);
            float q;
            const uint32_t qb = uint32_t(g_gs.rgbaq >> 32);
            std::memcpy(&q, &qb, 4);
            p.q = q;
            if ((g_gs.prim >> 8) & 1)
            {
                p.u = float(g_gs.uv & 0x3FFF) / 16.0f;
                p.v = float((g_gs.uv >> 16) & 0x3FFF) / 16.0f;
            }
            else
            {
                const uint32_t s = uint32_t(g_gs.st), t = uint32_t(g_gs.st >> 32);
                std::memcpy(&p.u, &s, 4);
                std::memcpy(&p.v, &t, 4);
            }
            g_gs.queue[g_gs.total % 3] = p;
            ++g_gs.total;
            if (draw)
                kick();
        }

        void write(uint32_t reg, uint64_t v)
        {
            switch (reg)
            {
            case 0x00: g_gs.prim = v; g_gs.total = 0; break;
            case 0x01: g_gs.rgbaq = v; break;
            case 0x02: g_gs.st = v; break;
            case 0x03: g_gs.uv = v; break;
            case 0x04: vertex(v, true, true); break;
            case 0x05: vertex(v, false, true); break;
            case 0x0C: vertex(v, true, false); break;
            case 0x0D: vertex(v, false, false); break;
            case 0x06: case 0x07: g_gs.tex0[reg - 0x06] = v; break;
            case 0x08: case 0x09: g_gs.clamp[reg - 0x08] = v; break;
            case 0x14: case 0x15: g_gs.tex1[reg - 0x14] = v; break;
            case 0x18: case 0x19: g_gs.xyoffset[reg - 0x18] = v; break;
            case 0x3B: g_gs.texa = v; break;
            case 0x42: case 0x43: g_gs.alpha[reg - 0x42] = v; break;
            case 0x47: case 0x48: g_gs.test[reg - 0x47] = v; break;
            case 0x4C: case 0x4D: g_gs.frame[reg - 0x4C] = v; break;
            case 0x50: g_gs.bitbltbuf = v; break;
            case 0x51: g_gs.trxpos = v; break;
            case 0x52: g_gs.trxreg = v; break;
            default: break;
            }
        }

        constexpr uint8_t kRegisterOf[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0xFF, 0x0C, 0x0D, 0xFF, 0xFF};

        // The GIF data of a transfer, as the GIF takes it: a tag's data may continue in the next DMA tag's.
        struct Gif
        {
            uint64_t regs = 0;
            uint32_t nreg = 0, flg = 0, loops = 0, r = 0;
            bool active = false;
            std::vector<uint8_t> image;

            void item(uint8_t *m, uint32_t at)
            {
                const uint32_t d = (regs >> (4 * r)) & 0xF;
                if (flg == 0 && d == 0xE)
                    write(rd<uint8_t>(m, at + 8), rd<uint64_t>(m, at));
                else if (kRegisterOf[d] != 0xFF)
                    write(kRegisterOf[d], rd<uint64_t>(m, at));
                if (++r == nreg)
                    r = 0, --loops;
            }

            void feed(uint8_t *m, uint32_t at, uint32_t end)
            {
                while (at < end)
                {
                    if (!active)
                    {
                        if (at + 16 > end)
                            return;
                        const uint64_t lo = rd<uint64_t>(m, at);
                        regs = rd<uint64_t>(m, at + 8);
                        at += 16;
                        loops = lo & 0x7FFF;
                        flg = (lo >> 58) & 3;
                        nreg = ((lo >> 60) & 0xF) ? uint32_t((lo >> 60) & 0xF) : 16;
                        r = 0;
                        if ((lo >> 46) & 1)
                            write(0x00, (lo >> 47) & 0x7FF);
                        active = loops != 0;
                        image.clear();
                        continue;
                    }
                    if (flg == 0)
                    {
                        if (at + 16 > end)
                            return;
                        item(m, at);
                        at += 16;
                    }
                    else if (flg == 1)
                    {
                        if (at + 8 > end)
                            return;
                        item(m, at);
                        at += 8;
                        if (!loops)
                            at = (at + 15) & ~15u;
                    }
                    else
                    {
                        const uint32_t n = std::min<uint32_t>(loops, (end - at) / 16);
                        if (!n)
                            return;
                        image.insert(image.end(), m + (at & 0x1FFFFFFu), m + ((at + n * 16) & 0x1FFFFFFu));
                        at += n * 16;
                        loops -= n;
                        if (!loops)
                            textures::upload(g_gs.bitbltbuf, g_gs.trxpos, g_gs.trxreg, image.data(), image.size());
                    }
                    if (!loops)
                        active = false;
                }
            }
        };

        // DMA tags from addr to the RET that ends a CALL's subroutine (or to `stop`, a packet's last tag), each sending its
        // data with the VIF codes in its upper half; the GIF data of DIRECT codes is played.
        void dmaList(uint8_t *m, uint32_t addr, uint32_t stop)
        {
            Gif gif;
            uint32_t stack[2] = {}, depth = 0;
            for (int guard = 0; guard < 4096 && addr; ++guard)
            {
                const uint64_t tag = rd<uint64_t>(m, addr);
                const uint32_t qwc = tag & 0xFFFF, id = (tag >> 28) & 7, ta = uint32_t(tag >> 32) & 0x7FFFFFF0u;
                const uint32_t data = (id == 3 || id == 4 || id == 0) ? ta : addr + 16;
                for (int k = 0; k < 2; ++k)
                {
                    const uint32_t code = rd<uint32_t>(m, addr + 8 + 4 * k), cmd = (code >> 24) & 0x7F;
                    if (cmd == 0x50 || cmd == 0x51)
                        gif.feed(m, data, data + qwc * 16);
                }
                if (addr == stop || id == 7 || id == 0)
                    return;
                if (id == 5)
                {
                    if (depth < 2)
                        stack[depth++] = addr + 16 + qwc * 16;
                    addr = ta;
                }
                else if (id == 6)
                {
                    if (!depth)
                        return;
                    addr = stack[--depth];
                }
                else
                    addr = id == 2 ? ta : (id == 3 || id == 4) ? addr + 16 : addr + 16 + qwc * 16;
            }
        }

        void link(uint8_t *m)
        {
            if (!g_packet.empty())
                g_buckets[rd<uint8_t>(m, kContext + 0x2E)].push_back(std::move(g_packet));
            g_packet.clear();
        }

        void resetCache(uint8_t *m)
        {
            for (uint32_t off = 0x08; off <= 0x28; off += 8)
                wr<int64_t>(m, kContext + off, -1);
        }

        void openTag(uint8_t *m, R5900Context *ctx, Runtime *)
        {
            g_mode = getRegU32(ctx, 4) ? 1 : 0;
            wr<uint8_t>(m, kContext + 0x2F, g_mode);
            ret(ctx);
        }
        void nothing(uint8_t *, R5900Context *ctx, Runtime *) { ret(ctx); }
        void mode(uint8_t *m, R5900Context *ctx, Runtime *rt) { openTag(m, ctx, rt); }
        void record(uint32_t reg, uint64_t v) { g_packet.push_back({Command::Write, reg, 0, 0, v}); }

        void registerWrite(uint8_t *, R5900Context *ctx, Runtime *)
        {
            const uint32_t reg = getRegU32(ctx, 4);
            const uint64_t v = getRegU64(ctx, 5);
            if (g_mode)
            {
                if (kRegisterOf[reg & 0xF] != 0xFF)
                    record(kRegisterOf[reg & 0xF], v);
            }
            else
                record(reg & 0xFF, v);
            ret(ctx);
        }
        void linkPacket(uint8_t *m, R5900Context *ctx, Runtime *)
        {
            link(m);
            ret(ctx);
        }
        void frameEnd(uint8_t *m, R5900Context *ctx, Runtime *)
        {
            link(m);
            resetCache(m);
            wr<uint8_t>(m, kContext + 0x2E, 0);
            ret(ctx);
        }
        void bucket(uint8_t *m, R5900Context *ctx, Runtime *)
        {
            const uint8_t b = static_cast<uint8_t>(getRegU32(ctx, 4));
            if (rd<uint8_t>(m, kContext + 0x2E) != b)
            {
                link(m);
                resetCache(m);
                wr<uint8_t>(m, kContext + 0x2E, b);
            }
            ret(ctx);
        }
        void prim(uint8_t *m, R5900Context *ctx, Runtime *)
        {
            const uint16_t v = static_cast<uint16_t>(getRegU32(ctx, 4));
            if (rd<uint16_t>(m, kContext + 0x2C) != v || (v & 7) == 5)
            {
                wr<uint16_t>(m, kContext + 0x2C, v);
                record(0x00, v);
            }
            ret(ctx);
        }
        void callData(uint8_t *, R5900Context *ctx, Runtime *)
        {
            g_packet.push_back({Command::Call, 0, getRegU32(ctx, 4), 0, 0});
            ret(ctx);
        }
        void copyData(uint8_t *m, R5900Context *ctx, Runtime *)
        {
            const uint32_t src = getRegU32(ctx, 4), qwc = getRegU32(ctx, 5);
            g_packet.push_back({Command::Copy, 0, src, src + qwc * 16, 0});
            resetCache(m);
            ret(ctx);
        }
    }

    namespace
    {
        constexpr uint32_t kOtLink = 0x00177340u;

        // A packet the game links into the ordering table itself (an upload list, a movie's picture): linked as the game
        // does, and played with the frame.
        void otLink(uint8_t *m, R5900Context *ctx, Runtime *)
        {
            const uint32_t bucket = getRegU32(ctx, 4), size = getRegU32(ctx, 5), start = getRegU32(ctx, 6);
            const uint32_t slot = rd<uint32_t>(m, getRegU32(ctx, 28) - 0x74F4u) + bucket * 16 + 4;
            const uint32_t old = rd<uint32_t>(m, slot);
            wr<uint32_t>(m, slot, start);
            wr<uint32_t>(m, start + size - 0xC, old);
            g_buckets[bucket].push_back({{Command::Linked, 0, start, start + size - 0x10, 0}});
            ret(ctx);
        }
    }

    void install(bozo::port::Registry &runtime)
    {
        runtime.registerFunction(kOtLink, &otLink);
        runtime.registerFunction(kOpenTag, &openTag);
        runtime.registerFunction(kCloseTag, &nothing);
        runtime.registerFunction(kOpenPacket, &nothing);
        runtime.registerFunction(kClosePacket, &nothing);
        runtime.registerFunction(kMode, &mode);
        runtime.registerFunction(kWrite, &registerWrite);
        runtime.registerFunction(kLink, &linkPacket);
        runtime.registerFunction(kFrameEnd, &frameEnd);
        runtime.registerFunction(kBucket, &bucket);
        runtime.registerFunction(kPrim, &prim);
        runtime.registerFunction(kCallData, &callData);
        runtime.registerFunction(kCopyData, &copyData);
    }

    void publish(uint8_t *m)
    {
        g_draws.clear();
        for (auto &[b, packets] : g_buckets)
            for (auto it = packets.rbegin(); it != packets.rend(); ++it)
                for (const Command &c : *it)
                    switch (c.kind)
                    {
                    case Command::Write: write(c.reg, c.value); break;
                    case Command::Call: dmaList(m, c.addr, 0); break;
                    case Command::Linked: dmaList(m, c.addr, c.stop); break;
                    case Command::Copy: { Gif gif; gif.feed(m, c.addr, c.stop); break; }
                    }
        const uint64_t off = g_gs.xyoffset[0];
        if (off)
        {
            g_screen.originX = float(off & 0xFFFF) / 16.0f;
            g_screen.originY = float((off >> 32) & 0xFFFF) / 16.0f;
        }
        g_screen.width = 640.0f;
        g_screen.height = 448.0f;
        auto &r = bozo::render::Renderer::instance();
        for (const Draw &d : g_draws)
            r.drawOverlay(d.material, d.vertices.data(), uint32_t(d.vertices.size()), d.texture);
        r.publishOverlay(g_screen);
        g_buckets.clear();
    }
}
