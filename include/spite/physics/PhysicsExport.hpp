#pragma once

// Export/import annotations for the SpitePhysics C++ API. In a monolithic build
// the macro is empty. When SpitePhysics is built as a DLL, SpitePhysics itself
// defines SPITE_PHYSICS_BUILD_DLL and every consumer defines SPITE_PHYSICS_USE_DLL
// (set by build plumbing, never by hand).

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_PHYSICS_BUILD_DLL
#define SPITE_PHYSICS_API __declspec(dllexport)
#elif defined(SPITE_PHYSICS_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_PHYSICS_API __declspec(dllimport)
#else
#define SPITE_PHYSICS_API
#endif
#else
#define SPITE_PHYSICS_API
#endif
