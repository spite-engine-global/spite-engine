#pragma once

// Export/import annotations for the SpiteAssetRuntime C++ API. In a monolithic
// build the macro is empty. When SpiteAssetRuntime is built as a DLL,
// SpiteAssetRuntime itself defines SPITE_ASSETRUNTIME_BUILD_DLL and every
// consumer defines SPITE_ASSETRUNTIME_USE_DLL (set by build plumbing, never by
// hand).

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_ASSETRUNTIME_BUILD_DLL
#define SPITE_ASSETRUNTIME_API __declspec(dllexport)
#elif defined(SPITE_ASSETRUNTIME_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_ASSETRUNTIME_API __declspec(dllimport)
#else
#define SPITE_ASSETRUNTIME_API
#endif
#else
#define SPITE_ASSETRUNTIME_API
#endif
