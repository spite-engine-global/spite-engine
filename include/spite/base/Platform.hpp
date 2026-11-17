#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define SPITE_PLATFORM_WINDOWS 1
#elif defined(__APPLE__) && defined(__MACH__)
#define SPITE_PLATFORM_MACOS 1
#elif defined(__linux__)
#define SPITE_PLATFORM_LINUX 1
#else
#error Unsupported platform
#endif

#if defined(_MSC_VER)
#define SPITE_COMPILER_MSVC 1
#elif defined(__clang__)
#define SPITE_COMPILER_CLANG 1
#elif defined(__GNUC__)
#define SPITE_COMPILER_GCC 1
#else
#error Unsupported compiler
#endif

#if !defined(SPITE_DEBUG_BUILD)
#error SPITE_DEBUG_BUILD must be defined as 0 or 1 by the build configuration
#elif SPITE_DEBUG_BUILD != 0 && SPITE_DEBUG_BUILD != 1
#error SPITE_DEBUG_BUILD must be 0 or 1
#endif

#if defined(SPITE_COMPILER_MSVC)
#define SPITE_FINLINE __forceinline
#define SPITE_DEBUG_BREAK __debugbreak()
#elif defined(SPITE_COMPILER_CLANG) || defined(SPITE_COMPILER_GCC)
#define SPITE_FINLINE inline __attribute__((always_inline))
#define SPITE_DEBUG_BREAK __builtin_trap()
#endif

namespace spite
{
    using u8 = std::uint8_t;
    using u16 = std::uint16_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;

    using i16 = std::int16_t;
    using i32 = std::int32_t;

    using f32 = float;

    using sizet = std::size_t;
} // namespace spite
