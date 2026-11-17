#pragma once

// Export/import annotations for the SpiteControl C++ API. The module defines
// SPITE_CONTROL_BUILD_DLL and every consumer defines SPITE_CONTROL_USE_DLL (set
// by build plumbing, never by hand).

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_CONTROL_BUILD_DLL
#define SPITE_CONTROL_API __declspec(dllexport)
#elif defined(SPITE_CONTROL_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_CONTROL_API __declspec(dllimport)
#else
#define SPITE_CONTROL_API
#endif
#else
#define SPITE_CONTROL_API
#endif
