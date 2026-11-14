// A headless picture of the 2D the game published: the overlay's triangles drawn on the CPU (nearest texels, the GS's
// modulate and blend) into a PNG, to check a screen with no window. Not how the window draws it: the GPU does that.
#pragma once

#include "bozo/render/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace sct2::snapshot
{
    namespace detail
    {
        inline uint32_t crc(const uint8_t *p, size_t n, uint32_t c = 0xFFFFFFFFu)
        {
            for (size_t i = 0; i < n; ++i)
            {
                c ^= p[i];
                for (int k = 0; k < 8; ++k)
                    c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
            }
            return c;
        }

        inline void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
        {
            auto be32 = [&](uint32_t v) { for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s)); };
            be32(uint32_t(data.size()));
            const size_t start = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data.begin(), data.end());
            be32(crc(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu);
        }

        inline bool writePng(const std::string &path, uint32_t w, uint32_t h, const std::vector<uint32_t> &rgba)
        {
            std::vector<uint8_t> raw;
            for (uint32_t y = 0; y < h; ++y)
            {
                raw.push_back(0);
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint32_t c = rgba[size_t(y) * w + x];
                    raw.push_back(uint8_t(c)), raw.push_back(uint8_t(c >> 8)), raw.push_back(uint8_t(c >> 16));
                }
            }
            std::vector<uint8_t> z = {0x78, 0x01};
            uint32_t a = 1, b = 0;
            for (uint8_t v : raw)
                a = (a + v) % 65521, b = (b + a) % 65521;
            for (size_t at = 0; at < raw.size();)
            {
                const size_t n = std::min<size_t>(65535, raw.size() - at);
                z.push_back(at + n == raw.size());
                z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8)), z.push_back(uint8_t(~n)), z.push_back(uint8_t(~n >> 8));
                z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
                at += n;
            }
            for (int s = 24; s >= 0; s -= 8)
                z.push_back(uint8_t(((b << 16) | a) >> s));
            std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
            std::vector<uint8_t> ihdr;
            for (uint32_t v : {w, h})
                for (int s = 24; s >= 0; s -= 8)
                    ihdr.push_back(uint8_t(v >> s));
            ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
            chunk(out, "IHDR", ihdr);
            chunk(out, "IDAT", z);
            chunk(out, "IEND", {});
            FILE *f = std::fopen(path.c_str(), "wb");
            if (!f)
                return false;
            std::fwrite(out.data(), 1, out.size(), f);
            std::fclose(f);
            return true;
        }

        inline int channel(int v) { return std::clamp(v, 0, 255); }
    }

    // The frame and the overlay published last, drawn at the screen's 640 x 448 GS pixels (depth where a draw tests it).
    inline bool overlayPng(const std::string &path)
    {
        using namespace bozo::render;
        const uint32_t w = 640, h = 448;
        std::vector<uint32_t> fb(size_t(w) * h, 0);
        std::vector<uint8_t> fa(size_t(w) * h, 0);
        std::vector<double> zb(size_t(w) * h, 0.0);
        for (const auto &frame : {Renderer::instance().published(), Renderer::instance().publishedOverlay()})
        if (frame)
        {
            const Screen &s = frame->screen;
            for (const Renderer::Batch &bt : frame->batches)
            {
                const Material &m = bt.material;
                const Texture *t = m.textured ? bt.texture.get() : nullptr;
                for (uint32_t k = 0; k + 2 < bt.count; k += 3)
                {
                    const Vertex *v = &frame->vertices[bt.first + k];
                    float px[3], py[3];
                    for (int i = 0; i < 3; ++i)
                        px[i] = v[i].x - s.originX, py[i] = v[i].y - s.originY;
                    const float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
                    if (std::fabs(area) < 1e-6f)
                        continue;
                    const int x0 = std::max(0, int(std::floor(std::min({px[0], px[1], px[2]})))),
                              x1 = std::min(int(w) - 1, int(std::ceil(std::max({px[0], px[1], px[2]})))),
                              y0 = std::max(0, int(std::floor(std::min({py[0], py[1], py[2]})))),
                              y1 = std::min(int(h) - 1, int(std::ceil(std::max({py[0], py[1], py[2]}))));
                    for (int y = y0; y <= y1; ++y)
                        for (int x = x0; x <= x1; ++x)
                        {
                            const float cx = float(x), cy = float(y);
                            const float l1 = ((px[2] - px[0]) * (cy - py[0]) - (cx - px[0]) * (py[2] - py[0])) / -area;
                            const float l2 = ((px[1] - px[0]) * (cy - py[0]) - (cx - px[0]) * (py[1] - py[0])) / area;
                            const float l0 = 1.0f - l1 - l2;
                            if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f)
                                continue;
                            const float l[3] = {l0, l1, l2};
                            float c[4] = {}, u = 0, vv = 0;
                            const double z = double(v[0].z) + l1 * (double(v[1].z) - v[0].z) + l2 * (double(v[2].z) - v[0].z);
                            const size_t zat = size_t(y) * w + x;
                            if (m.depthTest && !(m.depthGreater ? z > zb[zat] : z >= zb[zat]))
                                continue;
                            for (int i = 0; i < 3; ++i)
                            {
                                for (int ch = 0; ch < 4; ++ch)
                                    c[ch] += l[i] * float((v[i].rgba >> (8 * ch)) & 0xFF);
                                u += l[i] * v[i].u, vv += l[i] * v[i].v;
                            }
                            int r = int(c[0]), g = int(c[1]), b = int(c[2]), a = int(c[3]);
                            if (t && t->width && t->height)
                            {
                                int tx = int(std::floor(u * t->width)), ty = int(std::floor(vv * t->height));
                                tx = m.clampU ? std::clamp(tx, 0, int(t->width) - 1) : int(unsigned(tx) % t->width);
                                ty = m.clampV ? std::clamp(ty, 0, int(t->height) - 1) : int(unsigned(ty) % t->height);
                                const uint32_t texel = t->rgba[size_t(ty) * t->width + tx];
                                const int tr = texel & 0xFF, tg = (texel >> 8) & 0xFF, tb = (texel >> 16) & 0xFF,
                                          ta = int(((texel >> 24) & 0xFF) * 128 / 255);
                                if (m.textureFunction == Material::TextureFunction::Decal)
                                    r = tr, g = tg, b = tb, a = m.textureAlpha ? ta : a;
                                else
                                {
                                    r = tr * r >> 7, g = tg * g >> 7, b = tb * b >> 7;
                                    a = m.textureAlpha ? ta * a >> 7 : a;
                                }
                            }
                            if (m.alphaTest)
                            {
                                bool pass = true;
                                switch (m.alphaFunc)
                                {
                                case Material::AlphaFunc::Never: pass = false; break;
                                case Material::AlphaFunc::Less: pass = a < m.alphaRef; break;
                                case Material::AlphaFunc::LessEqual: pass = a <= m.alphaRef; break;
                                case Material::AlphaFunc::Equal: pass = a == m.alphaRef; break;
                                case Material::AlphaFunc::GreaterEqual: pass = a >= m.alphaRef; break;
                                case Material::AlphaFunc::Greater: pass = a > m.alphaRef; break;
                                case Material::AlphaFunc::NotEqual: pass = a != m.alphaRef; break;
                                default: break;
                                }
                                if (!pass)
                                    continue;
                            }
                            const size_t at = size_t(y) * w + x;
                            const int dr = fb[at] & 0xFF, dg = (fb[at] >> 8) & 0xFF, db = (fb[at] >> 16) & 0xFF, da = fa[at];
                            if (m.gsBlend)
                            {
                                const int src[3] = {r, g, b}, dst[3] = {dr, dg, db};
                                const int cv = m.blendC == 0 ? a : m.blendC == 1 ? da : m.blendFix;
                                int out[3];
                                for (int ch = 0; ch < 3; ++ch)
                                {
                                    const int av = m.blendA == 0 ? src[ch] : m.blendA == 1 ? dst[ch] : 0;
                                    const int bv = m.blendB == 0 ? src[ch] : m.blendB == 1 ? dst[ch] : 0;
                                    const int dv = m.blendD == 0 ? src[ch] : m.blendD == 1 ? dst[ch] : 0;
                                    out[ch] = detail::channel(((av - bv) * cv >> 7) + dv);
                                }
                                r = out[0], g = out[1], b = out[2];
                            }
                            if (!(m.colourMask & 1)) r = dr;
                            if (!(m.colourMask & 2)) g = dg;
                            if (!(m.colourMask & 4)) b = db;
                            fb[at] = uint32_t(detail::channel(r)) | uint32_t(detail::channel(g)) << 8 | uint32_t(detail::channel(b)) << 16;
                            if (m.colourMask & 8)
                                fa[at] = uint8_t(detail::channel(a));
                            if (m.depthWrite)
                                zb[zat] = z;
                        }
                }
            }
        }
        return detail::writePng(path, w, h, fb);
    }
}
