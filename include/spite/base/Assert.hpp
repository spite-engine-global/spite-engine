#pragma once

#include <stdexcept>

#include "base/BaseExport.hpp"
#include "base/CallstackDebug.hpp"
#include "base/Logging.hpp"
#include "base/Platform.hpp"

namespace spite
{
    // A failed assert in Debug-family builds breaks into the debugger unless the host
    // opted into throwing (test runners: composes with EXPECT_THROW).
    SPITE_BASE_API void setAssertsThrow(bool enabled);
    SPITE_BASE_API bool assertsThrow();
    // What SASSERT throws when asserts throw: a test signal that code guarding exceptions (the control server) lets
    // through
    struct AssertionFailure : std::runtime_error
    {
        AssertionFailure() : std::runtime_error("Assertion failed") {}
    };
} // namespace spite

#if SPITE_DEBUG_BUILD == 0
// Release builds log failures and continue: conditions here are invariants we
// do not want to crash shipped builds on, but their firing must be visible. A host that opted into
// throwing (the test mains) gets the throw here too, so a Release test run fails on an assert like a Debug one.
#define SASSERT(condition)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            SERROR_LOG("Assertion failed: {}", #condition);                                                            \
            if (spite::assertsThrow())                                                                                 \
            {                                                                                                          \
                throw spite::AssertionFailure();                                                                       \
            }                                                                                                          \
        }                                                                                                              \
    }                                                                                                                  \
    while (false)

#define SASSERTM(condition, ...)                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            SERROR_LOG("Assertion failed: {} | " __VA_ARGS__, #condition);                                             \
            if (spite::assertsThrow())                                                                                 \
            {                                                                                                          \
                throw spite::AssertionFailure();                                                                       \
            }                                                                                                          \
        }                                                                                                              \
    }                                                                                                                  \
    while (false)

#else
#define SASSERT(condition)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            spite::logCallstack(20, "Assertion stack:\n");                                                             \
            SDEBUG_LOG("Assertion failed: {}", #condition);                                                            \
            if (spite::assertsThrow())                                                                                 \
            {                                                                                                          \
                throw spite::AssertionFailure();                                                                       \
            }                                                                                                          \
            SPITE_DEBUG_BREAK;                                                                                         \
        }                                                                                                              \
    }                                                                                                                  \
    while (false)
#define SASSERTM(condition, ...)                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            spite::logCallstack(20, "Assertion stack:\n");                                                             \
            SDEBUG_LOG(__VA_ARGS__);                                                                                   \
            if (spite::assertsThrow())                                                                                 \
            {                                                                                                          \
                throw spite::AssertionFailure();                                                                       \
            }                                                                                                          \
            SPITE_DEBUG_BREAK;                                                                                         \
        }                                                                                                              \
    }                                                                                                                  \
    while (false)
#endif
