#pragma once

// Export/import annotations for the SpiteRenderRuntime C++ API. In a
// monolithic build the macro is empty. When SpiteRenderRuntime is built as a
// DLL, SpiteRenderRuntime itself defines SPITE_RENDER_RUNTIME_BUILD_DLL and
// every consumer defines SPITE_RENDER_RUNTIME_USE_DLL (set by build plumbing,
// never by hand).

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_RENDER_RUNTIME_BUILD_DLL
#define SPITE_RENDER_RUNTIME_API __declspec(dllexport)
#elif defined(SPITE_RENDER_RUNTIME_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_RENDER_RUNTIME_API __declspec(dllimport)
#else
#define SPITE_RENDER_RUNTIME_API
#endif
#else
#define SPITE_RENDER_RUNTIME_API
#endif
