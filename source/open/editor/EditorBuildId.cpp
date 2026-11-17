#include "EditorBuildId.hpp"

#if __has_include("SpiteBuildId.h")
#include "SpiteBuildId.h" // written by the SpiteBuildId target of SpiteEditor.vcxproj
#endif

#ifndef SPITE_BUILD_ID
#define SPITE_BUILD_ID "unknown"
#endif

namespace spite
{
    std::string_view editorBuildId() { return SPITE_BUILD_ID; }
} // namespace spite
