// SCTPT2's VU0 microprograms: those the game starts from EE code (VCALLMS), each the lifted microcode from its entry,
// and the collision query (0x1000B4) that feeds VU0 through VIF0. Its packets are the game's collision data: the boxes
// of a scene and, per box, its triangles, each packet a few VIF codes (ITOP with the count, UNPACK of the data into VU0's
// memory, the program's start or continuation). Here the query does what those codes ask, the programs run as their
// lifted microcode, and the result is what the query returns: the nearest hit's point, normal and triangle.
#include "sct2_vu0.h"

#include "bozo/ops_vu0.h"
#include "bozo/port.h"

#include <cstdio>
#include <cstring>
#include <set>

namespace sct2::vu0
{
    namespace
    {
        using bozo::vu0::Vec;

        constexpr uint32_t kCollide = 0x001000B4u;

        template <class T> T rd(const uint8_t *m, uint32_t a) { T v; std::memcpy(&v, m + (a & 0x1FFFFFFu), sizeof v); return v; }
        template <class T> void wr(uint8_t *m, uint32_t a, T v) { std::memcpy(m + (a & 0x1FFFFFFu), &v, sizeof v); }

        template <uint32_t kEntry>
        void entry(uint8_t *, R5900Context *ctx, Runtime *rt)
        {
            uint32_t tpc = 0;
            run(kEntry, ctx, rt, 0, tpc);
        }

        struct Vif
        {
            uint32_t itop = 0, tpc = 0, cl = 1, wl = 1;
        };

        // An UNPACK's vectors written into VU0's memory from `at`.
        uint32_t unpack(Runtime *rt, const uint8_t *m, uint32_t at, uint32_t code, Vif &vif)
        {
            const uint32_t cmd = (code >> 24) & 0x7F, num = ((code >> 16) & 0xFF) ? (code >> 16) & 0xFF : 256;
            const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, addr = code & 0x3FF;
            const bool usn = (code >> 14) & 1;
            const uint32_t bits = (32u >> vl) * (vn + 1);
            for (uint32_t k = 0; k < num; ++k)
            {
                uint8_t *q = bozo::vu0::quad(rt, addr + k);
                for (uint32_t c = 0; c <= vn; ++c)
                {
                    const uint32_t bit = (k * (vn + 1) + c) * (32u >> vl);
                    uint32_t v = 0;
                    if (vl == 0)
                        v = rd<uint32_t>(m, at + bit / 8);
                    else if (vl == 1)
                        v = usn ? rd<uint16_t>(m, at + bit / 8) : uint32_t(int32_t(rd<int16_t>(m, at + bit / 8)));
                    else if (vl == 2)
                        v = usn ? rd<uint8_t>(m, at + bit / 8) : uint32_t(int32_t(rd<int8_t>(m, at + bit / 8)));
                    std::memcpy(q + 4 * c, &v, 4);
                }
                if (vn == 0)
                    for (uint32_t c = 1; c < 4; ++c)
                        std::memcpy(q + 4 * c, q, 4);
            }
            (void)vif;
            return (bits * num + 31) / 32 * 4;
        }

        // The VIF codes of one transfer to VIF0, from `at` to `end`.
        void vifCodes(R5900Context *ctx, Runtime *rt, const uint8_t *m, uint32_t at, uint32_t end, Vif &vif)
        {
            while (at + 4 <= end)
            {
                const uint32_t code = rd<uint32_t>(m, at), cmd = (code >> 24) & 0x7F;
                at += 4;
                if ((cmd & 0x60) == 0x60)
                    at += unpack(rt, m, at, code, vif);
                else if (cmd == 0x01)
                    vif.cl = code & 0xFF, vif.wl = (code >> 8) & 0xFF;
                else if (cmd == 0x04)
                    vif.itop = code & 0x3FF;
                else if (cmd == 0x14 || cmd == 0x15)
                    run((code & 0xFFFF) * 8, ctx, rt, vif.itop, vif.tpc);
                else if (cmd == 0x17)
                    run(vif.tpc, ctx, rt, vif.itop, vif.tpc);
                else if (cmd == 0x20)
                    at += 4;
                else if (cmd == 0x30 || cmd == 0x31)
                    at += 16;
                else if (cmd == 0x4A)
                    at += (((code >> 16) & 0xFF) ? (code >> 16) & 0xFF : 256) * 8;
                else if (cmd != 0x00 && cmd != 0x02 && cmd != 0x03 && cmd != 0x05 && cmd != 0x07 && cmd != 0x10 && cmd != 0x11 &&
                         cmd != 0x13)
                {
                    static std::set<uint32_t> told;
                    if (told.insert(cmd).second)
                        std::fprintf(stderr, "[sct2:vu0] VIF0 code %02x in collision data is not done\n", cmd);
                }
            }
        }

        // collide(a0 from, a1 to, a2 result, a3 the scene's DMA tags, t0 a list in the scratchpad): VU0 culls the scene's
        // boxes against the segment, then tests the triangles of each box kept; the result is the hit point and normal,
        // the return the hit triangle's id (0 when no box is near).
        void collide(uint8_t *m, R5900Context *ctx, Runtime *rt)
        {
            const uint32_t from = getRegU32(ctx, 4), to = getRegU32(ctx, 5), result = getRegU32(ctx, 6);
            const uint32_t tags = getRegU32(ctx, 7), list = getRegU32(ctx, 8);
            Vec *vf = ctx->vu0_vf;
            std::memcpy(&vf[1], m + (from & 0x1FFFFFFu), 16);
            std::memcpy(&vf[2], m + (to & 0x1FFFFFFu), 16);
            Vif vif;
            const uint64_t boxes = rd<uint64_t>(m, tags);
            vifCodes(ctx, rt, m, uint32_t(boxes >> 32), uint32_t(boxes >> 32) + uint32_t(boxes & 0xFFFF) * 16, vif);
            uint32_t kept = ctx->vi[5] & 0xFFFF;
            if (!kept)
            {
                setReturnU32(ctx, 0);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            uint32_t tpc = 0;
            run(0x158, ctx, rt, 0, tpc);
            vif.tpc = tpc;
            uint8_t *spr = bozo::port::scratchpad(rt);
            uint8_t toggle = 0;
            for (uint32_t k = 0; k < kept; ++k)
            {
                const uint16_t index = rd<uint16_t>(bozo::vu0::quad(rt, k), 0xC);
                const uint64_t tag = rd<uint64_t>(m, tags + index * 8u);
                const uint32_t slot = (list + 16 * k) & 0x3FF0u;
                std::memcpy(spr + slot, &tag, 8);
                std::memset(spr + slot + 8, 0, 8);
                for (uint32_t at = uint32_t(tag >> 32), guard = 0; guard < 4096; ++guard)
                {
                    const uint32_t code = rd<uint32_t>(m, at), cmd = code >> 24;
                    if (cmd == 0x6C)
                    {
                        wr<uint8_t>(m, at, toggle);
                        toggle ^= 0x80;
                        at += (((code >> 16) & 0xFF) << 4);
                    }
                    at += 4;
                    if (cmd == 0x97)
                        break;
                }
            }
            const uint64_t end = 0x70000000ull;
            std::memcpy(spr + ((list + 16 * kept) & 0x3FF0u), &end, 8);
            for (uint32_t k = 0; k < kept; ++k)
            {
                uint64_t t;
                std::memcpy(&t, spr + ((list + 16 * k) & 0x3FF0u), 8);
                vifCodes(ctx, rt, m, uint32_t(t >> 32), uint32_t(t >> 32) + uint32_t(t & 0xFFFF) * 16, vif);
            }
            const Vec hit = vf[4];
            Vec acc;
            bozo::vu0::mul(ctx, acc, vf[28], bozo::vu0::bc(vf[2], 0), 0xE);
            bozo::vu0::madd(ctx, acc, acc, vf[29], bozo::vu0::bc(vf[2], 1), 0xE);
            bozo::vu0::madd(ctx, acc, acc, vf[30], bozo::vu0::bc(vf[2], 2), 0xE);
            bozo::vu0::madd(ctx, vf[4], acc, vf[1], bozo::vu0::bc(vf[0], 3), 0xE);
            bozo::vu0::mul(ctx, acc, vf[28], bozo::vu0::bc(vf[25], 0), 0xE);
            bozo::vu0::madd(ctx, acc, acc, vf[29], bozo::vu0::bc(vf[25], 1), 0xE);
            bozo::vu0::madd(ctx, vf[25], acc, vf[30], bozo::vu0::bc(vf[25], 2), 0xE);
            ctx->vu0_acc = acc;
            std::memcpy(m + (result & 0x1FFFFFFu), &vf[4], 16);
            std::memcpy(m + ((result + 16) & 0x1FFFFFFu), &vf[25], 16);
            setReturnU32(ctx, hit.u[3]);
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    void install(bozo::port::Registry &runtime)
    {
        runtime.vu0Program(0x158u, &entry<0x158u>);
        runtime.vu0Program(0x610u, &entry<0x610u>);
        runtime.vu0Program(0x6D0u, &entry<0x6D0u>);
        runtime.registerFunction(kCollide, &collide);
    }
}
