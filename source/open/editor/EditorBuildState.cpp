#include "EditorBuildState.hpp"
#include "engine/Module.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cwctype>
#include <ctime>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#endif

#include <nlohmann/json.hpp>

#include "EditorBuildProcess.hpp"
#include "EditorFeedback.hpp"
#include "base/Logging.hpp"
#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "engine/BuildStatus.hpp"
#include "engine/Engine.hpp"
#include "engine/ModuleHost.hpp"
#include "localization/Localization.hpp"
#include "profiling/Profiling.hpp"
#include <spite/profiling/Zone.hpp>
#include "serialization/FormatWalk.hpp"
#include "serialization/JsonFormat.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;

        constexpr std::string_view OWNER =
            "editorBuild"; // not the panel's editorModules: each unregisters only its own
        constexpr double PERIOD_SECONDS = 1.0;
        constexpr sizet SHOWN_DIAGNOSTICS = 3;
        constexpr sizet MAX_RECORDED_DIAGNOSTICS = 1000;
        constexpr sizet MAX_TOAST_NAMES = 3;
        constexpr sizet MAX_FAILED_TOASTS = 3; // a broken base library fails every dependent: the rest are one toast
        constexpr sizet MAX_MESSAGE_BYTES = 160;

        // A buildStatus reply parsed and fingerprinted on the worker: a read that changed nothing then costs the frame
        // thread a pointer move and no allocation. The payload is behind a pointer so the result moves
        // between the worker and update without a debug container proxy for each of its strings.
        struct BuildRead
        {
            struct Parsed
            {
                Json next; // the reply
                std::string print; // next without the ages, compact
            };

            std::optional<std::string> error; // a read that threw; not a bare string: Debug heaps a proxy per string
            std::unique_ptr<Parsed> parsed; // null: the reply was not a status
            u64 sequence = 0;
            u64 now = 0; // the FILETIME the ages are counted to
            bool unchanged = false; // the stat fingerprint matched the worker's last read: nothing was read or parsed
            u64 allocations = 0; // the worker's own heap allocations for this read
        };

        bool sameProject(const std::string_view a, const std::string_view b)
        {
            return std::ranges::equal(
                a, b, [](const char x, const char y)
                { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
        }

        std::string text(const Json& object, const char* key)
        {
            const auto it = object.find(key);
            return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
        }

        u32 recordPid(const Json& project)
        {
            const auto pid = project.find("pid");
            return pid != project.end() && pid->is_number_unsigned() && pid->get<u64>() <= UINT32_MAX ? pid->get<u32>()
                                                                                                      : 0u;
        }

        u64 filetime(const Json& object, const char* key)
        {
            const auto value = object.find(key);
            return value != object.end() && value->is_number_unsigned() ? value->get<u64>() : 0;
        }

        bool plainNote(const std::string_view value, const sizet max)
        {
            return !value.empty() && value.size() <= max &&
                std::ranges::none_of(value, [](const unsigned char c) { return c < 32 || c == 127; });
        }

        // the first error diagnostic of a failed project as "code: message", else its exit code
        std::string failureText(const Json& project, const sizet maxBytes = MAX_MESSAGE_BYTES)
        {
            std::string result;
            if (const auto diagnostics = project.find("diagnostics"); diagnostics != project.end())
            {
                for (const Json& entry : *diagnostics)
                {
                    if (text(entry, "severity") == "error")
                    {
                        const std::string code = text(entry, "code");
                        result = (code.empty() ? "" : code + ": ") + text(entry, "message");
                        break;
                    }
                }
            }
            if (result.empty())
            {
                const auto exit = project.find("exit");
                result = exit != project.end() && exit->is_number_integer()
                    ? std::format("exit code {}", exit->get<std::int64_t>())
                    : std::string("no diagnostics");
            }
            if (result.size() > maxBytes)
            {
                sizet end = maxBytes;
                while ((static_cast<unsigned char>(result[end]) & 0xc0u) == 0x80u)
                    --end;
                result.resize(end);
                result += "...";
            }
            return result;
        }

        void writeDiagnostics(IFormatWriter& out, const Json* project)
        {
            const Json* list = project && project->contains("diagnostics") ? &(*project)["diagnostics"] : nullptr;
            const sizet shown = list && list->is_array() ? std::min(list->size(), SHOWN_DIAGNOSTICS) : 0;
            out.beginArray("buildDiagnostics", static_cast<u32>(shown));
            for (sizet i = 0; i < shown; ++i)
            {
                out.beginObject("");
                out.writeString("file", text((*list)[i], "file"));
                out.writeString("severity", text((*list)[i], "severity"));
                out.writeString("code", text((*list)[i], "code"));
                out.writeString("message", text((*list)[i], "message"));
                const auto line = (*list)[i].find("line");
                out.writeU32("line", line != (*list)[i].end() && line->is_number_unsigned() ? line->get<u32>() : 0u);
                out.endObject();
            }
            out.endArray();
            out.writeU32("buildDiagnosticsTotal",
                         project && project->contains("diagnosticsTotal") &&
                                 (*project)["diagnosticsTotal"].is_number_unsigned()
                             ? (*project)["diagnosticsTotal"].get<u32>()
                             : 0u);
        }

        // a project without an age (no record) counts as just now: only a change of the shown age matters
        BuildAge ageOf(const Json& project)
        {
            const auto age = project.find("ageSec");
            return age != project.end() && age->is_number() ? buildAgeOf(age->get<double>()) : BuildAge{};
        }

#if !defined(_WIN32)
        std::string utcStamp()
        {
            const auto now = std::chrono::system_clock::now();
            const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
            return std::format("{:%FT%T}.{:03}Z", seconds,
                               std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count());
        }
#endif

        BuildRecord toRecord(const Json& project)
        {
            BuildRecord record;
            record.project = text(project, "project");
            record.state = text(project, "state");
            record.configuration = text(project, "configuration");
            record.module = text(project, "module");
            record.needsReload = project.value("needsReload", false);
            record.untracked = project.value("untracked", false);
            record.abandoned = project.value("abandoned", false);
            if (const auto age = project.find("ageSec"); age != project.end() && age->is_number())
            {
                record.ageSec = age->get<double>();
            }
            if (const auto list = project.find("diagnostics"); list != project.end() && list->is_array())
            {
                for (sizet i = 0; i < list->size() && i < MAX_RECORDED_DIAGNOSTICS; ++i)
                {
                    const Json& entry = (*list)[i];
                    const auto line = entry.find("line");
                    record.diagnostics.push_back(
                        {text(entry, "file"), text(entry, "severity"), text(entry, "code"), text(entry, "message"),
                         line != entry.end() && line->is_number_unsigned() ? line->get<u32>() : 0u});
                }
            }
            const auto total = project.find("diagnosticsTotal");
            record.diagnosticsTotal = total != project.end() && total->is_number_unsigned() ? total->get<u32>() : 0u;
            record.pid = recordPid(project);
            return record;
        }

        // the host's own DLLs: the running host cannot be relinked (SpiteRenderPipeline hot-swaps, so it may build)
        constexpr std::array<std::string_view, 2> HOST_OWNED{"SpiteEngine", "SpiteEditor"};
        constexpr std::array<std::string_view, 5> CONFIGURATIONS{"Debug", "Release", "DebugProfile", "ReleaseProfile",
                                                                 "ASan"};
        constexpr u32 CANCEL_WAIT_MS = 5000;
        constexpr std::chrono::milliseconds SCAN_BUDGET{150}; // knownProject's scan of the binaries directory

        bool projectName(const std::string_view name)
        {
            return !name.empty() &&
                std::ranges::all_of(
                    name, [](const char c)
                    { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'; });
        }

        // a DLL's stem as UTF-8 (path::string() throws for a name outside the code page)
        std::string stemOf(const std::filesystem::path& path)
        {
            const std::u8string stem = path.stem().u8string();
            return {stem.begin(), stem.end()};
        }

        // The configured build directory on Linux, or the checkout holding Build.ps1 on Windows.
        std::filesystem::path checkoutRoot(const std::filesystem::path& from)
        {
            std::error_code error;
            for (std::filesystem::path at = from; !at.empty(); at = at.parent_path())
            {
#if defined(_WIN32)
                if (std::filesystem::is_regular_file(at / "scripts" / "Build.ps1", error))
#else
                if (std::filesystem::is_regular_file(at / "CMakeCache.txt", error))
#endif
                {
                    return at;
                }
                if (at == at.parent_path())
                {
                    break;
                }
            }
            return {};
        }

        void writeAge(IFormatWriter& out, const Json* project)
        {
            if (project && project->contains("ageSec") && (*project)["ageSec"].is_number())
            {
                out.writeF64("ageSec", (*project)["ageSec"].get<double>());
            }
            else
            {
                out.writeNull("ageSec");
            }
        }

        // The status directory, the DLLs beside it and the loaded modules as one number, made without a heap
        // allocation through operator new (the file-system calls allocate inside Windows, uncounted): the
        // worker compares it with that of its last full read and reads nothing when it matches. It covers every input
        // of writeBuildStatus but the clock and the liveness of a building pid, so the worker never skips while a build
        // is recorded. It does not key on the directory's own write time (an IDE build changes a DLL without touching
        // .build): each status file's name, size and write time, the write time of the DLL each names and of
        // each module's file. False: a path too long for the fixed buffers.
        constexpr sizet STAT_PATH_CHARS = 1024;
        using PathChar = std::filesystem::path::value_type;
        using PathView = std::basic_string_view<PathChar>;

        struct StatHash
        {
            u64 value = 14695981039346656037ull;

            void mix(const u64 word) { value = (value ^ word) * 1099511628211ull; }

            void mix(const PathView chars)
            {
                for (const PathChar c : chars)
                {
                    mix(static_cast<u64>(c));
                }
                mix(chars.size());
            }
        };

#if defined(_WIN32)
        u64 writeTimeOf(const PathChar* path)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data) ||
                (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                return 0;
            }
            return (u64{data.ftLastWriteTime.dwHighDateTime} << 32) | data.ftLastWriteTime.dwLowDateTime;
        }
#else
        void mixFile(StatHash& hash, const struct stat& file)
        {
            hash.mix(static_cast<u64>(file.st_size));
#if defined(__APPLE__)
            hash.mix(static_cast<u64>(file.st_mtimespec.tv_sec));
            hash.mix(static_cast<u64>(file.st_mtimespec.tv_nsec));
#else
            hash.mix(static_cast<u64>(file.st_mtim.tv_sec));
            hash.mix(static_cast<u64>(file.st_mtim.tv_nsec));
#endif
        }

        u64 writeTimeOf(const PathChar* path)
        {
            struct stat file{};
            if (stat(path, &file) != 0 || !S_ISREG(file.st_mode))
                return 0;
            StatHash hash;
            mixFile(hash, file);
            return hash.value;
        }
#endif

        // the parts one after the other and a terminating zero; false when they do not fit
        bool compose(std::array<PathChar, STAT_PATH_CHARS>& buffer, const std::initializer_list<PathView> parts)
        {
            sizet at = 0;
            for (const PathView part : parts)
            {
                if (at + part.size() + 1 > buffer.size())
                {
                    return false;
                }
                part.copy(buffer.data() + at, part.size());
                at += part.size();
            }
            buffer[at] = PathChar{};
            return true;
        }

        bool statFingerprint(const std::filesystem::path& directory, const std::span<const AvailableModule> modules,
                             u64& result)
        {
            u64 sum = modules.size();
#if defined(_WIN32)
            const std::wstring_view base = directory.native();
            if (base.empty() || base.back() == L'\\' || base.back() == L'/')
            {
                return false;
            }
            const sizet separator = base.find_last_of(L"\\/");
            const std::wstring_view parent =
                separator == std::wstring_view::npos ? std::wstring_view{} : base.substr(0, separator);
            std::array<wchar_t, STAT_PATH_CHARS> buffer;
            if (!compose(buffer, {base, L"\\*.json"}))
            {
                return false;
            }
            WIN32_FIND_DATAW found{};
            const HANDLE handle = FindFirstFileExW(buffer.data(), FindExInfoBasic, &found, FindExSearchNameMatch,
                                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
            if (handle == INVALID_HANDLE_VALUE)
            {
                StatHash missing; // no directory, or no status file: the two list the "none" rows differently
                missing.mix(u64{GetLastError()});
                sum += missing.value;
            }
            else
            {
                constexpr std::wstring_view EXTENSION = L".json";
                bool fits = true;
                do
                {
                    const std::wstring_view name = found.cFileName;
                    if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name.size() <= EXTENSION.size() ||
                        !std::ranges::equal(name.substr(name.size() - EXTENSION.size()), EXTENSION,
                                            [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); }))
                    {
                        continue;
                    }
                    if (!compose(buffer, {parent, L"\\", name.substr(0, name.size() - EXTENSION.size()), L".dll"}))
                    {
                        fits = false;
                        break;
                    }
                    StatHash file; // summed over the files, so the order the file system lists them in is no input
                    file.mix(name);
                    file.mix((u64{found.nFileSizeHigh} << 32) | found.nFileSizeLow);
                    file.mix((u64{found.ftLastWriteTime.dwHighDateTime} << 32) | found.ftLastWriteTime.dwLowDateTime);
                    file.mix(writeTimeOf(buffer.data()));
                    sum += file.value;
                }
                while (FindNextFileW(handle, &found));
                FindClose(handle);
                if (!fits)
                {
                    return false;
                }
            }
#else
            const PathView base = directory.native();
            if (base.empty() || base.back() == '/')
                return false;
            const auto slash = base.find_last_of('/');
            const PathView parent = slash == base.npos ? PathView{"."} : base.substr(0, slash);
            if (DIR* handle = opendir(directory.c_str()))
            {
                bool fits = true;
                std::array<PathChar, STAT_PATH_CHARS> buffer;
                for (;;)
                {
                    errno = 0;
                    const dirent* found = readdir(handle);
                    if (!found)
                    {
                        fits = errno == 0;
                        break;
                    }
                    const PathView name = found->d_name;
                    constexpr PathView extension = ".json";
                    if (!name.ends_with(extension) || name.size() == extension.size())
                        continue;
                    struct stat status{};
                    if (fstatat(dirfd(handle), found->d_name, &status, 0) != 0)
                    {
                        fits = false;
                        break;
                    }
                    if (!S_ISREG(status.st_mode))
                        continue;
                    const auto stem = name.substr(0, name.size() - extension.size());
                    StatHash file;
                    file.mix(name);
                    mixFile(file, status);
                    if (!compose(buffer, {parent, "/", stem, ".so"}))
                    {
                        fits = false;
                        break;
                    }
                    file.mix(writeTimeOf(buffer.data()));
                    if (!compose(buffer, {parent, "/lib", stem, ".so"}))
                    {
                        fits = false;
                        break;
                    }
                    file.mix(writeTimeOf(buffer.data()));
                    sum += file.value;
                }
                closedir(handle);
                if (!fits)
                    return false;
            }
            else
            {
                StatHash missing;
                missing.mix(static_cast<u64>(errno));
                sum += missing.value;
            }
#endif
            for (const AvailableModule& module : modules)
            {
                StatHash entry;
                entry.mix(module.path.native());
                entry.mix(u64{module.loaded});
                entry.mix(module.loadedWriteFiletime);
                entry.mix(writeTimeOf(module.path.c_str()));
                for (const char c : module.name)
                {
                    entry.mix(static_cast<u64>(static_cast<unsigned char>(c)));
                }
                sum += entry.value;
            }
            result = sum;
            return true;
        }

        BuildRead readStatus(const std::filesystem::path& directory, const std::vector<AvailableModule>& modules,
                             const u64 sequence)
        {
            BuildRead result;
            result.sequence = sequence;
            result.now = currentFiletime();
            JsonFragmentWriter out;
            writeBuildStatus(directory, "", modules, result.now, out);
            std::string reply;
            out.dumpReply(reply);
            Json next = Json::parse(reply, nullptr, false);
            if (!next.is_object() || !next.contains("projects") || !next["projects"].is_array())
            {
                return result;
            }
            Json comparable = next;
            for (Json& project : comparable["projects"])
            {
                project.erase("ageSec");
            }
            result.parsed = std::make_unique<BuildRead::Parsed>(BuildRead::Parsed{std::move(next), comparable.dump()});
            return result;
        }

        // One thread that lives while the model watches: starting a read is a notify, so a quiet second costs the
        // frame thread no thread creation and no allocation. It reads one request at a time; the frame
        // thread holds the mutex only to hand a request over or collect a result, never across a read.
        //
        // The worker's baseline (the fingerprint of its last full read) must be the read whose result became the
        // frame's snapshot: when the frame drops a result (a direct read ran after it started, or it failed) the next
        // request asks for a full read, which makes the dropped baseline a fresh one.
        using WorkerHook = std::function<void()>;

        class BuildWorker
        {
        public:
            BuildWorker() : m_thread([this] { run(); }) {}
            BuildWorker(const BuildWorker&) = delete;
            BuildWorker& operator=(const BuildWorker&) = delete;

            ~BuildWorker()
            {
                {
                    const std::lock_guard lock(m_mutex);
                    m_quit = true;
                }
                m_wake.notify_one();
                m_thread.join();
            }

            // the replaced snapshot rides along (moved, never copied) and is freed on this thread, so the frame
            // thread never frees the tree
            void request(std::shared_ptr<const std::filesystem::path> directory,
                         std::shared_ptr<const std::vector<AvailableModule>> modules, const u64 sequence,
                         Json&& garbage, const bool full, std::shared_ptr<const WorkerHook> beforeRead)
            {
                {
                    const std::lock_guard lock(m_mutex);
                    m_full = full;
                    m_beforeRead = std::move(beforeRead);
                    m_directory = std::move(directory);
                    m_modules = std::move(modules);
                    m_sequence = sequence;
                    m_garbage = std::move(garbage);
                    m_requested = true;
                }
                m_wake.notify_one();
            }

            std::optional<BuildRead> poll()
            {
                const std::lock_guard lock(m_mutex);
                if (!m_ready)
                {
                    return std::nullopt;
                }
                m_ready = false;
                return std::optional<BuildRead>(std::move(m_result));
            }

        private:
            void run()
            {
                std::unique_lock lock(m_mutex);
                for (;;)
                {
                    m_wake.wait(lock, [this] { return m_quit || m_requested; });
                    if (m_quit)
                    {
                        return;
                    }
                    m_requested = false;
                    const std::shared_ptr<const std::filesystem::path> directory = std::move(m_directory);
                    const std::shared_ptr<const std::vector<AvailableModule>> modules = std::move(m_modules);
                    const u64 sequence = m_sequence;
                    Json garbage = std::move(m_garbage);
                    const bool full = m_full;
                    const std::shared_ptr<const WorkerHook> beforeRead = std::move(m_beforeRead);
                    lock.unlock();
                    BuildRead result;
                    try
                    {
                        if (beforeRead)
                        {
                            (*beforeRead)();
                        }
                        result = work(*directory, *modules, sequence, std::move(garbage), full);
                    }
                    catch (const std::exception& error)
                    {
                        result = BuildRead{};
                        result.error = error.what();
                        m_haveStat = false;
                    }
                    catch (...)
                    {
                        result = BuildRead{};
                        result.error = "unknown error";
                        m_haveStat = false;
                    }
                    lock.lock();
                    m_result = std::move(result);
                    m_ready = true;
                }
            }

            BuildRead work(const std::filesystem::path& directory, const std::vector<AvailableModule>& modules,
                           const u64 sequence, Json&& garbage, const bool full)
            {
                SPITE_ZONE("EditorBuildState.read");
                u64 allocations = 0;
                const prof::AllocationCountScope counted(allocations);
                garbage = Json();
                u64 stat = 0;
                const bool stated = statFingerprint(directory, modules, stat);
                BuildRead result;
                if (!full && stated && m_haveStat && stat == m_stat && !m_building)
                {
                    result.sequence = sequence;
                    result.now = currentFiletime();
                    result.unchanged = true;
                }
                else
                {
                    result = readStatus(directory, modules, sequence);
                    // a file that could not be opened or read (a virus scanner's lock) is no baseline: it is read again
                    // next time although its stat does not change
                    const bool transient = result.parsed &&
                        std::ranges::any_of(result.parsed->next["issues"], [](const Json& issue)
                                            { return text(issue, "message").starts_with("cannot be"); });
                    m_haveStat = stated && result.parsed && !transient;
                    m_stat = stat;
                    // a running build only: an abandoned record (dead pid) never revives, so it is no reason to read
                    m_building =
                        result.parsed &&
                        std::ranges::any_of(
                            result.parsed->next["projects"], [](const Json& project)
                            { return text(project, "state") == "building" && !project.value("abandoned", false); });
                }
                result.allocations = allocations;
                return result;
            }

            std::mutex m_mutex;
            std::condition_variable m_wake;
            bool m_quit = false, m_requested = false, m_ready = false;
            std::shared_ptr<const std::filesystem::path> m_directory;
            std::shared_ptr<const std::vector<AvailableModule>> m_modules;
            u64 m_sequence = 0;
            bool m_full = false;
            std::shared_ptr<const WorkerHook> m_beforeRead;
            Json m_garbage;
            BuildRead m_result;
            // the worker thread's alone: the fingerprint of its last full read and whether that read showed a build
            u64 m_stat = 0;
            bool m_haveStat = false, m_building = false;
            std::thread m_thread; // last: it runs from construction
        };
    } // namespace

    struct EditorBuildState::State
    {
        State(Engine& engine, EditorFeedback& feedback) :
            engine(&engine), control(&engine.control()), host(&engine.modules()), feedback(&feedback)
        {
        }

        Engine* engine;
        ControlServer* control;
        ModuleHost* host;
        EditorFeedback* feedback;
        Json snapshot = Json::object({{"projects", Json::array()}});
        Json retired; // the snapshot apply replaced, until the next worker read takes it to free
        std::string fingerprint;
        std::vector<u32> ageKey; // the shown ages of the snapshot's projects, unit and count packed
        std::vector<std::optional<double>> baseAges; // ageSec of each project at snapshotNow, none without one
        u64 snapshotNow = 0; // the FILETIME the snapshot was read at
        bool haveSnapshot = false;
        u64 revision = 0;
        u64 ageRevisions = 0;
        u64 workerReads = 0, directReads = 0, skippedReads = 0, workerAllocations = 0;
        ModuleWork work;
        u32 workPid = 0;
        u64 workBaseBinary = 0;
        std::string workDiagnostic, workOutput;
        struct BuildWait
        {
            DeferredReply reply;
            u64 operation;
            std::chrono::steady_clock::time_point deadline;
        };
        std::optional<BuildWait> buildWait;
        bool forceFull = false; // the last worker result was dropped: its fingerprint is not the snapshot's
        std::shared_ptr<const WorkerHook> beforeRead; // test seam, null in the editor
        bool workerBusy = false; // a request is out; its result is collected by update
        u64 started = 0; // sequence of the last worker read
        u64 supersededBy = 0; // worker reads up to this sequence are older than a direct one
        u64 seenGeneration = ~0ull;
        double lastStart = -PERIOD_SECONDS;
        std::unique_ptr<BuildWorker> worker; // started by the first watched read

        std::filesystem::path statusOverride;
        std::shared_ptr<const std::filesystem::path> directoryShared;
        std::shared_ptr<const std::vector<AvailableModule>> modulesShared;
        u64 modulesGeneration = 0;
        std::function<std::vector<AvailableModule>()> loadedSource;

        std::filesystem::path statusDirectory() const
        {
            return statusOverride.empty() ? host->searchDirectory() / ".build" : statusOverride;
        }

        // the loaded modules, copied for the worker: ModuleHost is the frame thread's
        std::vector<AvailableModule> loadedModules() const
        {
            if (loadedSource)
            {
                return loadedSource();
            }
            std::vector<AvailableModule> loaded;
            for (const ModuleHost::LoadedModule& module : host->modules())
            {
                loaded.push_back(
                    {module.name, module.source, true, module.descriptor->apiStamp, {}, {}, module.writeFiletime});
            }
            return loaded;
        }

        // Shared with the worker so a start copies pointers: the directory never changes, the loaded modules change
        // with the host's generation (a test source is asked every time).
        void refreshWorkerInputs()
        {
            if (!directoryShared)
            {
                directoryShared = std::make_shared<const std::filesystem::path>(statusDirectory());
            }
            if (loadedSource || !modulesShared || modulesGeneration != host->generation())
            {
                modulesGeneration = host->generation();
                modulesShared = std::make_shared<const std::vector<AvailableModule>>(loadedModules());
            }
        }

        void startWorker()
        {
            refreshWorkerInputs();
            if (!worker)
            {
                worker = std::make_unique<BuildWorker>();
            }
            worker->request(directoryShared, modulesShared, ++started, std::move(retired), forceFull, beforeRead);
            forceFull = false;
            workerBusy = true;
            ++workerReads;
            lastStart = feedback->now();
            seenGeneration = host->generation();
        }

        const Json* find(const std::string_view stem) const
        {
            for (const Json& project : snapshot["projects"])
            {
                if (sameProject(text(project, "project"), stem))
                {
                    return &project;
                }
            }
            return nullptr;
        }

        void publishWork()
        {
            if (!work.operation)
                return;
            std::string_view phase = "inspect", status = "running", action = "beginAgentModuleWork";
            std::string evidence;
            if (work.build == "pending" && !work.notes[1].summary.empty())
            {
                phase = "apply";
                action = "buildProject";
            }
            else if (work.build == "building")
            {
                phase = "apply";
                status = "waiting";
                action = "buildProject";
            }
            else if (work.build == "ok" && !work.loaded)
            {
                phase = "apply";
                status = "waiting";
                action = "reloadModule";
            }
            else if (work.loaded)
            {
                phase = "review";
                action = "checkAgentModuleWork";
                if (std::ranges::any_of(work.checks, [](const ModuleWork::Check& check) { return !check.passed; }))
                    status = "failed";
                else if (!work.notes[2].summary.empty())
                    status = "passed";
                const Localization& messages = engine->localization();
                std::string scratch;
                evidence = messages.resolve(InternedString(work.build == "active" ? "editor-agent.module.build.active"
                                                                                  : "editor-agent.module.loaded"),
                                            scratch);
                for (const auto& check : work.checks)
                {
                    evidence += '\n';
                    const MessageArg args[]{{"label", std::string_view(check.label)}};
                    evidence += messages.resolve(
                        InternedString(check.passed ? "editor-agent.module.passed" : "editor-agent.module.failed"),
                        args, scratch);
                }
            }
            else if (work.build != "pending")
            {
                phase = "apply";
                action = "buildProject";
                status =
                    work.build == "cancelled" || work.build == "unchanged" ? std::string_view(work.build) : "failed";
                evidence = workDiagnostic;
            }
            control->updateAgentActivity(work.operation, phase, status, action, evidence);
        }

        u64 activeModuleStamp() const
        {
            if (loadedSource)
            {
                const auto modules = loadedSource();
                const auto module = std::ranges::find(modules, work.module, &AvailableModule::name);
                return module != modules.end() && module->loaded ? module->loadedWriteFiletime : 0;
            }
            const auto modules = host->modules();
            const auto module = std::ranges::find(modules, work.module, &ModuleHost::LoadedModule::name);
            return module != modules.end() ? module->writeFiletime : 0;
        }

        void observeModuleWork()
        {
            if (workPid == 0 && work.build != "active")
                return;
            const Json* project = work.build == "active" ? nullptr : find(work.project);
            const u64 active = work.build == "active" ? activeModuleStamp()
                : project                             ? filetime(*project, "loadedWriteFiletime")
                                                      : 0;
            if (work.build == "building" || work.build == "unrecorded")
            {
                if (project && recordPid(*project) == workPid)
                {
                    const std::string state = text(*project, "state");
                    if (state == "ok")
                    {
                        const u64 stamp = filetime((*project)["binary"], "writeFiletime");
                        const std::string_view outcome = stamp == 0 ? "unrecorded"
                            : stamp == workBaseBinary               ? "unchanged"
                                                                    : "ok";
                        if (work.build != outcome)
                        {
                            work.binary = stamp;
                            work.build = outcome;
                            publishWork();
                        }
                    }
                    else if (state == "failed")
                    {
                        work.build = "failed";
                        workDiagnostic = failureText(*project, 512);
                        publishWork();
                    }
                }
                const StartedBuild* pending = startedBuild(work.project);
                if (work.build == "building" &&
                    (!pending || pending->process->pid() != workPid || !pending->process->running()))
                {
                    work.build = "unrecorded";
                    publishWork();
                }
            }
            const bool loaded = (work.build == "ok" || work.build == "active") && work.binary == active;
            if (work.loaded != loaded)
            {
                work.loaded = loaded;
                publishWork();
            }
        }

        // The worker found every input of the reply as it was at the snapshot: only the ages moved, by the time that
        // passed since the snapshot was read. They are written in place, so a quiet second allocates nothing.
        void applyUnchanged(const u64 now)
        {
            if (!haveSnapshot || now <= snapshotNow)
            {
                return;
            }
            const double elapsed = static_cast<double>(now - snapshotNow) / 1.0e7;
            Json& projects = snapshot.at("projects"); // operator[] builds a key string, a heap proxy in Debug
            bool agesMoved = false;
            for (sizet i = 0; i < projects.size() && i < baseAges.size(); ++i)
            {
                const auto age = projects[i].find("ageSec");
                if (!baseAges[i] || age == projects[i].end())
                {
                    continue;
                }
                *age = std::round((*baseAges[i] + elapsed) * 1000.0) / 1000.0;
                const BuildAge shown = buildAgeOf(age->get<double>());
                const u32 packed = (shown.count << 2) | static_cast<u32>(shown.unit);
                agesMoved |= ageKey[i] != packed;
                ageKey[i] = packed;
            }
            ageRevisions += agesMoved;
        }

        void apply(BuildRead&& read)
        {
            if (read.unchanged)
            {
                applyUnchanged(read.now);
                observeModuleWork();
                return;
            }
            if (!read.parsed)
            {
                return;
            }
            BuildRead::Parsed& parsed = *read.parsed;
            const Json& projects = parsed.next["projects"];
            bool agesMoved = ageKey.size() != projects.size();
            ageKey.resize(projects.size()); // allocates only when the project count grows
            baseAges.resize(projects.size());
            snapshotNow = read.now;
            for (sizet i = 0; i < projects.size(); ++i)
            {
                const auto base = projects[i].find("ageSec");
                baseAges[i] =
                    base != projects[i].end() && base->is_number() ? std::optional(base->get<double>()) : std::nullopt;
                const BuildAge age = ageOf(projects[i]);
                const u32 packed = (age.count << 2) | static_cast<u32>(age.unit);
                agesMoved |= ageKey[i] != packed;
                ageKey[i] = packed;
            }
            ageRevisions += agesMoved;
            const bool changed = parsed.print != fingerprint;
            if (changed && haveSnapshot)
            {
                postFinished(parsed.next);
            }
            retired = std::move(snapshot); // freed by the next worker read, not here
            snapshot = std::move(parsed.next);
            fingerprint = std::move(parsed.print);
            haveSnapshot = true;
            revision += changed;
            observeModuleWork();
        }

        // a project that finished (ok or failed) since the snapshot: it was building, or its finish time moved
        void postFinished(const Json& next)
        {
            std::vector<std::string> succeeded;
            sizet failed = 0;
            for (const Json& project : next["projects"])
            {
                const std::string state = text(project, "state");
                if (state != "ok" && state != "failed")
                {
                    continue;
                }
                const Json* before = find(text(project, "project"));
                if (before && text(*before, "state") == state &&
                    text(*before, "finishedUtc") == text(project, "finishedUtc"))
                {
                    continue;
                }
                const std::string name = text(project, "project");
                if (state == "failed" && ++failed > MAX_FAILED_TOASTS)
                {
                    continue;
                }
                if (state == "failed")
                {
                    const std::string message = failureText(project);
                    feedback->postToastKey(ToastLevel::Error, "editor.modules.build.failed",
                                           std::vector<MessageArg>{{"project", std::string_view(name)},
                                                                   {"message", std::string_view(message)}});
                }
                else
                {
                    succeeded.push_back(name);
                }
            }
            if (failed > MAX_FAILED_TOASTS)
            {
                feedback->postToastKey(
                    ToastLevel::Error, "editor.modules.build.failedMore",
                    std::vector<MessageArg>{{"count", static_cast<std::int64_t>(failed - MAX_FAILED_TOASTS)}});
            }
            if (succeeded.empty())
            {
                return;
            }
            std::string names;
            for (sizet i = 0; i < succeeded.size() && i < MAX_TOAST_NAMES; ++i)
            {
                names += (i ? ", " : "") + succeeded[i];
            }
            const auto more =
                static_cast<std::int64_t>(succeeded.size() > MAX_TOAST_NAMES ? succeeded.size() - MAX_TOAST_NAMES : 0);
            feedback->postToastKey(ToastLevel::Info, "editor.modules.build.ok",
                                   std::vector<MessageArg>{{"names", std::string_view(names)}, {"more", more}});
        }

        // the modules to reload, by module name
        std::vector<std::string> reloadable() const
        {
            std::vector<std::string> names;
            for (const Json& project : snapshot["projects"])
            {
                if (project.value("needsReload", false) && project.contains("module") && project["module"].is_string())
                {
                    names.push_back(project["module"].get<std::string>());
                }
            }
            std::ranges::sort(names);
            return names;
        }

        std::string reloadBuilt(IFormatWriter& out)
        {
            refreshNow();
            std::vector<std::string> reloaded;
            std::string failedName, failedMessage;
            for (const std::string& name : reloadable())
            {
                const Json reply =
                    Json::parse(control->handle(Json{{"command", "reloadModule"}, {"args", {{"name", name}}}}.dump()),
                                nullptr, false);
                if (reply.is_object() && reply.value("ok", false))
                {
                    reloaded.push_back(name);
                    continue;
                }
                failedName = name;
                failedMessage = reply.is_object() ? text(reply, "error") : std::string("no reply");
                break;
            }
            refreshNow();
            out.beginArray("reloaded", static_cast<u32>(reloaded.size()));
            for (const std::string& name : reloaded)
            {
                out.writeString("", name);
            }
            out.endArray();
            if (failedName.empty())
            {
                out.writeNull("failed");
            }
            else
            {
                out.beginObject("failed");
                out.writeString("name", failedName);
                out.writeString("message", failedMessage);
                out.endObject();
            }
            return std::string{};
        }

        void refreshNow()
        {
#if !defined(_WIN32)
            reap();
#endif
            ++directReads;
            supersededBy = started;
            apply(readStatus(statusDirectory(), loadedModules(), 0));
        }

        // A build the editor started (buildProject); the handles stay open until it is reaped, so its pid is ours.
        struct StartedBuild
        {
            std::string project, configuration;
            std::unique_ptr<EditorBuildProcess> process;
#if !defined(_WIN32)
            std::filesystem::path statusFile;
            Json statusRecord;
            std::chrono::steady_clock::time_point startedAt;
#endif
        };
        std::vector<StartedBuild> builds;
        struct OutputLog
        {
            std::string project;
            std::filesystem::path path;
            u32 pid;
            u32 previousPid = 0;
        };
        std::vector<OutputLog> outputLogs;

        void reap()
        {
#if defined(_WIN32)
            std::erase_if(builds, [](const StartedBuild& build) { return !build.process->running(); });
#else
            std::erase_if(
                builds,
                [&](StartedBuild& build)
                {
                    if (build.process->running())
                        return false;
                    const auto exit = build.process->exitCode();
                    if (!build.statusFile.empty() && exit)
                    {
                        const Json current = statusOf(build.configuration, build.project);
                        if (text(current, "state") == "building" && recordPid(current) == build.process->pid())
                        {
                            Json& record = build.statusRecord;
                            record["state"] = *exit == 0 ? "ok" : "failed";
                            record["finishedUtc"] = utcStamp();
                            record["durationSec"] =
                                std::chrono::duration<double>(std::chrono::steady_clock::now() - build.startedAt)
                                    .count();
                            record["exit"] = *exit;
                            if (*exit == 0)
                            {
                                const auto binaries = build.statusFile.parent_path().parent_path();
                                for (const std::string& name : {build.project + ".so", "lib" + build.project + ".so"})
                                {
                                    const auto binary = binaries / name;
                                    const auto stamp = fileWriteFiletime(binary);
                                    std::error_code io;
                                    const auto size = stamp ? std::filesystem::file_size(binary, io) : 0;
                                    if (stamp && !io)
                                    {
                                        record["binary"] = {{"file", name}, {"writeFiletime", *stamp}, {"size", size}};
                                        break;
                                    }
                                }
                            }
                            if (const auto written = writeTextFileAtomically(build.statusFile, record.dump()); !written)
                                SWARNING_LOG("cannot record the finished build of {}: {}", build.project,
                                             written.error().message);
                        }
                    }
                    return true;
                });
#endif
        }

        StartedBuild* startedBuild(const std::string_view project)
        {
            const auto it = std::ranges::find_if(builds, [&](const StartedBuild& build)
                                                 { return sameProject(build.project, project); });
            return it == builds.end() ? nullptr : &*it;
        }

        // the binaries directory the snapshot reads (<binaries>/.build) and that of another configuration beside it
        std::filesystem::path binaries() const { return statusDirectory().parent_path(); }
        std::filesystem::path statusDirectory(const std::string_view configuration) const
        {
#if defined(_WIN32)
            return binaries().filename() == configuration ? statusDirectory()
                                                          : binaries().parent_path() / configuration / ".build";
#else
            (void)configuration;
            return statusDirectory();
#endif
        }

        std::string hostConfiguration() const
        {
#if defined(_WIN32)
            const std::string name = binaries().filename().string();
            return std::ranges::find(CONFIGURATIONS, name) != CONFIGURATIONS.end() ? name
                : SPITE_DEBUG_BUILD                                                ? "Debug"
                                                                                   : "Release";
#else
            return SPITE_DEBUG_BUILD ? "Debug" : "Release";
#endif
        }

        // the status record of a project in a configuration's directory, an empty object without one
        Json statusOf(const std::string_view configuration, const std::string_view project) const
        {
            const BuildRead status = readStatus(statusDirectory(configuration), loadedModules(), 0);
            if (status.parsed)
            {
                for (const Json& entry : status.parsed->next["projects"])
                {
                    if (sameProject(text(entry, "project"), project))
                    {
                        return entry;
                    }
                }
            }
            return Json::object();
        }

        // the DLL stem of a project or module name the snapshot, the loaded modules or the module DLLs of the binaries
        // directory know (a DLL built in an IDE, or never built, has no record), empty when none does
        std::string knownProject(const std::string_view name) const
        {
            for (const Json& project : snapshot["projects"])
            {
                if (sameProject(text(project, "project"), name) || sameProject(text(project, "module"), name))
                {
                    return text(project, "project");
                }
            }
            for (const AvailableModule& module : loadedModules())
            {
                if (sameProject(module.name, name) || sameProject(stemOf(module.path), name))
                {
                    return stemOf(module.path);
                }
            }
            // a scan reads the export tables, never maps a DLL, and leaves out what the process maps (the host's own);
            // it runs on the calling (frame) thread, so it gets a short budget, not the scan's default
            for (const AvailableModule& module : ModuleHost::scan(binaries(), SCAN_BUDGET).modules)
            {
                if (sameProject(stemOf(module.path), name))
                {
                    return stemOf(module.path);
                }
            }
            return {};
        }

        std::string buildProject(const ControlArgs& args, IFormatWriter& out)
        {
            const std::string name = args.string("project").value_or("");
            const std::string configuration = args.string("configuration").value_or(hostConfiguration());
            refreshNow();
            reap();
            const std::string project = projectName(name) ? knownProject(name) : std::string{};
            if (project.empty())
            {
                return std::format("unknown project '{}': no status file or module DLL of that name in {}", name,
                                   binaries().string());
            }
            const std::optional<u64> operation = args.integer("operation");
            if (operation && activeOperation(*operation) && work.build == "active")
                return "verify-active operations use the selected binary";
            if (operation &&
                (!activeOperation(*operation) || !sameProject(work.project, project) ||
                 configuration != hostConfiguration() || work.notes[1].summary.empty()))
                return "a linked build needs the current module operation, its project, an Apply note and the host "
                       "configuration";
            const Json* before = operation ? find(project) : nullptr;
            const u64 priorBinary = before ? filetime(*before, "currentWriteFiletime") : 0;
            // the stem goes into Build.ps1's command line: a space or comma in it would add arguments
            if (!projectName(project))
            {
                return std::format("'{}' names the project '{}', which is not a project name Build.ps1 takes "
                                   "(letters, digits, _ - . only)",
                                   name, project);
            }
            if (std::ranges::any_of(HOST_OWNED,
                                    [&](const std::string_view owned) { return sameProject(owned, project); }))
            {
                return std::format("{} is a DLL of the running host, which cannot be relinked while it runs: close "
                                   "the editor and run spite build {}",
                                   project, project);
            }
            std::string overrideError;
            const std::filesystem::path override = EditorBuildProcess::scriptOverride(overrideError);
            if (!overrideError.empty())
            {
                return overrideError;
            }
            const std::filesystem::path root = override.empty() ? checkoutRoot(binaries()) : override.parent_path();
            if (root.empty())
            {
#if defined(_WIN32)
                return std::format("no scripts/Build.ps1 at or above {}: this layout has no build to start",
                                   binaries().string());
#else
                return std::format("no CMakeCache.txt at or above {}: this layout has no configured build to start",
                                   binaries().string());
#endif
            }
            if (const StartedBuild* running = startedBuild(project))
            {
                return std::format("the build of {} ({}) the editor started is still running (pid {}): cancelBuild "
                                   "ends it",
                                   project, running->configuration, running->process->pid());
            }
            const Json status = statusOf(configuration, project);
            if (text(status, "state") == "building" && !status.value("abandoned", false))
            {
                return std::format("{} ({}) is being built by pid {}", project, configuration,
                                   status.contains("pid") && status["pid"].is_number_unsigned()
                                       ? std::to_string(status["pid"].get<u64>())
                                       : std::string("unknown"));
            }
            std::string error;
#if defined(_WIN32)
            const std::filesystem::path script = override.empty() ? root / "scripts" / "Build.ps1" : override;
#else
            const std::filesystem::path& script = override;
#endif
            std::unique_ptr<EditorBuildProcess> process =
                EditorBuildProcess::start(script, root, project, configuration, error);
            if (!process)
            {
                return std::format("could not start the build of {}: {}", project, error);
            }
            StartedBuild build{project, configuration, std::move(process)};
#if defined(_WIN32)
            const auto scriptText = script.generic_u8string();
#else
            const auto scriptText = build.process->programPath().generic_u8string();
            if (override.empty())
            {
                build.statusFile = statusDirectory(configuration) / (project + ".json");
                build.startedAt = std::chrono::steady_clock::now();
                build.statusRecord = {{"schema", 1},
                                      {"project", project},
                                      {"configuration", configuration},
                                      {"state", "building"},
                                      {"startedUtc", utcStamp()},
                                      {"pid", build.process->pid()},
                                      {"diagnostics", Json::array()}};
                if (const auto written = writeTextFileAtomically(build.statusFile, build.statusRecord.dump()); !written)
                {
                    build.process->end(CANCEL_WAIT_MS);
                    return std::format("could not record the build of {}: {}", project, written.error().message);
                }
            }
#endif
            out.beginObject("started");
            out.writeString("project", project);
            out.writeString("configuration", configuration);
            out.writeU32("pid", build.process->pid());
            out.writeString("script", std::string(scriptText.begin(), scriptText.end()));
            out.writeBool("scriptOverride", !override.empty());
            const auto output = build.process->outputPath().generic_u8string();
            out.writeString("output", std::string(output.begin(), output.end()));
            out.endObject();
            const auto previous = std::ranges::find(outputLogs, project, &OutputLog::project);
            const auto* oldRecord = find(project);
            const u32 previousPid = oldRecord ? recordPid(*oldRecord) : 0;
            if (previous == outputLogs.end())
                outputLogs.push_back({project, build.process->outputPath(), build.process->pid(), previousPid});
            else
                *previous = {project, build.process->outputPath(), build.process->pid(), previousPid};
            builds.push_back(std::move(build));
            if (operation)
            {
                workPid = builds.back().process->pid();
                workBaseBinary = priorBinary;
                work.build = "building";
                work.binary = 0;
                work.loaded = false;
                work.checks.clear();
                work.notes[2] = {};
                workDiagnostic.clear();
                workOutput.assign(output.begin(), output.end());
                publishWork();
            }
            return {};
        }

        std::string cancelBuild(const ControlArgs& args, IFormatWriter& out)
        {
            const std::string name = args.string("project").value_or("");
            // resolved as buildProject resolves it: a module name finds the build of its DLL's project
            const std::string known = knownProject(name);
            StartedBuild* build = startedBuild(known.empty() ? name : known);
            if (!build)
            {
                return std::format("the editor started no build of '{}'", name);
            }
            const u32 pid = build->process->pid();
            if (!build->process->running())
            {
                const std::string project = build->project;
                reap();
                return std::format("the build of {} the editor started (pid {}) has already ended", project, pid);
            }
            // The editor holds this process's handle, so the pid is certainly its build's, also before Build.ps1 wrote
            // its record (it writes it once it holds a build slot). Refused only when the record names another live
            // build: then the status belongs to that one, which the editor did not start.
            const Json status = statusOf(build->configuration, build->project);
            const bool other = text(status, "state") == "building" && !status.value("abandoned", false) &&
                !(status.contains("pid") && status["pid"].is_number_unsigned() && status["pid"].get<u64>() == pid);
            if (other)
            {
                return std::format("the status file of {} ({}) shows another live build (pid {}), not the editor's "
                                   "(pid {}): nothing ended",
                                   build->project, build->configuration,
                                   status.contains("pid") && status["pid"].is_number_unsigned()
                                       ? std::to_string(status["pid"].get<u64>())
                                       : std::string("unknown"),
                                   pid);
            }
            const bool ended = build->process->end(CANCEL_WAIT_MS);
            if (ended && pid == workPid)
            {
                work.build = "cancelled";
                publishWork();
            }
            out.beginObject("cancelled");
            out.writeString("project", build->project);
            out.writeString("configuration", build->configuration);
            out.writeU32("pid", pid);
            out.writeBool("ended", ended);
            out.endObject();
            reap();
            return {};
        }

        bool activeOperation(const u64 id) const { return id != 0 && id == work.operation; }

        std::string beginModuleWork(const ControlArgs& args, IFormatWriter& out)
        {
            const std::string name = args.string("project").value_or("");
            const std::string title = args.string("title").value_or("");
            ModuleWork::Note note{args.string("summary").value_or(""), args.string("why").value_or("")};
            if (!plainNote(title, 96) || !plainNote(note.summary, 280) || !plainNote(note.why, 280))
                return "provide a short plain-text title, summary and rationale on one line each";
            const bool verifyActive = args.boolean("verifyActive").value_or(false);
            std::string project, module;
            u64 binary = 0;
            if (verifyActive)
            {
                const auto modules = loadedModules();
                const auto loaded = std::ranges::find_if(
                    modules,
                    [&name](const AvailableModule& entry)
                    {
                        return entry.loaded && (sameProject(entry.name, name) || sameProject(stemOf(entry.path), name));
                    });
                if (loaded == modules.end() || loaded->loadedWriteFiletime == 0)
                    return "verify-active needs a loaded module with a known binary write time";
                project = stemOf(loaded->path);
                module = loaded->name;
                binary = loaded->loadedWriteFiletime;
            }
            else
            {
                refreshNow();
                project = knownProject(name);
                const Json* entry = project.empty() ? nullptr : find(project);
                if (!entry || !entry->value("loaded", false))
                    return "module work needs a loaded module in the editor's build configuration";
                module = text(*entry, "module");
            }
            const auto rows = control->agentActivities();
            if (const auto previous = std::ranges::find(rows, work.operation, &ControlServer::AgentActivity::id);
                previous != rows.end() && previous->active() && previous->id != control->currentAgentActivity())
                control->updateAgentActivity(previous->id, previous->phase, "cancelled", previous->action,
                                             previous->evidence);
            const u64 operation = control->currentAgentActivity() != 0
                ? control->currentAgentActivity()
                : control->beginAgentActivity("module", title, note.why);
            if (!operation)
                return "the Agent feed has too many active operations";
            work = {.operation = operation, .project = std::move(project), .module = std::move(module), .title = title};
            work.notes[0] = std::move(note);
            if (verifyActive)
            {
                work.build = "active";
                work.binary = binary;
                work.loaded = true;
                out.writeU64("binary", binary);
            }
            workPid = 0;
            workDiagnostic.clear();
            workOutput.clear();
            publishWork();
            out.writeU64("operation", operation);
            return {};
        }

        std::string noteModuleWork(const ControlArgs& args, IFormatWriter&)
        {
            if (!activeOperation(args.integer("operation").value_or(0)))
                return "unknown module operation";
            ModuleWork::Note note{args.string("summary").value_or(""), args.string("why").value_or("")};
            if (!plainNote(note.summary, 280) || !plainNote(note.why, 280))
                return "provide a plain-text summary and rationale on one line each";
            const std::string stage = args.string("stage").value_or("");
            const sizet index = stage == "apply" ? 1 : 2;
            if (index == 2 && work.checks.empty())
                return "record a play-world check before adding a Review note";
            work.notes[index] = std::move(note);
            publishWork();
            return {};
        }

        std::string checkModuleWork(const ControlArgs& args, IFormatWriter& out)
        {
            if (!activeOperation(args.integer("operation").value_or(0)))
                return "unknown module operation";
            if (work.checks.size() >= 8)
                return "the module operation has eight recorded checks";
            const std::string label = args.string("label").value_or("");
            const std::string query = args.string("query").value_or("");
            const std::string pointer = args.string("pointer").value_or("");
            Json queryArgs = Json::parse(args.json("args").value_or("{}"), nullptr, false);
            const Json expected = Json::parse(args.json("equals").value_or(""), nullptr, false);
            if (!plainNote(label, 120) || !plainNote(pointer, 160) || pointer.front() != '/' ||
                !queryArgs.is_object() || expected.is_discarded())
                return "provide a short label, JSON pointer, query arguments object and expected JSON value";
            const auto world = queryArgs.find("world");
            if (world != queryArgs.end() && *world != "play")
                return "module checks run in the play world";
            if (work.build == "active")
                observeModuleWork();
            else
                refreshNow();
            if (!work.loaded || !engine->playWorld())
                return "activate the selected binary and start Play before checking it";
            if (query == "uiTree")
            {
                queryArgs["world"] = "play";
            }
            else
            {
                JsonFragmentWriter schema;
                control->describeOwner(work.module, schema);
                std::string result;
                schema.dumpReply(result);
                const Json described = Json::parse(result, nullptr, false);
                if (!described.is_object() ||
                    !std::ranges::any_of(described["entries"], [&](const Json& entry)
                                         { return entry["name"] == query && entry["kind"] == "query"; }))
                    return "module checks accept the module's queries or uiTree";
            }
            bool passed;
            try
            {
                const Json::json_pointer field(pointer);
                const Json reply =
                    Json::parse(control->handle(Json{{"query", query}, {"args", queryArgs}}.dump()), nullptr, false);
                if (!reply.is_object() || !reply.value("ok", false))
                    return "the runtime query was refused";
                const Json& result = reply["result"];
                passed = result.contains(field) && result.at(field) == expected;
            }
            catch (const Json::exception&)
            {
                return "invalid JSON pointer for the runtime result";
            }
            work.checks.push_back({label, passed});
            publishWork();
            out.writeBool("passed", passed);
            return {};
        }

        void writeModuleWork(IFormatWriter& out)
        {
            if (work.build == "active")
                observeModuleWork();
            else if (workPid != 0)
                refreshNow();
            out.writeU64("operation", work.operation);
            out.writeString("project", work.project);
            out.writeString("module", work.module);
            out.writeString("title", work.title);
            out.writeString("build", work.build);
            out.writeU64("binary", work.binary);
            out.writeBool("loaded", work.loaded);
            constexpr std::string_view stages[]{"inspect", "apply", "review"};
            for (sizet i = 0; i < work.notes.size(); ++i)
            {
                out.beginObject(stages[i]);
                out.writeString("summary", work.notes[i].summary);
                out.writeString("why", work.notes[i].why);
                out.endObject();
            }
            out.beginArray("checks", static_cast<u32>(work.checks.size()));
            for (const ModuleWork::Check& check : work.checks)
            {
                out.beginObject("");
                out.writeString("label", check.label);
                out.writeBool("passed", check.passed);
                out.endObject();
            }
            out.endArray();
        }

        void writeModuleBuild(IFormatWriter& out, const bool timedOut = false) const
        {
            out.writeU64("operation", work.operation);
            out.writeString("build", work.build);
            out.writeU64("binary", work.binary);
            out.writeString("module", work.module);
            out.writeBool("loaded", work.loaded);
            out.writeBool("timedOut", timedOut);
            out.writeString("diagnostic", workDiagnostic);
            out.writeString("log", workOutput);
        }

        std::string waitModuleBuild(const ControlArgs& args, IFormatWriter& out)
        {
            const u64 operation = args.integer("operation").value_or(0);
            const u64 seconds = args.integer("seconds").value_or(180);
            if (!activeOperation(operation) || workPid == 0)
                return "unknown module operation or no linked build";
            if (seconds == 0 || seconds > 600)
                return "wait seconds must be between 1 and 600";
            if (buildWait)
                return "a client is already waiting for the linked build";
            if (work.build == "building" || work.build == "unrecorded")
            {
                if (const DeferredReply reply = control->deferReply(); reply.isValid())
                {
                    buildWait =
                        BuildWait{reply, operation, std::chrono::steady_clock::now() + std::chrono::seconds(seconds)};
                    return {};
                }
                refreshNow();
            }
            writeModuleBuild(out);
            return {};
        }

        void finishBuildWait()
        {
            if (!buildWait)
                return;
            const BuildWait wait = *buildWait;
            const bool timedOut = std::chrono::steady_clock::now() >= wait.deadline;
            if (activeOperation(wait.operation) && !timedOut && work.build == "building")
                return;
            if (activeOperation(wait.operation) && work.build == "unrecorded")
                refreshNow(); // the process may have exited just after the worker read its receipt
            buildWait.reset();
            control->completeReply(wait.reply,
                                   [this, wait, timedOut](IFormatWriter& out)
                                   {
                                       if (!activeOperation(wait.operation))
                                           return std::string("the active module operation changed");
                                       writeModuleBuild(out, timedOut && work.build == "building");
                                       return std::string{};
                                   });
        }
    };

    EditorBuildState::EditorBuildState(Engine& engine, EditorFeedback& feedback) :
        m_state(std::make_unique<State>(engine, feedback))
    {
    }

    EditorBuildState::~EditorBuildState() { m_state->control->unregisterOwner(OWNER); }

    void EditorBuildState::registerCommands()
    {
        constexpr static std::string_view NOTE_STAGES[]{"apply", "review"};
        m_state->control->registerQuery(
            "agentModuleWork", OWNER,
            "latest game module operation in this editor: operation, project, module, title, agent notes (inspect, "
            "apply, review with summary and why), observed build status, binary write time, whether it is loaded, "
            "and recorded play-world checks (label and passed). Operation 0 means no operation yet",
            {},
            [this](const ControlArgs&, IFormatWriter& out)
            {
                m_state->writeModuleWork(out);
                return std::string{};
            });
        m_state->control->registerQuery(
            "waitAgentModuleBuild", OWNER,
            "waits for the linked build to finish and replies with operation, build, binary, module, loaded, timedOut, "
            "diagnostic and log. The frame thread keeps running. In-process clients receive the current status",
            {requiredArg("operation", ArgKind::Integer, "the operation id returned by beginAgentModuleWork"),
             optionalArg("seconds", ArgKind::Integer, "maximum wait, 1..600", "180")},
            [this](const ControlArgs& args, IFormatWriter& out) { return m_state->waitModuleBuild(args, out); });
        m_state->control->registerViewCommand(
            "beginAgentModuleWork", OWNER,
            "starts a local timeline for a loaded game module and records the agent's Inspect note. Replies with "
            "the operation id. verifyActive selects the loaded binary for checks and also replies with its stamp. "
            "Supply short, single-line plain language summaries",
            {requiredArg("project", ArgKind::String, "loaded module name or build project"),
             requiredArg("title", ArgKind::String, "the intended player-facing change, up to 96 bytes"),
             requiredArg("summary", ArgKind::String, "what the agent found, up to 280 bytes"),
             requiredArg("why", ArgKind::String, "why it matters, up to 280 bytes"),
             optionalArg("verifyActive", ArgKind::Boolean, "check the currently loaded binary", "false")},
            [this](const ControlArgs& args, IFormatWriter& out) { return m_state->beginModuleWork(args, out); });
        m_state->control->registerViewCommand(
            "noteAgentModuleWork", OWNER, "records a plain-language Apply or Review note for the current operation",
            {requiredArg("operation", ArgKind::Integer, "the operation id returned by beginAgentModuleWork"),
             requiredArg("stage", ArgKind::String, "apply before a linked build, review after a check", NOTE_STAGES),
             requiredArg("summary", ArgKind::String, "what changed or what was checked, up to 280 bytes"),
             requiredArg("why", ArgKind::String, "the reason or trade-off, up to 280 bytes")},
            [this](const ControlArgs& args, IFormatWriter& out) { return m_state->noteModuleWork(args, out); });
        m_state->control->registerViewCommand(
            "checkAgentModuleWork", OWNER,
            "checks a JSON result value while the linked binary is active in Play. Reads a module-owned query or "
            "uiTree and records only a plain-language label and whether it passed. Replies with passed",
            {requiredArg("operation", ArgKind::Integer, "the operation id returned by beginAgentModuleWork"),
             requiredArg("label", ArgKind::String, "what the check demonstrates, up to 120 bytes"),
             requiredArg("query", ArgKind::String, "a query the module owns, or uiTree"),
             optionalArg("args", ArgKind::Json, "query arguments as an object"),
             requiredArg("pointer", ArgKind::String, "JSON pointer inside the query result, for example /tree/0/text"),
             requiredArg("equals", ArgKind::Json, "the expected JSON value")},
            [this](const ControlArgs& args, IFormatWriter& out) { return m_state->checkModuleWork(args, out); });
        m_state->control->registerCommand(
            "moduleReloadBuilt", OWNER,
            "sends reloadModule for each loaded module whose DLL was rebuilt since it loaded (modulesState: "
            "needsReload), "
            "in module name order, and stops at the first refusal. Reply: reloaded (the module names) and failed "
            "({name, message}, null when nothing refused). It loads, unloads and builds nothing else.",
            {}, [this](const ControlArgs&, IFormatWriter& out) { return m_state->reloadBuilt(out); });
        m_state->control->registerCommand(
            "buildProject", OWNER,
#if defined(_WIN32)
            "starts scripts/Build.ps1 -Projects <project> -Configuration <configuration> (the checkout found walking "
            "up from the executable's directory; the environment variable SPITE_BUILD_SCRIPT, when set, names a script "
            "taking the same arguments instead, as a scenario's fake build) as a detached, windowless, "
            "below-normal-priority process; its "
            "progress and result are the project's buildStatus record. Nothing reloads by itself afterwards "
            "(moduleReloadBuilt, reloadModule). Reply: started {project, configuration, pid, script (the path run), "
            "scriptOverride (SPITE_BUILD_SCRIPT named it)}. Refused: an unknown "
            "project, a DLL of the running host (SpiteEngine, SpiteEditor), no scripts/Build.ps1 above the "
            "executable, a build of the project the editor started that still runs, and a building status record "
            "whose pid is alive.",
#else
            "runs cmake --build <tree> --target <project> in the tree's existing configuration, in "
            "a detached process group with a per-build output log. SPITE_BUILD_SCRIPT can name an executable "
            "taking -Projects <project> -Configuration <configuration> instead. While the editor tracks a CMake "
            "build, its result is recorded in buildStatus. Nothing reloads by itself (moduleReloadBuilt, "
            "reloadModule). Reply: started {project, configuration, pid, script, scriptOverride, output}.",
#endif
            {requiredArg("project", ArgKind::String,
                         "a project or module name the status files, the loaded modules or the module DLLs of the "
                         "executable's directory know (not a path)"),
             optionalArg("configuration", ArgKind::String, "the configuration to build", "the host's own",
                         CONFIGURATIONS),
             optionalArg("operation", ArgKind::Integer, "link this build to an active game module operation")},
            [this](const ControlArgs& args, IFormatWriter& out) { return m_state->buildProject(args, out); });
        m_state->control->registerCommand(
            "cancelBuild", OWNER,
#if defined(_WIN32)
            "ends the process tree of the build buildProject started for <project> while that process runs (the editor "
            "holds its handle, so the pid is its own), also before Build.ps1 wrote its record (a build queued for a "
            "build slot); refused when the project's status record shows another live build. A record the build "
            "wrote stays building with a dead pid, which buildStatus shows as abandoned. It "
            "ends no other process. Reply: cancelled {project, configuration, pid, ended (it exited within 5 s)}.",
#else
            "ends the process group of a build buildProject started for <project>. Refused when the status "
            "record shows another live build. Reply: cancelled {project, configuration, pid, ended "
            "(the build leader exited within 5 s)}.",
#endif
            {requiredArg("project", ArgKind::String, "the project buildProject started")},
            [this](const ControlArgs& args, IFormatWriter& out) { return m_state->cancelBuild(args, out); });
    }

    std::optional<u32> EditorBuildState::startedBuild(const std::string_view project) const
    {
        const State::StartedBuild* build = m_state->startedBuild(project);
        return build && build->process->running() ? std::optional(build->process->pid()) : std::nullopt;
    }

    const std::filesystem::path& EditorBuildState::outputPath(const std::string_view project) const
    {
        const auto found = std::ranges::find_if(m_state->outputLogs,
                                                [&](const auto& entry) { return sameProject(entry.project, project); });
        static const std::filesystem::path empty;
        if (found == m_state->outputLogs.end())
            return empty;
        if (const auto* active = m_state->startedBuild(project); active && active->process->running())
            return found->path;
        const auto* record = m_state->find(project);
        if (record && recordPid(*record) != found->pid && recordPid(*record) != found->previousPid)
            return empty;
        return found->path;
    }

    void EditorBuildState::update(const bool watched)
    {
        State& state = *m_state;
#if !defined(_WIN32)
        state.reap();
#endif
        if (state.worker)
        {
            if (std::optional<BuildRead> done = state.worker->poll())
            {
                state.workerBusy = false;
                state.skippedReads += done->unchanged;
                state.workerAllocations += done->allocations;
                if (!done->error && done->sequence > state.supersededBy)
                {
                    state.apply(std::move(*done));
                }
                else
                {
                    state.forceFull = true;
                }
            }
        }
        state.finishBuildWait();
        if ((!watched && !state.buildWait) || state.workerBusy)
        {
            return;
        }
        if (state.feedback->now() - state.lastStart >= PERIOD_SECONDS ||
            state.host->generation() != state.seenGeneration)
        {
            state.startWorker();
        }
    }

    void EditorBuildState::pollModuleWork()
    {
        const bool watch = m_state->work.build == "building" ||
            (m_state->work.binary != 0 && m_state->host->generation() != m_state->seenGeneration);
        if (watch || m_state->buildWait || (m_state->work.operation != 0 && m_state->workerBusy))
            update(watch);
    }

#if !defined(_WIN32)
    void EditorBuildState::pollStartedBuilds() { m_state->reap(); }
#endif

    void EditorBuildState::setStatusDirectory(std::filesystem::path directory)
    {
        m_state->directoryShared.reset();
        m_state->statusOverride = std::move(directory);
    }

    void EditorBuildState::setWorkerHook(std::function<void()> hook)
    {
        m_state->beforeRead = hook ? std::make_shared<const WorkerHook>(std::move(hook)) : nullptr;
    }

    void EditorBuildState::setLoadedModules(std::function<std::vector<AvailableModule>()> source)
    {
        m_state->loadedSource = std::move(source);
    }

    void EditorBuildState::refreshNow() { m_state->refreshNow(); }

    u64 EditorBuildState::revision() const { return m_state->revision; }

    const ModuleWork& EditorBuildState::moduleWork() const { return m_state->work; }

    u64 EditorBuildState::workerReads() const { return m_state->workerReads; }

    u64 EditorBuildState::directReads() const { return m_state->directReads; }

    bool EditorBuildState::reading() const { return m_state->workerBusy; }

    u64 EditorBuildState::skippedReads() const { return m_state->skippedReads; }

    u64 EditorBuildState::workerAllocations() const { return m_state->workerAllocations; }

    u64 EditorBuildState::displayRevision() const { return m_state->revision + m_state->ageRevisions; }

    bool EditorBuildState::hasProjects() const { return !m_state->snapshot["projects"].empty(); }

    bool EditorBuildState::building() const
    {
        return std::ranges::any_of(
            m_state->snapshot["projects"], [](const Json& project)
            { return text(project, "state") == "building" && !project.value("abandoned", false); });
    }

    u32 EditorBuildState::reloadableCount() const { return static_cast<u32>(m_state->reloadable().size()); }

    std::optional<BuildRecord> EditorBuildState::record(const std::string_view stem) const
    {
        const Json* project = m_state->find(stem);
        return project ? std::optional(toRecord(*project)) : std::nullopt;
    }

    std::vector<BuildRecord> EditorBuildState::others(const function_ref<bool(std::string_view)> listed) const
    {
        std::vector<BuildRecord> result;
        for (const Json& project : m_state->snapshot["projects"])
        {
            if (!listed(text(project, "project")) && text(project, "state") != "none")
            {
                result.push_back(toRecord(project));
            }
        }
        return result;
    }

    BuildAge buildAgeOf(const double ageSec, const double nowSec)
    {
        const double age = std::max(0.0, ageSec);
        const double minutes = std::floor(nowSec / 60.0) - std::floor((nowSec - age) / 60.0);
        if (minutes < 1.0)
        {
            return {BuildAgeUnit::Now, 0};
        }
        if (minutes < 60.0)
        {
            return {BuildAgeUnit::Minutes, static_cast<u32>(minutes)};
        }
        return minutes < 1440.0 ? BuildAge{BuildAgeUnit::Hours, static_cast<u32>(minutes / 60.0)}
                                : BuildAge{BuildAgeUnit::Days, static_cast<u32>(std::min(minutes / 1440.0, 99999.0))};
    }

    BuildAge buildAgeOf(const double ageSec)
    {
        return buildAgeOf(ageSec,
                          std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
    }

    void EditorBuildState::writeModule(IFormatWriter& out, const std::string_view stem) const
    {
        const Json* project = m_state->find(stem);
        const auto flag = [&](const char* key) { return project && project->value(key, false); };
        out.writeString("buildState", project ? text(*project, "state") : std::string("none"));
        out.writeBool("needsReload", flag("needsReload"));
        out.writeBool("untracked", flag("untracked"));
        out.writeBool("abandoned", flag("abandoned"));
        writeAge(out, project);
        writeDiagnostics(out, project);
    }

    void EditorBuildState::writeOthers(IFormatWriter& out, const function_ref<bool(std::string_view)> listed) const
    {
        std::vector<const Json*> others;
        for (const Json& project : m_state->snapshot["projects"])
        {
            if (!listed(text(project, "project")) && text(project, "state") != "none")
            {
                others.push_back(&project);
            }
        }
        out.beginArray("otherBuilds", static_cast<u32>(others.size()));
        for (const Json* project : others)
        {
            out.beginObject("");
            out.writeString("project", text(*project, "project"));
            out.writeString("buildState", text(*project, "state"));
            out.writeBool("abandoned", project->value("abandoned", false));
            writeAge(out, project);
            writeDiagnostics(out, project);
            out.endObject();
        }
        out.endArray();
    }
} // namespace spite
