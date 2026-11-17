#pragma once

// Export/import annotations for the SpiteProfiling C++ API. In a monolithic
// build the macro is empty. When SpiteProfiling is built as a DLL,
// SpiteProfiling itself defines SPITE_PROFILING_BUILD_DLL and every consumer
// defines SPITE_PROFILING_USE_DLL (set by build plumbing, never by hand).

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_PROFILING_BUILD_DLL
#define SPITE_PROFILING_API __declspec(dllexport)
#elif defined(SPITE_PROFILING_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_PROFILING_API __declspec(dllimport)
#else
#define SPITE_PROFILING_API
#endif
#else
#define SPITE_PROFILING_API
#endif
