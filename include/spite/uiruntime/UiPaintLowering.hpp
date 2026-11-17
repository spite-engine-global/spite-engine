#pragma once

#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "uiruntime/UiRuntimeExport.hpp"
#include "uiview/UiFrame.hpp"

namespace spite
{
    struct UiLowerTexture
    {
        std::optional<u32> bindlessIndex;
        std::optional<UiSpriteRect> sprite;
        bool waitForLoad = false;
    };

    struct UiPreparedGlyph
    {
        UiRect rect;
        Vec2 uvMin;
        Vec2 uvMax;
        UiTextureId texture = NO_UI_TEXTURE;
        u32 color = 0;
        float pixelRange = 0.0f;
        bool sprite = false;
    };

    struct UiPreparedTextRun
    {
        u32 first = 0;
        u32 end = 0;
        u32 waitFirst = 0;
        u32 waitEnd = 0;
    };

    struct UiPaintWait
    {
        u64 element = 0;
        u64 key = 0;
    };

    struct UiPreparedPaint
    {
        std::vector<UiLowerTexture> textures;
        std::unordered_map<UiTextureId, u32> textureLookup;
        UiTextureId lastTexture = NO_UI_TEXTURE;
        u32 lastTextureIndex = 0;
        std::vector<u32> imageTextures;
        std::vector<UiPreparedGlyph> glyphs;
        std::vector<UiPreparedTextRun> textRuns;
        std::vector<u8> validSeries;
        std::vector<UiPaintWait> textWaits;
        std::vector<UiPaintWait> waits;
    };

    SPITE_UI_RUNTIME_API void lowerUiPaint(std::span<const UiPaintRecord> records, std::span<const u32> recordEnds,
                                           UiPreparedPaint& prepared, std::vector<UiInstance>& instances,
                                           std::vector<u32>& instanceEnds);
} // namespace spite
