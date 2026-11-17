#pragma once

#include <functional>
#include <optional>

#include "base/Platform.hpp"

namespace spite
{
    struct SceneLayerPlacement
    {
        // An empty request resolves to the active scene. An unknown id is refused.
        std::function<std::optional<u32>(std::optional<u64>)> resolve;
    };
} // namespace spite
