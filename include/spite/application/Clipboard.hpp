#pragma once

#include <string>
#include <string_view>

#include "application/ApplicationExport.hpp"

namespace spite
{
    // The OS clipboard as UTF-8 text; needs the platform video layer up (an
    // Application). Empty when it holds no text.
    [[nodiscard]] SPITE_APPLICATION_API std::string getClipboardText();
    SPITE_APPLICATION_API void setClipboardText(std::string_view text);
} // namespace spite
