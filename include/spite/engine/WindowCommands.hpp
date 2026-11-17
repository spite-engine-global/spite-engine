#pragma once

#include <array>
#include <string>
#include <string_view>

#include "base/Platform.hpp"
#include "engine/EngineToolsExport.hpp"

namespace spite
{
    class ControlServer;
    class ServiceLocator;
    class Window;

    // Registers on control (owner "engine"):
    //   window query: state, fullscreen, focused, size, framebuffer, restoreSize
    //     (the windowed size it restores to and reopens at), position,
    //     displayScale and, with an IRenderer in services, the swapchain extent
    //     and its recreation count, and gpuHang while a frame fence hangs (IRenderer::gpuHang)
    //     (displayScale is the effective one: hostDisplayScale is the display's, displayScaleInjected whether
    //     injectDisplayScale stands in for it)
    //   setWindow [state=normal|minimized|maximized] [fullscreen=bool] [width height] -> queued:
    //     window commands, applied at the next frame start
    //   injectDisplayScale scale -> displayScale, hostDisplayScale, injected: a window moved to another monitor's
    //     density without one (0 clears; a non-finite or negative value is refused, others clamp to [0.25, 8])
    inline constexpr std::array<std::string_view, 3> WINDOW_VERBS{"window", "setWindow", "injectDisplayScale"};
    SPITE_ENGINE_TOOLS_API void registerWindowCommands(ControlServer& control, Window& window,
                                                       ServiceLocator& services);
    // Why no frame renders now, empty while frames render (and headless: no window): a minimized window renders
    // nothing, so a frame readback (screenshot, gpuBuffer, capture) refuses at once instead of waiting out its frame
    // timeout; a GPU hang (an IRenderer in services reporting gpuHang) renders nothing either.
    [[nodiscard]] SPITE_ENGINE_TOOLS_API std::string noFrameReason(const Window* window, ServiceLocator& services);

    // The default file name of a screenshot or capture, <stem>_<local second>_p<process id>_<number><extension>: the
    // captures root is shared by every run on the machine, so a per-run counter alone would let two runs capturing in
    // one second write one name.
    [[nodiscard]] SPITE_ENGINE_TOOLS_API std::string defaultReadbackName(std::string_view stem,
                                                                         std::string_view extension, u64 number);
} // namespace spite
