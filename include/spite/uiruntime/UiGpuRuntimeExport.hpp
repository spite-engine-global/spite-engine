#pragma once

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#if defined(SPITE_UI_GPU_RUNTIME_BUILD_DLL)
#define SPITE_UI_GPU_RUNTIME_API __declspec(dllexport)
#elif defined(SPITE_UI_GPU_RUNTIME_USE_DLL)
#define SPITE_UI_GPU_RUNTIME_API __declspec(dllimport)
#else
#define SPITE_UI_GPU_RUNTIME_API
#endif
#else
#define SPITE_UI_GPU_RUNTIME_API
#endif
