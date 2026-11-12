// SCTPT2's hardware-only output instructions: no consumer exists natively, so they are replaced before they reach MMIO.
#include "sct2_graphics.h"

#include "bozo/context.h"
#include "bozo/ee/kernel.h"
#include "bozo/port.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>

namespace sct2::graphics
{
    namespace
    {
        // bozo:native kHardwareOutput
        constexpr uint32_t kHardwareOutput[] = {
            // vblankHandler: CSR field, and the GS path recovery's VIF0/VIF1 stop and GIF CHCR clear.
            0x00178dbcu, 0x00178e54u, 0x00178e60u, 0x00178e6cu, 0x00178e74u,
            // Native presentation owns the display: DISPFB1/2, DISPLAY1/2, PMODE, CSR.
            0x00179060u, 0x0017907cu, 0x00179084u, 0x0017908cu, 0x001797acu, 0x001797ecu,
            // storeImage: the VIF1 reverse transfer of a GS readback.
            0x00179834u, 0x00179840u, 0x00179844u, 0x0017984cu, 0x00179850u, 0x0017986cu, 0x00179888u, 0x0017988cu,
            0x001798a4u, 0x001798a8u, 0x001798b0u, 0x001798b4u, 0x001798e4u, 0x001798e8u, 0x001798f4u, 0x001798fcu,
            0x00179904u,
            // vif0Reset: MARK, FBRST, ERR, the VU0 reset through FBRST and the FIFO's initial VIF codes.
            0x00180074u, 0x0018007cu, 0x00180080u, 0x00180084u, 0x0018008cu, 0x001800a4u, 0x001800b0u,
            // graphicsInit (.staytrash): the same channel stops and display registers.
            0x0046ec0cu, 0x0046ec18u, 0x0046ec20u, 0x0046ec30u, 0x0046ed54u, 0x0046ed64u, 0x0046ed74u, 0x0046ed84u,
        };

        constexpr auto kSorted = [] {
            auto sites = std::to_array(kHardwareOutput);
            std::sort(sites.begin(), sites.end());
            return sites;
        }();

        // dmaStart(chcr, madr, qwc) and dmaStartChain(chcr, tadr) serve every channel: only the graphics ones are retired.
        // bozo:native kDmaStart
        constexpr uint32_t kDmaStart[] = {0x00177380u, 0x0017739cu, 0x001773a0u, 0x001773b0u,
                                          0x0017f91cu, 0x0017f938u, 0x0017f93cu, 0x0017f94cu};

        bool graphicsChannel(R5900Context *ctx) { return (getRegU32(ctx, 4) & 0x1FFFFFFFu) < 0x1000B000u; }

        bool instruction(uint8_t *, R5900Context *ctx, Runtime *rt, uint32_t pc)
        {
            if (std::find(std::begin(kDmaStart), std::end(kDmaStart), pc) != std::end(kDmaStart))
            {
                if (!graphicsChannel(ctx))
                    return false;
                if (pc == 0x00177380u || pc == 0x0017f91cu)
                    setRegU64(ctx, 8, 0);
                return true;
            }
            if (!std::binary_search(kSorted.begin(), kSorted.end(), pc))
                return false;
            switch (pc)
            {
            case 0x00178dbcu:
                setRegU64(ctx, 2, (bozo::port::vblankCount(rt) & 1u) << 13);
                break;
            case 0x00178e6cu:
            case 0x0046ec20u:
                setRegU64(ctx, 2, 0);
                break;
            case 0x00179850u:
            case 0x001798b4u:
                setRegU64(ctx, 10, 0);
                break;
            case 0x00180080u:
                setRegU64(ctx, 8, 0);
                break;
            case 0x0017986cu:
                setRegU64(ctx, 10, 2);
                break;
            default:
                break;
            }
            return true;
        }

        constexpr uint32_t kWaitFrame = 0x00178ec8u;
        constexpr uint32_t kGraphicsBusy = 0x00178f88u;
        constexpr uint32_t kWakeKey = 0x53435432u;
        constexpr uint64_t kField = bozo::port::kEeClockHz * 1001u / 60000u;
        std::set<int32_t> g_sleeping;
        uint64_t g_deadline = 0;

        // The game sleeps until its vblank callback wakes it, and its other threads run meanwhile. The callback only
        // comes at the native frame boundary, so the thread sleeps until the next field on the EE clock instead,
        // comes back here and ends the frame then.
        void waitFrame(uint8_t *, R5900Context *ctx, Runtime *rt)
        {
            const int32_t self = bozo::port::currentThread(rt);
            if (g_sleeping.erase(self))
            {
                bozo::port::frameEnd(rt, ctx);
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            auto &kernel = bozo::port::kernel(rt);
            const uint64_t now = bozo::port::now(rt);
            g_deadline = std::max(g_deadline + kField, now);
            kernel.cancelWakeupThread(self, false);
            kernel.scheduleNative(kWakeKey, g_deadline, [&kernel, self](uint64_t) { kernel.wakeupThread(self, true); });
            g_sleeping.insert(self);
            kernel.sleepThread();
            ctx->pc = kWaitFrame;
            bozo::port::transfer(rt, ctx);
        }

        // The VIF1, GIF and GS path busy bits: native submissions complete when made.
        void graphicsBusy(uint8_t *, R5900Context *ctx, Runtime *)
        {
            setReturnS32(ctx, 0);
            ctx->pc = getRegU32(ctx, 31);
        }
    }

    void install(bozo::port::Registry &runtime)
    {
        runtime.nativeGraphics(&instruction);
        runtime.registerFunction(kWaitFrame, &waitFrame);
        runtime.registerFunction(kGraphicsBusy, &graphicsBusy);
    }
}
