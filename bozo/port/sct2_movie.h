#pragma once

#include <cstdint>

struct R5900Context;

namespace bozo::port
{
    class Registry;
}

namespace sct2::movie
{
    void install(bozo::port::Registry &runtime);
    // The native instructions the movie watches: true when `pc` is one, the store still made.
    bool observe(R5900Context *ctx, uint32_t pc);
}
