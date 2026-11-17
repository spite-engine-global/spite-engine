#pragma once
#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#if defined(SPITE_AUDIO_BUILD_DLL)
#define SPITE_AUDIO_API __declspec(dllexport)
#elif defined(SPITE_AUDIO_USE_DLL)
#define SPITE_AUDIO_API __declspec(dllimport)
#else
#define SPITE_AUDIO_API
#endif
#else
#define SPITE_AUDIO_API
#endif
