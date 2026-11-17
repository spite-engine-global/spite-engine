#pragma once

#include <expected>

#include "base/Error.hpp"

namespace spite
{
    template <typename T>
    using Expected = std::expected<T, Error>;
}
