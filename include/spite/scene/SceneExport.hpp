#pragma once

// Export/import annotations for the SpiteScene C++ API. In a monolithic build
// the macro is empty. When SpiteScene is built as a DLL, SpiteScene itself
// defines SPITE_SCENE_BUILD_DLL and every consumer defines SPITE_SCENE_USE_DLL
// (set by build plumbing, never by hand).

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_SCENE_BUILD_DLL
#define SPITE_SCENE_API __declspec(dllexport)
#elif defined(SPITE_SCENE_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_SCENE_API __declspec(dllimport)
#else
#define SPITE_SCENE_API
#endif
#else
#define SPITE_SCENE_API
#endif
