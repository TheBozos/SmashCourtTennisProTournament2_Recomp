"""SCTPT2's VU0 microprograms as C++ on BozoEngine's VU0 operations, from the microcode the game uploads (the MPG codes
of the VIF0 chain at 0x1A3F10 in SLUS_209.33). Each pair of instructions does what VU0 does with it: both halves read
the registers as they were, a branch's next pair runs before it is taken, an E bit stops after the next pair. VU0's
timing is followed while the program runs, as the microcode relies on it: a pair issues when the registers it reads are
written (4 cycles after the instruction writing them), the MAC, status and clip flags of an instruction are seen from 4
cycles after it, Q from 7 (13 for RSQRT) after its DIV, and WAITQ waits for it.

    python tools/sct2_vu0_lift.py <SLUS_209.33> <out.cpp> [--unit 1]

--unit 1 lifts VU1's microcode instead (the VIF1 chain at 0x1A4D40), with its 16 KB of memory, XGKICK handing a GIF
packet to the caller and XTOP / XITOP reading VIF1's TOP and ITOP.
"""
import struct
import sys
from pathlib import Path

UNITS = {0: (0x001A3F10, 225, 0x1000), 1: (0x001A4D40, 765, 0x4000)}
TEXT_VA = 0x00100000
BC = 'xyzw'


def microcode(elf, unit):
    chain, qwc, size = UNITS[unit]
    shoff, = struct.unpack_from('<I', elf, 0x20)
    base = None
    for k in range(struct.unpack_from('<H', elf, 0x30)[0]):
        sh = struct.unpack_from('<10I', elf, shoff + k * 40)
        if sh[3] == TEXT_VA:
            base = sh[4]
    at = base + chain - TEXT_VA
    stream = elf[at + 8:at + 16] + elf[at + 16:at + 16 + qwc * 16]
    code = bytearray(size)
    o = 0
    while o + 4 <= len(stream):
        w, = struct.unpack_from('<I', stream, o)
        cmd, imm, num = (w >> 24) & 0x7F, w & 0xFFFF, (w >> 16) & 0xFF
        o += 4
        if cmd == 0x4A:
            n = num or 256
            code[imm * 8:imm * 8 + n * 8] = stream[o:o + n * 8]
            o += n * 8
        elif cmd & 0x60 == 0x60:
            n = num or 256
            vn, vl = (cmd >> 2) & 3, cmd & 3
            o += ((32 >> vl) * (vn + 1) * n + 31) // 32 * 4
        elif cmd == 0x20:
            o += 4
        elif cmd in (0x30, 0x31):
            o += 16
        elif cmd in (0x50, 0x51):
            o = (o + 15) & ~15
            o += (imm or 65536) * 16
    return bytes(code)


def dest(w): return (w >> 21) & 0xF


def s11(w):
    v = w & 0x7FF
    return v - 0x800 if v & 0x400 else v


class Pair:
    def __init__(self, pc, lo, up):
        self.pc, self.lo, self.up = pc, lo, up
        self.e = bool((up >> 30) & 1)
        self.loi = bool((up >> 31) & 1)

    def branch(self):
        if self.loi:
            return None
        op7 = (self.lo >> 25) & 0x7F
        if op7 in (0x20, 0x21, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
            return ('cond' if op7 >= 0x28 else 'jump', self.pc + 8 + s11(self.lo) * 8)
        if op7 in (0x24, 0x25):
            return ('reg', None)
        return None


def vf(n): return f'vf[{n}]'


def upper(p):
    """C++ for the upper half: returns (code, reads, writes, fmac, clip)."""
    w = p.up
    op, d, ft, fs, fd = w & 0x3F, dest(w), (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    reads, writes = {fs}, set()
    names3 = {0: 'add', 1: 'sub', 6: 'mul'}

    def tgt(n): return 'junk' if n == 0 else vf(n)
    if op < 0x1C and (op >> 2) in (0, 1, 6):
        reads.add(ft)
        writes.add(fd)
        return f'{names3[op >> 2]}(ctx, {tgt(fd)}, {vf(fs)}, bc({vf(ft)}, {op & 3}), {d});', reads, writes, True, False
    if op < 0x1C and (op >> 2) in (2, 3):
        reads.add(ft)
        writes.add(fd)
        f = 'madd' if (op >> 2) == 2 else 'msub'
        return f'{f}(ctx, {tgt(fd)}, acc, {vf(fs)}, bc({vf(ft)}, {op & 3}), {d});', reads, writes, True, False
    if op < 0x1C and (op >> 2) in (4, 5):
        reads.add(ft)
        writes.add(fd)
        f = 'max' if (op >> 2) == 4 else 'mini'
        return f'{f}({tgt(fd)}, {vf(fs)}, bc({vf(ft)}, {op & 3}), {d});', reads, writes, False, False
    simple = {0x28: 'add', 0x2C: 'sub', 0x2A: 'mul'}
    if op in simple:
        reads.add(ft)
        writes.add(fd)
        return f'{simple[op]}(ctx, {tgt(fd)}, {vf(fs)}, {vf(ft)}, {d});', reads, writes, True, False
    if op in (0x29, 0x2D):
        reads.add(ft)
        writes.add(fd)
        f = 'madd' if op == 0x29 else 'msub'
        return f'{f}(ctx, {tgt(fd)}, acc, {vf(fs)}, {vf(ft)}, {d});', reads, writes, True, False
    if op in (0x2B, 0x2F):
        reads.add(ft)
        writes.add(fd)
        return f'{"max" if op == 0x2B else "mini"}({tgt(fd)}, {vf(fs)}, {vf(ft)}, {d});', reads, writes, False, False
    if op == 0x2E:
        reads.add(ft)
        writes.add(fd)
        return f'opmsub(ctx, {tgt(fd)}, acc, {vf(fs)}, {vf(ft)});', reads, writes, True, False
    if op in (0x1D, 0x1F):
        writes.add(fd)
        return f'{"max" if op == 0x1D else "mini"}({tgt(fd)}, {vf(fs)}, broadcast(ibits), {d});', reads, writes, False, False
    qi = {0x1C: ('mul', 'q'), 0x20: ('add', 'q'), 0x24: ('sub', 'q'), 0x1E: ('mul', 'i'), 0x22: ('add', 'i'), 0x26: ('sub', 'i'),
          0x21: ('madd', 'q'), 0x23: ('madd', 'i'), 0x25: ('msub', 'q'), 0x27: ('msub', 'i')}
    if op in qi:
        f, r = qi[op]
        writes.add(fd)
        src = 'broadcast(q)' if r == 'q' else 'broadcast(ibits)'
        if f in ('madd', 'msub'):
            return f'{f}(ctx, {tgt(fd)}, acc, {vf(fs)}, {src}, {d});', reads, writes, True, False
        return f'{f}(ctx, {tgt(fd)}, {vf(fs)}, {src}, {d});', reads, writes, True, False
    if op >= 0x3C:
        sp = ((w >> 4) & 0x7C) | (w & 3)
        if sp < 0x10 or 0x18 <= sp <= 0x1B:
            reads.add(ft)
            kind = sp >> 2
            src = f'bc({vf(ft)}, {sp & 3})'
            if kind in (0, 1, 6):
                f = {0: 'add', 1: 'sub', 6: 'mul'}[kind]
                return f'{f}(ctx, acc, {vf(fs)}, {src}, {d});', reads, writes, True, False
            f = 'madd' if kind == 2 else 'msub'
            return f'{f}(ctx, acc, acc, {vf(fs)}, {src}, {d});', reads, writes, True, False
        if sp == 0x2F:
            return '', set(), set(), False, False
        if 0x10 <= sp <= 0x17:
            writes.add(ft)
            f = 'itof' if sp < 0x14 else 'ftoi'
            bits = (0, 4, 12, 15)[sp & 3]
            return f'{f}({tgt(ft)}, {vf(fs)}, {d}, {bits});', reads, writes, False, False
        if sp == 0x1D:
            writes.add(ft)
            return f'abs({tgt(ft)}, {vf(fs)}, {d});', reads, writes, False, False
        if sp == 0x1F:
            reads.add(ft)
            return f'pendClip(clipFlags({vf(fs)}, {vf(ft)}));', reads, writes, False, True
        accq = {0x1C: ('mul', 'q'), 0x1E: ('mul', 'i'), 0x20: ('add', 'q'), 0x22: ('add', 'i'), 0x24: ('sub', 'q'),
                0x26: ('sub', 'i'), 0x21: ('madd', 'q'), 0x23: ('madd', 'i'), 0x25: ('msub', 'q'), 0x27: ('msub', 'i')}
        if sp in accq:
            f, r = accq[sp]
            src = 'broadcast(q)' if r == 'q' else 'broadcast(ibits)'
            if f in ('madd', 'msub'):
                return f'{f}(ctx, acc, acc, {vf(fs)}, {src}, {d});', reads, writes, True, False
            return f'{f}(ctx, acc, {vf(fs)}, {src}, {d});', reads, writes, True, False
        accv = {0x28: 'add', 0x2A: 'mul', 0x2C: 'sub'}
        if sp in accv:
            reads.add(ft)
            return f'{accv[sp]}(ctx, acc, {vf(fs)}, {vf(ft)}, {d});', reads, writes, True, False
        if sp in (0x29, 0x2D):
            reads.add(ft)
            return f'{"madd" if sp == 0x29 else "msub"}(ctx, acc, acc, {vf(fs)}, {vf(ft)}, {d});', reads, writes, True, False
        if sp == 0x2E:
            reads.add(ft)
            return f'opmula(ctx, acc, {vf(fs)}, {vf(ft)});', reads, writes, True, False
    raise ValueError(f'upper {w:#010x} at {p.pc:#x} is not lifted')


def lower(p):
    """C++ for the lower half: returns (code, reads, writes, q, branch)."""
    w = p.lo
    if p.loi:
        return f'ibits = {w:#010x}u;', set(), set(), None, None
    if w == 0x8000033C:
        return '', set(), set(), None, None
    op7 = (w >> 25) & 0x7F
    d, it, is_, id_ = dest(w), (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    imm = s11(w)

    def vi(n): return f'vi[{n}]'

    def tgt(n): return 'junk' if n == 0 else vf(n)

    def viw(n, expr): return '' if n == 0 else f'vi[{n}] = static_cast<uint16_t>({expr});'
    fsf, ftf = (w >> 21) & 3, (w >> 23) & 3
    if op7 == 0x40:
        op = w & 0x3F
        alu = {0x30: '+', 0x31: '-', 0x34: '&', 0x35: '|'}
        if op in alu:
            return viw(id_, f'vi[{is_}] {alu[op]} vi[{it}]'), set(), set(), None, None
        if op == 0x32:
            i5 = id_ - 32 if id_ & 16 else id_
            return viw(it, f'vi[{is_}] + {i5}'), set(), set(), None, None
        sp = ((w >> 4) & 0x7C) | (w & 3)
        if sp == 0x30:
            return f'move({tgt(it)}, {vf(is_)}, {d});', {is_}, {it}, None, None
        if sp == 0x31:
            return f'mr32({tgt(it)}, {vf(is_)}, {d});', {is_}, {it}, None, None
        if sp == 0x34:
            return f'lqm({tgt(it)}, vi[{is_}], {d}); ' + viw(is_, f'vi[{is_}] + 1'), set(), {it}, None, None
        if sp == 0x35:
            return f'sqm({vf(is_)}, vi[{it}], {d}); ' + viw(it, f'vi[{it}] + 1'), {is_}, set(), None, None
        if sp == 0x36:
            return viw(is_, f'vi[{is_}] - 1') + f' lqm({tgt(it)}, vi[{is_}], {d});', set(), {it}, None, None
        if sp == 0x37:
            return viw(it, f'vi[{it}] - 1') + f' sqm({vf(is_)}, vi[{it}], {d});', {is_}, set(), None, None
        if sp == 0x38:
            return f'pendQ(7, div(ctx, {vf(is_)}.u[{fsf}], {vf(it)}.u[{ftf}]));', {is_, it}, set(), 'div', None
        if sp == 0x39:
            return f'pendQ(7, sqrt(ctx, {vf(it)}.u[{ftf}]));', {it}, set(), 'div', None
        if sp == 0x3A:
            return f'pendQ(13, rsqrt(ctx, {vf(is_)}.u[{fsf}], {vf(it)}.u[{ftf}]));', {is_, it}, set(), 'div', None
        if sp == 0x3B:
            return 'waitQ();', set(), set(), 'wait', None
        if sp == 0x3C:
            return viw(it, f'{vf(is_)}.u[{fsf}]'), {is_}, set(), None, None
        if sp == 0x3D:
            return f'mfir({tgt(it)}, vi[{is_}], {d});', set(), {it}, None, None
        if sp == 0x3E:
            return viw(it, f'ilwm(vi[{is_}], {d})'), set(), set(), None, None
        if sp == 0x3F:
            return f'iswm(vi[{it}], vi[{is_}], {d});', set(), set(), None, None
        if sp == 0x69:
            return viw(it, 'itop'), set(), set(), None, None
        if sp == 0x68:
            return viw(it, 'top'), set(), set(), None, None
        if sp == 0x6C:
            return f'kick(user, vi[{is_}]);', set(), set(), None, None
        raise ValueError(f'lower {w:#010x} at {p.pc:#x} is not lifted')
    if op7 == 0x00:
        return f'lqm({tgt(it)}, vi[{is_}] + ({imm}), {d});', set(), {it}, None, None
    if op7 == 0x01:
        return f'sqm({vf(is_)}, vi[{it}] + ({imm}), {d});', {is_}, set(), None, None
    if op7 == 0x04:
        return viw(it, f'ilwm(vi[{is_}] + ({imm}), {d})'), set(), set(), None, None
    if op7 == 0x05:
        return f'iswm(vi[{it}], vi[{is_}] + ({imm}), {d});', set(), set(), None, None
    if op7 in (0x08, 0x09):
        imm15 = ((w >> 10) & 0x7800) | (w & 0x7FF)
        return viw(it, f'vi[{is_}] {"+" if op7 == 8 else "-"} {imm15}'), set(), set(), None, None
    if op7 == 0x11:
        return f'clipNow(); clip = {w & 0xFFFFFF:#x}u;', set(), set(), None, None
    if op7 in (0x10, 0x12, 0x13):
        m = w & 0xFFFFFF
        expr = {0x10: f'(clip & 0xFFFFFFu) == {m:#x}u ? 1 : 0', 0x12: f'(clip & {m:#x}u) ? 1 : 0',
                0x13: f'((clip | {m:#x}u) & 0xFFFFFFu) == 0xFFFFFFu ? 1 : 0'}[op7]
        return 'clipNow(); vi[1] = ' + expr + ';', set(), set(), None, None
    if op7 == 0x1C:
        return 'clipNow(); ' + viw(it, 'clip & 0xFFF'), set(), set(), None, None
    if op7 == 0x15:
        return 'flagsNow(); ctx->vu0_status = (ctx->vu0_status & 0x3Fu) | (' + f'{((w >> 10) & 0x800) | (w & 0x7FF):#x}u & 0xFC0u);', set(), set(), None, None
    if op7 in (0x14, 0x16, 0x17):
        m = ((w >> 10) & 0x800) | (w & 0x7FF)
        expr = {0x14: f'(ctx->vu0_status & 0xFFFu) == {m:#x}u ? 1 : 0', 0x16: f'ctx->vu0_status & {m:#x}u',
                0x17: f'(ctx->vu0_status | {m:#x}u) & 0xFFFu'}[op7]
        return 'flagsNow(); ' + viw(it, expr), set(), set(), None, None
    if op7 in (0x18, 0x1A, 0x1B):
        expr = {0x18: f'(ctx->vu0_mac & 0xFFFFu) == vi[{is_}] ? 1 : 0', 0x1A: f'ctx->vu0_mac & vi[{is_}]',
                0x1B: f'(ctx->vu0_mac | vi[{is_}]) & 0xFFFFu'}[op7]
        return 'flagsNow(); ' + viw(it, expr), set(), set(), None, None
    target = p.pc + 8 + imm * 8
    conds = {0x28: f'vi[{it}] == vi[{is_}]', 0x29: f'vi[{it}] != vi[{is_}]', 0x2C: f'static_cast<int16_t>(vi[{is_}]) < 0',
             0x2D: f'static_cast<int16_t>(vi[{is_}]) > 0', 0x2E: f'static_cast<int16_t>(vi[{is_}]) <= 0',
             0x2F: f'static_cast<int16_t>(vi[{is_}]) >= 0'}
    if op7 in conds:
        return '', set(), set(), None, ('cond', conds[op7], target)
    if op7 == 0x20:
        return '', set(), set(), None, ('jump', 'true', target)
    if op7 == 0x21:
        return viw(it, f'{(p.pc + 16) // 8}'), set(), set(), None, ('jump', 'true', target)
    if op7 == 0x24:
        return '', set(), set(), None, ('reg', f'vi[{is_}] * 8u', None)
    if op7 == 0x25:
        return f'const uint32_t jr = vi[{is_}] * 8u; ' + viw(it, f'{(p.pc + 16) // 8}'), set(), set(), None, ('reg', 'jr', None)
    raise ValueError(f'lower {w:#010x} at {p.pc:#x} is not lifted')


def pair_code(p):
    ucode, ureads, uwrites, fm, cl = upper(p)
    lcode, lreads, lwrites, q, br = lower(p)
    reads = sorted(r for r in (ureads | lreads) if r)
    out = [f'    L_{p.pc:04x}:', '    issue();' + ''.join(f' ready({r});' for r in reads)]
    if q == 'wait':
        out.append('    waitQ();')
    lsnap, usnap = lreads & uwrites, ureads & lwrites
    out.append('    {')
    if lsnap or usnap:
        for n in sorted(lsnap | usnap):
            out.append(f'      const Vec before{n} = {vf(n)};')
        for n in sorted(lsnap, reverse=True):
            lcode = lcode.replace(vf(n), f'before{n}')
        for n in sorted(usnap, reverse=True):
            ucode = ucode.replace(vf(n), f'before{n}')
    if br:
        kind, cond, target = br
        to = cond if kind == 'reg' else f'{target:#x}u'
        taken = 'true' if kind == 'reg' else cond
        out.append(f'      newTaken = {taken}; newTo = {to};')
    if ucode:
        out.append(f'      fmacBegin(); {ucode} fmacEnd();' if fm else f'      {ucode}')
    if lcode and q != 'wait':
        out.append(f'      {lcode}')
    for n in (uwrites | lwrites):
        if n:
            out.append(f'      written({n});')
    out.append('    }')
    out.append('    if (armed) { armed = false; if (armedTaken) { if (armedEnd) { tpc = armedTo; finish(); return; } '
               'pc = armedTo; goto dispatch; } if (armedEnd) { tpc = ' + f'{p.pc + 8:#x}u' + '; finish(); return; } }')
    if br or p.e:
        out.append(f'    armed = true; armedTaken = {"newTaken" if br else "false"}; armedTo = newTo; armedEnd = {"true" if p.e else "false"};')
    return out


def lift(code):
    pairs = {}
    for pc in range(0, len(code), 8):
        lo, up = struct.unpack_from('<II', code, pc)
        pairs[pc] = Pair(pc, lo, up)
    last = max(pc for pc, p in pairs.items() if p.lo or p.up) + 8
    lines = ['    switch (pc)', '    {']
    for pc in range(0, last + 8, 8):
        lines.append(f'    case {pc:#x}u: goto L_{pc:04x};')
    lines += ['    default: return;', '    }']
    for pc in range(0, last + 8, 8):
        lines += pair_code(pairs[pc])
    lines.append('    tpc = 0; finish(); return;')
    return lines


HEADER = '''// Generated by tools/sct2_vu0_lift.py from SLUS_209.33's VU0 microcode. Do not edit.
#include "sct2_vu0.h"

#include "bozo/ops_vu0.h"

#include <cstdint>
#include <cstring>

namespace sct2::@NS@
{
    using namespace bozo::vu0;

    namespace
    {
        uint32_t clipFlags(const Vec &s, const Vec &t)
        {
            const uint32_t w = t.u[3] & 0x7FFFFFFFu;
            uint32_t flags = 0;
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t a = s.u[k] & 0x7FFFFFFFu;
                const bool negative = s.u[k] >> 31;
                if (a > w)
                    flags |= negative ? 2u << (2 * k) : 1u << (2 * k);
            }
            return flags;
        }

        inline void mfir(Vec &d, uint32_t v, uint32_t dest)
        {
            const uint32_t x = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v)));
            for (int i = 0; i < 4; ++i)
                if (fieldOn(dest, i))
                    d.u[i] = x;
        }

    }

    // The microcode from `pc` until it stops, on `ctx`'s VU registers and the unit's memory (`mask` + 1 quadwords).
    void runUnit(uint32_t pc, R5900Context *ctx, uint8_t *mem, uint32_t mask, uint32_t itop, uint32_t top, uint32_t &tpc,
                 void (*kick)(void *, uint32_t), void *user)
    {
        auto quadAt = [&](uint32_t index) { return mem + ((index & mask) << 4); };
        auto lqm = [&](Vec &d, uint32_t index, uint32_t dest) {
            const uint8_t *q = quadAt(index);
            for (int i = 0; i < 4; ++i)
                if (fieldOn(dest, i))
                    std::memcpy(&d.u[i], q + 4 * i, 4);
        };
        auto sqm = [&](const Vec &s, uint32_t index, uint32_t dest) {
            uint8_t *q = quadAt(index);
            for (int i = 0; i < 4; ++i)
                if (fieldOn(dest, i))
                    std::memcpy(q + 4 * i, &s.u[i], 4);
        };
        auto ilwm = [&](uint32_t index, uint32_t dest) -> uint16_t {
            const uint8_t *q = quadAt(index);
            for (int i = 0; i < 4; ++i)
                if (fieldOn(dest, i))
                    return static_cast<uint16_t>(q[i * 4] | q[i * 4 + 1] << 8);
            return 0;
        };
        auto iswm = [&](uint32_t v, uint32_t index, uint32_t dest) {
            uint8_t *q = quadAt(index);
            for (int i = 0; i < 4; ++i)
                if (fieldOn(dest, i))
                {
                    q[i * 4] = static_cast<uint8_t>(v);
                    q[i * 4 + 1] = static_cast<uint8_t>(v >> 8);
                    q[i * 4 + 2] = 0;
                    q[i * 4 + 3] = 0;
                }
        };
        (void)kick;
        (void)user;
        (void)top;
        Vec *vf = ctx->vu0_vf;
        Vec &acc = ctx->vu0_acc;
        Vec junk;
        uint32_t &ibits = ctx->vu0_i_bits;
        uint32_t &qbits = ctx->vu0_q_bits;
        uint32_t &clip = ctx->vu0_clip_flags;
        uint16_t vi[16];
        for (int r = 0; r < 16; ++r)
            vi[r] = static_cast<uint16_t>(ctx->vi[r]);
        vi[0] = 0;
        uint64_t cycle = 0, readyAt[32] = {};
        struct Pending { uint64_t at; uint32_t mac, status; };
        Pending flags[8];
        unsigned flagCount = 0;
        uint64_t clipAt = 0, qAt = 0;
        uint32_t clipNext = 0, qNext = qbits;
        bool clipPending = false, qPending = false;
        uint32_t macSaved = 0, statusSaved = 0;
        auto issue = [&] { ++cycle; };
        auto ready = [&](int r) { if (readyAt[r] > cycle) cycle = readyAt[r]; };
        auto written = [&](int r) { readyAt[r] = cycle + 4; };
        auto flagsNow = [&] {
            unsigned keep = 0;
            for (unsigned k = 0; k < flagCount; ++k)
                if (flags[k].at <= cycle)
                {
                    ctx->vu0_mac = flags[k].mac;
                    ctx->vu0_status = (ctx->vu0_status & ~0xFu) | flags[k].status | ((flags[k].status & 0xFu) << 6);
                }
                else
                    flags[keep++] = flags[k];
            flagCount = keep;
        };
        auto fmacBegin = [&] { flagsNow(); macSaved = ctx->vu0_mac; statusSaved = ctx->vu0_status; };
        auto fmacEnd = [&] {
            if (flagCount < 8)
                flags[flagCount++] = {cycle + 4, ctx->vu0_mac, ctx->vu0_status & 0xFu};
            ctx->vu0_mac = macSaved;
            ctx->vu0_status = statusSaved;
        };
        auto clipNow = [&] {
            if (clipPending && clipAt <= cycle)
                clip = ((clip << 6) | clipNext) & 0xFFFFFFu, clipPending = false;
        };
        auto pendClip = [&](uint32_t f) { clipNow(); clipNext = f; clipAt = cycle + 4; clipPending = true; };
        auto qNow = [&] { if (qPending && qAt <= cycle) qbits = qNext, qPending = false; };
        auto pendQ = [&](unsigned latency, uint32_t v) { qNow(); qNext = v; qAt = cycle + latency; qPending = true; };
        auto waitQ = [&] { if (qPending && qAt > cycle) cycle = qAt; qNow(); };
        auto finish = [&] {
            cycle += 16;
            flagsNow();
            clipNow();
            qNow();
            for (int r = 1; r < 16; ++r)
                ctx->vi[r] = vi[r];
        };
        bool armed = false, armedTaken = false, armedEnd = false, newTaken = false;
        uint32_t armedTo = 0, newTo = 0;
    dispatch:
'''

FOOTER = '''    }

@WRAPPER@
}
'''

WRAPPERS = {
    0: '''    void run(uint32_t pc, R5900Context *ctx, Runtime *rt, uint32_t itop, uint32_t &tpc)
    {
        runUnit(pc, ctx, bozo_vu0_memory(rt), 0xFFu, itop, 0, tpc, nullptr, nullptr);
    }''',
    1: '''    void run(uint32_t pc, R5900Context *ctx, uint8_t *mem, uint32_t itop, uint32_t top, uint32_t &tpc,
             void (*kick)(void *, uint32_t), void *user)
    {
        runUnit(pc, ctx, mem, 0x3FFu, itop, top, tpc, kick, user);
    }''',
}


def main():
    elf = Path(sys.argv[1]).read_bytes()
    unit = 1 if '--unit' in sys.argv and sys.argv[sys.argv.index('--unit') + 1] == '1' else 0
    lines = lift(microcode(elf, unit))
    body = '\n'.join(lines)
    body = body.replace('broadcast(q)', '(qNow(), broadcast(qbits))')
    text = (HEADER + body + '\n' + FOOTER).replace('@NS@', 'vu1' if unit else 'vu0').replace('#include "sct2_vu0.h"', '#include "sct2_vu1.h"' if unit else '#include "sct2_vu0.h"').replace('@WRAPPER@', WRAPPERS[unit])
    out = Path(sys.argv[2])
    if not out.exists() or out.read_text() != text:
        out.write_text(text)


if __name__ == '__main__':
    main()
