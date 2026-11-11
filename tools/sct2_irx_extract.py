"""The IOP modules SLUS_209.33 loads, unpacked from its .staytrash as its boot code does (0x46F0C8: the packed block at
0x480650, decoded by 0x178B7C / 0x178B98), each written as <out dir>/<module name>.IRX.

    python tools/sct2_irx_extract.py <SLUS_209.33> <out dir>
"""
import struct
import sys
from pathlib import Path

SEGMENT_VA, SEGMENT_OFFSET = 0x0046EA80, 0x0010D510
HEADER, PACKED = 0x00480630, 0x00480650
WORDS = 0x4EDB


class Bits:
    def __init__(self, data, offset, words):
        self.data, self.offset, self.words = data, offset, words
        self.buffer, self.avail = 0, 0

    def get(self, n):
        if self.avail >= n:
            value = self.buffer & ((1 << n) - 1)
            self.buffer >>= n
            self.avail -= n
            return value
        if not self.words:
            raise ValueError('packed block ended early')
        need = n - self.avail
        word, = struct.unpack_from('<Q', self.data, self.offset)
        self.offset += 8
        self.words -= 1
        value = (self.buffer << need) | (word & ((1 << need) - 1))
        self.buffer, self.avail = word >> need, 64 - need
        return value


def unpack(elf):
    at = lambda va: SEGMENT_OFFSET + va - SEGMENT_VA
    size_word, = struct.unpack_from('<I', elf, at(HEADER))
    widths, = struct.unpack_from('<H', elf, at(HEADER + 0x18))
    out, remaining = bytearray(), size_word >> 4
    bits = Bits(elf, at(PACKED), WORDS)
    while remaining > 0:
        if bits.get(1):
            out.append(bits.get(8))
            remaining -= 1
            continue
        distance = bits.get(widths & 0xFF) + 1
        length = bits.get(widths >> 8) + 2
        if distance > len(out) or length > remaining:
            raise ValueError('bad match in the packed block')
        for _ in range(length):
            out.append(out[-distance])
        remaining -= length
    return bytes(out)


def module_name(irx):
    shoff, = struct.unpack_from('<I', irx, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', irx, 0x2E)
    for k in range(shnum):
        sh = struct.unpack_from('<10I', irx, shoff + k * shentsize)
        if sh[1] == 0x70000080:
            return irx[sh[4] + 0x1A:sh[4] + sh[5]].split(b'\0')[0].decode('ascii')
    raise ValueError('an IRX without .iopmod')


def main():
    elf, out = Path(sys.argv[1]).read_bytes(), Path(sys.argv[2])
    block = unpack(elf)
    count = struct.unpack_from('<I', block, 0)[0] >> 1
    out.mkdir(parents=True, exist_ok=True)
    for k in range(count):
        offset, size = struct.unpack_from('<II', block, 4 + 8 * k)
        irx = block[offset:offset + size]
        if irx[:4] != b'\x7fELF' or struct.unpack_from('<H', irx, 16)[0] != 0xFF80:
            sys.exit(f'sct2_irx_extract: entry {k} is not an IRX')
        name = module_name(irx)
        path = out / f'{name}.IRX'
        if not path.exists() or path.read_bytes() != irx:
            path.write_bytes(irx)
        print(f'{name} {size:#x}')


if __name__ == '__main__':
    main()
