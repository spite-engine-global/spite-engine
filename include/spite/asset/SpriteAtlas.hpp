#pragma once
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "asset/AssetExport.hpp"
#include "asset/AssetManifest.hpp"
#include "base/Expected.hpp"
#include "base/InternedString.hpp"
#include "base/Platform.hpp"

namespace spite
{
    // 9-slice borders in source pixels; all zero = a plain sprite.
    struct SpriteInsets
    {
        u16 left = 0;
        u16 top = 0;
        u16 right = 0;
        u16 bottom = 0;

        bool any() const { return left || top || right || bottom; }
    };

    struct SpriteInfo
    {
        std::string name; // path under the atlas folder without extension, '/' separated
        u16 x = 0; // pixel rect inside the page, origin top-left, gutter excluded
        u16 y = 0;
        u16 width = 0;
        u16 height = 0;
        SpriteInsets slice;
    };

    // v counts from the top row of the page (page rows are stored top first, unlike baked model textures).
    struct SpriteUvRect
    {
        f32 u0 = 0.f;
        f32 v0 = 0.f;
        f32 u1 = 0.f;
        f32 v1 = 0.f;
    };

    // The CPU sprite table of one baked atlas; the page itself is the manifest texture `texture`.
    struct SpriteAtlas
    {
        InternedString name;
        InternedString texture;
        std::string sourceHash;
        u32 pageWidth = 0;
        u32 pageHeight = 0;
        std::vector<SpriteInfo> sprites; // sorted by name

        // nullptr when the atlas has no such sprite
        SPITE_ASSET_API const SpriteInfo* find(std::string_view spriteName) const;
        SPITE_ASSET_API SpriteUvRect uv(const SpriteInfo& sprite) const;
    };

    constexpr u32 SPRITE_ATLAS_VERSION = 1;
    constexpr u32 SPRITE_ATLAS_MAX_SPRITES = 4096;
    constexpr u32 SPRITE_ATLAS_DEFAULT_MAX_SIZE = 2048;
    constexpr u32 SPRITE_ATLAS_LIMIT_MAX_SIZE = 4096;

    // Reads the atlas table the manifest names; every error names the bake file.
    SPITE_ASSET_API Expected<std::shared_ptr<SpriteAtlas>> loadBakedSpriteAtlas(const InternedString& atlasName,
                                                                                const AssetManifest& manifest);
} // namespace spite
