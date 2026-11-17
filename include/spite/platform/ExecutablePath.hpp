#pragma once

#include <filesystem>

#include "base/BaseExport.hpp"

namespace spite::platform
{
    SPITE_BASE_API std::filesystem::path executablePath();
} // namespace spite::platform
