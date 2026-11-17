#pragma once
#include <algorithm>
#include <cmath>

#include "base/FloatPacking.hpp"
#include "base/MathFunctions.hpp"
#include "base/MathTypes.hpp"

// The uv range of a submesh. Vertex::packedUv is two unorm16 fields, so a uv outside [0, 1] (a tiled
// glTF floor) needs a range the unorm spans: uv = offset + unorm * extent. The range is per submesh, in the baked TOC
// entry, in the object's record on the GPU (SceneObject::packedUvOffset and packedUvExtent) and read by every shader
// that unpacks a vertex uv. Each axis is a pair of binary16 values, the offset itself and the extent minus 1: the zero
// word of an old file or an unset record is the range [0, 1], which unpacks exactly like a plain unorm. A mesh whose
// uvs stay in [0, 1] always has that range, so its bytes and its pixels do not change. An axis out of [0, 1] takes
// the tight span of its uvs, so the step is extent / 65535 (a texel error of extent / 32 per 2k texture: 0.25 texel
// for a span of 8, one texel at 32); a span over what binary16 holds (65504) clamps to it.
// These compile with precise floats like FloatPacking.hpp: baked bytes come from them.
#pragma float_control(precise, on, push)
#pragma fp_contract(off)

namespace spite
{
    struct UvRange
    {
        u32 packedOffset = 0; // binary16 pair (u in the low half): the uv of unorm 0
        u32 packedExtent = 0; // binary16 pair: the span minus 1

        bool operator==(const UvRange&) const = default;
        bool isIdentity() const { return packedOffset == 0 && packedExtent == 0; }
    };

    // The span over which a mesh's uv precision stops being a fraction of a texel on a 2k texture (extent / 32 texels
    // is the step): the importer warns past it.
    constexpr f32 UV_RANGE_WARN_EXTENT = 32.0f;

    inline Vec2 uvRangeOffset(const UvRange& range)
    {
        return Vec2(halfBitsToFloat(static_cast<u16>(range.packedOffset & 0xFFFFu)),
                    halfBitsToFloat(static_cast<u16>(range.packedOffset >> 16)));
    }

    inline Vec2 uvRangeExtent(const UvRange& range)
    {
        return Vec2(1.0f + halfBitsToFloat(static_cast<u16>(range.packedExtent & 0xFFFFu)),
                    1.0f + halfBitsToFloat(static_cast<u16>(range.packedExtent >> 16)));
    }

    namespace uvrange
    {
        // the nearest binary16 at or below (above) x, saturating at +-65504: the nearest one, stepped one place when
        // it lies on the wrong side (a half's bits order its magnitude, the sign bit flips the direction)
        inline u16 halfBelow(const f32 x)
        {
            u16 bits = floatToHalfBits(x);
            if (halfBitsToFloat(bits) > x)
            {
                bits = (bits & 0x8000u) ? static_cast<u16>(std::min<u32>(bits + 1u, 0xFBFFu))
                                        : static_cast<u16>(bits - 1u);
            }
            return bits;
        }

        inline u16 halfAbove(const f32 x)
        {
            u16 bits = floatToHalfBits(x);
            if (halfBitsToFloat(bits) < x)
            {
                bits = (bits & 0x8000u) ? static_cast<u16>(bits - 1u)
                                        : static_cast<u16>(std::min<u32>(bits + 1u, 0x7BFFu));
            }
            return bits;
        }

        // the offset and extent-minus-1 words of one axis holding [lo, hi]
        inline void axis(const f32 lo, const f32 hi, u16& offset, u16& extentLess)
        {
            if (lo >= 0.0f && hi <= 1.0f)
            {
                offset = 0;
                extentLess = 0;
                return;
            }
            offset = halfBelow(lo);
            extentLess = halfAbove(std::max(hi - halfBitsToFloat(offset), 0.0f) - 1.0f);
        }
    } // namespace uvrange

    // The range of a mesh whose uvs span [lo, hi] on each axis. lo > hi (no uv at all) is the identity range.
    inline UvRange makeUvRange(const Vec2& lo, const Vec2& hi)
    {
        u16 offsetX = 0, offsetY = 0, extentX = 0, extentY = 0;
        if (lo.x <= hi.x)
        {
            uvrange::axis(lo.x, hi.x, offsetX, extentX);
        }
        if (lo.y <= hi.y)
        {
            uvrange::axis(lo.y, hi.y, offsetY, extentY);
        }
        return {static_cast<u32>(offsetX) | (static_cast<u32>(offsetY) << 16),
                static_cast<u32>(extentX) | (static_cast<u32>(extentY) << 16)};
    }

    // Vertex::packedUv of a uv inside the range (one outside clamps to its edge). The identity range packs exactly
    // like packUnorm2x16.
    inline u32 packUv(const Vec2& uv, const UvRange& range)
    {
        const Vec2 offset = uvRangeOffset(range);
        const Vec2 extent = uvRangeExtent(range);
        return packUnorm2x16(Vec2(extent.x > 0.0f ? (uv.x - offset.x) / extent.x : 0.0f,
                                  extent.y > 0.0f ? (uv.y - offset.y) / extent.y : 0.0f));
    }

    // The uv a packed one stands for: the arithmetic of unpackVertexUv in shared/bindless.slang
    inline Vec2 unpackUv(const u32 packedUv, const UvRange& range)
    {
        const Vec2 unorm = unpackUnorm2x16(packedUv);
        const Vec2 offset = uvRangeOffset(range);
        const Vec2 extent = uvRangeExtent(range);
        return Vec2(offset.x + unorm.x * extent.x, offset.y + unorm.y * extent.y);
    }
} // namespace spite

#pragma float_control(pop)
