#pragma once

// Export/import annotations for the SpiteEditor C++ API. SpiteEditor itself
// defines SPITE_EDITOR_BUILD_DLL and every consumer defines
// SPITE_EDITOR_USE_DLL (set by build plumbing, never by hand).

#ifdef _WIN32
#ifdef SPITE_EDITOR_BUILD_DLL
#define SPITE_EDITOR_API __declspec(dllexport)
#elif defined(SPITE_EDITOR_USE_DLL)
#define SPITE_EDITOR_API __declspec(dllimport)
#else
#define SPITE_EDITOR_API
#endif
#else
#define SPITE_EDITOR_API
#endif
