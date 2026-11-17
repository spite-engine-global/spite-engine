#include "EditorCapture.hpp"
#include "EditorCommandLog.hpp"
#include "EditorContext.hpp"
#include "EditorDialogs.hpp"
#include "EditorFeedback.hpp"
#include "EditorPanelList.hpp"
#include "EditorPanelRegistry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <format>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "application/Shell.hpp"
#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "serialization/FormatWalk.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiScope.hpp"
#include "uiview/UiViewContext.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using namespace panelui;

        constexpr std::string_view CAPTURE_VIEW = "editor.capture";
        constexpr std::string_view CAPTURE_TOOLBAR = "capture.toolbar";
        constexpr std::string_view CAPTURE_ROWS = "capture.rows";
        constexpr std::string_view CAPTURE_DETAIL = "capture.detail";
        constexpr std::string_view OWNER = "editorCapture";
        constexpr std::string_view TABLE = "editor-capture.";
        constexpr std::string_view CAPTURES_ROOT = "captures";
        constexpr float VIEW_WIDTH = 150.0f;
        constexpr sizet MAX_PATH_BYTES = 72;
        constexpr std::uint64_t REVEAL_GAP_MS = 1000; // the file manager is a window of another program
        constexpr u64 THUMB_SLOT = 0x5350495443415000ull; // "SPITCAP": the thumbnail's UiView image slot
        constexpr std::string_view SIZE_THUMB_WIDTH = "size.thumbnail.width";
        constexpr std::string_view SIZE_THUMB_HEIGHT = "size.thumbnail.height";
        constexpr u32 KIND_IMAGE = 2; // the index of "image" in KINDS

        // The buttons that run an engine command, then the actions on the picked row.
        enum Op : u32
        {
            BTN_FRAME,
            BTN_SNAPSHOT,
            BTN_SCREENSHOT,
            BTN_RENDERDOC,
            BTN_REVEAL,
            BTN_COPY,
            BTN_DELETE,
            BTN_COUNT,
        };
        constexpr u32 CAPTURE_OPS = BTN_RENDERDOC + 1;
        constexpr std::array<std::string_view, BTN_COUNT> ACTIONS{"frame",  "snapshot", "screenshot", "renderdoc",
                                                                  "reveal", "copy",     "delete"};
        constexpr std::array<std::string_view, 6> KINDS{"frame", "snapshot", "image", "renderdoc", "log", "other"};

        struct File
        {
            std::string name;
            u64 size = 0;
            std::string modified; // UTC, as listDirectory writes it
            std::string path; // absolute
        };

        // What the table shows of a file; path is no column, the query and the line under the table read it.
        struct ArtifactCells
        {
            std::string name;
            std::string kind;
            std::string size;
            std::string time;
            std::string path;
            bool operator==(const ArtifactCells&) const = default;
            SPITE_OBSERVED_SCHEMA(ArtifactCells, name, kind, size, time, path)
        };

        UiTableColumn column(const std::string_view field, const float width)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString(std::string(TABLE) + "column." + std::string(field)),
                    .sortable = false};
        }

        bool endsWith(const std::string& name, const std::string_view suffix)
        {
            return name.size() >= suffix.size() &&
                std::ranges::equal(std::string_view(name).substr(name.size() - suffix.size()), suffix,
                                   [](const char a, const char b)
                                   { return std::tolower(static_cast<unsigned char>(a)) == b; });
        }

        // by how the commands name their files: capture_*.sqlite, snapshot files, PNGs, .rdc, command logs
        u32 kindOf(const std::string& name)
        {
            if (endsWith(name, ".rdc"))
            {
                return 3;
            }
            if (endsWith(name, ".png"))
            {
                return 2;
            }
            if (endsWith(name, ".sqlite") || endsWith(name, ".db"))
            {
                return name.starts_with("capture") ? 0 : 1;
            }
            return endsWith(name, ".jsonl") ? 4 : 5;
        }

        std::string sizeText(const u64 bytes)
        {
            constexpr std::array<const char*, 4> UNITS{"KiB", "MiB", "GiB", "TiB"};
            if (bytes < 1024)
            {
                return std::format("{} B", bytes);
            }
            double value = static_cast<double>(bytes);
            u32 unit = 0;
            for (value /= 1024.0; value >= 1024.0 && unit + 1 < UNITS.size(); value /= 1024.0)
            {
                ++unit;
            }
            return std::format("{:.1f} {}", value, UNITS[unit]);
        }

        // The end of a path, cut at a character and led by an ellipsis: the line under the list stays short however
        // deep the folder is (the whole path is the row's `path` and what Copy Path copies).
        std::string pathTail(const std::string& path)
        {
            if (path.size() <= MAX_PATH_BYTES)
            {
                return path;
            }
            sizet from = path.size() - MAX_PATH_BYTES;
            while (from < path.size() && (static_cast<unsigned char>(path[from]) & 0xC0u) == 0x80u)
            {
                ++from;
            }
            return std::string{char(0xE2), char(0x80), char(0xA6)} + path.substr(from); // U+2026
        }

        std::string nameOf(const std::string& path) { return path.substr(path.find_last_of("/\\") + 1); }
    } // namespace

    struct EditorCapture::State
    {
        // The replies of the deferred commands, written by the engine's reply sink on the frame thread; the sink holds
        // it weakly, so a reply after the panel is gone does nothing.
        struct Inbox
        {
            std::vector<std::pair<u32, std::string>> done;
        };

        State(Engine& engine, EditorFeedback& feedback, EditorDialogs& dialogs) :
            engine(&engine), control(&engine.control()), feedback(&feedback), dialogs(&dialogs),
            scope(editorUi(engine).ui()), density(scope),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        EditorFeedback* feedback;
        EditorDialogs* dialogs;
        UiScope scope;
        DensityFollower density;
        const Localization* localization; // not owned: the engine's service
        Panel root;
        std::array<Button, BTN_COUNT> buttons;
        Button renderdocToggle;
        Button refresh;
        Dropdown viewChoice;
        Panel spacer; // pushes Refresh to the right of the capture buttons
        Label detail;
        Panel detailRow; // the thumbnail beside the detail line
        Image thumb;
        std::unique_ptr<RowTable<ArtifactCells>> rows;
        std::unique_ptr<UiDocumentView> view;
        std::vector<std::function<void()>> pending;
        std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();

        std::vector<File> files; // as listed: newest first
        std::string directory;
        std::string scanError;
        bool truncated = false;
        bool renderdoc = false;
        bool restartQueued = false;
        std::optional<bool> renderdocTarget;
        std::array<bool, CAPTURE_OPS> busy{};
        std::array<i32, BTN_COUNT> enabled;
        std::string renderdocTip;
        u32 viewIndex = 0; // the Screenshot dropdown: 0 the presented frame, k render view k - 1
        bool stale = true; // a capture ended or a file changed since the last read
        bool wasShown = false;
        bool selectionMoved = false;
        bool hasRevealed = false;
        std::uint64_t lastRevealMs = 0;
        u64 seenLocale = ~0ull;
        u64 scans = 0;
        std::string thumbPath; // the file the thumbnail slot was given, empty: none
        bool thumbWaiting = false; // given to the view, not yet read
        bool thumbFailed = false;
        Vec2 thumbPixels{}; // the texture the view made of it
        Vec2 thumbBox{128.0f, 72.0f}; // the most it takes, from the size.thumbnail.* tokens
        std::function<bool(const std::filesystem::path&)> revealFile = revealInFileManager;
        std::function<std::uint64_t()> clock = []
        {
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::steady_clock::now().time_since_epoch())
                                                  .count());
        };

        [[nodiscard]] std::string message(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            const std::string full = std::string(TABLE) + std::string(key);
            if (!localization)
            {
                return full;
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(full), args, scratch));
        }

        void toast(const ToastLevel level, const std::string_view key, const std::span<const MessageArg> args = {})
        {
            feedback->postToastKey(level, std::string(TABLE) + std::string(key), args);
        }

        void refused(const u32 action, const std::string& error)
        {
            const std::string label = message(std::string("action.") + std::string(ACTIONS[action]));
            const MessageArg args[]{{"action", std::string_view(label)}, {"error", std::string_view(error)}};
            toast(ToastLevel::Warn, "toast.refused", args);
        }

        // -- the directory ----------------------------------------------------------------------------------------

        void scan()
        {
            SPITE_ZONE("EditorCapture.scan");
            ++scans;
            stale = false;
            files.clear();
            scanError.clear();
            truncated = false;
            DirectoryListing listing;
            scanError = control->listDirectory(CAPTURES_ROOT, {}, false, listing, true);
            if (scanError.empty())
            {
                directory = listing.directory;
                truncated = listing.truncated;
                files.reserve(listing.entries.size());
                const std::filesystem::path base = pathFromUtf8(directory);
                for (DirectoryEntry& entry : listing.entries)
                {
                    if (entry.kind == DirectoryEntryKind::File)
                    {
                        std::string path = utf8Path(base / pathFromUtf8(entry.name));
                        files.push_back({.name = std::move(entry.name),
                                         .size = entry.size,
                                         .modified = std::move(entry.modified),
                                         .path = std::move(path)});
                    }
                }
            }
        }

        [[nodiscard]] std::vector<ArtifactCells> cellsOf() const
        {
            std::vector<ArtifactCells> cells;
            cells.reserve(files.size());
            for (const File& file : files)
            {
                cells.push_back({file.name, message(std::string("kind.") + std::string(KINDS[kindOf(file.name)])),
                                 sizeText(file.size), localStamp(file.modified), file.path});
            }
            return cells;
        }

        [[nodiscard]] std::optional<u32> pickedRow() const
        {
            const std::span<const Entity> picked = rows->table().selection();
            return picked.empty() ? std::nullopt : std::optional(picked.front().index() - 1);
        }

        [[nodiscard]] const File* picked() const
        {
            const std::optional<u32> row = pickedRow();
            return row && *row < files.size() ? &files[*row] : nullptr;
        }

        [[nodiscard]] std::string pickedName() const
        {
            const File* file = picked();
            return file ? file->name : std::string{};
        }

        // The table from the files, the pick kept by name (a new file shifts every row below it).
        void fillRows(const std::string& name)
        {
            rows->set(cellsOf());
            const auto at = std::ranges::find(files, name, &File::name);
            if (at != files.end() && !name.empty())
            {
                const Entity key(static_cast<u32>(at - files.begin()) + 1, 1u);
                rows->table().setSelection(std::span<const Entity>(&key, 1));
            }
            else if (!rows->table().selection().empty())
            {
                rows->table().setSelection({});
            }
            seenLocale = localization ? localization->revision() : 0;
        }

        // -- the controls and the line under the list -------------------------------------------------------------

        void setEnabled(const u32 slot, const bool on)
        {
            if (enabled[slot] != static_cast<i32>(on))
            {
                enabled[slot] = static_cast<i32>(on);
                buttons[slot].setInteractable(on);
            }
        }

        [[nodiscard]] UiViewContext& uiView() const { return editorUi(*engine).uiView(); }

        // The thumbnail keeps the image aspect inside thumbBox and never grows past its own pixels.
        void fitThumb()
        {
            const float scale = std::min({1.0f, thumbBox.x / thumbPixels.x, thumbBox.y / thumbPixels.y});
            thumb.setSize(Vec2{std::max(1.0f, std::round(thumbPixels.x * scale)),
                               std::max(1.0f, std::round(thumbPixels.y * scale))});
        }

        void dropThumb()
        {
            if (!thumbPath.empty())
            {
                uiView().clearImage(THUMB_SLOT);
            }
            thumbPath.clear();
            thumbWaiting = false;
            thumbFailed = false;
            if (thumb.isVisible())
            {
                thumb.setVisible(false);
            }
        }

        // The picked image file is the thumbnail's; any other pick, or a hidden panel, frees it.
        void showThumb()
        {
            const File* file = picked();
            if (!file || kindOf(file->name) != KIND_IMAGE || !shown(scope, root))
            {
                dropThumb();
                return;
            }
            if (file->path != thumbPath)
            {
                dropThumb();
                thumbPath = file->path;
                thumbWaiting = true;
                uiView().registerImageFile(THUMB_SLOT, thumbPath);
            }
        }

        // Once per frame while the view reads the file: shows the thumbnail when it is ready.
        void pollThumb()
        {
            const std::optional<UiImageInfo> info = uiView().imageInfo(THUMB_SLOT);
            if (!info || info->state == UiImageState::Pending)
            {
                return;
            }
            thumbWaiting = false;
            if (info->state == UiImageState::Ready)
            {
                thumbPixels = Vec2{static_cast<float>(info->width), static_cast<float>(info->height)};
                fitThumb();
                thumb.setVisible(true);
            }
            else
            {
                thumbFailed = true;
                showState();
            }
        }

        [[nodiscard]] std::string detailText(const File* file) const
        {
            std::vector<std::string> parts;
            if (file)
            {
                parts.push_back(pathTail(file->path));
            }
            else if (!scanError.empty())
            {
                const MessageArg args[]{{"error", std::string_view(scanError)}};
                parts.push_back(message("scanFailed", args));
            }
            else
            {
                u64 total = 0;
                for (const File& each : files)
                {
                    total += each.size;
                }
                const std::string size = sizeText(total);
                const MessageArg args[]{{"count", static_cast<std::int64_t>(files.size())},
                                        {"size", std::string_view(size)}};
                parts.push_back(message(files.empty() ? "empty" : "summary", args));
            }
            std::string working;
            for (u32 op = 0; op < CAPTURE_OPS; ++op)
            {
                if (busy[op])
                {
                    working +=
                        (working.empty() ? "" : ", ") + message(std::string("action.") + std::string(ACTIONS[op]));
                }
            }
            if (!working.empty())
            {
                const MessageArg args[]{{"actions", std::string_view(working)}};
                parts.push_back(message("busy", args));
            }
            if (truncated)
            {
                const MessageArg args[]{{"count", static_cast<std::int64_t>(files.size())}};
                parts.push_back(message("truncated", args));
            }
            if (thumbFailed)
            {
                parts.push_back(message("thumb.failed"));
            }
            if (restartQueued)
            {
                parts.push_back(message("renderdoc.restarting"));
            }
            else if (!file && !renderdoc)
            {
                parts.push_back(message("renderdoc.off"));
            }
            std::string text;
            for (const std::string& part : parts)
            {
                text += (text.empty() ? "" : " | ") + part;
            }
            return text;
        }

        void showState()
        {
            renderdoc = engine->renderDocEnabled();
            restartQueued = engine->rendererRestartPending();
            for (u32 op = 0; op < CAPTURE_OPS; ++op)
            {
                setEnabled(op, !restartQueued && !busy[op] && (op != BTN_RENDERDOC || renderdoc));
            }
            const char* toggleLabel = restartQueued ? "action.renderdoc.restarting"
                : renderdoc                         ? "action.renderdoc.disable"
                                                    : "action.renderdoc.enable";
            renderdocToggle.setLocalizedText(InternedString(std::string(TABLE) + toggleLabel));
            renderdocToggle.setInteractable(!restartQueued && !busy[BTN_RENDERDOC]);
            const File* file = picked();
            showThumb();
            for (const u32 action : {BTN_REVEAL, BTN_COPY, BTN_DELETE})
            {
                setEnabled(action, file != nullptr);
            }
            if (const std::string tip = renderdoc ? "tip.renderdoc" : "tip.renderdoc.off"; tip != renderdocTip)
            {
                renderdocTip = tip;
                buttons[BTN_RENDERDOC].setTooltip(InternedString(std::string(TABLE) + tip));
            }
            if (const std::string text = detailText(file); detail.text() != text)
            {
                detail.setText(text);
            }
            selectionMoved = false;
        }

        void toggleRenderDoc()
        {
            const bool enable = !engine->renderDocEnabled();
            const Json reply =
                request(*control, Json{{"command", "restartRenderer"}, {"args", {{"renderdoc", enable}}}});
            if (!reply.value("ok", false))
            {
                const std::string error = reply.value("error", std::string{});
                const MessageArg args[]{{"error", std::string_view(error)}};
                toast(ToastLevel::Warn, "toast.renderdoc.switchFailed", args);
                return;
            }
            renderdocTarget = enable;
            showState();
        }

        void followRenderDoc()
        {
            const bool enabled = engine->renderDocEnabled();
            const bool queued = engine->rendererRestartPending();
            if (renderdocTarget && !queued)
            {
                const Json reply = request(*control, Json{{"query", "rendererRestartStatus"}});
                const std::string status = reply.value("result", Json::object()).value("status", std::string{});
                if (status == "complete" && enabled == *renderdocTarget)
                    toast(ToastLevel::Info, enabled ? "toast.renderdoc.enabled" : "toast.renderdoc.disabled");
                else
                {
                    const MessageArg args[]{{"error", std::string_view(status)}};
                    toast(ToastLevel::Warn, "toast.renderdoc.switchFailed", args);
                }
                renderdocTarget.reset();
            }
            selectionMoved |= renderdoc != enabled || restartQueued != queued;
        }

        void showViews()
        {
            const u32 views = engine->config().graphics.renderViewCount;
            std::vector<UiChoiceItem> items;
            items.push_back({.label = message("view.presented")});
            for (u32 view = 0; view < views; ++view)
            {
                const MessageArg args[]{{"view", static_cast<std::int64_t>(view)}};
                items.push_back({.label = message("view.n", args)});
            }
            viewChoice.setItems(items);
            viewChoice.setSelected(static_cast<i32>(std::min<u32>(viewIndex, views)));
        }

        // Reads the root and shows it; the query and the commands that act on a row come here first, so a hidden
        // panel answers from the disk, not from its last look.
        void sync()
        {
            const std::string name = pickedName();
            scan();
            if (localization && localization->revision() != seenLocale)
            {
                showViews(); // a locale change while the panel was hidden
            }
            fillRows(name);
            showState();
        }

        // -- commands to the engine -------------------------------------------------------------------------------

        // The engine's answer to op: a toast with the file or the engine's reason, then the root is read again.
        void finish(const u32 op, const std::string& text)
        {
            busy[op] = false;
            stale = true;
            const Json reply = Json::parse(text, nullptr, false);
            if (!reply.is_object() || !reply.value("ok", false))
            {
                refused(op, reply.is_object() ? reply.value("error", std::string{}) : std::string("unparsable reply"));
                return;
            }
            const Json& result = reply["result"];
            const std::string name = nameOf(result.value("path", std::string{}));
            const MessageArg args[]{{"name", std::string_view(name)},
                                    {"width", static_cast<std::int64_t>(result.value("width", 0u))},
                                    {"height", static_cast<std::int64_t>(result.value("height", 0u))}};
            toast(ToastLevel::Info, std::string("toast.saved.") + std::string(ACTIONS[op]), args);
        }

        // Sends an engine command. A deferred one is answered through the inbox, on a later frame; the frame loop
        // never waits for it, and its button stays off meanwhile.
        void start(const u32 op, const char* kind, const char* command, const Json& args)
        {
            busy[op] = true;
            const std::string reply =
                control->handle(Json{{kind, command}, {"args", args}}.dump(),
                                [box = std::weak_ptr<Inbox>(inbox), op](const std::string_view text)
                                {
                                    if (const std::shared_ptr<Inbox> alive = box.lock())
                                    {
                                        alive->done.emplace_back(op, std::string(text));
                                    }
                                });
            if (!reply.empty())
            {
                inbox->done.emplace_back(op, reply);
            }
        }

        void capture(const u32 op)
        {
            if (busy[op] || engine->rendererRestartPending())
            {
                return;
            }
            if (op == BTN_FRAME)
            {
                start(op, "command", "capture", Json::object());
            }
            else if (op == BTN_SCREENSHOT)
            {
                start(op, "command", "screenshot", viewIndex == 0 ? Json::object() : Json{{"view", viewIndex - 1}});
            }
            else if (op == BTN_RENDERDOC)
            {
                start(op, "command", "renderdocCapture", Json::object());
            }
            else
            {
                start(op, "query", "snapshot", Json{{"path", snapshotName()}});
            }
            showState();
        }

        // snapshot writes over a file of its name: two clicks in one second get _2, _3, ...
        std::string snapshotName() const
        {
            const auto now = std::chrono::floor<std::chrono::seconds>(
                std::chrono::current_zone()->to_local(std::chrono::system_clock::now()));
            const std::string stem = std::format("snapshot_{:%Y%m%d-%H%M%S}", now);
            std::string name = stem + ".sqlite";
            std::filesystem::path path;
            std::error_code error;
            for (u32 n = 2;
                 control->resolveFile(CAPTURES_ROOT, name, path).empty() && std::filesystem::exists(path, error); ++n)
            {
                name = std::format("{}_{}.sqlite", stem, n);
            }
            return name;
        }

        // -- the picked file --------------------------------------------------------------------------------------

        // The picked file's place under the root, through the root's confinement; an error names what to do.
        std::string pickedPath(std::filesystem::path& out, std::string* name = nullptr)
        {
            sync();
            const File* file = picked();
            if (!file)
            {
                return "select one file first (captureSelect)";
            }
            if (name)
            {
                *name = file->name;
            }
            return control->resolveFile(CAPTURES_ROOT, file->name, out);
        }

        std::string revealPicked(IFormatWriter& out)
        {
            std::filesystem::path path;
            if (std::string error = pickedPath(path); !error.empty())
            {
                return error;
            }
            const std::uint64_t now = clock();
            if (hasRevealed && now - lastRevealMs < REVEAL_GAP_MS)
            {
                return "the file manager was shown a moment ago";
            }
            hasRevealed = true;
            lastRevealMs = now;
            if (!revealFile(path))
            {
                return "the file manager could not be started";
            }
            out.writeString("path", utf8Path(path));
            return {};
        }

        std::string copyPicked(IFormatWriter& out)
        {
            std::filesystem::path path;
            if (std::string error = pickedPath(path); !error.empty())
            {
                return error;
            }
            editorUi(*engine).ui().setClipboardText(utf8Path(path));
            out.writeString("path", utf8Path(path));
            toast(ToastLevel::Info, "toast.copied");
            return {};
        }

        std::string askDelete()
        {
            std::filesystem::path path;
            std::string name;
            if (std::string error = pickedPath(path, &name); !error.empty())
            {
                return error;
            }
            const MessageArg args[]{{"name", std::string_view(name)}};
            dialogs->confirm({.title = message("delete.title"),
                              .body = message("delete.body", args),
                              .confirmLabel = message("delete.confirm"),
                              .destructive = true},
                             [this, name](const bool confirmed)
                             {
                                 if (confirmed)
                                 {
                                     deleteFile(name);
                                 }
                             });
            return {};
        }

        void deleteFile(const std::string& name)
        {
            std::filesystem::path path;
            std::error_code error;
            if (std::string refusal = control->resolveFile(CAPTURES_ROOT, name, path); !refusal.empty())
            {
                refused(BTN_DELETE, refusal);
                return;
            }
            if (!std::filesystem::is_regular_file(path, error) || !std::filesystem::remove(path, error))
            {
                refused(BTN_DELETE, error ? error.message() : std::string("not a file"));
            }
            else
            {
                const MessageArg args[]{{"name", std::string_view(name)}};
                toast(ToastLevel::Info, "toast.deleted", args);
            }
            sync();
        }

        void select(const u32 row)
        {
            const Entity key(row + 1, 1u);
            rows->table().setSelection(std::span<const Entity>(&key, 1));
            showState();
        }

        // -- control ----------------------------------------------------------------------------------------------

        // A button of the row actions or Refresh: the view command, queued like every panel action; a refusal is a
        // toast with the engine's reason.
        void command(const u32 action, const char* name)
        {
            pending.push_back(
                [this, action, name]
                {
                    const Json reply = request(*control, Json{{"command", name}, {"args", Json::object()}});
                    if (!reply.value("ok", false))
                    {
                        refused(action, reply.value("error", std::string{}));
                    }
                });
        }

        void registerEntries()
        {
            control->registerViewCommand(
                "captureSelect", OWNER,
                "selects a file of the capture panel's list by name, or by row (from 0, newest first, as captureView "
                "lists them); the list is newest first and live, so a row alone can name another file after a capture: "
                "expect=<name> refuses when the row no longer holds that name",
                {optionalArg("name", ArgKind::String, "the file name; give this or row"),
                 optionalArg("row", ArgKind::Integer, "the row, as captureView lists them; give this or name"),
                 optionalArg("expect", ArgKind::String, "with row: the name the client saw there")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const std::optional<std::string> byName = args.string("name");
                    const std::optional<u64> byRow = args.integer("row");
                    const std::optional<std::string> expected = args.string("expect");
                    if (byName.has_value() == byRow.has_value())
                    {
                        return std::string("give name or row, one of them");
                    }
                    if (expected && !byRow)
                    {
                        return std::string("expect goes with row; name is the identity itself");
                    }
                    sync();
                    sizet index = 0;
                    if (byName)
                    {
                        const auto at = std::ranges::find(files, *byName, &File::name);
                        if (at == files.end())
                        {
                            return truncated ? std::format("no file {} among the newest {} listed of the captures root",
                                                           *byName, files.size())
                                             : std::format("no file {} in the captures root", *byName);
                        }
                        index = static_cast<sizet>(at - files.begin());
                    }
                    else
                    {
                        index = *byRow;
                        if (index >= files.size())
                        {
                            return std::format("row {} out of range: the list has {} file(s)", index, files.size());
                        }
                        if (expected && files[index].name != *expected)
                        {
                            return std::format("row {} holds {}, not {}: the list changed since it was read", index,
                                               files[index].name, *expected);
                        }
                    }
                    select(static_cast<u32>(index));
                    return std::string{};
                });
            control->registerViewCommand("captureRefresh", OWNER,
                                         "reads the captures root again for the capture panel's list", {},
                                         [this](const ControlArgs&, IFormatWriter&)
                                         {
                                             sync();
                                             return std::string{};
                                         });
            control->registerViewCommand("captureReveal", OWNER,
                                         "shows the picked file in the file manager (not twice in a second)", {},
                                         [this](const ControlArgs&, IFormatWriter& out) { return revealPicked(out); });
            control->registerViewCommand("captureCopy", OWNER, "copies the picked file's full path to the clipboard",
                                         {},
                                         [this](const ControlArgs&, IFormatWriter& out) { return copyPicked(out); });
            control->registerViewCommand("captureDelete", OWNER,
                                         "asks, then deletes the picked file from the captures root; the dialog is "
                                         "answered like any (answerDialog)",
                                         {}, [this](const ControlArgs&, IFormatWriter&) { return askDelete(); });
            control->registerQuery(
                "captureView", OWNER,
                "the capture panel read from the captures root, shown or hidden: rows (name, kind, size, time, path, "
                "bytes, modified; newest first, files at the top of the root), directory, selected, the line under the "
                "list "
                "(detail), the operations in progress (busy), truncated when the root holds more than the listing "
                "gives, renderdoc (whether the capture layer is enabled), restartQueued, the Screenshot dropdown's "
                "view (-1 the "
                "presented frame), the directory reads made (scans) and thumbnail: the picked PNG "
                "(path; state none|pending|ready|failed; pixelsWidth and pixelsHeight of the texture, "
                "at most 512 on a side; visible, and width and height of the fitted preview)",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    sync();
                    const std::vector<ArtifactCells> cells = cellsOf();
                    out.beginArray("rows", static_cast<u32>(cells.size()));
                    for (sizet i = 0; i < cells.size(); ++i)
                    {
                        out.beginObject("");
                        writeValue(ArtifactCells::spiteSchema(), &cells[i], out, FieldSet::All);
                        out.writeU64("bytes", files[i].size);
                        out.writeString("modified", files[i].modified);
                        out.endObject();
                    }
                    out.endArray();
                    out.writeString("directory", directory);
                    out.writeBool("truncated", truncated);
                    out.writeBool("renderdoc", renderdoc);
                    out.writeBool("restartQueued", restartQueued);
                    out.writeI32("view", static_cast<i32>(viewIndex) - 1);
                    out.beginArray("busy", 0);
                    for (u32 op = 0; op < CAPTURE_OPS; ++op)
                    {
                        if (busy[op])
                        {
                            out.writeString({}, ACTIONS[op]);
                        }
                    }
                    out.endArray();
                    const std::optional<u32> row = pickedRow();
                    out.writeI32("selected", row && *row < files.size() ? static_cast<i32>(*row) : -1);
                    out.writeString("detail", detail.text());
                    out.beginObject("thumbnail");
                    out.writeString("path", thumbPath);
                    const std::optional<UiImageInfo> info =
                        thumbPath.empty() ? std::nullopt : uiView().imageInfo(THUMB_SLOT);
                    out.writeString("state",
                                    !info                                     ? "none"
                                        : info->state == UiImageState::Ready  ? "ready"
                                        : info->state == UiImageState::Failed ? "failed"
                                                                              : "pending");
                    out.writeU32("pixelsWidth", info ? info->width : 0);
                    out.writeU32("pixelsHeight", info ? info->height : 0);
                    out.writeBool("visible", thumb.isVisible());
                    out.writeF32("width", thumb.isVisible() ? thumb.size().x : 0.0f);
                    out.writeF32("height", thumb.isVisible() ? thumb.size().y : 0.0f);
                    out.endObject();
                    out.writeString("error", scanError);
                    out.writeU64("scans", scans);
                    return std::string{};
                });
        }
    };

    EditorCapture::EditorCapture(Engine& engine, EditorFeedback& feedback, EditorDialogs& dialogs) :
        m_state(std::make_unique<State>(engine, feedback, dialogs))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.enabled.fill(-1);
        state.root = scope.createPanel("editor.panel", columnDesc());
        const auto fitButton =
            [&state, &scope](const char* id, const std::string_view label, const std::string_view tip)
        {
            UiRectDesc desc = barDesc(Vec2{});
            desc.contentSize = UiContentSizeComponent{
                .fitWidth = true, .fitHeight = false, .padding = Vec2{state.density.density().lg, 0.0f}};
            Button button = state.density.track(scope.createButton(id, "", desc), HEIGHT_CONTROL);
            bindFitPadding(button, SPACE_LG, SPACE_NONE);
            button.setLocalizedText(InternedString(std::string(TABLE) + "action." + std::string(label)));
            if (!tip.empty())
            {
                button.setTooltip(InternedString(std::string(TABLE) + std::string(tip)));
            }
            return button;
        };
        const char* ids[]{"capture.frame",  "capture.snapshot", "capture.screenshot", "capture.renderdoc",
                          "capture.reveal", "capture.copy",     "capture.delete"};
        const char* tips[]{"tip.frame",  "tip.snapshot", "tip.screenshot", "tip.renderdoc",
                           "tip.reveal", "tip.copy",     "tip.delete"};
        for (u32 action = 0; action < BTN_COUNT; ++action)
        {
            state.buttons[action] = fitButton(ids[action], ACTIONS[action], tips[action]);
            if (action < CAPTURE_OPS)
            {
                state.buttons[action].onClick(
                    [&state, action] { state.pending.push_back([&state, action] { state.capture(action); }); });
            }
        }
        state.buttons[BTN_REVEAL].onClick([&state] { state.command(BTN_REVEAL, "captureReveal"); });
        state.buttons[BTN_COPY].onClick([&state] { state.command(BTN_COPY, "captureCopy"); });
        state.buttons[BTN_DELETE].onClick([&state] { state.command(BTN_DELETE, "captureDelete"); });
        state.renderdocToggle = fitButton("capture.renderdoc.toggle", "renderdoc.enable", "tip.renderdoc.toggle");
        state.renderdocToggle.onClick([&state] { state.pending.push_back([&state] { state.toggleRenderDoc(); }); });
        state.refresh = fitButton("capture.refresh", "refresh", "tip.refresh");
        state.refresh.onClick(
            [&state]
            {
                state.pending.push_back(
                    [&state]
                    { (void)request(*state.control, Json{{"command", "captureRefresh"}, {"args", Json::object()}}); });
            });
        UiDropdownDesc viewDesc;
        viewDesc.rect = barDesc(Vec2{VIEW_WIDTH, 0.0f});
        state.viewChoice = state.density.track(scope.createDropdown("capture.view", viewDesc), HEIGHT_CONTROL);
        state.viewChoice.setAccessibleNameKey(InternedString(std::string(TABLE) + "action.view"));
        state.viewChoice.setTooltip(InternedString(std::string(TABLE) + "tip.view"));
        state.viewChoice.onChange([&state](const u32 index) { state.viewIndex = index; });
        state.showViews();

        state.rows = std::make_unique<RowTable<ArtifactCells>>(
            scope, "capture.table", InternedString(std::string(TABLE) + "table"),
            std::vector{column("name", 320.0f), column("kind", 110.0f), column("size", 90.0f), column("time", 150.0f)});
        state.rows->table().onSelect([&state](const std::span<const Entity>) { state.selectionMoved = true; });
        UiRectDesc detailDesc = barDesc(Vec2{});
        detailDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        detailDesc.layoutItem = fillItem();
        state.detail = scope.createLabel("capture.detail.text", "", detailDesc);
        state.detail.setStyleClass(InternedString("settings.noteWrap"));
        state.detail.setInteractable(false);
        UiRectDesc rowDesc =
            barDesc(Vec2{}, stack(UiLayoutDirection::Row, state.density.density().sm, Vec4{}, UiLayoutAlign::Center));
        rowDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        state.detailRow = scope.createPanel("capture.detail.row", rowDesc);
        bindGap(state.detailRow, SPACE_SM);
        state.thumb = scope.createImage("capture.thumb", barDesc(Vec2{}));
        state.thumb.setImageSlot(THUMB_SLOT);
        state.thumb.setAccessibleNameKey(InternedString(std::string(TABLE) + "access.thumbnail"));
        state.thumb.setVisible(false);
        state.thumb.setParent(state.detailRow);
        state.detail.setParent(state.detailRow);
        state.density.apply(
            [&state](const Density&)
            {
                const UiLayoutTokenTable& tokens = state.scope.layoutTokens();
                state.thumbBox = Vec2{tokens.find(InternedString(SIZE_THUMB_WIDTH)).value_or(128.0f),
                                      tokens.find(InternedString(SIZE_THUMB_HEIGHT)).value_or(72.0f)};
                if (state.thumb.isVisible())
                {
                    state.fitThumb();
                }
            });
        UiRectDesc spacerDesc = barDesc(Vec2{});
        spacerDesc.layoutItem = fillItem();
        state.spacer = scope.createPanel("capture.spacer", spacerDesc);
        state.spacer.setInteractable(false);
        state.registerEntries();
        scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(CAPTURE_TOOLBAR),
                      [&state](const Widget& slot)
                      {
                          for (Widget widget : std::initializer_list<Widget>{
                                   state.buttons[BTN_FRAME], state.buttons[BTN_SNAPSHOT], state.buttons[BTN_SCREENSHOT],
                                   state.viewChoice, state.renderdocToggle, state.buttons[BTN_RENDERDOC], state.spacer,
                                   state.buttons[BTN_REVEAL], state.buttons[BTN_COPY], state.buttons[BTN_DELETE],
                                   state.refresh})
                          {
                              putIn(state.scope, slot, widget);
                          }
                      });
        contract.slot(InternedString(CAPTURE_ROWS),
                      [&state](const Widget& slot) { state.rows->fill(state.scope, slot); });
        contract.slot(InternedString(CAPTURE_DETAIL),
                      [&state](const Widget& slot) { putIn(state.scope, slot, state.detailRow); });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                const Density density = Density::read(viewScope);
                const auto wrapBar = [&](const std::string_view id)
                {
                    UiLayoutComponent line =
                        stack(UiLayoutDirection::Row, density.xs,
                              Vec4{density.xs, density.xxs, density.xs, density.xxs}, UiLayoutAlign::Center);
                    line.wrap = true;
                    UiRectDesc desc = barDesc(Vec2{}, line);
                    desc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                    Panel bar = viewScope.createPanel(InternedString(id), desc);
                    bindGap(bar, SPACE_XS);
                    bindPadding(bar, SPACE_XS, SPACE_XXS, SPACE_XS, SPACE_XXS);
                    return bar;
                };
                wrapBar(CAPTURE_TOOLBAR).setParent(root);
                fillSlotPanel(viewScope, CAPTURE_ROWS, root);
                UiRectDesc detailDesc = barDesc(Vec2{},
                                                stack(UiLayoutDirection::Column, 0.0f,
                                                      Vec4{density.xs, density.hairline, density.xs, density.hairline},
                                                      UiLayoutAlign::Stretch));
                detailDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                Panel detail = viewScope.createPanel(InternedString(CAPTURE_DETAIL), detailDesc);
                bindPadding(detail, SPACE_XS, SPACE_HAIRLINE, SPACE_XS, SPACE_HAIRLINE);
                detail.setParent(root);
            });
        state.view = openView(engine, CAPTURE_VIEW, "assets/ui/editor/capture.json", std::move(contract), state.root);
    }

    EditorCapture::~EditorCapture() { m_state->control->unregisterOwner(OWNER); }

    Widget EditorCapture::content() const { return m_state->root; }

    void EditorCapture::setReveal(std::function<bool(const std::filesystem::path&)> reveal,
                                  std::function<std::uint64_t()> nowMs)
    {
        m_state->revealFile = std::move(reveal);
        if (nowMs)
        {
            m_state->clock = std::move(nowMs);
        }
    }

    void EditorCapture::update()
    {
        State& state = *m_state;
        if (!state.pending.empty())
        {
            std::vector<std::function<void()>> actions;
            actions.swap(state.pending);
            for (const std::function<void()>& action : actions)
            {
                action();
            }
        }
        if (!state.inbox->done.empty())
        {
            std::vector<std::pair<u32, std::string>> done;
            done.swap(state.inbox->done);
            for (const auto& [op, text] : done)
            {
                state.finish(op, text);
            }
        }
        state.followRenderDoc();
        if (!shown(state.scope, state.root))
        {
            state.wasShown = false; // a hidden panel reads nothing; showing it catches up
            state.dropThumb();
            return;
        }
        if (state.thumbWaiting)
        {
            state.pollThumb();
        }
        if (!state.wasShown || state.stale)
        {
            state.wasShown = true;
            state.sync();
        }
        else if (state.localization && state.localization->revision() != state.seenLocale)
        {
            state.fillRows(state.pickedName());
            state.showViews();
            state.showState();
        }
        else if (state.selectionMoved)
        {
            state.showState();
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "capture",
             .category = "debug",
             .order = 96,
             .dock = {.target = "project"},
             .zone = "EditorCapture",
             .make = [](EditorPanelContext& c)
             { return makeEditorPanel<EditorCapture>(c.engine, c.feedback, c.dialogs); }}};
    } // namespace
} // namespace spite
