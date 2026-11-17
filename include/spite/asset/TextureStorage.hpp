#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "asset/AssetExport.hpp"
#include "asset/BakedAssetFormat.hpp"
#include "base/Expected.hpp"

namespace spite
{
    // Disk storage only: decoding restores the exact GPU-format mip bytes. Older raw files remain readable.
    // A decoded file is bounded before allocation, including when its stored payload is tiny.
    constexpr u64 MAX_TEXTURE_PAYLOAD_BYTES = 2ull * 1024 * 1024 * 1024;
    SPITE_ASSET_API Expected<std::vector<std::byte>> encodeBakedTexture(SpiteTextureHeader header,
                                                                        std::span<const u8> pixels);
    SPITE_ASSET_API Expected<std::vector<u8>> decodeBakedTexturePayload(
        std::span<const std::byte> file, u64 maxDecodedBytes = MAX_TEXTURE_PAYLOAD_BYTES);
    // Checks a compressed stream, its checksum and exact length using bounded scratch space.
    SPITE_ASSET_API Expected<void> validateBakedTextureStorage(std::span<const std::byte> file);
} // namespace spite
