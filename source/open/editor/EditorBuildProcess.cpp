#include "EditorBuildProcess.hpp"
#include <spite/platform/ProcessId.hpp>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <format>
#include <initializer_list>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif
#endif

namespace spite
{
#if defined(_WIN32)
    namespace
    {
        std::wstring widen(const std::string_view text) { return std::filesystem::path(text).wstring(); }

        std::string lastError(const char* what) { return std::format("{} failed (error {})", what, GetLastError()); }

        // The editor's environment with `set` replacing any entry of the same name, as a CREATE_UNICODE_ENVIRONMENT
        // block.
        std::wstring environmentWith(const std::initializer_list<std::pair<std::wstring_view, std::wstring>> set)
        {
            std::wstring block;
            if (wchar_t* const strings = GetEnvironmentStringsW())
            {
                for (const wchar_t* at = strings; *at; at += wcslen(at) + 1)
                {
                    const std::wstring_view entry(at);
                    const std::wstring_view name = entry.substr(0, entry.find(L'=', 1)); // "=C:=C:\" names "=C:"
                    const bool replaced = std::ranges::any_of(
                        set,
                        [&](const auto& pair)
                        {
                            return CompareStringOrdinal(name.data(), static_cast<int>(name.size()), pair.first.data(),
                                                        static_cast<int>(pair.first.size()), TRUE) == CSTR_EQUAL;
                        });
                    if (!replaced)
                    {
                        block.append(entry).push_back(L'\0');
                    }
                }
                FreeEnvironmentStringsW(strings);
            }
            for (const auto& [name, value] : set)
            {
                block.append(name).append(L"=").append(value).push_back(L'\0');
            }
            block.push_back(L'\0');
            return block;
        }
    } // namespace

    std::filesystem::path EditorBuildProcess::scriptOverride(std::string& error)
    {
        // asked for its length first: a path of any length is read whole, never cut to a buffer
        const DWORD needed = GetEnvironmentVariableW(L"SPITE_BUILD_SCRIPT", nullptr, 0);
        if (needed <= 1)
        {
            return {};
        }
        std::wstring value(needed, L'\0');
        const DWORD length = GetEnvironmentVariableW(L"SPITE_BUILD_SCRIPT", value.data(), needed);
        if (length == 0 || length >= needed)
        {
            error = lastError("reading SPITE_BUILD_SCRIPT");
            return {};
        }
        value.resize(length);
        std::filesystem::path script(value);
        std::error_code ignored;
        if (!std::filesystem::is_regular_file(script, ignored))
        {
            const std::u8string text = script.generic_u8string();
            error = std::format("SPITE_BUILD_SCRIPT names {}, which is not a file: nothing started",
                                std::string(text.begin(), text.end()));
            return {};
        }
        return script;
    }

    std::unique_ptr<EditorBuildProcess> EditorBuildProcess::start(const std::filesystem::path& scriptPath,
                                                                  const std::filesystem::path& directory,
                                                                  const std::string_view project,
                                                                  const std::string_view configuration,
                                                                  std::string& error)
    {
        wchar_t pwsh[MAX_PATH]{};
        if (!SearchPathW(nullptr, L"pwsh.exe", nullptr, MAX_PATH, pwsh, nullptr))
        {
            error = lastError("finding pwsh.exe on the PATH");
            return nullptr;
        }
        // no kill-on-close: the build outlives the editor; the job is what end() terminates
        const HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (!job)
        {
            error = lastError("creating the build's job");
            return nullptr;
        }
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        const HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       &inherit, OPEN_EXISTING, 0, nullptr);
        if (nul == INVALID_HANDLE_VALUE)
        {
            error = lastError("opening NUL");
            CloseHandle(job);
            return nullptr;
        }
        static std::atomic<u32> builds = 0;
        const u32 ticket = ++builds;
        const auto outputDirectory = directory / "obj" / "build-output";
        std::error_code outputError;
        std::filesystem::create_directories(outputDirectory, outputError);
        FILETIME created{};
        GetSystemTimeAsFileTime(&created);
        const u64 logTime = (static_cast<u64>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        const auto outputPath = outputDirectory /
            std::format("{}-{}-{}-{}-{}.log", project, configuration, spite::platform::processId(), logTime, ticket);
        const HANDLE output = outputError
            ? INVALID_HANDLE_VALUE
            : CreateFileW(outputPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          &inherit, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output == INVALID_HANDLE_VALUE)
        {
            error = outputError ? "Cannot create build output directory: " + outputError.message()
                                : lastError("opening build output log");
            CloseHandle(nul);
            CloseHandle(job);
            return nullptr;
        }
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        const std::unique_ptr<char[]> attributeStorage(new char[attributeBytes]);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.get());
        const bool attributesReady = InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes);
        HANDLE inherited[] = {nul, output};
        const bool ready = attributesReady &&
            UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited),
                                      nullptr, nullptr);
        // No node reuse: MSBuild would hand work to idle nodes of other builds, which run outside the job at normal
        // priority; new nodes are children of this build. A private PDB server endpoint: a cl /Zi of this build never
        // joins an mspdbsrv another build uses, and the one it starts serves this build only, so ending the job never
        // ends a process another build needs (SPITE builds use /Z7, which starts none).
        std::wstring environment = environmentWith(
            {{L"MSBUILDDISABLENODEREUSE", L"1"},
             {L"_MSPDBSRV_ENDPOINT_", std::format(L"spite-editor-build-{}-{}", spite::platform::processId(), ticket)},
             {L"SPITE_BUILD_LIVE_OUTPUT", L"1"},
             {L"SPITE_BUILD_LIVE_HOST_PID", std::to_wstring(spite::platform::processId())}});
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = nul;
        startup.StartupInfo.hStdOutput = output;
        startup.StartupInfo.hStdError = output;
        startup.lpAttributeList = attributes;
        const std::wstring script = scriptPath.wstring();
        const std::wstring rootText = directory.wstring();
        PROCESS_INFORMATION process{};
        bool started = false;
        if (ready)
        {
            // CREATE_NO_WINDOW, not DETACHED_PROCESS: a hidden console MSBuild and the compilers inherit, so none of
            // them opens a window of its own
            const DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW |
                CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | BELOW_NORMAL_PRIORITY_CLASS;
            for (const DWORD breakaway : {DWORD{CREATE_BREAKAWAY_FROM_JOB}, DWORD{0}})
            {
                std::wstring command = L"\"" + std::wstring(pwsh) + L"\" -NoProfile -NonInteractive -File \"" + script +
                    L"\" -Projects " + widen(project) + L" -Configuration " + widen(configuration);
                started = CreateProcessW(pwsh, command.data(), nullptr, nullptr, TRUE, flags | breakaway,
                                         environment.data(), rootText.c_str(), &startup.StartupInfo, &process);
                // the host's job (a test runner's, RunEngine's) may refuse breakaway: the build then stays in it
                if (started || GetLastError() != ERROR_ACCESS_DENIED)
                {
                    break;
                }
            }
            if (!started)
            {
                error = lastError("starting pwsh");
            }
        }
        else
        {
            error = lastError("preparing the build process");
        }
        if (attributesReady)
        {
            DeleteProcThreadAttributeList(attributes);
        }
        CloseHandle(nul);
        CloseHandle(output);
        if (started && !AssignProcessToJobObject(job, process.hProcess))
        {
            error = lastError("assigning the build to its job");
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            started = false;
        }
        if (!started)
        {
            if (process.hThread)
            {
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
            CloseHandle(job);
            return nullptr;
        }
        if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
        {
            error = lastError("resuming the build process");
            TerminateJobObject(job, 1);
            WaitForSingleObject(process.hProcess, 5000);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(job);
            return nullptr;
        }
        CloseHandle(process.hThread);
        return std::unique_ptr<EditorBuildProcess>(
            new EditorBuildProcess(process.hProcess, job, static_cast<u32>(process.dwProcessId), outputPath));
    }

    EditorBuildProcess::~EditorBuildProcess()
    {
        CloseHandle(m_job);
        CloseHandle(m_process);
    }

    bool EditorBuildProcess::running() const { return WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT; }

    bool EditorBuildProcess::end(const u32 waitMs)
    {
        TerminateJobObject(m_job, 1);
        return WaitForSingleObject(m_process, waitMs) == WAIT_OBJECT_0;
    }
#else
    namespace
    {
        bool safeName(std::string_view name)
        {
            return !name.empty() && name.front() != '-' &&
                std::ranges::all_of(name,
                                    [](char c)
                                    {
                                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                            (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
                                    });
        }

        std::filesystem::path cmakeProgram(const std::filesystem::path& directory, std::string_view configuration,
                                           std::string& error)
        {
            std::ifstream cache(directory / "CMakeCache.txt");
            if (!cache)
            {
                error = "cannot read CMakeCache.txt in the configured build tree";
                return {};
            }
            std::string program, configured;
            for (std::string line; std::getline(cache, line);)
            {
                const auto assignment = line.find('=');
                if (assignment == line.npos)
                    continue;
                if (line.starts_with("CMAKE_COMMAND:"))
                    program = line.substr(assignment + 1);
                else if (line.starts_with("CMAKE_BUILD_TYPE:"))
                    configured = line.substr(assignment + 1);
            }
            if (configured != configuration)
            {
                error =
                    std::format("this CMake tree is configured for '{}', requested '{}'", configured, configuration);
                return {};
            }
            std::filesystem::path path(program);
            if (!path.is_absolute() || access(path.c_str(), X_OK) != 0)
            {
                error = "CMakeCache.txt does not name an executable CMake program";
                return {};
            }
            return path;
        }

        int aboveStdio(int fd)
        {
            if (fd < 0 || fd > STDERR_FILENO + 1)
                return fd;
            const int moved = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 2);
            const int saved = errno;
            close(fd);
            errno = saved;
            return moved;
        }

        int startChild(const std::filesystem::path& program, const std::filesystem::path& directory, char* const* args,
                       int output, int lock, pid_t& child)
        {
            posix_spawn_file_actions_t actions;
            int error = posix_spawn_file_actions_init(&actions);
            if (error != 0)
                return error;
            posix_spawnattr_t attributes;
            error = posix_spawnattr_init(&attributes);
            if (error == 0)
            {
#if defined(POSIX_SPAWN_SETSID)
                const short flags = POSIX_SPAWN_SETSID;
#else
                const short flags = POSIX_SPAWN_SETPGROUP;
                error = posix_spawnattr_setpgroup(&attributes, 0);
#endif
#if defined(__APPLE__)
                char** environment = *_NSGetEnviron();
                const short childFlags = flags | POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
                char** environment = environ;
                const short childFlags = flags;
#endif
                if (error == 0 && (error = posix_spawnattr_setflags(&attributes, childFlags)) == 0 &&
                    (error = posix_spawn_file_actions_addchdir_np(&actions, directory.c_str())) == 0 &&
                    (error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0)) == 0 &&
                    (error = posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO)) == 0 &&
                    (error = posix_spawn_file_actions_adddup2(&actions, output, STDERR_FILENO)) == 0)
                {
                    // The child retains the tree lock even if the editor exits during the build.
                    if (lock >= 0)
                        error = posix_spawn_file_actions_adddup2(&actions, lock, STDERR_FILENO + 1);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
                    if (error == 0)
                        error = posix_spawn_file_actions_addclosefrom_np(&actions, STDERR_FILENO + (lock >= 0 ? 2 : 1));
#endif
                    if (error == 0)
                        error = posix_spawn(&child, program.c_str(), &actions, &attributes, args, environment);
                }
                posix_spawnattr_destroy(&attributes);
            }
            posix_spawn_file_actions_destroy(&actions);
            return error;
        }

        // Leave the leader unreaped until cancellation is no longer possible, so its group ID stays ours.
        int childStatus(pid_t child)
        {
            siginfo_t info{};
            if (waitid(P_PID, static_cast<id_t>(child), &info, WEXITED | WNOHANG | WNOWAIT) != 0)
                return errno == EINTR ? -1 : -2;
            if (info.si_pid == 0)
                return -1;
            return info.si_code == CLD_EXITED ? info.si_status : 128 + info.si_status;
        }

#if defined(__linux__) && defined(POSIX_SPAWN_SETSID)
        std::optional<bool> signalBuildSession(pid_t session, std::chrono::steady_clock::time_point deadline)
        {
            struct ProcessDirectory
            {
                DIR* value = opendir("/proc");
                ~ProcessDirectory()
                {
                    if (value)
                        closedir(value);
                }
            } directory;
            if (!directory.value)
                return std::nullopt;
            bool live = false;
            for (;;)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                    return std::nullopt;
                errno = 0;
                const dirent* entry = readdir(directory.value);
                if (!entry)
                    return errno == 0 ? std::optional(live) : std::nullopt;
                const std::string_view name = entry->d_name;
                pid_t pid = 0;
                const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), pid);
                if (error != std::errc{} || end != name.data() + name.size() || pid <= 0 || getsid(pid) != session)
                    continue;
                // Ninja creates job groups of its own. A pidfd ties each signal to the process we inspected.
                const int descriptor = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
                if (descriptor < 0)
                {
                    if (errno == ESRCH)
                        continue;
                    return std::nullopt;
                }
                bool ok = true;
                if (getsid(pid) == session)
                {
                    pollfd observed{descriptor, POLLIN, 0};
                    const int ready = poll(&observed, 1, 0);
                    if (ready == 0)
                    {
                        ok = syscall(SYS_pidfd_send_signal, descriptor, SIGKILL, nullptr, 0) == 0 || errno == ESRCH;
                        live = true;
                    }
                    else if (ready < 0 && errno != EINTR)
                        ok = false;
                    else if (ready < 0)
                        live = true;
                }
                close(descriptor);
                if (!ok)
                    return std::nullopt;
            }
        }
#endif
    } // namespace

    std::filesystem::path EditorBuildProcess::scriptOverride(std::string& error)
    {
        const char* value = std::getenv("SPITE_BUILD_SCRIPT");
        if (!value || !*value)
            return {};
        std::error_code io;
        const auto script = std::filesystem::absolute(std::filesystem::path(value), io);
        if (io || !std::filesystem::is_regular_file(script, io) || access(script.c_str(), X_OK) != 0)
        {
            error = std::format("SPITE_BUILD_SCRIPT names {}, which is not an executable file: nothing started", value);
            return {};
        }
        return script;
    }

    std::unique_ptr<EditorBuildProcess> EditorBuildProcess::start(const std::filesystem::path& script,
                                                                  const std::filesystem::path& directory,
                                                                  std::string_view project,
                                                                  std::string_view configuration, std::string& error)
    {
        if (!safeName(project) || !safeName(configuration))
        {
            error = "invalid build project or configuration";
            return nullptr;
        }
        const auto program = script.empty() ? cmakeProgram(directory, configuration, error) : script;
        if (program.empty())
            return nullptr;
        std::vector<std::string> arguments{program.native()};
        if (script.empty())
        {
            arguments.insert(arguments.end(), {"--build", directory.native(), "--target", std::string(project)});
        }
        else
        {
            arguments.insert(arguments.end(),
                             {"-Projects", std::string(project), "-Configuration", std::string(configuration)});
        }
        std::vector<char*> args;
        args.reserve(arguments.size() + 1);
        for (auto& argument : arguments)
            args.push_back(argument.data());
        args.push_back(nullptr);

        static std::atomic<u32> builds = 0;
        const auto outputDirectory = directory / "obj" / "build-output";
        std::error_code io;
        std::filesystem::create_directories(outputDirectory, io);
        if (io)
        {
            error = "Cannot create build output directory: " + io.message();
            return nullptr;
        }
        const auto now = std::chrono::system_clock::now().time_since_epoch().count();
        const auto path = outputDirectory /
            std::format("{}-{}-{}-{}-{}.log", project, configuration, platform::processId(), now, ++builds);
        std::unique_ptr<EditorBuildProcess> process(new EditorBuildProcess(program, path));
        if (script.empty())
        {
            process->m_lockFd = aboveStdio(open((directory / ".spite-editor-build.lock").c_str(),
                                                O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
            if (process->m_lockFd < 0 || flock(process->m_lockFd, LOCK_EX | LOCK_NB) != 0)
            {
                error = process->m_lockFd >= 0 && (errno == EWOULDBLOCK || errno == EAGAIN)
                    ? "another editor build is using this CMake tree"
                    : std::format("cannot lock the CMake build tree: error {}", errno);
                return nullptr;
            }
        }
        const int output = aboveStdio(open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (output < 0)
        {
            error = std::format("cannot open build output log: error {}", errno);
            return nullptr;
        }
        pid_t child = -1;
        const int spawned = startChild(program, directory, args.data(), output, process->m_lockFd, child);
        close(output);
        if (spawned != 0)
        {
            error = std::format("cannot start {}: error {}", program.string(), spawned);
            return nullptr;
        }
        process->m_pid = static_cast<u32>(child);
        setpriority(PRIO_PROCESS, static_cast<id_t>(child), getpriority(PRIO_PROCESS, 0) + 1);
        return process;
    }

    EditorBuildProcess::~EditorBuildProcess()
    {
        if (m_lockFd >= 0)
            close(m_lockFd);
        if (m_pid == 0)
            return;
        const auto child = static_cast<pid_t>(m_pid);
        pid_t waited;
        do
        {
            waited = waitpid(child, nullptr, WNOHANG);
        }
        while (waited < 0 && errno == EINTR);
        if (waited != 0)
            return;
        // A build outlives the editor model. Reap it without waiting on the frame thread.
        try
        {
            std::thread(
                [child]
                {
                    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR)
                    {
                    }
                })
                .detach();
        }
        catch (const std::system_error&)
        {
            // With no reaper thread available, the host exit releases the child.
        }
    }

    bool EditorBuildProcess::running() const { return childStatus(static_cast<pid_t>(m_pid)) == -1; }

    std::optional<int> EditorBuildProcess::exitCode() const
    {
        const int status = childStatus(static_cast<pid_t>(m_pid));
        return status >= 0 ? std::optional(status) : std::nullopt;
    }

    bool EditorBuildProcess::end(const u32 waitMs)
    {
        const auto child = static_cast<pid_t>(m_pid);
        if (childStatus(child) == -2)
            return true;
        if (kill(-child, SIGKILL) != 0 && errno != ESRCH)
            return false;
        using namespace std::chrono;
        const auto deadline = steady_clock::now() + milliseconds(waitMs);
#if defined(__linux__) && defined(POSIX_SPAWN_SETSID)
        bool empty = false;
        for (;;)
        {
            const auto live = signalBuildSession(child, deadline);
            if (!live)
                return false;
            const bool ended = !*live && !running();
            if (ended && empty)
                return true;
            empty = ended;
            const auto now = steady_clock::now();
            if (now >= deadline)
                return false;
            std::this_thread::sleep_for(
                std::min(deadline - now, duration_cast<steady_clock::duration>(milliseconds(5))));
        }
#else
        while (running())
        {
            const auto now = steady_clock::now();
            if (now >= deadline)
                return false;
            std::this_thread::sleep_for(
                std::min(deadline - now, duration_cast<steady_clock::duration>(milliseconds(5))));
        }
        return true;
#endif
    }
#endif
} // namespace spite
