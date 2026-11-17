#pragma once

// Export/import annotations for the SpiteEcs C++ API. In a monolithic build
// the macro is empty. When SpiteEcs is built as a DLL, SpiteEcs itself defines
// SPITE_ECS_BUILD_DLL and every consumer defines SPITE_ECS_USE_DLL (set by
// build plumbing, never by hand).

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_ECS_BUILD_DLL
#define SPITE_ECS_API __declspec(dllexport)
#elif defined(SPITE_ECS_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_ECS_API __declspec(dllimport)
#else
#define SPITE_ECS_API
#endif
#else
#define SPITE_ECS_API
#endif
