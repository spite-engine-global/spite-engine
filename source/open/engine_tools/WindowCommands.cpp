#include "engine/WindowCommands.hpp"
#include "platform/ProcessId.hpp"


#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include "control/NumberArg.hpp"
#include "application/Window.hpp"
#include "base/ServiceLocator.hpp"
#include "control/ControlServer.hpp"
#include "rendering/IRenderer.hpp"
#include "serialization/FormatWalk.hpp"

namespace spite
{
    namespace
    {
        std::string_view stateName(const WindowState state)
        {
            switch (state)
            {
            case WindowState::Minimized:
                return "minimized";
            case WindowState::Maximized:
                return "maximized";
            case WindowState::Hidden:
                return "hidden";
            case WindowState::Normal:
            default:
                return "normal";
            }
        }

        constexpr u64 MAX_EXTENT = 16384;
        // pumps after setWindow's: the window polls its commands, then the swapchain follows the new size
        constexpr u32 SETTLE_FRAMES = 2;

        constexpr std::string_view REQUESTABLE_STATES[] = {"normal", "minimized", "maximized"};

        // the states setWindow can request
        std::optional<WindowState> parseState(const std::string_view name)
        {
            for (const WindowState state : {WindowState::Normal, WindowState::Minimized, WindowState::Maximized})
            {
                if (stateName(state) == name)
                {
                    return state;
                }
            }
            return std::nullopt;
        }

        void writeExtent(IFormatWriter& out, const std::string_view name, const u32 width, const u32 height)
        {
            out.beginObject(name);
            out.writeU32("width", width);
            out.writeU32("height", height);
            out.endObject();
        }
    } // namespace

    std::string noFrameReason(const Window* window, ServiceLocator& services)
    {
        if (window && window->isMinimized())
        {
            return "the window is minimized: no frame renders until it is restored (setWindow state=normal)";
        }
        const GpuHang* hang = services.contains<IRenderer>() ? services.read<IRenderer>()->gpuHang() : nullptr;
        if (!hang)
        {
            return {};
        }
        return std::format("the GPU hangs: frame {} (frame slot {}) has not finished in {} ms, so no frame renders; "
                           "the run ends with a crash dump at {} ms (0: never, graphics.gpuHangFatalMs)",
                           hang->frame, hang->slot, hang->waitedMs, hang->fatalAfterMs);
    }

    std::string defaultReadbackName(const std::string_view stem, const std::string_view extension, const u64 number)
    {
        const auto now = std::chrono::floor<std::chrono::seconds>(
            std::chrono::current_zone()->to_local(std::chrono::system_clock::now()));
        return std::format("{}_{:%Y-%m-%d_%H-%M-%S}_p{}_{}{}", stem, now, spite::platform::processId(), number,
                           extension);
    }

    void registerWindowCommands(ControlServer& control, Window& window, ServiceLocator& services)
    {
        control.registerQuery(
            WINDOW_VERBS[0], "engine",
            "the main window: state, fullscreen, sizes, the swapchain extent and its present mode (fifo while vsync is "
            "on); gpuHang (slot, frame, lastCompletedFrame, waitedMs, fatalAfterMs) while a frame fence has not "
            "signaled in graphics.gpuHangTimeoutMs and frames are skipped",
            {},
            [&window, &services](const ControlArgs&, IFormatWriter& out)
            {
                out.writeString("state", stateName(window.getState()));
                out.writeBool("fullscreen", window.isFullscreen());
                out.writeBool("focused", window.hasFocus());
                writeExtent(out, "size", window.getLogicalExtent().width, window.getLogicalExtent().height);
                writeExtent(out, "framebuffer", window.getFramebufferExtent().width,
                            window.getFramebufferExtent().height);
                writeExtent(out, "restoreSize", window.getConfig().width, window.getConfig().height);
                out.beginObject("position");
                out.writeI32("x", window.getPosition().x);
                out.writeI32("y", window.getPosition().y);
                out.endObject();
                out.writeF32("displayScale", window.getDisplayScale());
                out.writeF32("hostDisplayScale", window.getHostDisplayScale());
                out.writeBool("displayScaleInjected", window.isDisplayScaleInjected());
                if (services.contains<IRenderer>())
                {
                    const auto renderer = services.read<IRenderer>();
                    const Extent2D swapchain = renderer->getSwapchainExtent();
                    writeExtent(out, "swapchain", swapchain.width, swapchain.height);
                    out.writeU32("swapchainRecreations", renderer->getSwapchainRecreateCount());
                    out.writeString("presentMode", presentModeName(renderer->presentMode()));
                    if (const GpuHang* hang = renderer->gpuHang())
                    {
                        out.beginObject("gpuHang");
                        out.writeU32("slot", hang->slot);
                        out.writeU64("frame", hang->frame);
                        out.writeU64("lastCompletedFrame", hang->lastCompletedFrame);
                        out.writeU64("waitedMs", hang->waitedMs);
                        out.writeU64("fatalAfterMs", hang->fatalAfterMs);
                        out.endObject();
                    }
                }
                return std::string{};
            });
        control.registerViewCommand(
            WINDOW_VERBS[1], "engine",
            "changes the main window's state, fullscreen flag or size; the reply comes two frames later (once the OS "
            "applied it and the swapchain followed) with the state, fullscreen and size then, and settled: true",
            {optionalArg("state", ArgKind::String, "normal, minimized or maximized", {}, REQUESTABLE_STATES),
             optionalArg("fullscreen", ArgKind::Boolean, "true covers the display, false returns to the window"),
             optionalArg("width", ArgKind::Integer, "the windowed size to resize to (with height)"),
             optionalArg("height", ArgKind::Integer, "the windowed size to resize to (with width)")},
            [&control, &window](const ControlArgs& args, IFormatWriter& out)
            {
                const std::optional<std::string> stateArg = args.string("state");
                const std::optional<bool> fullscreen = args.boolean("fullscreen");
                const std::optional<u64> width = args.integer("width");
                const std::optional<u64> height = args.integer("height");
                if (!stateArg && !fullscreen && !width && !height)
                {
                    return std::string("setWindow needs state, fullscreen or width and height");
                }
                if (width.has_value() != height.has_value() || (width && (*width == 0 || *width > MAX_EXTENT)) ||
                    (height && (*height == 0 || *height > MAX_EXTENT)))
                {
                    return std::format("setWindow needs both width and height, each 1..{}", MAX_EXTENT);
                }
                if (width)
                {
                    window.setExtent(static_cast<u32>(*width), static_cast<u32>(*height));
                }
                const std::optional<WindowState> state = stateArg ? parseState(*stateArg) : std::nullopt;
                if (state)
                {
                    window.setState(*state);
                }
                if (fullscreen)
                {
                    window.setFullscreen(*fullscreen);
                }
                // the window applies its queued commands in the next frames: answer once they ran, with what it is then
                return control.replyAfterFrames(
                    SETTLE_FRAMES,
                    [&window](IFormatWriter& result)
                    {
                        result.writeString("state", stateName(window.getState()));
                        result.writeBool("fullscreen", window.isFullscreen());
                        writeExtent(result, "size", window.getLogicalExtent().width, window.getLogicalExtent().height);
                        return std::string{};
                    },
                    out);
            });
        control.registerViewCommand(
            WINDOW_VERBS[2], "engine",
            "stands in for the display's scale (a window moved to a monitor of another density) and reports the "
            "effective one; 0 returns to the display's; the UIs re-lay out at the next update",
            {requiredArg("scale", ArgKind::Number, "0 or a scale in [0.25, 8]")},
            [&window](const ControlArgs& args, IFormatWriter& out)
            {
                const std::optional<double> scale = numberArg(args, "scale");
                if (!scale ||
                    !window.injectDisplayScale(
                        static_cast<float>(std::min(*scale, static_cast<double>(Window::MAX_DISPLAY_SCALE)))))
                {
                    return std::string("injectDisplayScale needs a finite scale of 0 or more");
                }
                out.writeF32("displayScale", window.getDisplayScale());
                out.writeF32("hostDisplayScale", window.getHostDisplayScale());
                out.writeBool("injected", window.isDisplayScaleInjected());
                return std::string{};
            });
    }
} // namespace spite
