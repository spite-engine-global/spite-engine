#pragma once
#include <algorithm>
#include <bit>

#include "base/Platform.hpp"

// Encoders and decoders for the engine's 16-bit and packed HDR texel formats. Baked bytes come from these, so they
// compile with precise, uncontracted floats (the release build is /fp:fast) and only scale by powers of two, add and
// round (a rounding that must follow the specification's tie rule is done in double): a bake is the same in every
// configuration. Negative and NaN inputs encode as zero, large ones saturate
// at the format's largest finite value (an HDR sun must not turn into infinity on the way to the GPU).
#pragma float_control(precise, on, push)
#pragma fp_contract(off)

namespace spite
{
    namespace packing
    {
        // x >= 0 as a float with a 5-bit exponent (bias 15) and `mantissaBits` mantissa bits, rounded to nearest
        // even, subnormals included; saturates at the largest finite value (65504 for 10 mantissa bits).
        inline u32 toUnsignedSmallFloat(float x, const u32 mantissaBits)
        {
            const u32 maxFinite = (30u << mantissaBits) | ((1u << mantissaBits) - 1u);
            if (!(x > 0.0f))
            {
                return 0;
            }
            // below the smallest normal (2^-14): a multiple of the subnormal step 2^(-14 - mantissaBits)
            if (x < 6.103515625e-5f)
            {
                const float scaled = x * static_cast<float>(1u << (14 + mantissaBits)) + 8388608.0f;
                return std::bit_cast<u32>(scaled) & 0x7FFFFFu;
            }
            if (x >= 131072.0f)
            {
                return maxFinite;
            }
            const u32 shift = 23 - mantissaBits;
            const u32 rebiased = std::bit_cast<u32>(x) - ((127u - 15u) << 23);
            const u32 rounded = (rebiased + ((1u << (shift - 1)) - 1u) + ((rebiased >> shift) & 1u)) >> shift;
            return std::min(rounded, maxFinite);
        }

        inline float fromUnsignedSmallFloat(const u32 bits, const u32 mantissaBits)
        {
            const u32 exponent = bits >> mantissaBits;
            const u32 mantissa = bits & ((1u << mantissaBits) - 1u);
            if (exponent == 0)
            {
                return static_cast<float>(mantissa) * (1.0f / static_cast<float>(1u << (14 + mantissaBits)));
            }
            if (exponent == 31)
            {
                return std::bit_cast<float>(0x7F800000u | (mantissa << (23 - mantissaBits)));
            }
            return std::bit_cast<float>(((exponent + 112u) << 23) | (mantissa << (23 - mantissaBits)));
        }
    } // namespace packing

    // IEEE binary16, saturating at +-65504.
    inline u16 floatToHalfBits(const float x)
    {
        return static_cast<u16>(x < 0.0f ? 0x8000u | packing::toUnsignedSmallFloat(-x, 10)
                                         : packing::toUnsignedSmallFloat(x, 10));
    }

    inline float halfBitsToFloat(const u16 half)
    {
        const float magnitude = packing::fromUnsignedSmallFloat(half & 0x7FFFu, 10);
        return (half & 0x8000u) ? -magnitude : magnitude;
    }

    // VK_FORMAT_B10G11R11_UFLOAT_PACK32: R in bits 0-10, G in 11-21, B (10 bits) in 22-31; unsigned 5-bit exponent.
    inline u32 packB10G11R11(const float r, const float g, const float b)
    {
        return packing::toUnsignedSmallFloat(r, 6) | (packing::toUnsignedSmallFloat(g, 6) << 11) |
            (packing::toUnsignedSmallFloat(b, 5) << 22);
    }

    inline void unpackB10G11R11(const u32 texel, float& r, float& g, float& b)
    {
        r = packing::fromUnsignedSmallFloat(texel & 0x7FFu, 6);
        g = packing::fromUnsignedSmallFloat((texel >> 11) & 0x7FFu, 6);
        b = packing::fromUnsignedSmallFloat(texel >> 22, 5);
    }

    // VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: R in bits 0-8, G in 9-17, B in 18-26, a shared exponent (bias 15) in 27-31.
    // The shared exponent follows the largest channel, so the smaller ones keep fewer bits
    // (EXT_texture_shared_exponent).
    inline u32 packE5B9G9R9(float r, float g, float b)
    {
        // 511 / 512 * 2^16
        const auto clamp = [](const float v) { return v > 0.0f ? std::min(v, 65408.0f) : 0.0f; };
        r = clamp(r);
        g = clamp(g);
        b = clamp(b);
        const float largest = std::max({r, g, b});
        // floor(log2(largest)) from the float's exponent field; at least -16 so the exponent stays non-negative
        const i32 floorLog2 = largest > 0.0f ? static_cast<i32>(std::bit_cast<u32>(largest) >> 23) - 127 : -16;
        i32 exponent = std::max(-16, floorLog2) + 16;
        // the step between mantissa values: 2^(exponent - 15 - 9)
        float step = std::bit_cast<float>(static_cast<u32>(exponent - 24 + 127) << 23);
        // floor(x / step + 0.5) of the specification, in double: step is a power of two, so the quotient is exact and
        // the sum is too (a float sum rounds a value just under a tie up to it)
        const auto rounded = [](const float v, const float stepOf)
        { return static_cast<u32>(static_cast<double>(v) / stepOf + 0.5); };
        if (rounded(largest, step) == 512u)
        {
            step *= 2.0f;
            ++exponent;
        }
        return rounded(r, step) | (rounded(g, step) << 9) | (rounded(b, step) << 18) |
            (static_cast<u32>(exponent) << 27);
    }

    inline void unpackE5B9G9R9(const u32 texel, float& r, float& g, float& b)
    {
        const float step = std::bit_cast<float>(((texel >> 27) + 127u - 24u) << 23);
        r = static_cast<float>(texel & 0x1FFu) * step;
        g = static_cast<float>((texel >> 9) & 0x1FFu) * step;
        b = static_cast<float>((texel >> 18) & 0x1FFu) * step;
    }
} // namespace spite

#pragma float_control(pop)
