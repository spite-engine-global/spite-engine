#pragma once

#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "application/ApplicationExport.hpp"
#include "application/WindowTypes.hpp"

namespace spite
{
    class Window;
    class IWindowBackend;
    struct WindowConfig;

    /**
     * @brief Owns all windows: their platform-free core state (Window) and
     *        their platform adapters (internal IWindowBackend implementations,
     *        created here). User commands are recorded on the Window and
     *        applied by the adapter on drainCommands(); OS observations
     *        arrive through Window ingestion.
     */
    class SPITE_APPLICATION_API WindowService
    {
    public:
        WindowService();
        ~WindowService();
        WindowService(const WindowService&) = delete;
        WindowService& operator=(const WindowService&) = delete;
        WindowService(WindowService&&) = delete;
        WindowService& operator=(WindowService&&) = delete;

        [[nodiscard]] bool createPrimaryWindow(const WindowConfig& config);
        // A window beside the primary; INVALID_WINDOW_ID when the platform refuses it. It is reported by
        // the dispatcher as the primary is, and closing it only requests the close (Window::isCloseRequested): its
        // owner destroys it.
        [[nodiscard]] WindowId createWindow(const SecondaryWindowDesc& desc);
        // Destroys a window createWindow made; never the primary.
        void destroyWindow(WindowId id);
        // Whether any of the application's windows has the keyboard focus.
        [[nodiscard]] bool anyWindowFocused() const;
        [[nodiscard]] Window* getWindow(WindowId id);
        [[nodiscard]] const Window* getWindow(WindowId id) const;
        [[nodiscard]] Window* getPrimaryWindow();
        [[nodiscard]] const Window* getPrimaryWindow() const;
        [[nodiscard]] WindowId getPrimaryWindowId() const;
        [[nodiscard]] Window* findWindowByNativeId(u32 nativeId);
        [[nodiscard]] WindowId findWindowIdByNativeId(u32 nativeId) const;
        [[nodiscard]] WindowId windowUnderPointer() const;
        // Internal adapter access (event translation, framebuffer queries).
        [[nodiscard]] IWindowBackend* getBackend(WindowId id);
        void applyPrimaryConfig(const WindowConfig& config);
        // Applies every window's drained commands to its adapter.
        void drainCommands();

    private:
        std::unordered_map<WindowId, std::unique_ptr<Window>> m_windows;
        std::unordered_map<WindowId, std::unique_ptr<IWindowBackend>> m_backends;
        std::unordered_set<WindowId> m_ghostWindows;
        WindowId m_primaryWindowId = INVALID_WINDOW_ID;
        WindowId m_nextWindowId = 1;
    };
} // namespace spite
