#include "sct2_audio.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <string>

namespace sct2::audio
{
    namespace
    {
        constexpr uint32_t kHeapStart = 0x00034000u;
        constexpr uint32_t kElfHeaderBytes = 0x34u;
        constexpr uint32_t kSectionHeaderBytes = 0x28u;
        constexpr uint32_t kIopModSection = 0x70000080u;
        constexpr uint32_t kIopModNameOffset = 0x1Au;

        std::unique_ptr<bozo::native::irx::CompiledLibraries> g_host;
        std::vector<std::string> g_names;
        int32_t g_others = 0;

        uint32_t word(std::span<const uint8_t> bytes, uint32_t offset)
        {
            uint32_t value = 0;
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            return value;
        }

        uint16_t half(std::span<const uint8_t> bytes, uint32_t offset)
        {
            uint16_t value = 0;
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            return value;
        }

        std::string iopModName(uint32_t iopAddress)
        {
            const std::span<const uint8_t> header = g_host->allocatedBuffer(iopAddress, kElfHeaderBytes);
            if (header.size() < kElfHeaderBytes || std::memcmp(header.data(), "\x7F" "ELF", 4) != 0)
                return {};
            const uint32_t shoff = word(header, 0x20);
            const uint16_t shnum = half(header, 0x30);
            const std::span<const uint8_t> sections = g_host->allocatedBuffer(iopAddress, shoff + shnum * kSectionHeaderBytes);
            for (uint16_t k = 0; k < sections.size() / kSectionHeaderBytes && k < shnum; ++k)
            {
                const uint32_t at = shoff + k * kSectionHeaderBytes;
                if (word(sections, at + 4) != kIopModSection)
                    continue;
                const uint32_t offset = word(sections, at + 16), size = word(sections, at + 20);
                const std::span<const uint8_t> iopmod = g_host->allocatedBuffer(iopAddress, offset + size);
                if (iopmod.size() < offset + size || size <= kIopModNameOffset)
                    return {};
                const char *name = reinterpret_cast<const char *>(iopmod.data() + offset + kIopModNameOffset);
                return std::string(name, strnlen(name, size - kIopModNameOffset));
            }
            return {};
        }
    }

    void attach(bozo::libs::Libraries &libraries)
    {
        g_host = std::make_unique<bozo::native::irx::CompiledLibraries>(libraries, kHeapStart);
        g_host->useNativeDiscFiles();
        g_names.clear();
        for (auto &image : images())
        {
            g_names.push_back(image.name);
            g_host->addImage(std::move(image));
        }
        auto *h = g_host.get();
        libraries.sif.transfer = [h](uint32_t address, const uint8_t *data, uint32_t bytes) { h->writeBuffer(address, data, bytes); };
        libraries.sif.allocateBuffer = [h](uint32_t bytes) { return h->allocateBuffer(bytes); };
        libraries.sif.freeBuffer = [h](uint32_t address) { return h->freeBuffer(address); };
        libraries.sif.resetProvider = [h] { h->reset(); };
        libraries.audioProvider = [h](uint64_t cycle) { return h->advanceTo(cycle); };
    }

    int32_t loadBuffer(uint32_t iopAddress)
    {
        const std::string image = iopModName(iopAddress) + ".IRX;1";
        if (std::find(g_names.begin(), g_names.end(), image) != g_names.end())
            return g_host->startImage(image);
        return 0x1000 + ++g_others;
    }
}
