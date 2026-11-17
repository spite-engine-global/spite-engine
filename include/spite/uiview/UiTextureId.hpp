#pragma once

#include "base/Platform.hpp"

namespace spite
{
    // Dense index into a UiViewContext's texture table.
    using UiTextureId = u32;
    inline constexpr UiTextureId NO_UI_TEXTURE = ~0u;
} // namespace spite
