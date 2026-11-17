#include "EditorCaptureFiles.hpp"
#include <algorithm>
#include <chrono>
#include "base/io/FileSystem.hpp"
#include "application/Shell.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "ProfilerViews.hpp"
namespace spite
{
    namespace
    {
        constexpr std::string_view OWNER = "editorCaptureFiles";
        constexpr std::string_view SIDECAR_SUFFIX = ".timeline.sqlite";
    } // namespace
    struct EditorCaptureFiles::State
    {
        ControlServer* control;
        CaptureStore* store;
        std::function<CaptureId()> selectedCapture;
        std::function<std::vector<CaptureFile>()> listFiles;
        std::function<void(const std::string&)> refreshView;
        // a few seconds, so a loop of profilerReveal commands cannot flood the desktop
        static constexpr u64 REVEAL_GAP_MS = 1000;
        static constexpr u64 REVEAL_SAME_FILE_MS = 5000;
        std::function<bool(const std::filesystem::path&)> revealFile = revealInFileManager;
        std::function<u64()> revealClock = []
        {
            return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
        };
        std::filesystem::path lastRevealed;
        u64 lastRevealMs = 0;
        bool hasRevealed = false;
        // What a refusal offers: the artifacts root and the first few sidecars of the list
        [[nodiscard]] std::string validNames() const
        {
            const auto files = listFiles ? listFiles() : std::vector<CaptureFile>{};
            std::string text = " (artifacts root " + utf8Path(store->artifactsRoot()) + "; e.g.";
            for (u32 listed = 0; listed < std::min<sizet>(files.size(), 5); ++listed)
            {
                text += " " + files[listed].path;
            }
            return text + (files.size() > 5 ? ", ... " + std::to_string(files.size()) + " in all)" : ")");
        }

        // Whether `file` is <machine>/prof/<name>.timeline.sqlite below `root` and nothing else
        [[nodiscard]] static bool sidecarLayout(const std::filesystem::path& file, const std::filesystem::path& root)
        {
            const std::filesystem::path under = file.lexically_relative(root);
            if (std::distance(under.begin(), under.end()) != 3)
            {
                return false;
            }
            auto part = under.begin();
            const std::filesystem::path machine = *part++;
            const std::filesystem::path prof = *part++;
            return machine != ".." && machine != "." && prof == "prof" && utf8Path(*part).ends_with(SIDECAR_SUFFIX);
        }

        // The sidecar a list row stands for, named by its store id or its artifacts-relative path (what profilerState
        // lists). Checked on disk, never trusted: the layout above on the normalised path and again on the resolved
        // one, the machine and prof folders and the file plain entries (no junction or link; the artifacts root itself
        // may be one). A refusal says which names and root are valid.
        std::string sidecarOf(const ControlArgs& args, std::filesystem::path& out, CaptureId& openId)
        {
            openId = 0;
            const std::optional<u64> named = args.integer("capture");
            const std::string relative = args.string("path").value_or("");
            if (named.has_value() == !relative.empty())
            {
                return "give capture=<store id> or path=<sidecar under the artifacts root>, as profilerState captures "
                       "lists them" +
                    validNames();
            }
            std::error_code error;
            std::filesystem::path root = std::filesystem::absolute(store->artifactsRoot(), error).lexically_normal();
            if (root.filename().empty())
            {
                root = root.parent_path();
            }
            std::filesystem::path candidate;
            CaptureInfo info;
            if (named)
            {
                if (!store->info(static_cast<CaptureId>(std::min<u64>(*named, UINT32_MAX)), info))
                {
                    return "no capture " + std::to_string(*named) + " in the store" + validNames();
                }
                openId = info.id;
                candidate = std::filesystem::absolute(info.path, error);
            }
            else
            {
                try
                {
                    candidate = root / pathFromUtf8(relative);
                }
                catch (const std::exception&)
                {
                    return "the path is not valid UTF-8";
                }
            }
            candidate = candidate.lexically_normal();
            const std::string what =
                named ? "capture " + std::to_string(*named) : "'" + profilerSafeText(relative, 96) + "'";
            if (error || !sidecarLayout(candidate, root))
            {
                return what + " is not <machine>/prof/<name>.timeline.sqlite under the artifacts root" + validNames();
            }
            const std::filesystem::path machineDir = root / *candidate.lexically_relative(root).begin();
            if (!isPlainEntry(machineDir, true) || !isPlainEntry(machineDir / "prof", true) ||
                !isPlainEntry(candidate, false))
            {
                return what + " is missing, or the file or its folders are links" + validNames();
            }
            std::error_code first, second;
            const std::filesystem::path file = std::filesystem::weakly_canonical(candidate, first);
            const std::filesystem::path resolvedRoot = std::filesystem::weakly_canonical(root, second);
            if (first || second || !sidecarLayout(file, resolvedRoot))
            {
                return "refused: " + what + " is outside the artifacts folder";
            }
            if (!named)
            {
                for (const CaptureInfo& row : store->list())
                {
                    std::error_code same;
                    if (store->info(row.id, info) && std::filesystem::equivalent(info.path, file, same) && !same)
                    {
                        openId = row.id;
                    }
                }
            }
            out = file;
            return {};
        }

        // Deletes a listed capture's sidecar (its zones report beside it stays). A capture open in the store is closed
        // first; one still being written is refused. If the file cannot then be removed the capture stays closed and
        // the refusal says so.
        std::string deleteCapture(const ControlArgs& args, IFormatWriter& out)
        {
            std::filesystem::path file;
            CaptureId openId = 0;
            if (std::string error = sidecarOf(args, file, openId); !error.empty())
            {
                return error;
            }
            if (std::string error; openId != 0 && !store->close(openId, error))
            {
                return error;
            }
            const std::string name = utf8Path(file.filename());
            std::error_code failed;
            const u64 bytes = std::filesystem::file_size(file, failed);
            failed.clear();
            const bool removed = std::filesystem::remove(file, failed);
            if (failed)
            {
                return "could not delete '" + name + "': " + failed.message() +
                    (openId != 0 ? " (the capture was closed)" : "");
            }
            if (refreshView)
                refreshView(name);
            out.writeString("name", name);
            out.writeBool("deleted", removed);
            if (!removed)
            {
                out.writeString("reason", "the file was already gone");
            }
            out.writeBool("closed", openId != 0);
            out.writeU64("bytes", removed ? bytes : 0);
            return {};
        }

        // Only a file under the artifacts folder is shown: the path is the store's, and still checked, never trusted.
        // With capture= or path= the file is a list row's sidecar, else the selected capture's report.
        std::string reveal(const ControlArgs& args, IFormatWriter& out)
        {
            std::filesystem::path path;
            if (args.integer("capture") || args.string("path"))
            {
                CaptureId ignored = 0;
                if (std::string error = sidecarOf(args, path, ignored); !error.empty())
                {
                    return error;
                }
            }
            else
            {
                const CaptureId selected = selectedCapture ? selectedCapture() : 0;
                CaptureInfo info;
                if (selected && store->info(selected, info))
                {
                    path = info.report.empty() ? info.path : info.path.parent_path() / info.report;
                }
                if (path.empty())
                {
                    return "select a capture first (profilerSelect capture=<id>)";
                }
                if (!info.report.empty() && std::filesystem::path(info.report).filename() != info.report)
                {
                    return "refused: the report name is not a plain file name";
                }
            }
            std::error_code first, second;
            const std::filesystem::path file = std::filesystem::weakly_canonical(path, first);
            const std::filesystem::path root = std::filesystem::weakly_canonical(store->artifactsRoot(), second);
            const std::filesystem::path relative = file.lexically_relative(root);
            if (first || second || relative.empty() || relative == "." || *relative.begin() == "..")
            {
                return "refused: the file is outside the artifacts folder";
            }
            if (!std::filesystem::is_regular_file(file, first))
            {
                return "the file is gone: " + utf8Path(file.filename());
            }
            const u64 now = revealClock();
            if (hasRevealed &&
                (now - lastRevealMs < REVEAL_GAP_MS ||
                 (file == lastRevealed && now - lastRevealMs < REVEAL_SAME_FILE_MS)))
            {
                return "refused: the file manager was shown a moment ago";
            }
            hasRevealed = true;
            lastRevealed = file;
            lastRevealMs = now;
            if (!revealFile(file))
            {
                return "the file manager could not be started";
            }
            out.writeString("path", utf8Path(file));
            return {};
        }

        void registerCommands()
        {
            control->registerViewCommand(
                "profilerReveal", OWNER,
                "shows a file in the file manager: the selected capture's report (the Reveal button), or with "
                "capture= / path= a list row's sidecar (the Show file button). Refused for a file outside the "
                "artifacts folder or one that is gone; the refusal lists the valid names and the root",
                {optionalArg("capture", ArgKind::Integer, "store id of a capture in the list; its sidecar is shown"),
                 optionalPath("path", "artifacts", "a list row's sidecar, <machine>/prof/<name>.timeline.sqlite")},
                [this](const ControlArgs& args, IFormatWriter& out) { return reveal(args, out); });
            control->registerViewCommand(
                "profilerDelete", OWNER,
                "deletes a capture's sidecar file from the artifacts folder (the Delete button, which asks first); "
                "the zones report beside it stays. A capture open in the store is closed first, one still being "
                "written is refused. Name it by capture= (store id) or path= (a profilerState captures[].path). The "
                "refusal lists the valid names and the root",
                {optionalArg("capture", ArgKind::Integer, "store id of a capture in the list"),
                 optionalPath("path", "artifacts", "a listed sidecar, <machine>/prof/<name>.timeline.sqlite")},
                [this](const ControlArgs& args, IFormatWriter& out) { return deleteCapture(args, out); });
        }
    };
    EditorCaptureFiles::EditorCaptureFiles(Engine& engine) : m_state(std::make_unique<State>())
    {
        m_state->control = &engine.control();
        m_state->store = &engine.services().get<CaptureStore>();
        m_state->registerCommands();
    }
    EditorCaptureFiles::~EditorCaptureFiles() { m_state->control->unregisterOwner(OWNER); }
    void EditorCaptureFiles::setView(std::function<CaptureId()> selected,
                                     std::function<std::vector<CaptureFile>()> files,
                                     std::function<void(const std::string&)> refresh)
    {
        m_state->selectedCapture = std::move(selected);
        m_state->listFiles = std::move(files);
        m_state->refreshView = std::move(refresh);
    }
    void EditorCaptureFiles::setReveal(std::function<bool(const std::filesystem::path&)> reveal,
                                       std::function<std::uint64_t()> nowMs)
    {
        m_state->revealFile = std::move(reveal);
        if (nowMs)
            m_state->revealClock = std::move(nowMs);
    }
} // namespace spite
