// SCTPT2's 2D drawn natively. The game's menus, HUD and text go through its 2D library (0x179AB0-0x17A3A8): cached setters
// (ALPHA, TEX0, TEX1, TEST, PRIM, RGBAQ), vertex writes (UV, XYZF2, XYZF3) and register writes, collected into packets
// linked into the frame's ordering table by bucket; a few screens (the movies, the upload lists) link packets of their own
// data there, and the 3D links VIF1 packets that run VU1 microprograms. Here the library builds no packet: its register
// writes are kept per packet, the game's own packets by where they are, and at the frame's end the frame's DMA chain is
// played as VIF1 plays it (UNPACKs into VU1 memory, the lifted VU1 microcode run where a packet starts it, DIRECT data
// and XGKICKs read as GIF packets), the library's packets where they were linked, into primitives drawn through
// Renderer::draw, and images kept in sct2::textures, where the textures drawn are decoded from.
#include "sct2_2d.h"
#include "sct2_textures.h"
#include "sct2_vu1.h"

#include "bozo/context.h"
#include "bozo/port.h"
#include "bozo/render/renderer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace sct2::draw2d
{
    namespace
    {
        using bozo::render::Material;
        using bozo::render::Vertex;

        constexpr uint32_t kContext = 0x00463370u;
        constexpr uint32_t kDrawEnv = 0x00240FF0u;
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
                Copy,
            } kind;
            uint32_t reg, addr, stop;
            uint64_t value;
        };
        using Packet = std::vector<Command>;

        struct Gs
        {
            uint64_t prim = 0, rgbaq = 0x3F80000000000000ull, st = 0, uv = 0, texa = 0, bitbltbuf = 0, trxpos = 0, trxreg = 0;
            std::array<uint64_t, 2> tex0{}, tex1{}, clamp{}, alpha{}, test{}, frame{}, xyoffset{}, zbuf{};
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
        std::vector<Packet> g_frame2d;
        constexpr uint32_t kMarker = 0x5C7A2D00u;
        bozo::render::Screen g_screen;

        template <class T> T rd(uint8_t *m, uint32_t a) { T v; std::memcpy(&v, m + (a & 0x1FFFFFFu), sizeof v); return v; }
        template <class T> void wr(uint8_t *m, uint32_t a, T v) { std::memcpy(m + (a & 0x1FFFFFFu), &v, sizeof v); }

        void ret(R5900Context *ctx) { ctx->pc = getRegU32(ctx, 31); }

        Material material(uint32_t c, std::shared_ptr<const bozo::render::Texture> &texture)
        {
            const uint64_t prim = g_gs.prim;
            Material m;
            const uint64_t ztest = g_gs.test[c];
            const bool zte = (ztest >> 16) & 1;
            const uint32_t ztst = (ztest >> 17) & 3;
            m.depthTest = zte && ztst != 1;
            m.depthGreater = ztst == 3;
            m.depthWrite = zte && !((g_gs.zbuf[c] >> 32) & 1);
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
            case 0x4E: case 0x4F: g_gs.zbuf[reg - 0x4E] = v; break;
            case 0x50: g_gs.bitbltbuf = v; break;
            case 0x51: g_gs.trxpos = v; break;
            case 0x52: g_gs.trxreg = v; break;
            default: break;
            }
        }

        constexpr uint8_t kRegisterOf[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0xFF, 0x0C, 0x0D, 0xFF, 0xFF};

        template <class T> T at(const uint8_t *p) { T v; std::memcpy(&v, p, sizeof v); return v; }

        // GIF data as the GIF takes it (PATH2 from VIF1's DIRECT, PATH1 from VU1's XGKICK): a tag's data may continue in the
        // next transfer. `done` once a tag with EOP is through.
        struct Gif
        {
            uint64_t regs = 0;
            uint32_t nreg = 0, flg = 0, loops = 0, r = 0;
            bool active = false, eop = false, done = false, stopAtEop = false;
            std::vector<uint8_t> image;

            // A PACKED quadword in its register's packed format.
            static void packed(uint32_t d, const uint8_t *p)
            {
                const uint32_t w0 = at<uint32_t>(p), w1 = at<uint32_t>(p + 4), w2 = at<uint32_t>(p + 8), w3 = at<uint32_t>(p + 12);
                switch (d)
                {
                case 0x1:
                    g_gs.rgbaq = (g_gs.rgbaq & 0xFFFFFFFF00000000ull) | (w0 & 0xFF) | (w1 & 0xFF) << 8 | (w2 & 0xFF) << 16 |
                                 uint64_t(w3 & 0xFF) << 24;
                    break;
                case 0x2:
                    g_gs.st = w0 | uint64_t(w1) << 32;
                    g_gs.rgbaq = (g_gs.rgbaq & 0xFFFFFFFFull) | uint64_t(w2) << 32;
                    break;
                case 0x3: g_gs.uv = (w0 & 0x3FFF) | (w1 & 0x3FFF) << 16; break;
                case 0x4:
                    write((w3 >> 15) & 1 ? 0x0C : 0x04,
                          (w0 & 0xFFFF) | (w1 & 0xFFFF) << 16 | uint64_t((w2 >> 4) & 0xFFFFFF) << 32 | uint64_t((w3 >> 4) & 0xFF) << 56);
                    break;
                case 0x5: write((w3 >> 15) & 1 ? 0x0D : 0x05, (w0 & 0xFFFF) | (w1 & 0xFFFF) << 16 | uint64_t(w2) << 32); break;
                case 0xA: break;
                case 0xC:
                    write(0x0C, (w0 & 0xFFFF) | (w1 & 0xFFFF) << 16 | uint64_t((w2 >> 4) & 0xFFFFFF) << 32 | uint64_t((w3 >> 4) & 0xFF) << 56);
                    break;
                case 0xD: write(0x0D, (w0 & 0xFFFF) | (w1 & 0xFFFF) << 16 | uint64_t(w2) << 32); break;
                case 0xE: write(p[8], at<uint64_t>(p)); break;
                case 0xF: break;
                default: write(kRegisterOf[d], at<uint64_t>(p)); break;
                }
            }

            void item(const uint8_t *p)
            {
                const uint32_t d = (regs >> (4 * r)) & 0xF;
                if (flg == 0)
                    packed(d, p);
                else if (kRegisterOf[d] != 0xFF)
                    write(kRegisterOf[d], at<uint64_t>(p));
                if (++r == nreg)
                    r = 0, --loops;
            }

            // Returns the bytes taken.
            size_t feed(const uint8_t *data, size_t bytes)
            {
                size_t o = 0;
                while (o < bytes && !done)
                {
                    if (!active)
                    {
                        if (o + 16 > bytes)
                            break;
                        const uint64_t lo = at<uint64_t>(data + o);
                        regs = at<uint64_t>(data + o + 8);
                        o += 16;
                        loops = lo & 0x7FFF;
                        flg = (lo >> 58) & 3;
                        nreg = ((lo >> 60) & 0xF) ? uint32_t((lo >> 60) & 0xF) : 16;
                        eop = (lo >> 15) & 1;
                        r = 0;
                        if ((lo >> 46) & 1)
                            write(0x00, (lo >> 47) & 0x7FF);
                        active = loops != 0;
                        image.clear();
                        if (!active && eop && stopAtEop)
                            done = true;
                        continue;
                    }
                    if (flg == 0)
                    {
                        if (o + 16 > bytes)
                            break;
                        item(data + o);
                        o += 16;
                    }
                    else if (flg == 1)
                    {
                        if (o + 8 > bytes)
                            break;
                        item(data + o);
                        o += 8;
                        if (!loops)
                            o = (o + 15) & ~size_t(15);
                    }
                    else
                    {
                        const uint32_t n = std::min<uint32_t>(loops, uint32_t((bytes - o) / 16));
                        if (!n)
                            break;
                        image.insert(image.end(), data + o, data + o + n * 16);
                        o += n * 16;
                        loops -= n;
                        if (!loops)
                            textures::upload(g_gs.bitbltbuf, g_gs.trxpos, g_gs.trxreg, image.data(), image.size());
                    }
                    if (!loops)
                    {
                        active = false;
                        if (eop && stopAtEop)
                            done = true;
                    }
                }
                return o;
            }
        };

        // VIF1 as the game's packets drive it: its registers, VU1's memory and registers, and the lifted VU1 microcode
        // run where a packet starts or continues it, its XGKICKs drawn as GIF packets.
        struct Vif1
        {
            uint32_t cl = 1, wl = 1, mask = 0, mode = 0, base = 0, offset = 0, tops = 0, top = 0, itops = 0, itop = 0, dbf = 0, tpc = 0;
            uint32_t row[4] = {}, col[4] = {};
            R5900Context vu;
            std::vector<uint8_t> mem = std::vector<uint8_t>(0x4000);

            static void kick(void *user, uint32_t qw)
            {
                auto &self = *static_cast<Vif1 *>(user);
                Gif gif;
                gif.stopAtEop = true;
                const uint32_t from = (qw & 0x3FF) * 16;
                const size_t took = gif.feed(self.mem.data() + from, 0x4000 - from);
                if (!gif.done && took == 0x4000 - from)
                    gif.feed(self.mem.data(), from);
            }

            void unpack(uint32_t code, const uint32_t *w, size_t words)
            {
                const uint32_t cmd = (code >> 24) & 0x7F, vn = (cmd >> 2) & 3, vl = cmd & 3;
                const uint32_t num = ((code >> 16) & 0xFF) ? (code >> 16) & 0xFF : 256;
                const bool usn = (code >> 14) & 1, masked = cmd & 0x10;
                uint32_t addr = code & 0x3FF;
                if ((code >> 15) & 1)
                    addr += tops;
                const uint32_t width = vl == 3 ? 16 : 32u >> vl, comps = vl == 3 ? 1 : vn + 1;
                size_t bit = 0;
                auto take = [&](uint32_t n) -> uint32_t {
                    const size_t word = bit / 32, shift = bit % 32;
                    bit += n;
                    if (word >= words)
                        return 0;
                    uint64_t v = w[word];
                    if (word + 1 < words)
                        v |= uint64_t(w[word + 1]) << 32;
                    return uint32_t((v >> shift) & ((n == 32) ? 0xFFFFFFFFull : ((1ull << n) - 1)));
                };
                for (uint32_t k = 0; k < num; ++k)
                {
                    const uint32_t slot = (cl >= wl) ? addr + (k / wl) * cl + (k % wl) : addr + k;
                    uint8_t *q = mem.data() + ((slot & 0x3FF) * 16);
                    uint32_t field[4] = {};
                    if (vl == 3)
                    {
                        const uint32_t c = take(16);
                        field[0] = (c & 0x1F) << 3;
                        field[1] = ((c >> 5) & 0x1F) << 3;
                        field[2] = ((c >> 10) & 0x1F) << 3;
                        field[3] = ((c >> 15) & 1) << 7;
                    }
                    else
                        for (uint32_t c = 0; c < comps; ++c)
                        {
                            uint32_t v = take(width);
                            if (width < 32 && !usn && (v >> (width - 1)) & 1)
                                v |= ~((1u << width) - 1);
                            field[c] = v;
                        }
                    if (vn == 0 && vl != 3)
                        field[1] = field[2] = field[3] = field[0];
                    const uint32_t present = vl == 3 ? 4 : (vn == 0 ? 4 : comps);
                    const uint32_t rowIndex = std::min<uint32_t>(k % (wl ? wl : 1), 3);
                    for (uint32_t c = 0; c < 4; ++c)
                    {
                        uint32_t sel = masked ? (mask >> (rowIndex * 8 + c * 2)) & 3 : 0;
                        if (sel == 0 && c >= present)
                            continue;
                        uint32_t v;
                        if (sel == 3)
                            continue;
                        if (sel == 1)
                            v = row[c];
                        else if (sel == 2)
                            v = col[rowIndex];
                        else
                        {
                            v = field[c];
                            if (mode == 1)
                                v += row[c];
                            else if (mode == 2)
                                v += row[c], row[c] = v;
                        }
                        std::memcpy(q + 4 * c, &v, 4);
                    }
                }
                (void)words;
            }

            void start(uint32_t pc)
            {
                top = tops;
                itop = itops;
                vu1::run(pc, &vu, mem.data(), itop, top, tpc, &Vif1::kick, this);
                dbf ^= 1;
                tops = base + (dbf ? offset : 0);
            }

            void codes(const std::vector<uint32_t> &w, const std::vector<uint8_t> &lane, Gif &direct)
            {
                size_t i = 0;
                while (i < w.size())
                {
                    const uint32_t code = w[i++], cmd = (code >> 24) & 0x7F, imm = code & 0xFFFF;
                    const uint32_t num = (code >> 16) & 0xFF;
                    if ((cmd & 0x60) == 0x60)
                    {
                        const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, n = num ? num : 256;
                        const size_t bits = size_t(vl == 3 ? 16 : (32u >> vl) * (vn + 1)) * n;
                        const size_t need = (bits + 31) / 32;
                        unpack(code, w.data() + i, std::min(need, w.size() - i));
                        i += need;
                    }
                    else if (cmd == 0x50 || cmd == 0x51)
                    {
                        while (i < w.size() && lane[i] != 0)
                            ++i;
                        const size_t n = size_t(imm ? imm : 65536) * 4;
                        const size_t m = std::min(n, w.size() - i);
                        direct.feed(reinterpret_cast<const uint8_t *>(w.data() + i), m * 4);
                        i += m;
                    }
                    else switch (cmd)
                    {
                    case 0x01: cl = imm & 0xFF, wl = (imm >> 8) & 0xFF; break;
                    case 0x02: offset = imm & 0x3FF, dbf = 0, tops = base; break;
                    case 0x03: base = imm & 0x3FF; break;
                    case 0x04: itops = imm & 0x3FF; break;
                    case 0x05: mode = imm & 3; break;
                    case 0x14: case 0x15: start(imm * 8); break;
                    case 0x17: start(tpc); break;
                    case 0x20: if (i < w.size()) mask = w[i++]; break;
                    case 0x30: for (int k = 0; k < 4 && i < w.size(); ++k) row[k] = w[i++]; break;
                    case 0x31: for (int k = 0; k < 4 && i < w.size(); ++k) col[k] = w[i++]; break;
                    case 0x4A: i += size_t(num ? num : 256) * 2; break;
                    default: break;
                    }
                }
            }
        };

        Vif1 g_vif1;

        // DMA tags from addr to the RET that ends a CALL's subroutine (or to `stop`, a packet's last tag): what they send to
        // VIF1 (each tag's upper half, then its data) played as VIF1 plays it.
        void replay(uint8_t *m, const Packet &packet);
        uint8_t *g_spr = nullptr;

        // A DMA address: bit 31 selects the scratchpad.
        uint32_t dmaWord(uint8_t *m, uint32_t a)
        {
            if ((a & 0x80000000u) && g_spr)
            {
                uint32_t v;
                std::memcpy(&v, g_spr + (a & 0x3FFCu), 4);
                return v;
            }
            return rd<uint32_t>(m, a);
        }

        void dmaList(uint8_t *m, uint32_t addr, uint32_t stop, uint32_t skip = 0)
        {
            std::vector<uint32_t> words;
            std::vector<uint8_t> lane;
            Gif direct;
            uint32_t stack[2] = {}, depth = 0;
            for (int guard = 0; guard < 65536 && addr; ++guard)
            {
                const uint64_t tag = dmaWord(m, addr) | uint64_t(dmaWord(m, addr + 4)) << 32;
                const uint32_t qwc = tag & 0xFFFF, id = (tag >> 28) & 7, ta = uint32_t(tag >> 32) & 0x8FFFFFF0u;
                const uint32_t data = (id == 3 || id == 4 || id == 0) ? ta : addr + 16;
                if (addr == stop)
                    break;
                if (dmaWord(m, addr + 12) == kMarker)
                {
                    g_vif1.codes(words, lane, direct);
                    words.clear(), lane.clear();
                    const uint32_t index = rd<uint32_t>(m, addr + 8);
                    if (index < g_frame2d.size())
                        replay(m, g_frame2d[index]);
                }
                else if (addr != skip)
                {
                    words.push_back(dmaWord(m, addr + 8)), lane.push_back(2);
                    words.push_back(dmaWord(m, addr + 12)), lane.push_back(3);
                    for (uint32_t k = 0; k < qwc * 4; ++k)
                        words.push_back(dmaWord(m, data + 4 * k)), lane.push_back(uint8_t(k & 3));
                }
                if (id == 7 || id == 0)
                    break;
                if (id == 5)
                {
                    if (depth < 2)
                        stack[depth++] = addr + 16 + qwc * 16;
                    addr = ta;
                }
                else if (id == 6)
                {
                    if (!depth)
                        break;
                    addr = stack[--depth];
                }
                else
                    addr = id == 2 ? ta : (id == 3 || id == 4) ? addr + 16 : addr + 16 + qwc * 16;
            }
            g_vif1.codes(words, lane, direct);
        }

        uint32_t g_gp = 0;

        void replay(uint8_t *m, const Packet &packet)
        {
            for (const Command &c : packet)
                switch (c.kind)
                {
                case Command::Write: write(c.reg, c.value); break;
                case Command::Call: dmaList(m, c.addr, 0); break;
                case Command::Copy: { Gif gif; gif.feed(m + (c.addr & 0x1FFFFFFu), c.stop - c.addr); break; }
                }
        }

        // The packet the library would have linked: a marker tag in its unused packet buffer, linked into the ordering
        // table as the game links a packet, played from there with the frame.
        void link(uint8_t *m)
        {
            if (!g_packet.empty() && g_gp)
            {
                const uint32_t index = uint32_t(g_frame2d.size());
                g_frame2d.push_back(std::move(g_packet));
                const uint32_t marker = rd<uint32_t>(m, kContext + 0x34) + index * 16;
                const uint32_t slot = rd<uint32_t>(m, g_gp - 0x74F4u) + rd<uint8_t>(m, kContext + 0x2E) * 16 + 4;
                const uint32_t old = rd<uint32_t>(m, slot);
                wr<uint64_t>(m, marker, (uint64_t(old) << 32) | 0x20000000ull);
                wr<uint32_t>(m, marker + 8, index);
                wr<uint32_t>(m, marker + 12, kMarker);
                wr<uint32_t>(m, slot, marker);
            }
            g_packet.clear();
        }

        void resetCache(uint8_t *m)
        {
            for (uint32_t off = 0x08; off <= 0x20; off += 8)
                wr<int64_t>(m, kContext + off, -1);
            wr<int32_t>(m, kContext + 0x28, -1);
            wr<int16_t>(m, kContext + 0x2C, -1);
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
            g_gp = getRegU32(ctx, 28);
            link(m);
            ret(ctx);
        }
        void frameEnd(uint8_t *m, R5900Context *ctx, Runtime *)
        {
            g_gp = getRegU32(ctx, 28);
            link(m);
            resetCache(m);
            wr<uint8_t>(m, kContext + 0x2E, 0);
            ret(ctx);
        }
        void bucket(uint8_t *m, R5900Context *ctx, Runtime *rt)
        {
            g_spr = bozo::port::scratchpad(rt);
            g_gp = getRegU32(ctx, 28);
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
        void otLink(uint8_t *m, R5900Context *ctx, Runtime *rt)
        {
            g_spr = bozo::port::scratchpad(rt);
            const uint32_t bucket = getRegU32(ctx, 4), size = getRegU32(ctx, 5), start = getRegU32(ctx, 6);
            const uint32_t slot = rd<uint32_t>(m, getRegU32(ctx, 28) - 0x74F4u) + bucket * 16 + 4;
            const uint32_t old = rd<uint32_t>(m, slot);
            wr<uint32_t>(m, slot, start);
            wr<uint32_t>(m, start + size - 0xC, old);
            (void)bucket;
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
        // The frame as its DMA chain plays it: the draw environment (all but the copy of the last frame to the display
        // buffer), then the ordering table being built, where the 2D library's packets are marked.
        if (g_gp)
        {
            dmaList(m, kDrawEnv, kDrawEnv + 0x200, kDrawEnv + 0xC0);
            dmaList(m, rd<uint32_t>(m, g_gp - 0x74F4u), 0);
        }
        g_frame2d.clear();
        // The frame's origin: XYOFFSET_1 of the draw environment the game sends first each frame (A+D writes after the
        // display packet's DMA tag).
        for (uint32_t at = kDrawEnv + 0x20, end = kDrawEnv + 0x10 + 11 * 16; at < end; at += 16)
            if (rd<uint8_t>(m, at + 8) == 0x18)
            {
                const uint64_t off = rd<uint64_t>(m, at);
                g_screen.originX = float(off & 0xFFFF) / 16.0f;
                g_screen.originY = float((off >> 32) & 0xFFFF) / 16.0f;
                break;
            }
        g_screen.width = 640.0f;
        g_screen.height = 448.0f;
        const uint32_t zpsm = (g_gs.zbuf[0] >> 24) & 0xF;
        g_screen.depthMax = zpsm == 0 ? 4294967295.0f : zpsm == 1 ? 16777215.0f : 65535.0f;
        // Every primitive in the order the GS draws them, tested against depth as the game set it.
        static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        auto &r = bozo::render::Renderer::instance();
        r.beginFrame(g_screen);
        for (const Draw &d : g_draws)
            r.draw(d.material, kIdentity, d.vertices.data(), uint32_t(d.vertices.size()), d.texture);
        r.endFrame();
        r.publishOverlay(g_screen);
    }
}
