#pragma once
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "base/MathTypes.hpp"
#include "rendering/GpuSharedTypes.hpp"

namespace spite
{
    constexpr u32 INVALID_GLOBAL_ADDRESS_SLOT = 0xFFFFFFFFu;
    constexpr u32 GLOBAL_ADDRESS_FIRST_RUNTIME_SLOT = GLOBAL_ADDRESS_BUILT_IN_SLOT_COUNT;

    static_assert(GLOBAL_ADDRESS_BUILT_IN_SLOT_COUNT <= GLOBAL_ADDRESS_SLOT_COUNT);

    // Palette delta records the SkinPaletteUpdateBuffer holds at first; it grows when a frame changes more joints
    constexpr u32 INITIAL_SKIN_UPDATE_RECORDS = 1024;

    struct ObjectStaticInfo
    {
        u32 firstMeshlet;
        u32 meshletCount;
    };

    // A material's class by its alpha mode and sidedness, as cull_meshlets reads it (bindless.slang materialClassOf):
    // the alpha, premultiplied and additive modes are the blend class; the mask class is the one drawn without
    // back-face culling, so a double-sided opaque material is in it too (its coverage test passes)
    constexpr u32 materialClassOf(const u32 flags)
    {
        const u32 mode = flags & MATERIAL_ALPHA_MODE_MASK;
        if (mode == 0u)
        {
            return (flags & MATERIAL_FLAG_DOUBLE_SIDED) != 0u ? MATERIAL_CLASS_MASK : MATERIAL_CLASS_OPAQUE;
        }
        return mode == MATERIAL_CLASS_MASK ? MATERIAL_CLASS_MASK : MATERIAL_CLASS_BLEND;
    }

    // The flag bits of a MaterialAlphaMode value (0 opaque, 1 mask, 2 blend, 3 premultiplied, 4 additive): glTF's three
    // keep their values in the alpha mode field, the two beyond them share its value 3 and the additive flag splits
    // them
    constexpr u32 materialAlphaFlags(const u32 alphaMode)
    {
        return alphaMode <= MATERIAL_ALPHA_BLEND ? alphaMode
            : alphaMode == 3u                    ? MATERIAL_ALPHA_EXTENDED_BLEND
                                                 : MATERIAL_ALPHA_EXTENDED_BLEND | MATERIAL_FLAG_ADDITIVE;
    }

    // Whether a material is a blended one that casts a shadow: the blend alpha mode with the flag, which
    // the premultiplied and additive modes (the same alpha field value) use as their additive split instead
    constexpr bool materialCastsBlendedShadow(const u32 flags)
    {
        return (flags & MATERIAL_ALPHA_MODE_MASK) == MATERIAL_ALPHA_BLEND && (flags & MATERIAL_FLAG_CASTS_SHADOW) != 0u;
    }

    // What a material draws into the shadow maps: opaque solid, a double-sided opaque material solid from both faces,
    // the mask class alpha-tested, a casting blended material dithered, and every other blended one nothing
    enum class ShadowCasting : u32
    {
        Solid,
        TwoSided,
        Mask,
        Dithered,
        None
    };
    constexpr ShadowCasting shadowCastingOf(const u32 flags)
    {
        const u32 materialClass = materialClassOf(flags);
        return materialClass == MATERIAL_CLASS_OPAQUE ? ShadowCasting::Solid
            : materialClass == MATERIAL_CLASS_MASK
            ? ((flags & MATERIAL_ALPHA_MODE_MASK) == 0u ? ShadowCasting::TwoSided : ShadowCasting::Mask)
            : materialCastsBlendedShadow(flags) ? ShadowCasting::Dithered
                                                : ShadowCasting::None;
    }

    // Whether an edit from one GpuMaterial to another changes what a material casts into the shadow maps
    // a switch between the five kinds does; within the mask class and within the dithered kind the
    // coverage inputs do (albedo alpha and texture, the uv transform and the albedo sampler, the double-sided flag, and
    // the mask's cutoff: the dither compares alpha alone); nothing else does (colour, roughness, emission).
    inline bool materialShadowDiffers(const GpuMaterial& before, const GpuMaterial& after)
    {
        const ShadowCasting kind = shadowCastingOf(after.flags);
        if (shadowCastingOf(before.flags) != kind)
        {
            return true;
        }
        if (kind != ShadowCasting::Mask && kind != ShadowCasting::Dithered)
        {
            return false;
        }
        constexpr u32 SAMPLER_KEY = ((1u << MATERIAL_SAMPLER_BITS) - 1u) << MATERIAL_SAMPLER_SHIFT; // the albedo slot's
        constexpr u32 FLAGS = MATERIAL_FLAG_DOUBLE_SIDED | MATERIAL_FLAG_UV_TRANSFORM | SAMPLER_KEY;
        return (kind == ShadowCasting::Mask && before.alphaCutoff != after.alphaCutoff) ||
            before.albedoFactor.w != after.albedoFactor.w || before.albedoTextureIndex != after.albedoTextureIndex ||
            ((before.flags ^ after.flags) & FLAGS) != 0 || before.uvOffset != after.uvOffset ||
            before.uvScale != after.uvScale || before.uvRotation != after.uvRotation;
    }

    // The registered materials of each class: the asset factory counts them (any thread) at registration, release and
    // edit, so a material still uploading counts already (a frame of routing it never needs is harmless; the reverse
    // would draw it opaque). The scene upload reads which non-opaque classes exist once a frame.
    struct MaterialClassCounts
    {
        std::atomic<u32> counts[3]{};
        // the blended materials that cast a shadow, a subset of the blend class
        std::atomic<u32> dithered{0};

        // one bit per non-opaque class: bit 0 the mask class, bit 1 the blend class, bit 2 a blended material that
        // casts a shadow (it implies bit 1). Mask and blend route to a list of their own on both raster paths, each
        // drawn there: the mask list by the visibility raster, the blend list by the Transparent pass;
        // the shadow instances draw both again, the blend list's casters dithered. What the
        // scene upload builds the render graph for.
        u32 present() const
        {
            const bool casting = dithered.load(std::memory_order_relaxed) != 0;
            return (counts[MATERIAL_CLASS_MASK].load(std::memory_order_relaxed) != 0 ? 1u : 0u) |
                (casting || counts[MATERIAL_CLASS_BLEND].load(std::memory_order_relaxed) != 0 ? 2u : 0u) |
                (casting ? 4u : 0u);
        }
        void add(const u32 flags)
        {
            counts[materialClassOf(flags)].fetch_add(1, std::memory_order_relaxed);
            dithered.fetch_add(materialCastsBlendedShadow(flags) ? 1u : 0u, std::memory_order_relaxed);
        }
        void remove(const u32 flags)
        {
            counts[materialClassOf(flags)].fetch_sub(1, std::memory_order_relaxed);
            dithered.fetch_sub(materialCastsBlendedShadow(flags) ? 1u : 0u, std::memory_order_relaxed);
        }

        // The edits of materials, by material id, for the scene upload's trackers: `shadowEdits` the ones that changed
        // what a material casts into the shadow maps (materialShadowDiffers; the caster tracker marks the maps that
        // hold its casters), `lightEdits` every one that changed the material at all (the probe volumes' tracker marks
        // the probes its renderables reach). Each is a log with a running count, so an idle frame reads one
        // atomic and each world drains the log from its own position.
        class EditLog
        {
        public:
            static constexpr u64 CAPACITY = 256;
            void note(const u32 materialId) const
            {
                std::lock_guard lock(m_mutex);
                m_log.push_back(materialId);
                if (m_log.size() > CAPACITY)
                {
                    m_log.erase(m_log.begin());
                }
                m_count.fetch_add(1, std::memory_order_release);
            }
            u64 count() const { return m_count.load(std::memory_order_acquire); }
            // The ids edited after the edit count `seen`, appended to out; false when the log has dropped some of them
            // (the caller then treats every material as edited). `seen` moves to the current count.
            bool since(u64& seen, std::vector<u32>& out) const
            {
                std::lock_guard lock(m_mutex);
                const u64 total = m_count.load(std::memory_order_relaxed);
                const u64 pending = total - seen;
                seen = total;
                if (pending > m_log.size())
                {
                    return false;
                }
                out.insert(out.end(), m_log.end() - static_cast<std::ptrdiff_t>(pending), m_log.end());
                return true;
            }

        private:
            mutable std::mutex m_mutex;
            mutable std::vector<u32> m_log;
            mutable std::atomic<u64> m_count{0};
        };
        EditLog shadowEdits;
        EditLog lightEdits;
    };
} // namespace spite
