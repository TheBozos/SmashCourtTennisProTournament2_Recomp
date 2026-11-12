// sct2-headless: Smash Court Tennis Pro Tournament 2 on BozoEngine with no window. Every library native, the disc image read,
// BozoEngine's own timing, SCTPT2's overrides native.
//
//   sct2-headless [--frames n] [--elf SLUS_209.33] [--symbols map] [--iso image] [--pad-script [n=]file]... [--card-dir dir]
//                [--report n] [--print] [--quiet] [--no-sound]
#include "sct2_audio.h"
#include "sct2_functions.h"

#include "bozo/app/determinism.h"
#include "bozo/ee/dispatcher.h"
#include "bozo/host/free_host.h"
#include "bozo/port.h"
#include "bozo/render/renderer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

#ifndef BOZO_RUN_DEFAULT_ELF
#define BOZO_RUN_DEFAULT_ELF ""
#endif
#ifndef BOZO_RUN_DEFAULT_SYMBOLS
#define BOZO_RUN_DEFAULT_SYMBOLS ""
#endif

int main(int argc, char **argv)
{
    bozo::host::FreeOptions opt;
    opt.elf = BOZO_RUN_DEFAULT_ELF;
    opt.symbolMap = BOZO_RUN_DEFAULT_SYMBOLS;
    opt.framePeriodCycles = bozo::port::kEeClockHz * 1001u / 60000u;
    uint64_t report = 500;
    for (int k = 1; k < argc; ++k)
    {
        const std::string a = argv[k];
        auto value = [&]() -> std::string { return k + 1 < argc ? argv[++k] : ""; };
        if (a == "--frames")
            opt.frames = static_cast<uint32_t>(std::stoul(value()));
        else if (a == "--elf")
            opt.elf = value();
        else if (a == "--symbols")
            opt.symbolMap = value();
        else if (a == "--iso")
            opt.iso = value();
        else if (a == "--pad-script")
        {
            std::string v = value();
            int player = 1;
            if (v.size() > 2 && v[1] == '=' && v[0] >= '1' && v[0] <= '4')
            {
                player = v[0] - '0';
                v = v.substr(2);
            }
            (player == 1 ? opt.padScript : opt.playerScripts[static_cast<size_t>(player - 2)]) = v;
        }
        else if (a == "--card-dir")
            opt.cardDir = value();
        else if (a == "--report")
            report = std::stoull(value());
        else if (a == "--quiet")
            opt.quiet = true;
        else if (a == "--print")
            opt.printGameOutput = true;
        else if (a == "--no-sound")
            opt.sound = false;
        else
        {
            std::fprintf(stderr, "usage: sct2-headless [--frames n] [--elf path] [--symbols map] [--iso image] [--pad-script [n=]file]... "
                                 "[--card-dir dir] [--report n] [--print] [--quiet] [--no-sound]\n");
            return 2;
        }
    }
    if (opt.elf.empty() || opt.iso.empty())
    {
        std::fprintf(stderr, "sct2-headless: --elf and --iso are required\n");
        return 2;
    }
    bozo::det::setEnabled(true);

    const bozo::port::Port *port = bozo::port::port();
    bozo::port::Registry registry;
    port->install(registry);
    const std::vector<uint32_t> missing = bozo::port::undeclared(registry, sct2_external_table, sct2_external_count);
    if (!missing.empty())
    {
        std::fprintf(stderr, "%zu registered overrides are not externals of the generated code (regenerate):", missing.size());
        for (uint32_t a : missing)
            std::fprintf(stderr, " 0x%x", a);
        std::fprintf(stderr, "\n");
        return 2;
    }

    bozo::host::FreeHost host(opt);
    bozo::ee::Dispatcher d(host);
    d.setFunctions(sct2_function_table, sct2_function_count, sct2_resume_table, sct2_resume_count);
    std::string error;
    if (!host.setUp(d, sct2_external_table, sct2_external_count, &registry, error))
    {
        std::fprintf(stderr, "sct2-headless: %s\n", error.c_str());
        return 2;
    }
    sct2::audio::attach(host.libraries());
    std::printf("sct2-headless: %s, %zu overrides, ELF %s, disc %s\n", port->title, registry.functions().size(),
                opt.elf.c_str(), opt.iso.c_str());

    std::string waiter;
    std::map<std::string, uint64_t> waiters;
    const auto t0 = std::chrono::steady_clock::now();
    auto state = [&](uint64_t frame) {
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const bozo::render::Renderer &r = bozo::render::Renderer::instance();
        const auto published = r.published();
        const auto overlay = r.publishedOverlay();
        std::printf("frame %llu: EE cycle %llu (%llu vblanks), loop %s; renderer: 3D %zu batches, 2D %zu batches; %.1f s\n",
                    static_cast<unsigned long long>(frame), static_cast<unsigned long long>(d.now()),
                    static_cast<unsigned long long>(d.kernel().vblankCount()), waiter.c_str(),
                    published ? published->batches.size() : 0u, overlay ? overlay->batches.size() : 0u, secs);
        std::fflush(stdout);
    };
    host.onFrame = [&](uint64_t frame, const R5900Context *ctx) {
        if (ctx)
        {
            waiter = host.functionAt(ctx->r[31].uw[0]);
            ++waiters[waiter.substr(0, waiter.find('+'))];
        }
        if (report && (frame + 1) % report == 0 && !opt.quiet)
            state(frame + 1);
    };
    const uint64_t frames = host.run();
    state(frames);
    const bozo::host::FreeStats &s = host.stats();
    std::printf("end: %s; %llu frames; %llu library calls, %llu kernel calls, %llu override calls, %llu VU0 programs, %llu "
                "hardware accesses (%llu to registers the I/O model does not know)\n",
                host.endReason().empty() ? "every thread ended" : host.endReason().c_str(),
                static_cast<unsigned long long>(frames), static_cast<unsigned long long>(s.libraryCalls),
                static_cast<unsigned long long>(s.kernelCalls), static_cast<unsigned long long>(s.overrideCalls),
                static_cast<unsigned long long>(s.vu0Calls), static_cast<unsigned long long>(s.ioAccesses),
                static_cast<unsigned long long>(s.ioUnknown));
    std::printf("frame ends by loop function:");
    for (const auto &[fn, n] : waiters)
        std::printf(" %s %llu;", fn.c_str(), static_cast<unsigned long long>(n));
    std::printf("\noverride calls:");
    for (const auto &[a, n] : host.overrideCalls())
        std::printf(" 0x%x:%llu", a, static_cast<unsigned long long>(n));
    std::printf("\n");
    const bozo::libs::PadState &pads = host.libraries().pad;
    std::printf("pad reads (port.slot: reads; multitap %s):", pads.multitap ? "open" : "closed");
    for (int p = 0; p < 2; ++p)
        for (int sl = 0; sl < 4; ++sl)
            std::printf(" %d.%d: %llu;", p + 1, sl, static_cast<unsigned long long>(pads.socketReads[p][sl]));
    std::printf("\n");
    for (const auto &[name, n] : s.unanswered)
        std::printf("no native answer: %s (%llu)\n", name.c_str(), static_cast<unsigned long long>(n));
    return host.endReason().rfind("FAULT", 0) == 0 ? 1 : 0;
}
