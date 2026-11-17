#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "application/ApplicationEvents.hpp"
#include "application/EventBus.hpp"
#include "application/input/InputTypes.hpp"
#include "base/InternedString.hpp"
#include "engine/EngineExport.hpp"
#include "ui/UiInputSnapshot.hpp"
#include "ui/UiNavigation.hpp"

namespace spite
{
    class InputSystem;
    class UiContext;
    class UiRouter;
    class Window;
    class WindowService;
    struct RenderView;

    // A UI layer and where it is drawn: over the window (no view), or into a
    // render view's target shown at its windowRect, which maps the pointer into
    // the target's pixels (viewPointer).
    struct UiInputLayer
    {
        UiContext* ui = nullptr;
        const RenderView* view = nullptr;
    };

    // The application side of the UI router (Input router):
    // fills the UI input snapshot from the input core — pointer in framebuffer
    // pixels, button edges, wheel, the key transitions (repeats flagged, with
    // modifiers) since the last snapshot, the held modifiers and this frame's text — and hands the router's
    // verdicts back:
    // the text-input request, and its claimed controls, which no input
    // context resolves this frame. With UI layers, apply takes every router
    // that saw input this frame: the claims of all, text input if any wants it.
    //
    // Navigation intents come from input actions, so remapping applies: ui.up, ui.down, ui.left,
    // ui.right, ui.accept, ui.cancel, ui.nextTab, ui.previousTab, ui.nextLandmark and ui.previousLandmark, Axis
    // actions whose value says how far the control is pressed (a key or button 1, a stick the deflection past the
    // input deadzone; bindings of the opposite stick direction carry a negative scale). bindActions creates the
    // "ui" input context with the bindings of a mapping file (config/project/ui/navigation.json), whose actions are
    // the player's to rebind.
    // Each frame the intents become presses and held-direction repeats (UiNavRepeater, driven by the frame's
    // unscaled delta time, so a paused world still navigates and a fixed delta replays), and what the routers
    // acted on keeps its controls off the input contexts for as long as it is held. A frame in which no action is
    // active and no intent is held costs one flag test.
    class SPITE_ENGINE_API UiInputAdapter
    {
    public:
        explicit UiInputAdapter(ApplicationEventBus& events);
        ~UiInputAdapter();
        UiInputAdapter(const UiInputAdapter&) = delete;
        UiInputAdapter& operator=(const UiInputAdapter&) = delete;

        // Creates the "ui" input context at `priority` with the bindings of the mapping file; false (and no
        // intents ever) when the file does not load. The context goes with the adapter.
        bool bindActions(InputSystem& input, const std::filesystem::path& mappingsFile, i32 priority);
        [[nodiscard]] static InternedString intentAction(UiNavIntent intent);

        // Valid until the next call. `deltaTime`: the frame's seconds, which advance held-direction repeats.
        [[nodiscard]] const UiInputSnapshot& snapshot(const InputSystem& input, const Window* window,
                                                      float deltaTime = 0.0f);
        // the application's windows; a pointer in one beside `window` reaches the UI in `window`'s pixels
        // past its edges (by the windows' desktop positions) with UiInputSnapshot::pointerWindow set.
        void setWindowService(const WindowService* windows) { m_windows = windows; }
        static void apply(InputSystem& input, std::span<const UiRouter* const> routers);

        // One frame for UI layers stacked over the window, topmost first: each
        // processes what the layers above left over (inputBelow), then the
        // claims of all are applied. A layer drawn into a view gets the pointer
        // in the view's pixels, and none outside the view unless it holds a press.
        void route(InputSystem& input, const Window* window, std::span<const UiInputLayer> layers,
                   float deltaTime = 0.0f);

    private:
        void stepNavigation(const InputSystem& input, float deltaTime);
        void claimNavigation(InputSystem& input);

        ApplicationEventBus::Subscription m_keys;
        std::vector<UiKeyTransition> m_pendingKeys;
        std::vector<UiKeyTransition> m_frameKeys;
        std::string m_text;
        UiInputSnapshot m_snapshot;
        const WindowService* m_windows = nullptr;
        std::vector<std::vector<UiKeyTransition>> m_layerKeys;
        std::vector<std::vector<UiNavTransition>> m_layerNav;
        UiNavRepeater m_repeater;
        std::vector<UiNavTransition> m_nav;
        std::vector<InputControl> m_controls; // scratch
        InputSystem* m_actionsInput = nullptr;
        InputContextId m_actionsContext = INVALID_INPUT_CONTEXT_ID;
        std::vector<const UiRouter*> m_routers;
    };
} // namespace spite
