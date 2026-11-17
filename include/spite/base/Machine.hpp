#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "base/BaseExport.hpp"
#include "base/Platform.hpp"

namespace spite
{
    // The machine-scoped artifact folder, <cpu>_<gpu>_<ramGB>: artifacts/<machine>/ keeps runs (perf reports,
    // captures) from different machines apart in the shared artifacts tree.
    SPITE_BASE_API std::string machineFolderName();
    // The CPU brand string; "unknown" without the brand leaves.
    SPITE_BASE_API std::string queryCpuBrand();
    // The largest dedicated adapter's name; software (WARP) adapters are skipped.
    SPITE_BASE_API std::string queryGpuName(sizet& dedicatedVramMb);
    SPITE_BASE_API sizet queryTotalRamMb();
    // Run identity a perf report records .
    // "Debug" or "Release".
    SPITE_BASE_API std::string buildConfigName();
    SPITE_BASE_API bool addressSanitizerLoaded();
    // Vulkan layer DLLs the loader loaded into this process, sorted.
    SPITE_BASE_API std::vector<std::string> activeVulkanLayers();
    SPITE_BASE_API std::string queryOsVersion();
    // HEAD hash of the SPITE checkout (.git, CMakeLists.txt and ReleaseVersion.json) at or above `directory` (default:
    // the running exe's directory, never the process working directory); "unknown" without git or outside a checkout;
    // dirty: tracked changes. Each git child is killed, its tree with it, and given up on after timeoutMs (a test sets
    // its own bound).
    SPITE_BASE_API std::string queryGitCommit(bool& dirty, bool* dirtyKnown = nullptr,
                                              const std::filesystem::path& directory = {}, unsigned timeoutMs = 5000);
} // namespace spite
