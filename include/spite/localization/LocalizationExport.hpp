#pragma once

// Export/import annotations for the SpiteLocalization C++ API. SpiteLocalization defines
// SPITE_LOCALIZATION_BUILD_DLL and every consumer defines SPITE_LOCALIZATION_USE_DLL (set by build plumbing, never by
// hand); otherwise the macro is empty.

#if defined(_WIN32) && !defined(SPITE_SHIPPING)
#ifdef SPITE_LOCALIZATION_BUILD_DLL
#define SPITE_LOCALIZATION_API __declspec(dllexport)
#elif defined(SPITE_LOCALIZATION_USE_DLL) && !defined(SPITE_SHIPPING)
#define SPITE_LOCALIZATION_API __declspec(dllimport)
#else
#define SPITE_LOCALIZATION_API
#endif
#else
#define SPITE_LOCALIZATION_API
#endif
