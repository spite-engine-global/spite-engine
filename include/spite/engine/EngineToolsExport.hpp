#pragma once

// Export/import annotations for the SpiteEngineTools C++ API. In a monolithic build
// the macro is empty. When SpiteEngine is built as a DLL, SpiteEngineTools itself
// defines SPITE_ENGINE_TOOLS_BUILD_DLL and every consumer defines
// SPITE_ENGINE_TOOLS_USE_DLL (set by build plumbing, never by hand).

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_ENGINE_TOOLS_BUILD_DLL
#define SPITE_ENGINE_TOOLS_API __declspec(dllexport)
#elif defined(SPITE_ENGINE_TOOLS_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_ENGINE_TOOLS_API __declspec(dllimport)
#else
#define SPITE_ENGINE_TOOLS_API
#endif
#else
#define SPITE_ENGINE_TOOLS_API
#endif
