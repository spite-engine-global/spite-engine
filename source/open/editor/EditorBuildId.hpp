#pragma once

#include <string_view>

namespace spite
{
    // The short commit id the editor library was built from ("unknown" without git): the status bar's DEV segment.
    [[nodiscard]] std::string_view editorBuildId();
} // namespace spite
