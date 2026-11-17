#pragma once

// Export/import annotations for the SpiteSerialization C++ API. In a monolithic
// build the macro is empty. When SpiteSerialization is built as a DLL, the
// module itself defines SPITE_SERIALIZATION_BUILD_DLL and every consumer defines
// SPITE_SERIALIZATION_USE_DLL (set by build plumbing, never by hand).

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_SERIALIZATION_BUILD_DLL
#define SPITE_SERIALIZATION_API __declspec(dllexport)
#elif defined(SPITE_SERIALIZATION_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_SERIALIZATION_API __declspec(dllimport)
#else
#define SPITE_SERIALIZATION_API
#endif
#else
#define SPITE_SERIALIZATION_API
#endif
