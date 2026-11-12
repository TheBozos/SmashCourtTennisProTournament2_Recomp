#pragma once

#include "bozo/render/texture.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace sct2::textures
{
    // An image the game sends to the GS (BITBLTBUF, TRXPOS, TRXREG and the IMAGE data), kept where the GS would keep it so
    // that a texture drawn from it in another format reads the same bits. Only uploads write here; draws never do.
    void upload(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg, const uint8_t *data, size_t bytes);

    // The texture a TEX0 (and TEXA, for 24-bit texels) names, decoded to RGBA from those uploads.
    std::shared_ptr<const bozo::render::Texture> texture(uint64_t tex0, uint64_t texa);
}
