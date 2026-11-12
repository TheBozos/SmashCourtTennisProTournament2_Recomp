# Smash Court Tennis Pro Tournament 2 on BozoEngine

How the port works: what the game does on the PS2 and what `bozo/port/` does in its place. The PS2Recomp-era notes
(how the port was first brought up, on an emulated GS and IOP) are in the git history before the BozoEngine migration.

## The disc

The ISO 9660 file system holds only `SYSTEM.CNF`, `SLUS_209.33` and `IOPRP300.IMG`; the image is 4.4 GB. Everything
else, data and movies included, sits in raw sectors the game reads by LSN. BozoEngine serves those reads from
the disc image (`--iso`).

## Code

`SLUS_209.33` is stripped, entry `0x100000`: `.text` at `0x100000` and `.staytrash` at `0x46EA80`, writable and
executable. `recomp/sct2_symbols.txt` names the Sony library functions by address; `bozo-recomp` binds them to
BozoEngine's native libraries and compiles the rest.

The game loads three code overlays into `.staytrash`. The ELF's table at `0x1A7E10` lists them: LZ-packed images at
LSN `0x213932`, `0x21381A` and `0x2138A8`, each behind a 16-byte sector header whose word at `+8` gives the packing's
widths. `tools/sct2_overlays.py` unpacks them at build time into one ELF each, plus the main ELF with `.staytrash`
marked as data, and writes `sct2_overlays.cpp`, which registers each as a `FunctionTable` module. At run time the
module in place is the one whose 32 bytes at `0x46EAA0` match. Each module is compiled with its own symbol prefix; a
native instruction handler belongs to one module (`// bozo:native kName module` in `bozo/port/`, read by
`tools/sct2_overrides_list.py`).

## The frame

The main loop ends each frame in `0x178EC8` (FlushCache, CancelWakeup, SleepThread until the vblank handler wakes it).
The native version (`sct2_graphics.cpp`) sleeps the main thread to the next field through `Kernel::scheduleNative`,
then plays the frame's drawing (`draw2d::publish`) and ends the frame. The memory card thread busy-polls, so lowering
the main thread's priority to wait starved it; the timed sleep does not. `0x178F88` (the GS busy wait) answers idle.
The hardware register writes, DMA kicks and polls of the game's own code are retired at their sites.

## Drawing

`sct2_2d.cpp` plays the frame's DMA chain at the frame's end, as the DMA controller and VIF1 would:

- The draw environment at `0x240FF0` (all but its copy of the last frame to the display), then the ordering table
  (`rd(gp - 0x74F4)`, 16 bytes per bucket). The draw environment's `XYOFFSET_1` is the screen's origin.
- The 2D library (`0x179AB0`-`0x17A3A8`, context at `0x463370`: bucket at `+0x2E`, mode at `+0x2F`, cached `ALPHA`,
  `TEX0`, `TEX1`, `RGBAQ` and `TEST` at `+0x08`-`+0x28`, `PRIM` at `+0x2C`) builds no packet: its register writes,
  DMA calls and copies are kept per packet, and a marker tag in its packet buffer is linked into the ordering table
  where the packet would have been. Packets the game links itself go through `0x177340`, native.
- VIF1 codes: UNPACK (every format, masks, modes, write cycles) into VU1's memory with `TOPS` double-buffered,
  MSCAL / MSCNT running the lifted VU1 microcode, DIRECT data to the GIF. DMA addresses with bit 31 read the
  scratchpad.
- The GIF: PACKED (every register format, `ADC` vertices not drawn), REGLIST and IMAGE. IMAGE uploads go to
  `sct2_textures.cpp`, which keeps GS memory in its own layout (CT32, T8 and T4 swizzles in `sct2_gs_layout.h`): the
  game uploads 4-bit textures as 32-bit words and draws them back as 4-bit. Textures are decoded from it when drawn.
- Primitives become `Renderer::draw` calls with the material the GS registers describe (blend, alpha and depth
  tests, depth writes, colour mask, clamping, perspective).

## VU microcode

`tools/sct2_vu0_lift.py` lifts both units' microcode to C++ at build time: VU0's from the VIF0 chain at `0x1A3F10`
(225 qwords), VU1's from the VIF1 chain at `0x1A4D40` (765 qwords). The lifted code follows the pipeline the
microcode relies on: a register written is read by the next instructions as it was until 4 cycles later, with the
stalls a read causes; MAC, status and clip flags land 4 cycles after their instruction, in order; Q lands 7 cycles
after DIV (13 after RSQRT); FSSET clears the sticky flags in program order. The scene program (VU1 `0x1A0`) culls
triangles with the sticky sign flags right after an FSSET, so this matters.

VU0 programs started by `VCALLMS` (`0x158`, `0x610`, `0x6D0`) are registered entries (`sct2_vu0.cpp`). The collision
query at `0x1000B4` feeds VU0 through VIF0 packets; it is native: it walks the packets' VIF codes, applies their
UNPACKs, runs the microprogram and returns its result.

## Movies

A movie either streams from the disc (an MPEG program stream opened by `0x17DB40`; its LSN and size are taken at
`0x17DB7C` / `0x17DB9C`) or sits in RAM, handed to libmpeg with `sceMpegAddBs` (the main menu's, looped).
`sct2_movie.cpp` answers `sceMpegGetPicture` (`0x186068`) and `sceMpegIsEnd` (`0x186188`): it reads the stream,
decodes its video with BozoEngine's decoder and writes each picture where the IPU would have put it, RGBA in 16-pixel
strips of macroblocks. A streamed movie's sound (private stream 1: SPU ADPCM after an `SShd` / `SSbd` header, 44.1 kHz
stereo, channels interleaved in `0x1000`-byte blocks) is decoded with it and fed to the port's sound stream.

## Sound

The game's IOP modules are packed in `.staytrash` (the block at `0x480650`, unpacked by `0x178B7C` / `0x178B98` at
boot). `tools/sct2_irx_extract.py` unpacks them at build time and BozoEngine's `native_irx` tools compile them
(`tools/sct2_irx_images.py` writes their image records); they run natively on BozoEngine's IOP host
(`sct2_audio.cpp`), their SPU2 work going to BozoEngine's mixer.

## Checking a run

`sct2-headless --shot-dir dir --shot-frames n,...` rasterizes the published frame on the CPU (`bozo/sct2_snapshot.h`),
`--wav file` writes the sound, `--pad-script file` drives the pads frame by frame.
