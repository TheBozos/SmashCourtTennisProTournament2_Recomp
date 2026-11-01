# Smash Court Tennis Pro Tournament 2 recompilation notes

## The disc

The ISO 9660 file system holds only `SYSTEM.CNF`, `SLUS_209.33` and `IOPRP300.IMG` (1.6 MB); the image is 4.4 GB.
Everything else, IRX modules included (the ELF names no `cdrom0:` IRX path, only `IOPRP300.IMG`), sits in raw
sectors the game reads by LSN. `scripts/extract_iso.sh` links the image as `game/disc.iso` and
`runtime/sct2_overrides.cpp` sets it as the runtime's CD image, which serves any read no file entry covers.

## Mapping the ELF

`SLUS_209.33` is stripped, entry `0x100000`, two loadable segments: `.text` at `0x100000` (0x10C510 bytes) and
`.staytrash` at `0x46EA80` (0x393B7 bytes, writable and executable, holds code). `recomp/sct2.toml` is
`ps2_analyzer` output with the paths made relative to the repository root:

- 1,889 functions extracted; 158 SCE SDK functions bound to runtime handlers (`[general].stubs`,
  `name@0xADDRESS`), 289 more recognised as library code without a runtime handler.
- Two jump tables detected (`0x207790`, `0x2079C0`), no patches.
- `.text` mixes code and data, and without symbols a function runs to the next known start. The analyzer also
  takes data words that decode as `JAL` for calls, so it found 21 "functions" that start in data (most of
  `0x1D4800`-`0x20C500`, plus `0x483804` and `0x4A2C0C` in `.staytrash`), and four real functions swallowed the data
  after them: `0x1A7D30`, `0x205DD8`, `0x206D70` and `0x46F0C8`, up to 13 MB of generated C++ each and 27,692
  undecodable words in total. The 21 are in `[general].skip`; `recomp/function_bounds.csv` (`ghidra_output`) carries
  every function with those four ends cut after their last `jr $ra`. A map turns the recompiler's own discovery
  off, so the map must list every function: `scripts/function_bounds.py` writes it from a run with
  `ghidra_output = ""` (`--end 0xSTART=0xEND` for the cuts). Result: 1,939 sources, 0 decode errors.
- A headless Ghidra run (stock `MIPS:LE:32`, no Emotion Engine extension) is not usable as the map: it does not
  decode R5900 instructions, cuts functions after 4 or 8 bytes (`_InitSys` at `0x19F860` became 4 bytes, the main
  thread died on its first call) and labels the 273 KB of data at `0x1C3410` as one function.

## VU1 microcode

`ps2_vu_aot` finds 5 microcode fragments (2,076 instruction pairs, one also used as VU0 microcode) in the ELF and
compiles them into 13 files. Microcode loaded from the disc at run time runs in the VU1 interpreter until it is found
and compiled.

## Boot

- `.staytrash` (`0x46EA80`) holds the IOP side of the boot: the code at `0x46F0C8` reboots the IOP and loads 12 IRX
  modules embedded in the ELF from `0x4A7EC0` on through `sceSifLoadModuleBuffer`. Before that it calls
  `sceSifSearchModuleByName("cdvd_ee_driver")` to stop an old copy. The runtime has no handler for it and no loadfile
  RPC server, so libkernel's `_lf_bind` retried sid `0x80000006` for ever; the override answers
  `KE_UNKNOWN_MODULE` (-202), which is what a freshly rebooted IOP says.
- The IOP's first raw read works (`lsn=0x213932`, 6 sectors, from `game/disc.iso`), the game sets PMODE and the
  display buffers and draws through PATH2/PATH3.

- crt0 calls `SetupThread` with the main stack at `0x1FF8000`-`0x2000000`, where the runtime keeps its interrupt and
  callback stacks by default: the main thread's saved `$ra` was overwritten and it returned to 0 after five frames.
  The override moves the runtime's reserved range to kernel RAM (`configureReservedMemory(0x80000, 0x100000,
  0x1FF8000)`), as in the Fighting for One Piece and Soulcalibur III ports.
- The main loop (`0x100270`) then runs at 60 Hz: the VSync callback `0x178D98` (registered with
  `sceGsSyncVCallback`) wakes the main thread, which sleeps in `0x178EC8` every frame.

## Code overlays in .staytrash

`.staytrash` is boot code the game overwrites with code it decompresses from disc (thread 2, entry `0x17F9B8`, is
an LZ decoder fed by the request block at `0x464640`; the images are not stored raw on disc). The per-frame task
list (`0x176760`, tasks of 0x80 bytes with the function at `+0` and the next task at `+8`) calls into them.

- `.staytrash` is left out of the main function table (`scripts/function_bounds.py --exclude`): the main table wins
  over overlay modules, so the boot routine kept running where the overlay was.
- `scripts/recomp_overlays.sh` recompiles `0x46EA80`-`0x46F424` of the ELF (the boot code; IRX images and data
  follow it) as module `boot`, with the 7 functions `.text` calls as entry points, and every captured image in
  `game/overlays/` as module `ovl_<hash>`. Functions an overlay reaches only through its own data (task tables) are
  added as entry points: words pointing at an `addiu $sp, $sp, -N` inside the image.
- Capture: `PS2X_SCT2_OVERLAY_DUMP=game/overlays` writes the bytes of `.staytrash` that differ from the ELF before
  each run of the task list, named by the hash of the first 64 bytes, the guard the runtime tells modules apart by
  (the overlay's own data changes from frame to frame).
- `scripts/update_bounds.sh` adds the `.text` functions only the captured overlays call (`0x1332F0` and 16 more
  for the first overlay); rerun it, `scripts/recomp.sh` and `scripts/recomp_overlays.sh` after each new capture.

First overlay: `0x46EA80`-`0x4776D8`, 82 sources (68 decode errors, all in data at its end).

Pro Tour and Challenge each load overlays of their own (`ovl_a4ee7a1b5f8fe4be`, `0x46EA80`-`0x6AD44C`, 587 sources, and
`ovl_9f389073fd591f8a`); without them the task list's call to `0x46EE90` found no code and the main thread stopped on
Pro Tour's loading screen. Captured with a run into each mode, then `update_bounds.sh`, `recomp.sh` and
`recomp_overlays.sh` as above.

## The black screen: StartThread's return value

Frames were presented but black: the scene task (`0x126A08`, state byte at task `+0x14`, jump table `0x207C70`)
sat in state 2 waiting for the stream library's status byte (`0x463C00`) to reach 2. That library's worker thread
(entry `0x17CA90`) had run 16 cycles in total. Its init (`0x17CC78`) does `tid = CreateThread(...)`, then
`StartThread(tid, 0)` and stores StartThread's result over the id (`bltz v0; sw v0, 0x514(s1)` at `0x17CD4C`). The
EE kernel returns the thread id there; the runtime returned `KE_OK` (0), so every request woke thread 0 instead of
the worker. Fixed in the runtime on TheBozos/PS2Recomp branch `sctpt2/startthread-tid` (with a test), which the
submodule tracks until it is merged. Found by classifying the 8 writers of the overlay task's wait flag (`+0x12`)
and walking back from the ones that never ran, as the room's coordinator suggested.

## Title screen and functions reached through pointers

Past the memory card prompt the title screen called `.text` functions nothing else calls: `0x138FF8` through a
pointer in data, `0x139528` through a table of leaf functions copied to the stack. `scripts/function_bounds.py
--pointers` adds every function a data word, or a lui/addiu pair in code, points at when the target is an
`addiu $sp, $sp, -N` prologue or follows a `jr $ra` and its delay slot (2,212 functions), with the data blocks of
`.text` left out of the map (`--exclude`), since byte patterns there look like pointers.

## The menu movie: DMA channel 4

The main menu plays a movie behind its panels. After a first `sceMpegAddBs` the game feeds the IPU itself:
before each `sceMpegGetPicture` (`0x1780F4`) it writes `D4_TADR`, `D4_QWC = 0` and `D4_CHCR = 0x105`
(`0x178008`-`0x178030`). The chain is `next` (`0x1241090`) to `ref` (`0x7D00` quadwords at `0x11C4080`) and back: the
whole 500 KB movie held in RAM, looped by a chain that never ends. The runtime had no channel 4, so `GetPicture`
waited for ever on `EeWaitReason::Mpeg`; reading the chain at once took 256 MiB of repeated stream instead. On
TheBozos/PS2Recomp branch `sctpt2/ipu-in-dma` channel 4 stays busy after a start and `sceMpegGetPicture` pulls
64 KiB at a time from where the chain stopped until a picture decodes.

## Into a match

- Overlays are filled by the decompressor thread over several frames: the capture waits for the request's status
  byte (`0x464640 + 0x1E`) and takes the overlay's size from the request (`+0x8`), since the match overlay runs to
  `0x576928`, past `.staytrash`. Overlay modules recompile in two passes (`scripts/module_bounds.py`) so the data in
  an image does not become code: every module now has 0 decode errors.
- `function_bounds.py --pointers` took a switch's case labels (they follow a `jr $ra`) for functions and cut
  `0x1065F8` in two; switch tables are now found from the `jr` that dispatches through them and left out.

Exhibition reaches the match intro (Rod Laver Arena, Roddick against Henin-Hardenne, 3D photographers through VU1).
Each frame `0x1000B4` sends a DMA tag from `0x1B81A0` to VIF0 (ITOP `0xF`, an `UNPACK` and `MSCAL 0`), reads the
batch count VU0 left in `vi05`, runs VU0 microprogram `0x158`, then builds a scratchpad tag list at `0x70000220` and
starts VIF0 on it as a chain. Two runtime bugs broke it; both are fixed on TheBozos/PS2Recomp branch `sctpt2/vu0-dma`:

- The game stores the whole tag (`0x30000018`) in `D0_QWC`. The runtime used all 32 bits and sent 21 MB of RAM through
  VIF0, whose stray `MPG` codes overwrote the VU0 microcode. Only the low 16 bits are the count.
- VIF0's `ITOP` only reached the VIF registers, so `XITOP` in microprogram `0x0` read a stale value and the count was
  wrong. The tag list then ended on stale scratchpad data, the chain ran on through memory and overwrote VU0 again,
  and the main thread spun in the walk. VU0 programs now read the ITOP VIF0 last set.

The match now reaches the player cards and the "FINAL SET" banner, and play runs (score, HUD, ball, points).

### The 3D scene: vf0 written inside an instruction pair

The court, stadium and players were black from the intro on. A GS dump during play (`PS2X_GS_DUMP`, CPU renderer)
showed only a darkening pass (untextured, RGBA `0x00000040`) and the small player textures; the GS renderers, the VU
compilers (`PS2X_VU_AOT=0 PS2X_VU_LIFTED=0`), the VU0 cycle limit and a VIF0 interrupt handler were ruled out.

During play VU1 runs 13 entry points of one microcode image (`PS2X_VU1_RECORD`; `0x01A0` and `0x2150` take 87% of the
cycles). A `0x01A0` run set ADC on nearly every vertex and left X/Y in clip space (`FTOI4` values like `0x0789BED8`):
only the first vertex of a batch was divided by w. The camera matrix VU1 reads (qwords `0x5`-`0x8`) is right.
The perspective divide is the pair at `0x5F0`, `SUB.xyw vf0, vf25, vf21` (for its flags) with
`DIV Q, vf0.w, vf27.w`: the interpreter and `ps2_vu_aot`'s blocks wrote the upper result into vf0 before the lower
half read it, so Q was `1 / 2170` style garbage instead of `1 / w`. Upper writes to vf0 are now discarded
(TheBozos/PS2Recomp branch `sctpt2/vu-vf0-discard`, with a test), and the match draws its court, players, line judges
and stadium.

### Players teleported into the net between points

After each point the player-update thread (overlay `0x4B6098`) builds a previous point 10 cm behind the new position,
`new - d / |d| * 0.1`. For a player who has not moved `|d|` is 0, so the previous point is 0/0 * 0.1 and, scaled by
100 for the net test, overflows to the float maximum (`0xFFFFFFFF`). The collision routine (`0x497338`) asks `0x10FB20`
(a VU0 test through `0x1000B4`) whether the segment from there crosses the net; in the port it reports a crossing,
the routine returns flags 3 with the crossing point (0, y, 0), and that point becomes the player's anchor and serve
position (tables at `0x355424` and `0x2BA000`): the player appeared in the middle of the net. PCSX2 keeps the player
in place. `sct2_overrides.cpp` answers "no crossing" for a segment that starts from an overflowed point; with it the
point, replay and next serve follow the same sequence as PCSX2.

The VU0 side is not the cause. VIF0's `MSCAL 0` is a box cull: for each net box (min, max) it subtracts the corners
from both segment ends and keeps the box unless both ends are on the same outer side (status S flag, any negative
component). With the start point `(0xFFFFFFFF, -50, 0xFFFFFFFF)` and the end `(-100, -50, 0)`, the PS2's own float
rules (exponent 255 as a number) and an operand clamp to the float maximum both keep 5 boxes, as the port does;
microprograms `0x158` (the segment's frame) and `0x268` then find the crossing. Only x86 NaN rules keep none: `0xFFFFFFFF`
is a NaN there, `max - from` returns it with its sign bit set and every box is rejected. So the difference with PCSX2
is either that PCSX2 never builds this start point or its NaN handling; which one holds on the console is not known.

## The native target

The port follows the other ports onto TheBozos/PS2Recomp `native/renderer` (no paraLLEl-GS, no GS emulation), one
layer at a time, each checked with a boot and a match:

- GS native (`PS2X_GS_RENDERER=native`, OpenGL): boot, menus, player select and a match draw right with no code
  change, except two textures: the player card's stat values and the net (`PSMT4` 64x64 at block 12172, garbled where
  the CPU GS only shows a moire for lack of mipmaps). A CPU GS primitive dump during the card shows how the values are
  drawn: sprites textured `PSMT4HH` at TBP 0, inside the CT32 frame buffer the game draws into (FRAME_1
  `0x7F000000000A0000`: FBMSK `0x7F000000` writes only the top alpha bit). The font lives in the alpha nibbles of
  the frame and is kept there across frames by that mask. The native back end applied FBMSK per whole channel, so
  every draw wrote the whole alpha byte; partial alpha masks are kept since `native/gs-fbmsk-alpha` (10e1f0c). The
  values stay invisible with it: a 4-bit read of a live CT32 target goes through GS memory, which does not carry the
  target's alpha bits, which is the back end's target/memory coherence (VP2S's rework). Reading indexed textures
  from the CLUT buffer instead of GS memory at CBP (`native/gs-clut-buffer`) changed nothing here.
  VP2S's page-versioned coherence between targets and GS memory (`native/gs-backend` 5112816) fixes it: the card's
  values and the memory card prompt's text draw as on the CPU GS, the prompt's `(PS2)` symbol and the card's
  punctuation since 80f30b7. Still open on the OpenGL GS: the player card shows white ghost trails of the player that
  the CPU GS does not draw (the net's holes went with the VU1 sandbox, below).
- Sound on the host clock (`PS2X_SPU2_WALLCLOCK=1 PS2X_IOP_WALLCLOCK=1`): 48 kHz exactly (240000 samples per 5 s),
  no underruns, the same voices as on the IOP clock. The game streams no AutoDMA (`adma=0`). Recorded with
  `PS2X_SPU2_WAV` over 150 s (logos, menu movie, a match), the output has, second by second, the zero crossings
  and level of the IOP-clock run, the menu movie's soundtrack included, with no clipping.
- IOP modules recompiled (`PS2X_IOP_NATIVE=1`): all 12 modules the game loads (pad, multitap, memory card, the sound
  drivers modhsyn/modmidi/modmsin/modsein and their helpers, filectrl) recompile with nothing left to the
  interpreter. They come from one EE buffer, so every dump has its name; `scripts/recomp_iop.sh` names each after its
  dump file. filectrl's checksum loop branches back into the delay slot of the branch before it, which
  `tools/iop_recomp.py` emitted without a label: fixed on `native/iop-recomp-slot-label` (9098b17).
- Since `native/renderer` 4674b8b presents on the GPU, `PS2X_FRAME_DUMP` writes nothing until the frame-dump fix
  (native/renderer 175ed5c); `PS2X_GS_NATIVE_CPUPRESENT=1` keeps the CPU presentation for scripted checks.

- Frame loop on the host clock (vblanks out): the main thread ends each frame in `0x178EC8` (FlushCache,
  CancelWakeupThread(main), SleepThread), woken by the vblank handler `0x178D98` once the frame interval has counted
  down and the DMA paths are idle; the loader and sound threads run while it sleeps. `sct2_overrides.cpp` replaces that
  wait with `EeScheduler::waitHostDeadline` (native/frame-sleep, JC2): the main thread waits for the next deadline at
  the FramePacer cap while the other threads run, then the wake-up charges the EE the frame's real time, presents and
  counts the frame; the handler's wake-ups are still cancelled each frame. 300 game frames per 5 s in menus and a
  match, fewer only while loading (169-274 in the loading windows since native/renderer d191191's idle waits), EE on
  real time, no underruns; the sound, the menu movie's included, matches the vblank loop second by second (`PS2X_SPU2_WAV`).
  The frame wait's return value is not read by its callers, so the replacement returns none. `SCT2_NATIVE_FRAMES=0`
  keeps the vblank loop. The game
  copies each frame into one display buffer (DISPFB1 stays `0x5148C`), so `display flips` stays 0; `game frames`
  counts them.
- VU1 program lifted to C++, emulation still to remove: the one microcode image the game runs (`95d44bbc45fcd2fe`,
  1496 pairs) is lifted by `PS2Recomp/tools/vu1_lift.py` (spec `recomp/vu1_lift_spec.json`, 60 start pcs, output
  `generated/vu1lift/` by `scripts/compile_vu1.sh`) and runs in place of the sandbox by default
  (`runtime/sct2_vu1_port.cpp`, `SCT2_VU1_PORT=0` for the sandbox). Still on the VIF1 thread through
  `ps2x::native::Vu1StreamAdapter`; the emulated VU1 runs nothing (`vu1 mscal=0`). `SCT2_VU1_PORT=check` runs the
  lifted program and the sandbox from the same state and compares registers, flags, VU memory, PATH1 packets and
  cycles: 0 mismatches, 0 unknown JR targets and 0 starts left on the sandbox (any other microcode image would be) over
  Exhibition, the Tutorial, Challenge and Spectator, about 0.7-1.1 million programs per 600 frames; the menus and the
  Pro Tour screens reached by the test script start no VU1 program. The last Spectator mismatches (0x148/0x1a0,
  12 cycles more on the sandbox) were the sandbox's: its compiled block at 0x5d8 kept the sticky sign of the
  `ADD.xyz` sharing a pair with `FSSET`, so `FSAND 0xc0` at 0x618 read a stale bit and the strip loop stored ADC more
  often; the interpreter (`PS2X_VU_AOT=0`) agreed with the lifted program. Fixed in ps2_vu_aot (PS2Recomp
  sctpt2/vu1-lift-fixes 5cfa0bc), so `scripts/compile_vu1.sh` has to run again. The Challenge practice court now runs
  at 296-300 game frames per 5 s (249-264 on the sandbox), Spectator at 300 on both. This is not native rendering
  yet: the lifted program still builds GIF packets in VU memory and they go through the GS register state
  (`gsWrites`) as before. The goal stays native draws built from the game's own data (the VIF1 packets' vertices,
  matrices and GS setup); the direct-draw output of `vu1_lift.py` is being built for the SC3 port first.
- Movie pictures on the host clock: the menu movie's `sceMpegGetPicture` (runtime MPEG HLE) waited for vsyncs to
  pace its pictures, the last vblank wait left in a native run. With native frames it now waits for the picture's
  deadline on the host clock (native/mpeg-host-clock 8508fd6): 0 vsync waits in a 150 s run through the movie, menus
  and a match, 300 game frames per 5 s outside loading, the movie advances every dump and its sound matches the
  previous run second by second.
- Internal scale 2 (`PS2X_GS_SCALE=2`, 1280x896): menus and a match draw the same as at 1x, with the same ~70% idle EE.

## Status

Boots, menus, player select, Exhibition and World Tournament matches with music and sound effects, on the native
target by default (GS on OpenGL at 2x, IOP modules recompiled, the VU1 program lifted to C++, sound and frames on
the host clock, 60 fps). The memory card works: Options > SAVE writes the system data to `game/mc0/BASLUS-20933SCSYS/`
(150 KB data, `icon.sys`, icon), a second save offers to overwrite it, the next boot loads it ("Load Successful") and
Options > LOAD reads it back. Pro Tour (mode select, new player profile), Challenge (Shooting Service, hits and high
score), Spectator (a computer match) and the Tutorial (lessons) work. Open: ghost trails on the player card. The
practice court of the Tutorial and Challenge sends much more VU1 work than a match: on the sandbox it ran at about
52 fps (249-264 game frames per 5 s in Challenge, VIF1 thread about 80% busy); with the lifted VU1 program Challenge runs at 296-300. VIF1
and the VU1 program run on their own thread by default (`PS2X_VIF1_THREAD=1`).
