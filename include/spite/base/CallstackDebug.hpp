#pragma once
#include "BaseExport.hpp"

namespace spite
{
    SPITE_BASE_API void logCallstack(int depth = 15, const char* context_message = nullptr);
}
