// sct2: Smash Court Tennis Pro Tournament 2 on BozoEngine in a window, in real time. The free host as sct2-headless runs it,
// on the host's clock, each frame presented in an SDL3 window through OpenGL 3.3 (bozo::app::runWindow). The pads: the
// PC's devices through input.ini (bozo::app::DevicePad), or the frame-keyed scripts given.
//
//   sct2 [--iso image] [--elf SLUS_209.33] [--card-dir dir] [--width w --height h] [--fullscreen] [--no-vsync]
//        [--render-scale s] [--capture-dir dir] [--capture-frames n,n,...] [--quit-after n] [--pad-script [n=]file]...
//        [--print] [--no-sound] [--audio-latency ms] [--wav file]
#include "sct2_main.h"

#include "bozo/app/app.h"
#include "bozo/app/audio_device.h"
#include "bozo/app/determinism.h"
#include "bozo/app/device_pad.h"
#include "bozo/app/video_settings.h"
#include "bozo/app/window.h"
#include "bozo/audio/wav.h"

#include <cstdio>
#include <filesystem>
#include <sstream>
#include <string>

#ifndef BOZO_RUN_DEFAULT_ELF
#define BOZO_RUN_DEFAULT_ELF ""
#endif
#ifndef BOZO_RUN_DEFAULT_SYMBOLS
#define BOZO_RUN_DEFAULT_SYMBOLS ""
#endif

int main(int argc, char **argv)
{
    bozo::app::Identity identity;
    identity.title = "Smash Court Tennis Pro Tournament 2";
    identity.folder = "SmashCourtTennisProTournament2_Recomp";
    identity.xdgFolder = "sctpt2_recomp";
    identity.bootElf = "SLUS_209.33";
    identity.dataFolder = "game";
    bozo::app::setIdentity(identity);
    bozo::app::startLogging();
    bozo::app::installCrashHandler();

    bozo::host::FreeOptions opt;
    opt.framePeriodCycles = sct2::kFramePeriodCycles;
    opt.elf = BOZO_RUN_DEFAULT_ELF;
    opt.symbolMap = BOZO_RUN_DEFAULT_SYMBOLS;
    opt.realTime = true;
    opt.quiet = true;
    bozo::app::WindowOptions window;
    window.title = "Smash Court Tennis Pro Tournament 2 (BozoEngine)";
    bozo::app::AudioOptions audio;
    bool sound = true;
    std::string wavPath;
    int width = 0, height = 0;
    bool fullscreen = false;
    float renderScale = 0.0f;
    for (int k = 1; k < argc; ++k)
    {
        const std::string a = argv[k];
        auto value = [&]() -> std::string { return k + 1 < argc ? argv[++k] : ""; };
        if (a == "--elf")
            opt.elf = value();
        else if (a == "--iso")
            opt.iso = value();
        else if (a == "--card-dir")
            opt.cardDir = value();
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
        else if (a == "--width")
            width = std::stoi(value());
        else if (a == "--height")
            height = std::stoi(value());
        else if (a == "--fullscreen")
            fullscreen = true;
        else if (a == "--no-vsync")
            window.vsync = false;
        else if (a == "--render-scale")
            renderScale = std::stof(value());
        else if (a == "--capture-dir")
            window.captureDir = value();
        else if (a == "--capture-frames")
        {
            std::stringstream list(value());
            for (std::string item; std::getline(list, item, ',');)
                if (!item.empty())
                    window.captureFrames.push_back(std::stoull(item));
        }
        else if (a == "--quit-after")
            window.quitAfterFrames = std::stoull(value());
        else if (a == "--print")
            opt.printGameOutput = true;
        else if (a == "--no-sound")
            sound = false;
        else if (a == "--audio-latency")
            audio.latencyMs = std::stod(value());
        else if (a == "--wav")
            wavPath = value();
        else
        {
            std::fprintf(stderr, "usage: sct2 [--iso image] [--elf path] [--card-dir dir] [--width w] [--height h] [--fullscreen] "
                                 "[--no-vsync] [--render-scale s] [--capture-dir dir] [--capture-frames n,n,...] [--quit-after n] "
                                 "[--pad-script [n=]file]... [--print] [--no-sound] [--audio-latency ms] [--wav file]\n");
            return 2;
        }
    }
    bozo::app::video::loadAtStartup(window.width, window.height);
    {
        bozo::app::video::Settings &v = bozo::app::video::edited();
        if (width > 0 && height > 0)
            v.width = width, v.height = height;
        if (fullscreen)
            v.mode = bozo::app::video::WindowMode::Borderless;
        if (renderScale > 0.0f)
            v.renderScale = renderScale;
    }
    window.videoSettings = true;
    std::error_code ec;
    if (opt.elf.empty() || !std::filesystem::is_regular_file(opt.elf, ec))
    {
        std::string searched;
        const std::filesystem::path found = bozo::app::findGameData(&searched);
        if (found.empty())
        {
            std::fprintf(stderr, "sct2: no ELF (--elf); looked for the game data at\n%s", searched.c_str());
            return 2;
        }
        opt.elf = found.string();
    }
    if (opt.cardDir.empty() && !bozo::app::userDirectory().empty())
        opt.cardDir = bozo::app::userDirectory().string();
    if (opt.iso.empty())
        opt.iso = sct2::findDiscImage(opt.elf);
    if (opt.iso.empty())
    {
        std::fprintf(stderr, "sct2: no disc image (--iso, SCT2_ISO, or one .iso beside the ELF)\n");
        return 2;
    }
    bozo::det::setEnabled(false);

    bozo::port::Registry registry;
    if (!sct2::installPort("sct2", registry))
        return 2;
    bozo::host::FreeHost host(opt);
    bozo::ee::Dispatcher d(host);
    if (!sct2::setUp("sct2", host, d, registry))
        return 2;
    bozo::app::DevicePad pad;
    bool scripted = !opt.padScript.empty();
    for (const std::string &script : opt.playerScripts)
        scripted = scripted || !script.empty();
    if (!scripted)
        host.libraries().pad.input = &pad;
    std::fprintf(stderr, "sct2: %s, %zu overrides, ELF %s, disc %s, user folder %s, memory cards in %s\n",
                 bozo::port::port()->title, registry.functions().size(), opt.elf.c_str(), opt.iso.c_str(),
                 bozo::app::userDirectory().string().c_str(), opt.cardDir.empty() ? "memory" : opt.cardDir.c_str());

    bozo::app::AudioDevice device(audio);
    bozo::audio::WavWriter wav;
    if (!wavPath.empty() && !wav.open(wavPath))
        std::fprintf(stderr, "sct2: cannot write %s\n", wavPath.c_str());
    if (sound)
        device.open();
    if (device.isOpen() || wav.isOpen())
        host.onAudio = [&](uint64_t, const int16_t *samples, size_t count) {
            device.push(samples, count);
            wav.write(samples, count);
        };

    bozo::app::Game game;
    game.run = [&] {
        const uint64_t frames = host.run();
        device.close();
        wav.close();
        std::fprintf(stderr, "sct2: %s; %llu frames\n", host.endReason().c_str(), static_cast<unsigned long long>(frames));
        return host.endReason().rfind("FAULT", 0) == 0 ? 1 : 0;
    };
    game.stop = [&] { host.requestStop(); };
    game.framesEnded = [&] { return host.framesEnded(); };
    return bozo::app::runWindow(window, game, &pad);
}
