#pragma once

// Export/import annotations for the SpiteAnimationAuthoring C++ API. SpiteAnimationAuthoring itself defines
// SPITE_ANIMATION_AUTHORING_BUILD_DLL and every consumer defines SPITE_ANIMATION_AUTHORING_USE_DLL (set by build
// plumbing, never by hand).

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_ANIMATION_AUTHORING_BUILD_DLL
#define SPITE_ANIMATION_AUTHORING_API __declspec(dllexport)
#elif defined(SPITE_ANIMATION_AUTHORING_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_ANIMATION_AUTHORING_API __declspec(dllimport)
#else
#define SPITE_ANIMATION_AUTHORING_API
#endif
#else
#define SPITE_ANIMATION_AUTHORING_API
#endif
