#pragma once

// What a DLL the host maps at runtime (a runtime module, SpiteRenderPipeline) stamps into its descriptor so the host
// can refuse a build it cannot call.

// SPITE_API_STAMP: a 64-bit hash of every header under include/spite, written by tools/ApiStamp before the build
// compiles anything that includes this file. A DLL built against other headers carries another stamp, so the host
// refuses it: nobody numbers breaking changes by hand.
#include "base/ApiStamp.generated.hpp"

#define SPITE_DETAIL_MODULE_STR(x) #x
#define SPITE_DETAIL_MODULE_XSTR(x) SPITE_DETAIL_MODULE_STR(x)
#if defined(__linux__) || defined(__APPLE__)
#include <version>
#if defined(__APPLE__)
#define SPITE_DETAIL_MODULE_OS "macos"
#else
#define SPITE_DETAIL_MODULE_OS "linux"
#endif

#if defined(__x86_64__)
#define SPITE_DETAIL_MODULE_ARCH "x86_64"
#elif defined(__aarch64__)
#define SPITE_DETAIL_MODULE_ARCH "aarch64"
#else
#error Unsupported module architecture
#endif

#if defined(__clang__)
#define SPITE_DETAIL_MODULE_COMPILER "clang" __clang_version__
#elif defined(__GNUC__)
#define SPITE_DETAIL_MODULE_COMPILER "gcc" __VERSION__
#else
#error Unsupported module compiler
#endif

#if defined(__GLIBCXX__)
#ifdef _GLIBCXX_DEBUG
#define SPITE_DETAIL_MODULE_STDLIB_DEBUG "-stldebug"
#else
#define SPITE_DETAIL_MODULE_STDLIB_DEBUG ""
#endif
#define SPITE_DETAIL_MODULE_STDLIB                                                                                     \
    "libstdc++" SPITE_DETAIL_MODULE_XSTR(__GLIBCXX__) "-cxx11abi" SPITE_DETAIL_MODULE_XSTR(_GLIBCXX_USE_CXX11_ABI)     \
        SPITE_DETAIL_MODULE_STDLIB_DEBUG
#elif defined(_LIBCPP_VERSION)
#define SPITE_DETAIL_MODULE_STDLIB                                                                                     \
    "libc++" SPITE_DETAIL_MODULE_XSTR(_LIBCPP_VERSION) "-abi" SPITE_DETAIL_MODULE_XSTR(_LIBCPP_ABI_VERSION)
#else
#error Unsupported module C++ standard library
#endif

#if defined(__SANITIZE_ADDRESS__)
#define SPITE_DETAIL_MODULE_ASAN "-asan"
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define SPITE_DETAIL_MODULE_ASAN "-asan"
#else
#define SPITE_DETAIL_MODULE_ASAN ""
#endif
#else
#define SPITE_DETAIL_MODULE_ASAN ""
#endif

#ifdef SPITE_SHIPPING
#define SPITE_DETAIL_MODULE_MODE "-shipping"
#else
#define SPITE_DETAIL_MODULE_MODE "-developer"
#endif

#define SPITE_TOOLSET_STAMP                                                                                            \
    SPITE_DETAIL_MODULE_OS "-" SPITE_DETAIL_MODULE_ARCH "-" SPITE_DETAIL_MODULE_COMPILER                               \
                           "-" SPITE_DETAIL_MODULE_STDLIB "-debug" SPITE_DETAIL_MODULE_XSTR(SPITE_DEBUG_BUILD)         \
                               SPITE_DETAIL_MODULE_MODE SPITE_DETAIL_MODULE_ASAN
#else

#ifdef _DLL
#define SPITE_DETAIL_MODULE_CRT "md"
#else
#define SPITE_DETAIL_MODULE_CRT "mt"
#endif
#ifdef __SANITIZE_ADDRESS__
#define SPITE_DETAIL_MODULE_ASAN "-asan"
#else
#define SPITE_DETAIL_MODULE_ASAN ""
#endif
// What must match for C++ types to cross the boundary: compiler version, CRT
// flavor, STL iterator debugging and the engine's debug switch.
#define SPITE_TOOLSET_STAMP                                                                                            \
    "msvc" SPITE_DETAIL_MODULE_XSTR(_MSC_VER) "-" SPITE_DETAIL_MODULE_CRT "-idl" SPITE_DETAIL_MODULE_XSTR(             \
        _ITERATOR_DEBUG_LEVEL) "-debug" SPITE_DETAIL_MODULE_XSTR(SPITE_DEBUG_BUILD) SPITE_DETAIL_MODULE_ASAN
#endif
