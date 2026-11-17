#pragma once

// Capture compare: two captures' zones reports compared by PerfCli compare, plus their frame-time
// distributions from the sidecars. There is one comparison in the repo, tools/perfdash/PerfCore.py (verdicts, noise
// floor, environment checks); this runs it (`PerfCli.py compare --json`) in a bounded child process, so the Profiler
// panel, the profileCompare query and the terminal give the same rows. Everything here blocks (a process, store
// reads): call it from a worker, never the frame thread. The CLI's output and the reports behind it are untrusted
// input: every text is valid UTF-8 and clipped, numbers are finite, rows and texts are capped, no call throws.
// Std-only.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/CaptureAnalysis.hpp"
#include "engine/CaptureStore.hpp"
#include "engine/EngineExport.hpp"

namespace spite
{
    class IFormatWriter;

    inline constexpr std::size_t COMPARE_MAX_ROWS = 4096; // rows kept; more are dropped (rowsDropped)
    inline constexpr std::size_t COMPARE_MAX_NAME = 256; // bytes of a metric name
    inline constexpr std::size_t COMPARE_MAX_TEXTS = 32; // errors, warnings and commits kept
    inline constexpr std::size_t COMPARE_MAX_TEXT = 400; // bytes of one of them
    inline constexpr std::uint32_t COMPARE_BINS = 160; // bins of a frame-time distribution
    inline constexpr std::uint64_t COMPARE_MAX_FRAMES = std::uint64_t{1} << 18; // frames read per capture
    inline constexpr std::uint64_t COMPARE_MAX_OUTPUT = std::uint64_t{32} << 20; // bytes of PerfCli output read
    inline constexpr std::uint32_t COMPARE_TOOL_MS = 240000; // PerfCli's time: a first run indexes the whole pool
    // The child runs below normal priority; with this environment variable set to "1" it runs at normal priority. For
    // tests: a below-normal child on a saturated machine gets CPU only from the scheduler's starvation boosts.
    inline constexpr const wchar_t* COMPARE_NORMAL_PRIORITY_ENV = L"SPITE_COMPARE_NORMAL_PRIORITY";

    // PerfCore.VERDICTS, in its order
    enum class CompareVerdict : std::uint8_t
    {
        Regress,
        Improve,
        Flat,
        Unknown,
        Info,
    };
    inline constexpr std::size_t COMPARE_VERDICTS = 5;
    [[nodiscard]] SPITE_ENGINE_API const char* compareVerdictName(CompareVerdict verdict);

    // One PerfCore row: a metric of B against A. Times are ms, memory MB (`unit`); `kind` is PerfCore's (ms, msFine,
    // count, mb, ratio, perFrame).
    struct CompareRow
    {
        std::string metric, kind, unit;
        int direction = 0; // -1: lower is better, +1 higher, 0 informational
        CompareVerdict verdict = CompareVerdict::Unknown;
        bool floored = false; // past the noise but under the per-frame cost floor: reads flat
        bool absentA = false, absentB = false; // the side's zone did not run: counted as 0
        double a = 0.0, b = 0.0, delta = 0.0, rel = 0.0;
        std::optional<double> noiseRel; // none: no replicate evidence
        std::optional<double> deltaMsPerFrame; // the change of the metric's frame-time cost
        std::optional<double> costMs; // what ranks the row
        std::optional<double> p99A, p99B; // per-call p99 where the report has one
        std::string renamedTo, renamedFrom;
    };

    struct CompareResult
    {
        std::string group; // machine, scene, build of the comparable-runs class
        std::vector<std::string> baselineCommits, candidateCommits;
        // PerfCli's own: errors make the comparison meaningless (machine, scene, build, layers differ) and
        // block it unless forced, warnings weaken it
        std::vector<std::string> errors, warnings;
        bool blocked = false; // errors and not forced: no rows
        std::uint32_t replicateSets = 0;
        std::array<std::uint64_t, COMPARE_VERDICTS> counts{};
        std::vector<CompareRow> rows; // all of them, in PerfCli's order (by frame-time impact)
        std::uint64_t rowsDropped = 0; // beyond COMPARE_MAX_ROWS
    };

    struct CompareToolOptions
    {
        std::filesystem::path artifactsRoot; // PerfCli's --artifacts
        std::filesystem::path tool; // PerfCli.py; empty: tools/perfdash/PerfCli.py above the executable
        std::uint32_t timeoutMs = COMPARE_TOOL_MS;
        std::uint64_t maxOutput = COMPARE_MAX_OUTPUT; // bytes of its output read; more kills it
    };

    // The seams of compareReports' trust decisions. The tool: `given`, else tools/perfdash/PerfCli.py in the nearest of
    // six directories from `exeDirectory` up (never the working directory, which others may write); empty if none.
    // Python: python.exe in an absolute directory of `pathList` (PATH's format), never the executable's or working
    // directory; one under WindowsApps (a shim that starts the interpreter outside the kill job) only if no other. A
    // report name: zones_<letters, digits, _ . ->.json, which PerfCli's selector syntax (',', latest, last:N, a commit
    // prefix) cannot match.
    [[nodiscard]] SPITE_ENGINE_API std::filesystem::path compareFindTool(const std::filesystem::path& given,
                                                                         const std::filesystem::path& exeDirectory);
    [[nodiscard]] SPITE_ENGINE_API std::filesystem::path compareFindPython(std::wstring_view pathList);
    [[nodiscard]] SPITE_ENGINE_API bool compareReportName(std::string_view name);

    // Runs `python -E -s PerfCli.py compare --json --all --top 0 [--force] -- baseline candidate` where the arguments
    // are zones report file names (CaptureInfo::report). Returns the error text, empty on success; a blocked comparison
    // is a success with `blocked`. "superseded" once cancelled() is true (the child is killed).
    [[nodiscard]] SPITE_ENGINE_API std::string compareReports(const CompareToolOptions& options,
                                                              std::string_view baselineReport,
                                                              std::string_view candidateReport, bool force,
                                                              const CaptureCancelFn& cancelled, CompareResult& out);

    // One side's CPU frame times: statistics over the first COMPARE_MAX_FRAMES frames and `bins` counts over the
    // distribution's range (frames beyond it count in the last bin).
    struct CompareFrames
    {
        std::uint64_t count = 0;
        bool capped = false; // the capture has more frames than were read
        double meanMs = 0.0, p50Ms = 0.0, p99Ms = 0.0, maxMs = 0.0;
        std::vector<std::uint64_t> bins;
    };

    struct CompareDistribution
    {
        double rangeMs = 0.0; // the upper edge of the last bin: 1.05 x the larger p99.9
        CompareFrames baseline, candidate;
    };

    [[nodiscard]] SPITE_ENGINE_API std::string compareDistribution(CaptureStore& store, CaptureId baseline,
                                                                   CaptureId candidate,
                                                                   const CaptureCancelFn& cancelled,
                                                                   CompareDistribution& out);

    // Everything one comparison shows: the rows and the distributions.
    struct CompareOutcome
    {
        CaptureId baseline = 0, candidate = 0;
        CompareResult result;
        CompareDistribution distribution;
    };

    // Both captures must be ready, hold a zones report (CaptureInfo::report, beside the sidecar) and differ. The
    // store's artifacts root locates the reports. The distribution is not read when `result` is blocked and not forced.
    [[nodiscard]] SPITE_ENGINE_API std::string compareCaptures(CaptureStore& store, CaptureId baseline,
                                                               CaptureId candidate, bool force,
                                                               const CaptureCancelFn& cancelled,
                                                               const CompareToolOptions& tool, CompareOutcome& out);

    enum class CompareSort : std::uint8_t
    {
        Impact, // |frame-time cost| (PerfCli's order)
        Mean, // |relative change of the mean|
        P99, // |change of the p99|, rows without one last
        Name,
        Verdict, // regress, improve, unknown, flat, info
    };
    inline constexpr std::size_t COMPARE_SORTS = 5;
    [[nodiscard]] SPITE_ENGINE_API const char* compareSortName(CompareSort sort);

    struct CompareFilter
    {
        CompareSort sort = CompareSort::Impact;
        bool descending = true; // largest first (Name: Z first)
        bool all = false; // flat and info rows too
        std::string name; // case-insensitive substring of the metric
    };

    // Indexes into result.rows of the rows `filter` keeps, in order. Stable and total: equal keys keep PerfCli's order,
    // and a row with no value for the key sorts last either way.
    [[nodiscard]] SPITE_ENGINE_API std::vector<std::uint32_t> compareOrder(const CompareResult& result,
                                                                           const CompareFilter& filter);

    // The answer of profileCompare, and the numbers the Compare view reports under profilerPanes: one writer, so the
    // panel and the query give the same fields. `limit` rows from `offset` of compareOrder(filter); the distribution
    // is written when `distribution`.
    SPITE_ENGINE_API void writeCompare(IFormatWriter& out, const CompareOutcome& outcome, const CompareFilter& filter,
                                       std::uint64_t offset, std::uint64_t limit, bool distribution);

    // A text from an untrusted file made safe to show: invalid UTF-8 bytes become '?', control characters a space,
    // cut at a code point boundary within `limit` bytes (plus "...").
    [[nodiscard]] SPITE_ENGINE_API std::string compareSafeText(std::string_view text, std::size_t limit);
} // namespace spite
