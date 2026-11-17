#pragma once

#include "engine/EngineToolsExport.hpp"

namespace spite
{
    class ControlServer;
    class ServiceLocator;

    // Registers on control (owner "engine") the commands that inject device input into a running engine, for
    // play-testing a game and for testing the editor's own GUI:
    //   injectKey key= state=press|release, injectMouseButton [button=] state= [x= y=],
    //   injectMouseMove x= y=, injectScroll [x= y=], injectText text= [composition= cursor= selected=],
    //   injectAction action= state= [device=]  (presses or releases a keyboard, mouse or gamepad control the action
    //     is bound to: the first keyboard binding, else mouse, else gamepad; an axis action takes its first key or
    //     button), injectGamepad button=|axis=|connected= (the injected gamepad, a pad in the input core with no
    //     hardware behind it; it lives through the same path a real pad's snapshot takes), and the `gamepads` query
    //     (connected pads, buttons down, axis values)
    // Each request is one batch the application ingests in the next frame's event poll, through the layer SDL events
    // enter (InputInjector), so input contexts, the context priority floor and the UI's claims apply as to a real
    // device. A paused world (the world arg, the default one without) steps `frames` simulation frames in that frame:
    // default 1, so a stepped play world sees a press and its release in separate frames. The resolved frames and the
    // request go to that world's command log. The reply says what was queued (observations, frames, pending batches),
    // not that it landed: that is the next frame. Without an InputSystem and InputInjector in services (a tool, a
    // headless test) every command is refused.
    SPITE_ENGINE_TOOLS_API void registerInputCommands(ControlServer& control, ServiceLocator& services);
} // namespace spite
