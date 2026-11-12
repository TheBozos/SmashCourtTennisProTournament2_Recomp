#pragma once

#include "bozo/context.h"

#include <cstdint>

namespace sct2::vu1
{
    // The game's VU1 microcode lifted to C++ (tools/sct2_vu0_lift.py --unit 1): runs from `pc` until the program ends on
    // `ctx`'s VU registers and VU1's 16 KB of memory `mem`; XTOP and XITOP read `top` and `itop`, each XGKICK calls
    // `kick` with the quadword its GIF packet starts at, `tpc` is where the program stopped.
    void run(uint32_t pc, R5900Context *ctx, uint8_t *mem, uint32_t itop, uint32_t top, uint32_t &tpc,
             void (*kick)(void *, uint32_t), void *user);
}
