#pragma once

#include <ctime>

namespace spite::platform
{
    inline bool toLocalTime(const std::time_t seconds, std::tm& local)
    {
#if defined(_WIN32)
        return localtime_s(&local, &seconds) == 0;
#else
        return localtime_r(&seconds, &local) != nullptr;
#endif
    }

    inline std::time_t fromUtc(std::tm& utc)
    {
#if defined(_WIN32)
        return _mkgmtime(&utc);
#else
        return timegm(&utc);
#endif
    }
} // namespace spite::platform
