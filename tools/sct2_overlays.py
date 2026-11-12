"""SLUS_209.33's code overlays as images bozo-recomp compiles one by one, all at 0x46EA80: the ELF's own boot code and
the three overlays the ELF's table at 0x1A7E10 lists, unpacked from the disc as the game's decompressor thread
(0x17F9B8) does. Writes into <out dir>:

- main.elf: SLUS_209.33 with .staytrash no longer executable, so the code of .text reaches the region through the
  function table, where the resident module is chosen;
- <module>.elf and <module>.entries for each module (entry points: words, jal targets and lui/addiu pairs pointing at a
  function prologue in the module);
- sct2_overlays.cpp: sct2::overlays::attach, registering every module by the code it holds.

    python tools/sct2_overlays.py <SLUS_209.33> <disc image> <out dir>
    python tools/sct2_overlays.py --externals <externals> <overrides list> <out> [module]

The second form writes one compiled image's externals: every stub, and the overrides list's lines for that image (those
tagged "@module", or the untagged ones for the main program).
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sct2_irx_extract as irx  # noqa: E402

BASE = 0x0046EA80
BOOT_BYTES = 0x9A4
TABLE, ENTRIES = 0x001A7E10, 3
SECTOR = 2048
IDENTITY = (0x20, 32)


def sections(elf):
    shoff, = struct.unpack_from('<I', elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', elf, 0x2E)
    heads = [struct.unpack_from('<10I', elf, shoff + k * shentsize) for k in range(shnum)]
    names = heads[shstrndx][4]
    return shoff, shentsize, [(elf[names + h[0]:].split(b'\0')[0].decode(), h) for h in heads]


def at_va(elf, va):
    for _, h in sections(elf)[2]:
        if h[3] <= va < h[3] + h[5] and h[1] != 8:
            return h[4] + va - h[3]
    raise ValueError(f'{va:#x} is not in the ELF')


def unpack(data, header, widths):
    out, remaining = bytearray(), header >> 4
    bits = irx.Bits(data, 0, len(data) // 8)
    while remaining > 0:
        if bits.get(1):
            out.append(bits.get(8))
            remaining -= 1
            continue
        distance = bits.get(widths & 0xFF) + 1
        length = bits.get(widths >> 8) + 2
        if distance > len(out) or length > remaining:
            raise ValueError('bad match in a packed overlay')
        for _ in range(length):
            out.append(out[-distance])
        remaining -= length
    return bytes(out)


def overlays(elf, disc):
    found = {}
    for k in range(ENTRIES):
        widths, header, lsn, packed = struct.unpack_from('<4I', elf, at_va(elf, TABLE + 16 * k))
        disc.seek(lsn * SECTOR)
        head = disc.read(16)
        if struct.unpack_from('<I', head, 8)[0] != widths:
            sys.exit(f'sct2_overlays: the overlay at LSN {lsn:#x} does not start as the table says')
        found[f'ovl_{lsn:x}'] = unpack(disc.read((packed + 7) // 8 * 8), header, widths)
    return found


def image_elf(image):
    shstr = b'\0.text\0.shstrtab\0'
    text_at = 0x1000
    shstr_at = text_at + len(image)
    shoff = (shstr_at + len(shstr) + 15) & ~15
    header = struct.pack('<4s5B7x2H5I6H', b'\x7fELF', 1, 1, 1, 0, 0, 2, 8, 1, BASE, 0x34, shoff, 0, 0x34, 0x20, 1, 40, 3, 2)
    phdr = struct.pack('<8I', 1, text_at, BASE, BASE, len(image), len(image), 7, 0x10)
    shdrs = struct.pack('<10I', *([0] * 10))
    shdrs += struct.pack('<10I', 1, 1, 6, BASE, text_at, len(image), 0, 0, 0x10, 0)
    shdrs += struct.pack('<10I', 7, 3, 0, 0, shstr_at, len(shstr), 0, 0, 1, 0)
    out = bytearray(header + phdr)
    out += b'\0' * (text_at - len(out)) + image + shstr
    out += b'\0' * (shoff - len(out)) + shdrs
    return bytes(out)


def main_elf(elf):
    shoff, shentsize, heads = sections(elf)
    out = bytearray(elf)
    for k, (name, h) in enumerate(heads):
        if name == '.staytrash':
            struct.pack_into('<I', out, shoff + k * shentsize + 8, h[2] & ~4)
    return bytes(out)


def prologue(image, target):
    off = target - BASE
    if not 0 <= off <= len(image) - 4 or off % 4:
        return False
    word, = struct.unpack_from('<I', image, off)
    return word >> 16 == 0x27BD and word & 0x8000


def after_return(image, target):
    off = target - BASE
    return 8 <= off <= len(image) - 4 and off % 4 == 0 and struct.unpack_from('<I', image, off - 8)[0] == 0x03E00008


def entries(text, text_va, image):
    found = set()
    for blob, va in ((text, text_va), (image, BASE)):
        for off in range(0, len(blob) - 3, 4):
            word, = struct.unpack_from('<I', blob, off)
            if prologue(image, word):
                found.add(word)
            if word >> 26 == 3:
                target = ((va + off) & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
                if prologue(image, target) or (blob is text and after_return(image, target)):
                    found.add(target)
            if word >> 26 == 0x0F:
                reg, high = (word >> 16) & 31, (word & 0xFFFF) << 16
                for k in range(1, 9):
                    if off + 4 * k + 4 > len(blob):
                        break
                    nxt, = struct.unpack_from('<I', blob, off + 4 * k)
                    if nxt >> 26 == 0x09 and (nxt >> 21) & 31 == reg:
                        target = (high + (nxt & 0xFFFF) - (0x10000 if nxt & 0x8000 else 0)) & 0xFFFFFFFF
                        if prologue(image, target):
                            found.add(target)
                        break
    found.discard(BASE)
    return sorted(found)


def attach_source(modules):
    lines = ['// Generated by tools/sct2_overlays.py from the game\'s disc. Do not edit.',
             '#include "bozo/runtime.h"', '', '#include <cstddef>', '#include <cstdint>', '']
    for name in modules:
        lines += [f'extern const BozoFunctionEntry sct2_{name}_function_table[];',
                  f'extern const size_t sct2_{name}_function_count;',
                  f'extern const BozoResumeEntry sct2_{name}_resume_table[];',
                  f'extern const size_t sct2_{name}_resume_count;']
    lines += ['', 'namespace sct2::overlays', '{', '    void attach(Runtime &rt)', '    {']
    offset, size = IDENTITY
    for name, image in modules.items():
        bytes_ = ', '.join(f'0x{b:02x}' for b in image[offset:offset + size])
        lines += [f'        {{ static const uint8_t identity[] = {{{bytes_}}};',
                  f'          rt.functions.addModule(0x{BASE:08x}u, 0x{BASE + len(image):08x}u, '
                  f'rt.memory.ram() + 0x{BASE + offset:08x}u, {{identity, identity + sizeof identity}},',
                  f'              sct2_{name}_function_table, sct2_{name}_function_count, sct2_{name}_resume_table, '
                  f'sct2_{name}_resume_count); }}']
    lines += ['    }', '}', '']
    return '\n'.join(lines)


def write(path, data):
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def externals(source, overrides, target, module):
    kept = [line for line in Path(source).read_text().splitlines() if line.startswith('stub ')]
    for line in Path(overrides).read_text().splitlines():
        if line.startswith('#') or not line.strip():
            continue
        tag = line.rsplit('@', 1)[1].strip() if '@' in line else None
        if tag == module:
            kept.append(line.split('#')[0].rstrip())
    write(Path(target), ('\n'.join(kept) + '\n').encode())


def main():
    if sys.argv[1] == '--externals':
        externals(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5] if len(sys.argv) > 5 else None)
        return
    elf = Path(sys.argv[1]).read_bytes()
    out = Path(sys.argv[3])
    out.mkdir(parents=True, exist_ok=True)
    with open(sys.argv[2], 'rb') as disc:
        modules = {'boot': elf[at_va(elf, BASE):at_va(elf, BASE) + BOOT_BYTES], **overlays(elf, disc)}
    offset, size = IDENTITY
    identities = {image[offset:offset + size] for image in modules.values()}
    if len(identities) != len(modules) or any(len(i) != size for i in identities):
        sys.exit('sct2_overlays: the modules are not told apart by their identity bytes')
    text_head = next(h for name, h in sections(elf)[2] if name == '.text')
    text = elf[text_head[4]:text_head[4] + text_head[5]]
    write(out / 'main.elf', main_elf(elf))
    for name, image in modules.items():
        write(out / f'{name}.elf', image_elf(image))
        write(out / f'{name}.entries', ''.join(f'0x{e:08x}\n' for e in entries(text, text_head[3], image)).encode())
        print(f'{name} {len(image):#x}')
    write(out / 'sct2_overlays.cpp', attach_source(modules).encode())


if __name__ == '__main__':
    main()
