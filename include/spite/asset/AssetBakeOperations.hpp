#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "asset/AssetData.hpp"
#include "asset/AssetExport.hpp"
#include "asset/ModelImportSettings.hpp"
#include "base/Expected.hpp"

namespace spite
{
    // Updates skinned submesh and meshlet bounds for the encoded skeleton and clips; static submeshes are unchanged.
    SPITE_ASSET_API Expected<void> bakeSkinnedBounds(ParsedModelData& model, std::span<const std::byte> skeletonFile,
                                                     std::span<const std::vector<std::byte>> clipFiles,
                                                     const ModelImportSettings& settings);
} // namespace spite
