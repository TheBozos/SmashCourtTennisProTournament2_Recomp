// Where the GS keeps a pixel of each storage format in its local memory: SCTPT2 uploads images in one format and draws
// them as another (a 4-bit texture sent as 32-bit words), so a texture is read back through both layouts.
#pragma once

#include <cstdint>

namespace sct2::gs
{
    enum Psm : uint32_t
    {
        kCt32 = 0x00,
        kCt24 = 0x01,
        kCt16 = 0x02,
        kCt16s = 0x0A,
        kT8 = 0x13,
        kT4 = 0x14,
        kT8h = 0x1B,
        kT4hl = 0x24,
        kT4hh = 0x2C,
    };

    // The 32-bit word holding (x, y) of a CT32-layout image at block `bp`, `bw` 64-pixel units wide.
    inline uint32_t wordCt32(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        const uint32_t page = (bp >> 5) + (y >> 5) * (bw ? bw : 1) + (x >> 6);
        const uint32_t bx = (x >> 3) & 7, by = (y >> 3) & 3;
        const uint32_t block = (bp & 31) + ((bx & 1) | ((by & 1) << 1) | (((bx >> 1) & 1) << 2) | ((by >> 1) << 3) | ((bx >> 2) << 4));
        const uint32_t cx = x & 7, cy = y & 7;
        const uint32_t column = (cy >> 1) * 16 + ((cx & 1) | ((cy & 1) << 1) | ((cx >> 1) << 2));
        return page * 2048 + block * 64 + column;
    }

    // The byte holding (x, y) of a T8 image.
    inline uint32_t byteT8(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        const uint32_t pages = (bw >> 1) ? (bw >> 1) : 1;
        const uint32_t page = (bp >> 5) + (y >> 6) * pages + (x >> 7);
        const uint32_t bx = (x >> 4) & 7, by = (y >> 4) & 3;
        const uint32_t block = (bp & 31) + ((bx & 1) | ((by & 1) << 1) | (((bx >> 1) & 1) << 2) | ((by >> 1) << 3) | ((bx >> 2) << 4));
        const uint32_t c = (y >> 2) & 3, yy = y & 3, xl = x & 7;
        const uint32_t xr = ((yy >> 1) != (c & 1)) ? (xl ^ 4) : xl;
        const uint32_t w = (xr >> 1) * 4 + (xr & 1) + (yy & 1) * 2;
        return page * 8192 + block * 256 + c * 64 + w * 4 + (yy >> 1) + ((x >> 3) & 1) * 2;
    }

    // The nibble holding (x, y) of a T4 image.
    inline uint32_t nibbleT4(uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        const uint32_t pages = (bw >> 1) ? (bw >> 1) : 1;
        const uint32_t page = (bp >> 5) + (y >> 7) * pages + (x >> 7);
        const uint32_t bx = (x >> 5) & 3, by = (y >> 4) & 7;
        const uint32_t block = (bp & 31) + ((by & 1) | ((bx & 1) << 1) | (((by >> 1) & 1) << 2) | ((bx >> 1) << 3) | ((by >> 2) << 4));
        const uint32_t c = (y >> 2) & 3, yy = y & 3, xl = x & 7;
        const uint32_t xr = ((yy >> 1) != (c & 1)) ? (xl ^ 4) : xl;
        const uint32_t w = (xr >> 1) * 4 + (xr & 1) + (yy & 1) * 2;
        return page * 16384 + block * 512 + c * 128 + w * 8 + 2 * ((x >> 3) & 3) + (yy >> 1);
    }
}
