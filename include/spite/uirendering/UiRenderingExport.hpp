#pragma once

#if defined(_WIN32) && defined(SPITE_UI_RENDERING_BUILD_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_UI_RENDERING_API __declspec(dllexport)
#elif defined(_WIN32) && defined(SPITE_UI_RENDERING_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_UI_RENDERING_API __declspec(dllimport)
#else
#define SPITE_UI_RENDERING_API
#endif
