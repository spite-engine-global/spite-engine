#pragma once

// Export/import annotations for the SpiteUi C++ API. SpiteUi itself defines
// SPITE_UI_BUILD_DLL; every consumer defines SPITE_UI_USE_DLL (set by build
// plumbing, never by hand).

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_UI_BUILD_DLL
#define SPITE_UI_API __declspec(dllexport)
#elif defined(SPITE_UI_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_UI_API __declspec(dllimport)
#else
#define SPITE_UI_API
#endif
#else
#define SPITE_UI_API
#endif
