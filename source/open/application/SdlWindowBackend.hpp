#pragma once

#include <SDL3/SDL_mouse.h>

#include "application/vulkan/IVulkanWindowSurface.hpp"
#include "WindowBackend.hpp"

struct SDL_Window;

namespace spite
{
    struct WindowConfig;

    // SDL adapter for one window: owns the OS window, cursor cache, and the
    // Vulkan surface creation; applies drained core commands. No engine
    // policy lives here.
    class SdlWindowBackend final : public IWindowBackend, public IVulkanWindowSurface
    {
    public:
        explicit SdlWindowBackend(WindowId id);
        ~SdlWindowBackend() override;

        bool open(const WindowConfig& config) override;
        // An owned resizable window over `parent` (an SDL_Window*, null: none), at the desktop
        // position the description names.
        bool openSecondary(const SecondaryWindowDesc& desc, void* parent);
        void apply(const std::vector<WindowCommand>& commands) override;

        [[nodiscard]] WindowId getId() const { return m_id; }
        [[nodiscard]] u32 nativeId() const override { return m_sdlWindowId; }
        [[nodiscard]] void* nativeHandle() const override { return m_window; }
        [[nodiscard]] WindowExtent logicalExtent() const override;
        [[nodiscard]] WindowExtent framebufferExtent() const override;
        [[nodiscard]] float displayScale() const override;
        [[nodiscard]] WindowPosition position() const override;

        [[nodiscard]] IVulkanWindowSurface& surface() { return *this; }
        [[nodiscard]] const char* const* getRequiredInstanceExtensions(u32& extensionCount) const override;
        [[nodiscard]] vk::SurfaceKHR createSurface(vk::Instance instance) const override;

    private:
        [[nodiscard]] SDL_Cursor* cursorForShape(CursorShape shape) const;

        WindowId m_id = INVALID_WINDOW_ID;
        SDL_Window* m_window = nullptr;
        unsigned int m_sdlWindowId = 0;
        SDL_Cursor* m_defaultCursor = nullptr;
        SDL_Cursor* m_handCursor = nullptr;
        SDL_Cursor* m_iBeamCursor = nullptr;
        SDL_Cursor* m_resizeHorizontalCursor = nullptr;
        SDL_Cursor* m_resizeVerticalCursor = nullptr;
        SDL_Cursor* m_resizeDiagonalNorthWestSouthEastCursor = nullptr;
        SDL_Cursor* m_resizeDiagonalNorthEastSouthWestCursor = nullptr;
        CursorShape m_currentCursorShape = CursorShape::Default;
    };
} // namespace spite
