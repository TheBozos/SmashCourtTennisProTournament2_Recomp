// What sct2-headless and sct2 share: the port's install, the generated code, the overlays and the IOP modules.
#pragma once

#include "sct2_audio.h"
#include "sct2_functions.h"
#include "sct2_overlays.h"

#include "bozo/ee/dispatcher.h"
#include "bozo/host/free_host.h"
#include "bozo/port.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace sct2
{
    constexpr uint64_t kFramePeriodCycles = bozo::port::kEeClockHz * 1001u / 60000u;

    inline bool installPort(const char *program, bozo::port::Registry &registry)
    {
        bozo::port::port()->install(registry);
        const std::vector<uint32_t> missing = bozo::port::undeclared(registry, sct2_external_table, sct2_external_count);
        if (missing.empty())
            return true;
        std::fprintf(stderr, "%s: %zu registered overrides are not externals of the generated code (regenerate):", program,
                     missing.size());
        for (uint32_t a : missing)
            std::fprintf(stderr, " 0x%x", a);
        std::fprintf(stderr, "\n");
        return false;
    }

    inline bool setUp(const char *program, bozo::host::FreeHost &host, bozo::ee::Dispatcher &d, bozo::port::Registry &registry)
    {
        d.setFunctions(sct2_function_table, sct2_function_count, sct2_resume_table, sct2_resume_count);
        std::string error;
        if (!host.setUp(d, sct2_external_table, sct2_external_count, &registry, error))
        {
            std::fprintf(stderr, "%s: %s\n", program, error.c_str());
            return false;
        }
        overlays::attach(d.runtime());
        audio::attach(host.libraries());
        return true;
    }

    // The disc image: SCT2_ISO, else the one .iso beside the ELF.
    inline std::string findDiscImage(const std::string &elf)
    {
        if (const char *env = std::getenv("SCT2_ISO"))
            return env;
        std::error_code ec;
        std::string found;
        for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::path(elf).parent_path(), ec))
            if (entry.path().extension() == ".iso")
            {
                if (!found.empty())
                    return {};
                found = entry.path().string();
            }
        return found;
    }
}
