<div align="center">

# SmashCourtTennisProTournament2_Recomp

**A native Linux port of *Smash Court Tennis Pro Tournament 2* (NTSC-U, `SLUS-20933`), made by statically recompiling the game's PlayStation 2 code to C++.**<br>
Not an emulator: the game's own MIPS R5900 code, rebuilt as a native program on [BozoEngine](https://github.com/TheBozos/BozoEngine), with the Sony libraries, the graphics and the sound native.

![Platform](https://img.shields.io/badge/platform-Linux-1b8cff)
![Compiler](https://img.shields.io/badge/compiler-Clang-1b8cff)
![Based on](https://img.shields.io/badge/based%20on-BozoEngine-17a35b)
![Status](https://img.shields.io/badge/status-in%20progress-e3a21a)

<img src="docs/screenshots/main_menu.jpg" width="49%"> <img src="docs/screenshots/title_screen.jpg" width="49%">
<img src="docs/screenshots/player_select.jpg" width="49%"> <img src="docs/screenshots/match_play.jpg" width="49%">

<sub>Captured from the earlier PS2Recomp build of the port.</sub>

</div>

> [!IMPORTANT]
> No game files are included: no ELF, IRX modules, data archives, or code generated from them.
> You need **your own copy** of the North American release, PS2 ID **`SLUS-20933`**. The port is built against that disc's executable:
>
> | File | SHA-1 |
> |---|---|
> | `SLUS_209.33` | `34da8370f6aca2c4c51e249cc97162aa03999fe5` |

## Status

The game runs on BozoEngine with nothing emulated: no GS, VIF, DMA controller or IOP. Headless runs boot through the
memory card check, the intro movie (with its sound), the title, the menus and the selection screens into Exhibition,
Arcade, Tutorial, Challenge and Spectator play, with the court, stadium, players, shadows, HUD, music and sound effects.

| Milestone | |
|---|:---:|
| Main ELF and its three code overlays recompiled by `bozo-recomp` | ✅ |
| Boot, memory card, disc reads by sector | ✅ |
| Movies: streamed from the disc and held in RAM, with sound | ✅ |
| 2D: menus, text, HUD | ✅ |
| VU0 microcode lifted to C++, collision query native | ✅ |
| VU1 microcode lifted to C++, the frame's VIF1 packets played natively: the 3D | ✅ |
| Music and sound effects (the game's IOP modules on BozoEngine's sound) | ✅ |
| Exhibition, Arcade, Tutorial, Challenge and Spectator | ✅ |
| Pro Tour (its profile screen checked) | ⏳ |
| The window build (`sct2`) at run time | ⏳ |

✅ works · 🔧 in progress · ⏳ not checked yet

[RECOMP_NOTES.md](RECOMP_NOTES.md) describes how the port works.

## How it works

```
SLUS_209.33 ──► bozo-recomp ──► C++ (main ELF)
disc image  ──► tools/sct2_overlays.py ──► 3 code overlays ──► bozo-recomp ──► C++ (one module each)
SLUS_209.33 ──► tools/sct2_vu0_lift.py ──► VU0 and VU1 microcode as C++
                                │
     bozo/port/ (SCTPT2's native graphics, movies, VU programs) + BozoEngine ──► sct2, sct2-headless
```

- The ELF has no symbols. `recomp/sct2_symbols.txt` names the Sony library functions, which BozoEngine runs natively.
- The game unpacks three code overlays from the disc into `.staytrash` (`0x46EA80`). `tools/sct2_overlays.py` unpacks
  them at build time and each is compiled as its own module, chosen at run time by its first bytes.
- `bozo/port/` replaces the game's hardware work: its 2D library, the frame's DMA chain (VIF1 and the lifted VU1
  microcode, the GIF packets they make) and its texture uploads become `Renderer` draws; movies are decoded natively.

## Requirements

- Linux (developed on Arch)
- CMake 3.21+, Ninja, Clang with C++20, Python 3
- FFmpeg development libraries (`libavcodec`, `libavutil`), for the movies
- OpenGL 3.3 and the X11 / Wayland development headers for the window build (SDL3 is fetched and built)
- Your own copy of `SLUS-20933` as a disc image, and its `SLUS_209.33` extracted

## Build and run

```bash
git clone --recurse-submodules https://github.com/TheBozos/SmashCourtTennisProTournament2_Recomp.git
cd SmashCourtTennisProTournament2_Recomp
scripts/extract_iso.sh "/path/to/Smash Court Tennis - Pro Tournament 2 (USA).iso"   # SLUS_209.33 + disc.iso link -> game/
cmake -S . -B build/bozo -G Ninja -DSCT2_ELF=game/SLUS_209.33 -DSCT2_DISC=game/disc.iso
ninja -C build/bozo sct2 sct2-headless
build/bozo/sct2 --iso game/disc.iso
```

`sct2` runs the game in a window in real time (its options are listed at the top of `bozo/sct2_window.cpp`; pads come
from `input.ini`).
`sct2-headless` runs it with no window as fast as it goes: `--frames n`, `--pad-script file` (frame-keyed pad input),
`--card-dir dir`, `--shot-dir dir --shot-frames n,n,...` (CPU-rasterized PNGs of the frames) and `--wav file`.
`-DSCT2_WINDOW=OFF` builds only the headless program. `scripts/remote_bozo_build.sh` builds on another x86-64 Linux
box over ssh.

## Disc facts

| | |
|---|---|
| Boot ELF | `SLUS_209.33`, entry `0x100000`, stripped; `.text` at `0x100000` and a second code segment `.staytrash` at `0x46EA80` |
| Video mode | NTSC (`SYSTEM.CNF` `VER = 1.00`) |
| IOP image | `IOPRP300.IMG` |
| File system | Only `SYSTEM.CNF`, `SLUS_209.33` and `IOPRP300.IMG`; the other ~4.4 GB of the DVD (data, movies) are raw sectors the game reads by LSN |

## Repository layout

| Path | Contents |
|---|---|
| `BozoEngine/` | The engine (git submodule, TheBozos/BozoEngine) |
| `CMakeLists.txt`, `cmake/bozo.cmake` | The build: recompilation, overlays, microcode lifting, the programs |
| `bozo/` | The programs (`sct2_window.cpp`, `sct2_headless.cpp`) and `port/`, SCTPT2's native code |
| `recomp/sct2_symbols.txt` | The Sony library functions by address |
| `tools/` | Build-time tools: overlay and IOP module unpacking, microcode lifting, the override list |
| `scripts/` | Disc extraction, remote build, disassembly and IRX helpers |
| `game/`, `build/` | Created locally, ignored by git: disc files, builds |

Everything derived from the game (disc files, generated code, builds, logs) stays outside this repository.
Never commit or redistribute it.

## Credits

- [BozoEngine](https://github.com/TheBozos/BozoEngine) by The Bozos, the engine and recompiler this port runs on.
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors, which the port was first brought up on.
- *Smash Court Tennis Pro Tournament 2* © Namco. This is an unofficial fan project, not affiliated with or endorsed by them.
