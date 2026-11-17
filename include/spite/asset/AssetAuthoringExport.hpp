#pragma once

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_ASSET_AUTHORING_BUILD_DLL
#define SPITE_ASSET_AUTHORING_API __declspec(dllexport)
#elif defined(SPITE_ASSET_AUTHORING_USE_DLL)
#define SPITE_ASSET_AUTHORING_API __declspec(dllimport)
#else
#define SPITE_ASSET_AUTHORING_API
#endif
#else
#define SPITE_ASSET_AUTHORING_API
#endif
