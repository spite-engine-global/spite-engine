#pragma once

// Export/import annotations for the SpiteEngine C++ API. In a monolithic build
// the macro is empty. When SpiteEngine is built as a DLL, SpiteEngine itself
// defines SPITE_ENGINE_BUILD_DLL and every consumer defines
// SPITE_ENGINE_USE_DLL (set by build plumbing, never by hand).

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_ENGINE_BUILD_DLL
#define SPITE_ENGINE_API __declspec(dllexport)
#elif defined(SPITE_ENGINE_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_ENGINE_API __declspec(dllimport)
#else
#define SPITE_ENGINE_API
#endif
#else
#define SPITE_ENGINE_API
#endif
