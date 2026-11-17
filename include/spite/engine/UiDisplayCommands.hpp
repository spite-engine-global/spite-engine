#pragma once

#include <functional>

#include "base/Platform.hpp"
#include "engine/EngineToolsExport.hpp"
#include "engine/UiInspectCommands.hpp"

namespace spite
{
    class ControlServer;

    // The user's UI display settings, shared by every UI of the engine; the engine applies a change at
    // the next update, once for all the UIs. `uiScale` multiplies the window's display scale into the layout scale
    // (every length and the text), `textScale` only the text sizes (the stylesheet's setTextScale), `reducedMotion`
    // the tweens' switch. `applied` counts the updates that applied a change.
    struct UiDisplaySettings
    {
        static constexpr float MIN_UI_SCALE = 0.5f;
        static constexpr float MAX_UI_SCALE = 4.0f;
        float uiScale = 1.0f;
        float textScale = 1.0f;
        bool reducedMotion = false;
        float headlessDisplayScale = 1.0f; // the display scale of an engine without a window (a test seam)
        u32 applied = 0;
    };

    // Registers on control (owner "engine"):
    //   uiDisplay [uiScale] [textScale] [reducedMotion] [world] -> displayScale (the window's, 1 headless), uiScale,
    //     textScale, reducedMotion (the settings), layoutScale (the scale `world`'s UI laid out at: displayScale
    //     times uiScale once applied), applied (updates that re-laid out), activeTweens. A non-finite or
    //     non-positive scale is refused; others clamp (uiScale to [0.5, 4], textScale to [0.5, 4]). The settings
    //     apply to every UI at the next update, in one re-layout.
    SPITE_ENGINE_TOOLS_API void registerUiDisplayCommands(ControlServer& control, UiDisplaySettings& settings,
                                                          std::function<float()> displayScale, UiOfFn uiOf);
} // namespace spite
