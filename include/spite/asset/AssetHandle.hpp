#pragma once
#include "asset/AssetData.hpp"
#include "base/HashCombine.hpp"
#include "base/InternedString.hpp"
#include "base/Platform.hpp"
#include "base/Schema.hpp"

namespace spite
{
    struct AssetHandle
    {
        InternedString name;
        AssetType assetType = AssetType::UNKNOWN;
        u32 subAssetIndex = 0;

        SPITE_SCHEMA(AssetHandle, name, assetType, subAssetIndex)

        bool isValid() const { return name.isValid(); }

        auto operator<=>(const AssetHandle& other) const = default;

        struct Hash
        {
            sizet operator()(const AssetHandle& handle) const
            {
                sizet seed = 0;
                hashCombine(seed, handle.name, static_cast<u8>(handle.assetType), handle.subAssetIndex);
                return seed;
            }
        };
    };

    inline AssetHandle textureAssetHandle(const InternedString name, const TextureColorSpace space)
    {
        return AssetHandle{name, AssetType::TEXTURE, static_cast<u32>(space)};
    }
} // namespace spite
