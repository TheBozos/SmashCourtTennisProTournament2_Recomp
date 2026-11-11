#include "bozo/context.h"
#include "bozo/port.h"

#include <cstdint>

namespace
{
    constexpr uint32_t kSct2Entry = 0x00100000u;
    constexpr uint32_t kSceSifSearchModuleByName = 0x0019EBD0u;
    constexpr int32_t kKeUnknownModule = -202;

    // The boot code asks for "cdvd_ee_driver" to stop an old copy before rebooting the IOP: a fresh IOP has none.
    void searchModuleByNameUnknown(uint8_t *, R5900Context *ctx, Runtime *)
    {
        setReturnS32(ctx, kKeUnknownModule);
        ctx->pc = getRegU32(ctx, 31);
    }

    void install(bozo::port::Registry &runtime)
    {
        runtime.registerFunction(kSceSifSearchModuleByName, &searchModuleByNameUnknown);
    }
}

BOZO_PORT("Smash Court Tennis Pro Tournament 2 (USA)", "SLUS_209.33", kSct2Entry, &install)
