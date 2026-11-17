#pragma once

#include <string_view>

#include "asset/AssetData.hpp"

namespace spite
{
    inline bool editorAssetVisible(AssetType type, std::string_view name)
    {
        return type != AssetType::MATERIAL || !name.starts_with("__");
    }
} // namespace spite
