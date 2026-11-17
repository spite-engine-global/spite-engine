#pragma once

// Export/import annotations for the SpiteWorld C++ API. In a monolithic build
// the macro is empty. When SpiteWorld is built as a DLL, SpiteWorld itself
// defines SPITE_WORLD_BUILD_DLL and every consumer defines SPITE_WORLD_USE_DLL
// (set by build plumbing, never by hand).

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_WORLD_BUILD_DLL
#define SPITE_WORLD_API __declspec(dllexport)
#elif defined(SPITE_WORLD_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_WORLD_API __declspec(dllimport)
#else
#define SPITE_WORLD_API
#endif
#else
#define SPITE_WORLD_API
#endif
