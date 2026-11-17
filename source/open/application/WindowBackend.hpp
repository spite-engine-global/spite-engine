#pragma once

#include <vector>

#include "application/Window.hpp"
#include "application/WindowTypes.hpp"

namespace spite
{
    // Platform adapter seam for one OS window (internal: implemented inside
    // the application DLL by the SDL backend). The core drives it through
    // command queues; the adapter queries nothing back except OS state.
    class IWindowBackend
    {
    public:
        virtual ~IWindowBackend() = default;

        // Opens the OS window; returns false on failure.
        virtual bool open(const WindowConfig& config) = 0;
        // Applies drained core commands to the OS window.
        virtual void apply(const std::vector<WindowCommand>& commands) = 0;

        [[nodiscard]] virtual u32 nativeId() const = 0;
        [[nodiscard]] virtual void* nativeHandle() const = 0;
        [[nodiscard]] virtual WindowExtent logicalExtent() const = 0;
        [[nodiscard]] virtual WindowExtent framebufferExtent() const = 0;
        [[nodiscard]] virtual float displayScale() const = 0;
        [[nodiscard]] virtual WindowPosition position() const = 0;
    };
} // namespace spite
