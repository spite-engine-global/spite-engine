#pragma once

#include <type_traits>

#include "base/Platform.hpp"

namespace spite
{
    template <typename T, typename... Rest>
    void hashCombine(sizet& seed, const T& v, const Rest&... rest)
    {
        std::hash<T> hasher;
        seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        (hashCombine(seed, rest), ...);
    };
} // namespace spite
