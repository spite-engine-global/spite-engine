#include "base/ModuleAbi.hpp"

#if defined(_WIN32)
#define SPITE_IDENTITY_EXPORT __declspec(dllexport)
#else
#define SPITE_IDENTITY_EXPORT __attribute__((visibility("default")))
#endif

extern "C" SPITE_IDENTITY_EXPORT const char* spiteSourceApiStamp()
{
    return "SPITE_SDK_API_STAMP=" SPITE_DETAIL_MODULE_XSTR(SPITE_API_STAMP);
}

extern "C" SPITE_IDENTITY_EXPORT const char* spiteSourceToolsetStamp() { return SPITE_TOOLSET_STAMP; }

#if defined(SPITE_SOURCE_GPU_CONTRACT_ID)
extern "C" SPITE_IDENTITY_EXPORT const char* spiteSourceGpuContract()
{
    return "SPITE_GPU_CONTRACT_ID=" SPITE_SOURCE_GPU_CONTRACT_ID;
}
#endif
