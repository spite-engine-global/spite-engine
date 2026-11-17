#pragma once
// Export/import annotations for the SpiteUiView C++ API. SpiteUiView itself
// defines SPITE_UIVIEW_BUILD_DLL; every consumer defines SPITE_UIVIEW_USE_DLL
// (set by build plumbing, never by hand).
#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_UIVIEW_BUILD_DLL
#define SPITE_UIVIEW_API __declspec(dllexport)
#elif defined(SPITE_UIVIEW_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_UIVIEW_API __declspec(dllimport)
#else
#define SPITE_UIVIEW_API
#endif
#else
#define SPITE_UIVIEW_API
#endif
