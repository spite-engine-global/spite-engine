#pragma once

#include <string>
#include <string_view>

namespace spite
{
    // The system clipboard as text inputs see it, supplied by the host (the
    // ui module never calls the OS). Without one, a context keeps its own.
    class UiClipboard
    {
    public:
        virtual ~UiClipboard() = default;

        [[nodiscard]] virtual std::string text() const = 0;
        virtual void setText(std::string_view text) = 0;
    };
} // namespace spite
