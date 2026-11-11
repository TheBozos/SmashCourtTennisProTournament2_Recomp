// SCTPT2's sound and file modules from its ELF (libsd, the standard kit's MIDI sequencer, synthesizer and inputs, the
// game's Filectrl and Sound), compiled to native code and run by BozoEngine's compiled-library host over native libsd.
#pragma once

#include "bozo/libs/libraries.h"
#include "bozo/native/compiled_libraries.h"

#include <cstdint>
#include <vector>

namespace sct2::audio
{
    std::vector<bozo::native::irx::Image> images();
    void attach(bozo::libs::Libraries &libraries);
    // sceSifLoadModuleBuffer of the IRX the game copied to `iopAddress`: its compiled image started, or a module id for
    // one BozoEngine answers on the EE side (pad, multitap, memory card).
    int32_t loadBuffer(uint32_t iopAddress);
}
