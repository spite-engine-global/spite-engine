#pragma once

// Export macro for the SpiteRendering DLL. Modules that BUILD the rendering
// module define SPITE_RENDERING_BUILD_DLL (see SpiteRendering.vcxproj);
// consumers define SPITE_RENDERING_USE_DLL to link against it. Without either,
// symbols are plain (monolith builds compile the module into the exe).
#if defined(_WIN32) && defined(SPITE_RENDERING_BUILD_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_RENDERING_API __declspec(dllexport)
#elif defined(_WIN32) && defined(SPITE_RENDERING_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_RENDERING_API __declspec(dllimport)
#else
#define SPITE_RENDERING_API
#endif
