#pragma once

#include "bozo/context.h"

#include <cstdint>

namespace bozo::port
{
    class Registry;
}

namespace sct2::vu0
{
    // The game's VU0 microcode lifted to C++ (tools/sct2_vu0_lift.py): runs from `pc` until the program ends; `itop` is
    // what XITOP reads, `tpc` where the program stopped (a continuation's start).
    void run(uint32_t pc, R5900Context *ctx, Runtime *rt, uint32_t itop, uint32_t &tpc);
    void install(bozo::port::Registry &runtime);
}
