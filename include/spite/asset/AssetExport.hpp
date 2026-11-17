#pragma once

// Export/import annotations for the SpiteAsset C++ API. In a monolithic build
// the macro is empty. When SpiteAsset is built as a DLL, SpiteAsset itself
// defines SPITE_ASSET_BUILD_DLL and every consumer defines SPITE_ASSET_USE_DLL
// (set by build plumbing, never by hand).

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_ASSET_BUILD_DLL
#define SPITE_ASSET_API __declspec(dllexport)
#elif defined(SPITE_ASSET_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_ASSET_API __declspec(dllimport)
#else
#define SPITE_ASSET_API
#endif
#else
#define SPITE_ASSET_API
#endif
