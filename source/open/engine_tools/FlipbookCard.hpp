#pragma once
#include <filesystem>
#include <string>
#include "base/MathTypes.hpp"

namespace spite
{
    struct FlipbookCardRequest
    {
        u32 columns = 1, rows = 1;
        Vec2 size{1.0f};
        Vec2 pivot{0.5f, 0.0f};
        bool emissive = false;
    };
    struct FlipbookCardResult
    {
        std::string error;
        u32 width = 0, height = 0;
    };
    FlipbookCardResult prepareFlipbookCard(const std::filesystem::path& source, const std::filesystem::path& output,
                                           const FlipbookCardRequest& request, u64 jobId);
} // namespace spite
