#include "EditorProfiler.hpp"
#include "EditorCaptureFiles.hpp"
#include "EditorPanelRegistry.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "EditorContext.hpp"
#include "EditorDialogs.hpp"
#include "EditorFeedback.hpp"
#include "EditorPanelUi.hpp"
#include "EditorTimeline.hpp"
#include "EditorProfilerList.hpp"
#include "EditorProfilerPanes.hpp"
#include "EditorStatusText.hpp"
#include "ProfilerData.hpp"
#include "ProfilerSelection.hpp"
#include "ProfilerView.hpp"
#include "application/Shell.hpp"
#include "base/Logging.hpp"
#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "engine/CaptureStore.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "rendering/IRenderer.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiChart.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    using namespace panelui;

    namespace
    {
        constexpr std::string_view OWNER = "editorProfiler";
        constexpr std::array<std::string_view, 4> CLEAR_CHOICES{"frames", "zone", "gpu", "all"};
        constexpr u32 SERIES_CPU = 0;
        constexpr u32 SERIES_GPU = 1;
        constexpr u32 SERIES_BUDGET = 2; // 16.7 ms, then 33.3 ms
        constexpr u32 MAX_ENTRIES = 512;
        constexpr u32 PAGES_PER_FRAME = 2;
        constexpr u32 LIVE_WINDOW = 1200; // frames the live graph shows
        constexpr u32 MAX_ERROR_CHARS = 160;
        constexpr std::array<std::string_view, 6> SCROLL_TARGETS{"top", "list", "graph", "panes", "timeline", "bottom"};
        constexpr std::array<const char*, 5> PANE_IDS{"counters", "gpu", "memory", "calls", "compare"};

        std::string clipped(const std::string_view text, const u32 limit = MAX_ERROR_CHARS)
        {
            if (text.size() <= limit)
            {
                return std::string(text);
            }
            u32 end = limit;
            while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
            {
                --end;
            }
            return std::string(text.substr(0, end)) + "...";
        }

        // An engine error as a person reads it: what was being done first, and quoted paths cut to their file name
        std::string friendly(const UiScope& scope, const std::string_view line, const std::string_view raw)
        {
            struct Action
            {
                std::string_view command;
                const char* key;
            };
            static constexpr Action ACTIONS[] = {
                {"\"profileStart\"", "error.start"},    {"\"profileStop\"", "error.stop"},
                {"\"profileOpen\"", "error.open"},      {"\"profileClose\"", "error.close"},
                {"\"profilerSelect\"", "error.select"}, {"\"profilerCopyPath\"", "error.copyPath"},
                {"\"profilerReveal\"", "error.reveal"}, {"\"profilerDelete\"", "error.delete"}};
            std::string text;
            for (sizet at = 0; at < raw.size();)
            {
                const sizet quote = raw.find('\'', at);
                const sizet end = quote == std::string_view::npos ? quote : raw.find('\'', quote + 1);
                if (end == std::string_view::npos)
                {
                    text += raw.substr(at);
                    break;
                }
                const std::string_view quoted = raw.substr(quote + 1, end - quote - 1);
                const sizet slash = quoted.find_last_of("\\/");
                text += raw.substr(at, quote - at);
                text += slash == std::string_view::npos ? raw.substr(quote, end - quote + 1) : quoted.substr(slash + 1);
                at = end + 1;
            }
            const auto action = std::ranges::find_if(ACTIONS, [&](const Action& a)
                                                     { return line.find(a.command) != std::string_view::npos; });
            return clipped((action == std::end(ACTIONS) ? std::string() : profilerText(scope, action->key) + ": ") +
                           text);
        }

        // One capture of the store, as profileCaptures reports it
        struct StoreRow
        {
            CaptureId id = 0;
            std::string name;
            std::string state;
            std::string error;
            bool live = false;
            u64 frames = 0;
            u64 durationNs = 0;
            u64 fileBytes = 0;

            bool operator==(const StoreRow&) const = default;
        };

        // One row of the capture list: a store capture, or a sidecar on disk the store does not hold
        struct Entry
        {
            bool inStore = false;
            CaptureId id = 0;
            std::string path; // files: relative to the artifacts root, what profileOpen takes
            std::string name;
            std::string state; // ready, writing, failed; files: file
            bool live = false;
            u64 frames = 0;
            u64 durationNs = 0;
            u64 bytes = 0;
            std::string error;
            std::int64_t modifiedMs = 0; // unix time of the sidecar; 0 when the scan has not listed it
        };

        // A sidecar under this is a stub (the test and smoke runs leave dozens): hidden unless asked for
        constexpr u64 SMALL_BYTES = u64{1} << 20;

        // The document's ids (assets/ui/editor/profiler.json): the view, the page and the column the code marks, and
        // the slots it fills
        constexpr std::string_view VIEW_ID = "editor.profiler";
        constexpr std::string_view PAGE_ID = "profiler.scroll";
        constexpr std::string_view LEFT_ID = "profiler.left";
        constexpr std::string_view BAR_ID = "profiler.bar";
        constexpr std::string_view NOTICE_ID = "profiler.noticeRow";
        constexpr std::string_view REPORT_ID = "profiler.report";
        constexpr std::string_view SMALL_BAR_ID = "profiler.smallBar";
        constexpr std::string_view LIST_BAR_ID = "profiler.listBar";
        constexpr std::string_view LIST_ID = "profiler.list";
        constexpr std::string_view GRAPH_ID = "profiler.graphColumn";

    } // namespace

    struct EditorProfiler::State
    {
        static constexpr u32 REFRESH_FRAMES = 30;
        static constexpr u32 WRITING_REFRESH_FRAMES = 6;
        static constexpr float LIST_WIDTH = 340.0f;
        // the list gives way to the graph column down to LIST_MIN; the column keeps what the timeline bar needs (the
        // find box's least width and its four buttons), so the centre dock group at 1024 px shows every control
        static constexpr float LIST_MIN = 236.0f;
        static constexpr float GRAPH_MIN = 296.0f;

        // The scrolling page's minimum sizes: the regions never get less than these, the page grows and
        // scrolls instead. The panes' tab bar sits above the timeline: its place depends on the viewport only, an
        // open view adds its height to the page below the tab bar.
        static constexpr float CHART_MIN = 120.0f;

        // The page's minimum height at the density: the bar, the graph column or the left column (the taller), and the
        // page's padding and gaps
        [[nodiscard]] float rowHeight() const { return HEIGHT_BUTTON.of(density.density()); }
        [[nodiscard]] float listMin() const { return 4.0f * ProfilerCaptureList::rowHeight(density.density()); }
        [[nodiscard]] float pageMin() const
        {
            const Density& d = density.density();
            const float graph = rowHeight() + CHART_MIN + ProfilerPanes::collapsedHeight(d) +
                ProfilerTimeline::MIN_HEIGHT + ProfilerTimeline::chromeHeight(d) + 5.0f * d.xs;
            const float leftColumn = 3.0f * (rowHeight() + d.xs) + listMin();
            return 2.0f * d.xs + rowHeight() + d.xs + std::max(graph, leftColumn);
        }

        using Clock = std::chrono::steady_clock;

        // profileStatus read straight off the query, without a JSON round trip: a refresh allocates nothing
        struct Status : IFormatWriter
        {
            bool armed = false;
            bool stopping = false; // the last stop's report is still being written
            bool lastReportValid = true;
            double seconds = 0.0;
            u32 lastCapture = 0;
            std::string lastWarning;

            u32 depth = 0; // only the top-level keys are the panel's: stop{} nests its own seconds

            void beginObject(std::string_view) override { ++depth; }
            void endObject() override { --depth; }
            void beginArray(std::string_view, u32) override { ++depth; }
            void endArray() override { --depth; }
            void writeBool(const std::string_view name, const bool v) override
            {
                if (depth != 0)
                {
                    return;
                }
                armed = name == "armed" ? v : armed;
                stopping = name == "stopping" ? v : stopping;
                lastReportValid = name == "lastReportValid" ? v : lastReportValid;
            }
            void writeU8(std::string_view, u8) override {}
            void writeU16(std::string_view, u16) override {}
            void writeU32(const std::string_view name, const u32 v) override
            {
                if (depth == 0 && name == "lastCapture")
                {
                    lastCapture = v;
                }
            }
            void writeU64(std::string_view, u64) override {}
            void writeI32(std::string_view, i32) override {}
            void writeF32(const std::string_view name, const f32 v) override { writeF64(name, v); }
            void writeF64(const std::string_view name, const double v) override
            {
                if (depth != 0)
                {
                    return;
                }
                if (name == "seconds")
                {
                    seconds = v;
                }
            }
            void writeFloats(std::string_view, std::span<const f32>) override {}
            void writeString(const std::string_view name, const std::string_view v) override
            {
                if (depth != 0)
                {
                    return;
                }
                if (name == "lastWarning" && lastWarning != v)
                {
                    lastWarning.assign(v);
                }
            }
            void writeEntity(std::string_view, Entity) override {}
            void writeNull(std::string_view) override {}
        };

        enum class Mode : u8
        {
            None,
            Live, // the frames since profileStart, appended as they pass
            Stored, // the selected capture's frames, loaded by the worker
        };

        // A panel request: a command line, and for profileOpen whether to select what it opened
        struct Pending
        {
            std::string line;
            bool selectOpened = false;
            bool view = false; // a profilerView of the timeline's gestures: the panel needs no refresh for it
            bool automatic = false; // the panel's own selection of a written capture, not a choice of the caller
        };

        // The frame graph's data, apart from the samples (those live in the chart's series)
        struct Graph
        {
            CaptureId capture = 0;
            bool loading = false;
            bool done = false;
            bool capped = false;
            bool gpuCalibrated = false;
            bool lanesKnown = false;
            u64 frames = 0; // the capture's
            u64 loaded = 0; // samples appended
            std::vector<u64> lanes; // zones per lane
            ProfilerFrameStats stats;
            std::string error;
        };

        State(Engine& engine, EditorFeedback& feedback, EditorDialogs& dialogs, const DockSpace& dockSpace) :
            engine(&engine), control(&engine.control()), feedback(&feedback), dialogs(&dialogs), dock(dockSpace),
            store(engine.services().contains<CaptureStore>() ? &engine.services().get<CaptureStore>() : nullptr),
            scope(editorUi(engine).ui())
        {
            if (store)
            {
                data = std::make_unique<ProfilerData>(*store);
            }
        }

        Engine* engine;
        ControlServer* control;
        EditorFeedback* feedback;
        EditorDialogs* dialogs;
        DockSpace dock; // the panel's own tab group is maximized and restored here (profilerMaximize)
        const InternedString panelName{"profiler"};
        CaptureStore* store;
        EditorCaptureFiles* captureFiles = nullptr;
        UiScope scope;
        DensityFollower density{scope};
        std::unique_ptr<ProfilerData> data;
        std::unique_ptr<ProfilerPanes> panes; // the views under the graph
        Panel root;
        ScrollView scroll; // the document's, bound by its contract (profiler.json)
        Panel bar;
        Panel left;
        Panel noticeRow;
        Panel graphStack; // the graph column's content: the summary, the chart, the panes and the timeline
        Button start;
        Button stop;
        Button refreshButton;
        Button closeButton;
        Button maximizeButton;
        bool maximized = false; // what the button shows
        StatusTips tips;
        StatusText notice; // the last capture was invalid
        Panel reportRow; // the document's report slot: shown while a capture with a report is selected
        StatusText reportPath; // the selected capture's zones report
        Button copyButton;
        Button revealButton;
        Button smallButton;
        Button revealFileButton;
        Button deleteButton;
        std::unique_ptr<ProfilerCaptureList> captureList;
        StatusText summary;
        Chart chart;
        std::unique_ptr<ProfilerTimeline> timeline;
        std::unique_ptr<UiDocumentView> view; // after the widgets its slots hold
        std::vector<Pending> pending;
        Status status;
        std::vector<StoreRow> storeRows;
        std::vector<CaptureFile> files;
        std::vector<Entry> entries;
        std::string actionError;
        std::string filesError;
        std::string lastSummary;
        Graph graph;
        Mode mode = Mode::None;
        std::vector<float> cpuScratch, gpuScratch, budgetFast, budgetSlow;
        std::string locale = "en";
        u64 storeGeneration = ~0ull;
        u64 storeReads = 0;
        u64 localeRevision = ~0ull;

        // Reveal starts a window of another program: one a second at most, and the same file is not shown again for
        u64 seenCommand = ~0ull;
        u32 frames = 0;
        bool wasShown = false;
        bool startEnabled = true;
        bool stopEnabled = true;
        bool reportDirty = true;
        bool reportShown = false;
        bool noticeShown = false;
        bool wasArmed = false;
        bool wasWriting = false;
        bool filesRequested = false;
        bool filesListed = false;
        bool filesTruncated = false;
        std::string truncatedNotice; // said once per list rebuild, not per refresh
        bool entriesDirty = false;
        bool showSmall = false; // the stub sidecars (SMALL_BYTES) of the artifacts folder are listed too
        u32 smallFiles = 0; // the unopened sidecars under SMALL_BYTES, listed or not
        bool selectionDirty = false;
        bool summaryDirty = true;
        bool mirrorPending = false;
        bool mirroring = false;
        std::optional<std::pair<u64, u64>> band; // the timeline's window in frames, marked on the graph
        bool bandPending = false; // the chart has not taken `band` yet
        bool bandApproximate = false; // its last frame is an estimate
        bool seenLastCapture = false;
        u32 lastCaptureSeen = 0;
        CaptureId autoSelect = 0;
        bool dispatchingAutomatic = false; // the profilerSelect being handled is the panel's own auto-selection
        CaptureId autoFloor = 0; // captures up to this id predate the last explicit select: never auto-selected
        u64 token = 0;
        Clock::time_point lastTick = Clock::now();

        // counters for tests and the profilerState query
        u64 refreshes = 0;
        u64 listRebuilds = 0;
        u64 scans = 0;
        u64 pagesApplied = 0;
        u64 selects = 0;
        u64 liveFrames = 0;
        double liveCpuMs = 0.0;
        double liveGpuMs = 0.0;

        // -- state ------------------------------------------------------------------------------------------------

        [[nodiscard]] const ProfilerSelectionComponent& selection() const
        {
            return scope.world().getConstComponent<ProfilerSelectionComponent>(root.entity());
        }

        [[nodiscard]] ProfilerSelectionComponent& writableSelection() const
        {
            auto& sel =
                scope.world().getComponent<ProfilerSelectionComponent>(root.entity()); // query-write-ok: applySelection
            return sel;
        }

        void send(const Json& line, const bool selectOpened = false, const bool automatic = false)
        {
            pending.push_back(
                {line.dump(-1, ' ', false, Json::error_handler_t::replace), selectOpened, false, automatic});
        }

        // a message of the profiler's table (ProfilerViews.hpp)
        [[nodiscard]] std::string say(const std::string_view key,
                                      const std::initializer_list<MessageArg> args = {}) const
        {
            return profilerText(scope, key, args);
        }

        [[nodiscard]] std::string number(const double value) const
        {
            return uiChartFormat(locale, std::round(value * 100.0) / 100.0, 0.0);
        }

        [[nodiscard]] std::string whole(const u64 value) const
        {
            return uiChartFormat(locale, static_cast<double>(value), 1.0);
        }

        [[nodiscard]] const StoreRow* storeRow(const CaptureId id) const
        {
            const auto it = std::ranges::find(storeRows, id, &StoreRow::id);
            return it == storeRows.end() ? nullptr : &*it;
        }

        [[nodiscard]] bool anyWriting() const
        {
            return std::ranges::any_of(storeRows, [](const StoreRow& row) { return row.state == "writing"; });
        }

        // -- requests ---------------------------------------------------------------------------------------------

        void dispatchPending()
        {
            std::vector<Pending> requests;
            requests.swap(pending);
            for (const Pending& request : requests)
            {
                const u64 before = control->commandGeneration();
                dispatchingAutomatic = request.automatic;
                const Json reply = Json::parse(control->handle(request.line), nullptr, false);
                dispatchingAutomatic = false;
                if (request.view && seenCommand == before)
                {
                    seenCommand = control->commandGeneration(); // the timeline applied its own view: nothing to read
                }
                if (request.view && !(reply.is_object() && reply.value("ok", false)))
                {
                    continue; // a gesture's view for a capture that went away: nothing to report
                }
                if (!reply.is_object() || !reply.value("ok", false))
                {
                    actionError = friendly(scope, request.line,
                                           reply.is_object() ? reply.value("error", say("error.failed"))
                                                             : say("error.unparsable"));
                    summaryDirty = true;
                    SWARNING_LOG("profiler: {} -> {}", request.line, reply.dump());
                    continue;
                }
                if (!actionError.empty())
                {
                    actionError.clear();
                    summaryDirty = true;
                }
                if (request.selectOpened && reply.contains("result") && reply["result"].contains("capture"))
                {
                    const Json select = {{"command", "profilerSelect"},
                                         {"args", {{"capture", reply["result"]["capture"].value("id", 0u)}}}};
                    const Json selected = Json::parse(control->handle(select.dump()), nullptr, false);
                    if (!selected.is_object() || !selected.value("ok", false))
                    {
                        actionError = friendly(scope, select.dump(),
                                               selected.is_object() ? selected.value("error", say("error.failed"))
                                                                    : say("error.unparsable"));
                    }
                }
            }
        }

        // -- the capture list -------------------------------------------------------------------------------------

        void rebuildEntries()
        {
            ++listRebuilds;
            // the cursor stays on the same capture: by its file name, which a file keeps when the store opens it
            std::string cursorName;
            if (const i32 cursor = captureList->cursor(); cursor >= 0 && cursor < static_cast<i32>(entries.size()))
            {
                cursorName = entries[static_cast<u32>(cursor)].name;
            }
            entries.clear();
            smallFiles = 0;
            std::vector<const StoreRow*> byId;
            for (const StoreRow& row : storeRows)
            {
                byId.push_back(&row);
            }
            std::ranges::sort(byId, std::greater<>{}, &StoreRow::id);
            for (const StoreRow* row : byId)
            {
                Entry entry{.inStore = true,
                            .id = row->id,
                            .name = row->name,
                            .state = row->state,
                            .live = row->live,
                            .frames = row->frames,
                            .durationNs = row->durationNs,
                            .bytes = row->fileBytes,
                            .error = row->error};
                if (const auto file = std::ranges::find(files, row->id, &CaptureFile::openId); file != files.end())
                {
                    entry.modifiedMs = file->modifiedMs;
                    entry.path = file->path;
                }
                entries.push_back(std::move(entry));
            }
            for (const CaptureFile& file : files)
            {
                if (file.openId != 0 && storeRow(file.openId))
                {
                    continue; // listed above
                }
                const bool small = file.bytes < SMALL_BYTES;
                smallFiles += small;
                if (entries.size() < MAX_ENTRIES && (showSmall || !small))
                {
                    entries.push_back({.path = file.path,
                                       .name = file.name,
                                       .state = "file",
                                       .bytes = file.bytes,
                                       .modifiedMs = file.modifiedMs});
                }
            }
            captureList->setRowCount(static_cast<u32>(entries.size()));
            if (const auto kept = std::ranges::find(entries, cursorName, &Entry::name);
                !cursorName.empty() && kept != entries.end())
            {
                captureList->setCursor(static_cast<i32>(kept - entries.begin()), false);
            }
            captureList->rebindVisible();
            updateListBar();
            truncatedNotice = filesTruncated ? say("list.truncated", {{"count", whole(files.size())}}) : std::string();
            showNotice();
            entriesDirty = false;
            summaryDirty = true;
        }

        // The two lines a row shows: the time and what the capture is, then duration, frames and size
        [[nodiscard]] std::string megabytes(const u64 bytes) const
        {
            return number(static_cast<double>(bytes) / (1024.0 * 1024.0));
        }

        [[nodiscard]] ProfilerListRow rowText(const Entry& entry) const
        {
            const std::string size = megabytes(entry.bytes);
            const bool active = entry.inStore && entry.id == selection().capture;
            const char* tag = active       ? "tag.active"
                : !entry.inStore           ? "tag.file"
                : entry.state == "writing" ? "tag.writing"
                : entry.state == "failed"  ? "tag.failed"
                : entry.live               ? "tag.live"
                                           : "tag.opened";
            ProfilerListRow row;
            row.title = say(
                "row.title",
                {{"time", entry.modifiedMs > 0 ? profilerLocalTime(entry.modifiedMs) : entry.name}, {"tag", say(tag)}});
            if (!entry.inStore)
            {
                row.detail = say("row.file", {{"size", size}});
            }
            else if (entry.state == "ready")
            {
                row.detail = say("row.ready",
                                 {{"seconds", number(static_cast<double>(entry.durationNs) * 1e-9)},
                                  {"frames", whole(entry.frames)},
                                  {"size", size}});
            }
            else if (entry.state == "writing")
            {
                row.detail = say("row.writing", {{"id", std::to_string(entry.id)}});
            }
            else
            {
                row.detail = say("row.failed", {{"id", std::to_string(entry.id)}, {"error", clipped(entry.error, 60)}});
                row.warn = true;
            }
            return row;
        }

        void updateListBar()
        {
            const std::string text =
                say(showSmall ? "list.hideSmall" : "list.showSmall", {{"count", whole(smallFiles)}});
            if (smallButton.text() != text)
            {
                smallButton.setText(text);
            }
            smallButton.setInteractable(smallFiles > 0);
            const bool row = captureList->cursor() >= 0;
            revealFileButton.setInteractable(row);
            deleteButton.setInteractable(row);
        }

        // The store's list (what profileCaptures answers), re-read only when its generation moved (or forced): an
        // unrelated command costs one atomic load. Read from the store itself, which is safe from any thread: a control
        // query cannot run inside the profilerState handler that wants it current.
        void readStore(const bool force = false)
        {
            if (!store || (!force && store->generation() == storeGeneration))
            {
                return;
            }
            storeGeneration = store->generation(); // before the read: a change during it is read next time
            ++storeReads;
            const std::vector<CaptureInfo> infos = store->list();
            std::vector<StoreRow> now;
            now.reserve(infos.size());
            for (const CaptureInfo& info : infos)
            {
                static constexpr const char* STATES[] = {"writing", "ready", "failed"};
                now.push_back({info.id, info.name, STATES[static_cast<u32>(info.state)], info.error, info.live,
                               info.frames, static_cast<u64>(std::max<std::int64_t>(0, info.durationNs)),
                               info.fileBytes});
            }
            if (now != storeRows)
            {
                storeRows = std::move(now);
                entriesDirty = true;
                reportDirty = true;
                if (data && filesRequested)
                {
                    data->requestFiles(); // which files the store holds changed (a written sidecar is on disk now)
                    ++scans;
                }
            }
            wasWriting = anyWriting();
            // a closed or evicted capture takes its selection with it
            if (const ProfilerSelectionComponent& sel = selection();
                sel.capture != 0 && (!storeRow(sel.capture) || storeRow(sel.capture)->state != "ready"))
            {
                applySelection({});
            }
        }

        // A stop that was written selects its capture (the command a person would type), once the store has it ready.
        void selectWrittenCapture()
        {
            if (autoSelect == 0)
            {
                return;
            }
            if (status.armed)
            {
                autoSelect = 0; // a new capture started before the write finished: the live view stays
                return;
            }
            if (const StoreRow* row = storeRow(autoSelect); !row || row->state == "failed")
            {
                autoSelect = 0;
            }
            else if (row->state == "ready")
            {
                send({{"command", "profilerSelect"}, {"args", {{"capture", autoSelect}}}}, false, true);
                autoSelect = 0;
            }
        }

        // -- the selection ----------------------------------------------------------------------------------------

        // Writes the selection (the one place): reloads the graph when the capture changed.
        void applySelection(ProfilerSelectionComponent next)
        {
            const ProfilerSelectionComponent& cur = selection();
            const bool same = cur.capture == next.capture && cur.hasFrames == next.hasFrames &&
                cur.firstFrame == next.firstFrame && cur.lastFrame == next.lastFrame && cur.hasZone == next.hasZone &&
                cur.zoneLane == next.zoneLane && cur.zoneIndex == next.zoneIndex && cur.hasGpuPass == next.hasGpuPass &&
                cur.gpuFrame == next.gpuFrame && cur.gpuPass == next.gpuPass;
            if (same)
            {
                return;
            }
            ProfilerSelectionComponent& sel = writableSelection();
            const CaptureId previous = sel.capture;
            next.generation = sel.generation + 1;
            sel = next;
            ++selects;
            selectionDirty = true;
            reportDirty = true;
            if (previous != next.capture)
            {
                resetGraph(next.capture);
                if (panes)
                {
                    panes->captureChanged(next.capture);
                }
            }
            else
            {
                mirrorPending = true;
            }
            if (timeline)
            {
                timeline->onSelection();
            }
            if (panes)
            {
                panes->selectionChanged(sel);
            }
        }

        void resetGraph(const CaptureId capture)
        {
            if (timeline)
            {
                timeline->reset(capture);
            }
            ensureSeries();
            graph = Graph{};
            graph.capture = capture;
            chart.clear();
            chart.setWindow(0, 0);
            chart.setVisible(capture != 0); // no capture: the hint says what to do, not an empty plot
            mirrorPending = false;
            band.reset(); // chart.clear() took it off the graph
            bandPending = false;
            summaryDirty = true;
            if (capture != 0)
            {
                mode = Mode::Stored;
                graph.loading = true;
                if (data)
                {
                    data->requestLoad(capture, ++token);
                }
            }
            else
            {
                if (data)
                {
                    data->requestLoad(0, ++token);
                }
                if (status.armed)
                {
                    startLive();
                }
                else
                {
                    mode = Mode::None;
                }
            }
        }

        // profilerSelect: validates everything against the store before it changes anything.
        std::string select(const ControlArgs& args, IFormatWriter& out)
        {
            ProfilerSelectionComponent next = selection();
            if (const std::string clear = args.string("clear").value_or(""); clear == "all")
            {
                next = ProfilerSelectionComponent{};
            }
            else if (clear == "frames")
            {
                next.hasFrames = false;
            }
            else if (clear == "zone")
            {
                next.hasZone = false;
            }
            else if (clear == "gpu")
            {
                next.hasGpuPass = false;
            }
            if (const std::optional<u64> named = args.integer("capture"))
            {
                const auto id = static_cast<CaptureId>(std::min<u64>(*named, UINT32_MAX));
                if (id == 0)
                {
                    next = ProfilerSelectionComponent{};
                }
                else
                {
                    CaptureInfo info;
                    if (!store || !store->info(id, info))
                    {
                        return "no capture " + std::to_string(*named) + " in the store (profileCaptures lists them)";
                    }
                    if (info.state != CaptureInfo::State::Ready)
                    {
                        return "capture " + std::to_string(id) + " is not ready (" +
                            (info.state == CaptureInfo::State::Writing ? "still being written"
                                                                       : "failed: " + info.error) +
                            ")";
                    }
                    if (id != next.capture)
                    {
                        next.capture = id;
                        next.hasFrames = next.hasZone = next.hasGpuPass = false;
                    }
                }
            }
            const std::optional<u64> first = args.integer("firstFrame");
            const std::optional<u64> last = args.integer("lastFrame");
            const std::optional<u64> lane = args.integer("zoneLane");
            const std::optional<u64> index = args.integer("zoneIndex");
            const std::optional<u64> gpuFrame = args.integer("gpuFrame");
            const std::optional<u64> gpuPass = args.integer("gpuPass");
            if (first || last || lane || index || gpuFrame || gpuPass)
            {
                CaptureInfo info;
                if (next.capture == 0 || !store || !store->info(next.capture, info))
                {
                    return "select a capture first (profilerSelect capture=<id>)";
                }
                if (first || last)
                {
                    const u64 from = first ? *first : *last;
                    const u64 to = last ? *last : from;
                    if (from > to)
                    {
                        return "firstFrame " + std::to_string(from) + " is after lastFrame " + std::to_string(to);
                    }
                    if (to >= info.frames)
                    {
                        return "frames " + std::to_string(from) + ".." + std::to_string(to) + " are outside capture " +
                            std::to_string(next.capture) + " (" + std::to_string(info.frames) + " frames)";
                    }
                    next.hasFrames = true;
                    next.firstFrame = from;
                    next.lastFrame = to;
                }
                if (lane || index)
                {
                    if (!lane || !index)
                    {
                        return "zoneLane and zoneIndex go together";
                    }
                    const bool exact = graph.capture == next.capture && graph.lanesKnown;
                    const u64 laneCount = exact ? graph.lanes.size() : info.threads;
                    const u64 zones = exact ? (*lane < laneCount ? graph.lanes[*lane] : 0) : info.zones;
                    if (*lane >= laneCount)
                    {
                        return "lane " + std::to_string(*lane) + " does not exist: capture " +
                            std::to_string(next.capture) + " has " + std::to_string(laneCount) + " lanes";
                    }
                    if (*index >= zones)
                    {
                        return "zone " + std::to_string(*index) + " is outside lane " + std::to_string(*lane) + " (" +
                            std::to_string(zones) + " zones)";
                    }
                    next.hasZone = true;
                    next.zoneLane = static_cast<u32>(*lane);
                    next.zoneIndex = *index;
                }
                if (gpuFrame || gpuPass)
                {
                    if (!gpuFrame || !gpuPass)
                    {
                        return "gpuFrame and gpuPass go together";
                    }
                    CaptureGpuFrame frame;
                    if (std::string error =
                            profilerFrameStoreCall([&] { return store->gpuFrame(next.capture, *gpuFrame, frame); });
                        !error.empty())
                    {
                        return error;
                    }
                    if (*gpuPass >= frame.passes.size())
                    {
                        return "pass " + std::to_string(*gpuPass) + " is outside GPU frame " +
                            std::to_string(*gpuFrame) + " (" + std::to_string(frame.passes.size()) + " passes)";
                    }
                    next.hasGpuPass = true;
                    next.gpuFrame = *gpuFrame;
                    next.gpuPass = static_cast<u32>(*gpuPass);
                }
            }
            // a choice made now outranks the auto-selection of every capture stopped so far, seen by a refresh or not
            if (store && !dispatchingAutomatic)
            {
                autoSelect = 0;
                std::erase_if(pending, [](const Pending& request) { return request.automatic; }); // queued, not sent
                for (const CaptureInfo& info : store->list())
                {
                    autoFloor = std::max(autoFloor, info.id);
                }
            }
            applySelection(next);
            writeSelection(out);
            return {};
        }

        void writeSelection(IFormatWriter& out) const
        {
            const ProfilerSelectionComponent& sel = selection();
            out.writeU32("capture", sel.capture);
            out.writeBool("hasFrames", sel.hasFrames);
            out.writeU64("firstFrame", sel.firstFrame);
            out.writeU64("lastFrame", sel.lastFrame);
            out.writeBool("hasZone", sel.hasZone);
            out.writeU32("zoneLane", sel.zoneLane);
            out.writeU64("zoneIndex", sel.zoneIndex);
            out.writeBool("hasGpuPass", sel.hasGpuPass);
            out.writeU64("gpuFrame", sel.gpuFrame);
            out.writeU32("gpuPass", sel.gpuPass);
            out.writeU64("generation", sel.generation);
        }

        // -- the graph --------------------------------------------------------------------------------------------

        // The four series (1 MiB each) are made when a graph is first needed: an unused panel holds none.
        void ensureSeries()
        {
            if (chart.seriesCount() != 0)
            {
                return;
            }
            struct SeriesSpec
            {
                u32 color;
                float thickness;
            };
            for (const SeriesSpec& spec : {SeriesSpec{canvasColor(scope, "canvas.series.a"), 1.5f},
                                           SeriesSpec{canvasColor(scope, "canvas.series.b"), 1.5f},
                                           SeriesSpec{withAlpha(canvasColor(scope, "canvas.series.c"), 200), 1.0f},
                                           SeriesSpec{withAlpha(canvasColor(scope, "canvas.series.d"), 200), 1.0f}})
            {
                UiChartSeriesDesc series;
                series.color = spec.color;
                series.thickness = spec.thickness;
                series.capacity = PROFILER_MAX_GRAPH_FRAMES;
                chart.addSeries(series);
            }
            nameSeries();
            chart.setWindow(0, 0);
        }

        // the legend and the tooltip name the series in the locale's words
        void nameSeries()
        {
            if (chart.seriesCount() == 0)
            {
                return;
            }
            chart.setSeriesName(SERIES_CPU, say("chart.cpu"));
            chart.setSeriesName(SERIES_GPU, say("chart.gpu"));
            chart.setSeriesName(SERIES_BUDGET, say("chart.budget", {{"ms", 16.7}}));
            chart.setSeriesName(SERIES_BUDGET + 1, say("chart.budget", {{"ms", 33.3}}));
        }

        void appendPage(const ProfilerEvent& event)
        {
            const sizet n = event.rows.size();
            if (event.first != graph.loaded || n == 0 || n > budgetFast.size())
            {
                graph.error = say("error.pages");
                graph.loading = false;
                return;
            }
            cpuScratch.resize(n);
            gpuScratch.resize(n);
            for (sizet i = 0; i < n; ++i)
            {
                cpuScratch[i] = static_cast<float>(event.rows[i].cpuNs) * 1e-6f;
                gpuScratch[i] = static_cast<float>(event.rows[i].gpuNs) * 1e-6f;
            }
            const bool ok = chart.append(SERIES_CPU, cpuScratch) && chart.append(SERIES_GPU, gpuScratch) &&
                chart.append(SERIES_BUDGET, std::span<const float>(budgetFast.data(), n)) &&
                chart.append(SERIES_BUDGET + 1, std::span<const float>(budgetSlow.data(), n));
            if (!ok)
            {
                graph.error = say("error.chart");
                graph.loading = false;
                return;
            }
            graph.loaded += n;
            ++pagesApplied;
            summaryDirty = true;
        }

        void pumpData(const u32 maxPages)
        {
            ProfilerEvent event;
            u32 pages = 0;
            while (pages < maxPages && data->take(event))
            {
                using Kind = ProfilerEvent::Kind;
                if (event.kind == Kind::Files)
                {
                    files = std::move(event.files);
                    filesTruncated = event.truncated;
                    filesError = clipped(event.error);
                    filesListed = true;
                    entriesDirty = true;
                    continue;
                }
                if (event.kind == Kind::Task)
                {
                    if (event.view == ProfilerTimeline::TASK_VIEW)
                    {
                        if (timeline)
                        {
                            timeline->onEvent(event);
                        }
                    }
                    else if (panes)
                    {
                        panes->take(event);
                    }
                    continue;
                }
                if (event.kind >= Kind::Marks)
                {
                    if (timeline)
                    {
                        timeline->onEvent(event);
                    }
                    continue;
                }
                if (event.token != token)
                {
                    continue; // a superseded load
                }
                switch (event.kind)
                {
                case Kind::Lanes:
                    if (timeline)
                    {
                        timeline->onLanes(event.capture, std::move(event.lanes), event.durationNs);
                    }
                    graph.lanes = std::move(event.laneZones);
                    graph.lanesKnown = true;
                    graph.frames = event.frames;
                    graph.gpuCalibrated = event.gpuCalibrated;
                    summaryDirty = true;
                    break;
                case Kind::Page:
                    if (graph.error.empty())
                    {
                        appendPage(event);
                    }
                    ++pages;
                    break;
                case Kind::Done:
                    graph.loading = false;
                    graph.done = graph.error.empty();
                    graph.capped = event.capped;
                    graph.stats = event.stats;
                    mirrorPending = true;
                    summaryDirty = true;
                    break;
                case Kind::Error:
                    graph.loading = false;
                    graph.error = clipped(event.error);
                    summaryDirty = true;
                    break;
                default:
                    break;
                }
            }
        }

        void mirrorSelection()
        {
            const ProfilerSelectionComponent& sel = selection();
            mirroring = true;
            bool done = true;
            if (sel.hasFrames)
            {
                done = chart.select(static_cast<double>(sel.firstFrame), static_cast<double>(sel.lastFrame));
            }
            else
            {
                chart.clearSelection();
            }
            mirroring = false;
            mirrorPending = !done;
        }

        // a drag or a click on the graph: the same command a person at the console would type
        void selectFrames(const u64 first, const u64 last)
        {
            if (mode != Mode::Stored || mirroring)
            {
                return;
            }
            // names the capture the gesture was made on: a selection that changed before the line is dispatched
            // must not take this range
            send({{"command", "profilerSelect"},
                  {"args", {{"capture", graph.capture}, {"firstFrame", first}, {"lastFrame", last}}}});
            // and the timeline shows it: a range the drag picks is the range to look at (the graph then marks the
            // timeline's window, and later zooms and pans in the timeline move that mark, not the selection)
            pending.push_back({Json{{"command", "profilerView"},
                                    {"args", {{"capture", graph.capture}, {"firstFrame", first}, {"lastFrame", last}}}}
                                   .dump(),
                               false, true});
        }

        // The timeline's window in frames, once known: the band over the graph. A chart that has no plot yet refuses
        // it; update() tries again until it is taken.
        void showBand(const std::optional<std::pair<u64, u64>> frames, const bool approximate)
        {
            band = frames;
            bandApproximate = approximate;
            bandPending = true;
        }

        void applyBand()
        {
            bandPending = false;
            if (!band)
            {
                chart.clearBand();
            }
            else if (!chart.setBand(static_cast<double>(band->first), static_cast<double>(band->second)))
            {
                bandPending = true;
            }
        }

        // The frame graph's keys: Left and Right move the selected frame one sample (Shift: the range's
        // end), Home and End jump to the first and last; the answer is whether the key was the graph's.
        bool graphKey(const u32 scancode, const u8 modifiers, const bool repeat)
        {
            constexpr u32 HOME = 74;
            constexpr u32 END = 77;
            constexpr u32 RIGHT = 79;
            constexpr u32 LEFT = 80;
            const bool shift = modifiers == UiKeyModifier::SHIFT;
            if ((modifiers != 0 && !shift) ||
                (scancode != HOME && scancode != END && scancode != RIGHT && scancode != LEFT))
            {
                return false;
            }
            if (mode != Mode::Stored || mirroring || graph.frames == 0 || graph.loaded == 0)
            {
                return true; // a live or loading graph has no frame to move
            }
            const ProfilerSelectionComponent& chosen = selection();
            const u64 last = std::min(graph.frames, graph.loaded) - 1;
            u64 first = chosen.hasFrames ? std::min(chosen.firstFrame, last) : 0;
            u64 to = chosen.hasFrames ? std::min(chosen.lastFrame, last) : 0;
            if (scancode == HOME || scancode == END)
            {
                first = to = scancode == HOME ? 0 : last;
            }
            else if (!chosen.hasFrames)
            {
                first = to = scancode == LEFT ? last : 0; // the first press picks an end
            }
            else if (shift)
            {
                to = scancode == RIGHT ? std::min(to + 1, last) : std::max(to > 0 ? to - 1 : 0, first);
            }
            else
            {
                first = to = scancode == RIGHT ? std::min(to + 1, last) : (first > 0 ? first - 1 : 0);
            }
            if (!chosen.hasFrames || first != chosen.firstFrame || to != chosen.lastFrame)
            {
                selectFrames(first, to);
            }
            return true;
        }

        // East and Escape in a region of the panel go back to the toolbar; on the toolbar they are the default (the
        // focus drops). False when nothing moved.
        bool focusToolbar()
        {
            UiRouter& router = scope.router();
            const UiHierarchyManager& hierarchy = scope.hierarchy();
            if (!router.navigationEnabled())
            {
                return false;
            }
            u32 depth = 0;
            for (Entity at = router.focused(); at != Entity::undefined() && depth < 64;
                 at = hierarchy.parentOf(at), ++depth)
            {
                if (at == bar.entity())
                {
                    return false;
                }
            }
            router.focusInto(bar.entity());
            return true;
        }

        void appendLive(const double cpuMs, const double gpuMs)
        {
            const float cpu = static_cast<float>(cpuMs);
            const float gpu = static_cast<float>(gpuMs);
            const float fast = static_cast<float>(PROFILER_BUDGET_MS);
            const float slow = static_cast<float>(PROFILER_BUDGET_SLOW_MS);
            chart.append(SERIES_CPU, std::span<const float>(&cpu, 1));
            chart.append(SERIES_GPU, std::span<const float>(&gpu, 1));
            chart.append(SERIES_BUDGET, std::span<const float>(&fast, 1));
            chart.append(SERIES_BUDGET + 1, std::span<const float>(&slow, 1));
            ++liveFrames;
            liveCpuMs = cpuMs;
            liveGpuMs = gpuMs;
        }

        // A rolling window that starts full: an append then moves no label and no axis (the chart's cheap case), where
        // a growing window would lower the axes again every frame.
        void startLive()
        {
            ensureSeries();
            if (data)
            {
                data->requestLoad(0, ++token);
            }
            graph = Graph{};
            mirrorPending = false;
            band.reset();
            bandPending = false;
            chart.clear();
            chart.setVisible(true);
            mode = Mode::Live;
            chart.follow(LIVE_WINDOW);
            const std::vector<float> empty(LIVE_WINDOW, 0.0f);
            const std::vector<float> fast(LIVE_WINDOW, static_cast<float>(PROFILER_BUDGET_MS));
            const std::vector<float> slow(LIVE_WINDOW, static_cast<float>(PROFILER_BUDGET_SLOW_MS));
            chart.append(SERIES_CPU, empty);
            chart.append(SERIES_GPU, empty);
            chart.append(SERIES_BUDGET, fast);
            chart.append(SERIES_BUDGET + 1, slow);
            liveFrames = 0;
            lastTick = Clock::now();
            summaryDirty = true;
        }

        void onArmed()
        {
            applySelection(ProfilerSelectionComponent{}); // what profilerSelect clear=all does
            if (mode != Mode::Live)
            {
                startLive();
            }
        }

        void showSummary()
        {
            std::string text;
            const ProfilerSelectionComponent& sel = selection();
            const std::string id = std::to_string(graph.capture); // an id is not grouped
            if (!actionError.empty())
            {
                text = say("status.error", {{"error", actionError}});
            }
            else if (!graph.error.empty())
            {
                text = say("status.captureError", {{"id", id}, {"error", graph.error}});
            }
            else if (mode == Mode::Live)
            {
                const std::string frames = whole(liveFrames);
                text = status.armed ? say("status.capturing",
                                          {{"seconds", whole(static_cast<u64>(status.seconds))},
                                           {"frames", frames},
                                           {"cpu", number(liveCpuMs)},
                                           {"gpu", number(liveGpuMs)}})
                                    : say(status.stopping ? "status.writing" : "status.stopped", {{"frames", frames}});
            }
            else if (mode == Mode::Stored)
            {
                std::string detail;
                if (graph.loading)
                {
                    detail = say("status.loading", {{"loaded", whole(graph.loaded)}, {"frames", whole(graph.frames)}});
                }
                else
                {
                    detail = say("status.stats",
                                 {{"count", whole(graph.stats.count)},
                                  {"mean", number(graph.stats.meanMs)},
                                  {"p99", number(graph.stats.p99Ms)},
                                  {"max", number(graph.stats.maxMs)},
                                  {"fast", 16.7},
                                  {"over", whole(graph.stats.overBudget)},
                                  {"slow", 33.3},
                                  {"overSlow", whole(graph.stats.overSlow)}});
                    if (graph.capped)
                    {
                        detail += "  " + say("status.capped", {{"frames", whole(PROFILER_MAX_GRAPH_FRAMES)}});
                    }
                    if (!graph.gpuCalibrated)
                    {
                        detail += "  " + say("status.noGpuClock");
                    }
                }
                if (sel.hasFrames)
                {
                    detail += "  " +
                        (sel.firstFrame == sel.lastFrame
                             ? say("status.frame", {{"frame", whole(sel.firstFrame)}})
                             : say("status.frames",
                                   {{"first", whole(sel.firstFrame)}, {"last", whole(sel.lastFrame)}}));
                }
                text = say("status.capture", {{"id", id}, {"detail", detail}});
            }
            else
            {
                text = say("status.none");
            }
            summary.set(text);
            summaryDirty = false;
        }

        // number formats follow the Localization's locale
        void checkLocale()
        {
            const Localization* localization = scope.hierarchy().localization();
            if (!localization && scope.world().services().contains<Localization>())
            {
                localization = &scope.world().services().get<Localization>();
            }
            if (localization && localization->revision() != localeRevision)
            {
                localeRevision = localization->revision();
                locale = localization->locale();
                entriesDirty = true;
                summaryDirty = true;
                if (timeline)
                {
                    timeline->setLocale(locale);
                }
                nameSeries();
            }
        }

        // -- refresh ----------------------------------------------------------------------------------------------

        // profileStatus and what a change in it means: a capture started, a stop written
        bool readStatus()
        {
            if (!control->query("profileStatus", status).empty())
            {
                return false;
            }
            if (!seenLastCapture)
            {
                seenLastCapture = true;
                lastCaptureSeen = status.lastCapture;
            }
            else if (status.lastCapture != lastCaptureSeen)
            {
                lastCaptureSeen = status.lastCapture;
                if (status.lastCapture > autoFloor)
                {
                    autoSelect = status.lastCapture;
                }
            }
            summaryDirty |= mode == Mode::Live;
            if (status.armed != wasArmed)
            {
                wasArmed = status.armed;
                if (status.armed)
                {
                    onArmed();
                }
                else
                {
                    summaryDirty = true;
                }
            }
            return true;
        }

        // The line over the list: the last capture's warning, else that the scan listed only the newest files
        void showNotice()
        {
            notice.set(!status.lastReportValid ? std::string_view(status.lastWarning)
                           : filesTruncated    ? std::string_view(truncatedNotice)
                                               : std::string_view());
            if (noticeShown != !notice.full().empty())
            {
                noticeShown = !notice.full().empty();
                noticeRow.setVisible(noticeShown);
            }
        }

        void refresh()
        {
            SPITE_ZONE("EditorProfiler.refresh");
            ++refreshes;
            checkLocale();
            frames = 0;
            if (!readStatus())
            {
                return;
            }
            updateButtons();
            showNotice();
            readStore(autoSelect != 0);
            selectWrittenCapture();
            scanOnce();
        }

        // The first scan of the artifacts folder (the next ones follow a written capture or profilerRefresh).
        void scanOnce()
        {
            if (!filesRequested && data)
            {
                filesRequested = true;
                data->requestFiles();
                ++scans;
            }
        }

        // -- the status block -------------------------------------------------------------------------------------

        // Start and Stop are for the state a capture is in: Start while nothing runs or is being written, Stop while
        // one runs
        void updateButtons()
        {
            const bool canStart = !status.armed && !status.stopping;
            if (canStart != startEnabled)
            {
                startEnabled = canStart;
                start.setInteractable(canStart);
            }
            if (status.armed != stopEnabled)
            {
                stopEnabled = status.armed;
                stop.setInteractable(status.armed);
            }
        }

        // The button says what a press does: Maximize, or Restore while the panel's group is the maximized one
        void updateMaximizeButton()
        {
            if (const bool now = dock.isMaximized(panelName); now != maximized)
            {
                maximized = now;
                maximizeButton.setIcon(now ? "corners-in" : "corners-out");
                maximizeButton.setAccessibleNameKey(profilerKey(now ? "restore" : "maximize"));
                maximizeButton.setTooltip(profilerKey(now ? "restore" : "maximize"));
            }
        }

        std::string setMaximized(const ControlArgs& args, IFormatWriter& out)
        {
            const bool on = args.boolean("on").value_or(!dock.isMaximized(panelName));
            if (!dock.isOpen(panelName))
            {
                return "the Profiler panel is closed (dock kind=show panel=profiler)";
            }
            if (dock.floatingRect(panelName).size.x > 0.0f)
            {
                return "the Profiler panel is in a floating window: only a docked panel's group is maximized (dock "
                       "kind=dock panel=profiler target=scene)";
            }
            if (!on)
            {
                if (!dock.apply(UiDockOp{.kind = UiDockOpKind::Restore, .panel = panelName}))
                {
                    return "another group is maximized: restore it first (dock kind=restore panel=<a panel of that "
                           "group>)";
                }
            }
            else if (!dock.apply(UiDockOp{.kind = UiDockOpKind::Activate, .panel = panelName}) ||
                     !dock.apply(UiDockOp{.kind = UiDockOpKind::Maximize, .panel = panelName}))
            {
                return "the Profiler panel cannot be maximized (dock kind=show panel=profiler)";
            }
            out.writeBool("maximized", dock.isMaximized(panelName));
            return {};
        }

        // The selected capture's zones report (a capture opened from disk has none: its sidecar); empty without one
        [[nodiscard]] std::filesystem::path selectedPath() const
        {
            CaptureInfo info;
            const CaptureId id = selection().capture;
            if (id == 0 || !store || !store->info(id, info))
            {
                return {};
            }
            return info.report.empty() ? info.path : info.path.parent_path() / info.report;
        }

        void showReport()
        {
            reportDirty = false;
            const std::string path = utf8Path(selectedPath());
            reportPath.set(path);
            if (reportShown != !path.empty())
            {
                reportShown = !path.empty();
                reportRow.setVisible(reportShown);
            }
        }

        std::string copyPath(IFormatWriter& out)
        {
            const std::string path = utf8Path(selectedPath());
            if (path.empty())
            {
                return "select a capture first (profilerSelect capture=<id>)";
            }
            UiContext& ui = editorUi(*engine).ui();
            ui.setClipboardText(path);
            if (ui.clipboardText() != path)
            {
                return "the clipboard did not take the text";
            }
            out.writeString("path", path);
            return {};
        }

        // The control line for a list row's file: its store id when the store holds it, else its path
        [[nodiscard]] static Json rowLine(const char* command, const Entry& entry)
        {
            Json args = entry.inStore ? Json{{"capture", entry.id}} : Json{{"path", entry.path}};
            return {{"command", command}, {"args", std::move(args)}};
        }

        // Delete asks first: a sidecar cannot be taken back. The buttons under the list act on the cursor row.
        void listAction(const bool remove)
        {
            const i32 cursor = captureList->cursor();
            if (cursor < 0 || cursor >= static_cast<i32>(entries.size()))
            {
                return;
            }
            const Entry& entry = entries[static_cast<u32>(cursor)];
            if (!remove)
            {
                send(rowLine("profilerReveal", entry));
                return;
            }
            EditorConfirm ask;
            ask.title = say("delete.title");
            ask.body = say("delete.body", {{"name", entry.name}, {"size", megabytes(entry.bytes)}});
            ask.confirmLabel = say("delete.confirm");
            ask.destructive = true;
            dialogs->confirm(std::move(ask),
                             [this, line = rowLine("profilerDelete", entry)](const bool confirmed)
                             {
                                 if (confirmed)
                                 {
                                     send(line);
                                 }
                             });
        }

        std::string setSmall(const ControlArgs& args, IFormatWriter& out)
        {
            showSmall = args.boolean("showSmall").value_or(!showSmall);
            entriesDirty = true;
            seenCommand = ~0ull;
            out.writeBool("showSmall", showSmall);
            return {};
        }

        void writeStatus(IFormatWriter& out) const
        {
            out.beginObject("report");
            out.writeString("path", utf8Path(selectedPath()));
            out.writeBool("visible", reportShown);
            out.endObject();
            out.beginObject("buttons");
            out.writeBool("startEnabled", startEnabled);
            out.writeBool("stopEnabled", stopEnabled);
            out.endObject();
            out.writeBool("tipShown", tips.tip.isShown());
            out.writeString("tip", tips.tip.label().text());
            out.beginArray("texts", static_cast<u32>(tips.items().size()));
            for (const StatusText* item : tips.items())
            {
                out.beginObject({});
                out.writeString("id",
                                scope.world().getConstComponent<UiIdentityComponent>(item->text.entity()).id.toView());
                out.writeString("full", item->full());
                out.writeString("shown", item->shown());
                out.writeBool("truncated", item->truncated());
                out.writeF64("width", item->width());
                out.endObject();
            }
            out.endArray();
        }

        // -- the page ---------------------------------------------------------------------------------------------

        // The page's own layout (the document owns what is in it): a column, the density's gap and padding, and the
        // scroll bar's room on the right. Runs when the document binds the scroll view and on a density change.
        void placePage(const Density& d)
        {
            if (scroll.isValid())
            {
                Panel page = scroll.content();
                page.setLayout(stack(UiLayoutDirection::Column, d.xs, Vec4{d.xs, d.xs, d.xs + THUMB, d.xs},
                                     UiLayoutAlign::Stretch));
            }
        }

        // The scrolling page is as tall as the viewport, or its minimum when that is taller, plus the open view's
        // height: only a change of the viewport or of the pane writes anything.
        void fitPage()
        {
            const Vec2 view = scroll.size();
            if (view.x <= 0.0f || view.y <= 0.0f)
            {
                return;
            }
            if (panes)
            {
                panes->setViewportHeight(std::max(0.0f, view.y - 2.0f * density.density().xs));
            }
            const float extra = panes ? panes->extraHeight() : 0.0f;
            const Vec2 want{view.x, std::max(view.y, pageMin()) + extra};
            if (scroll.contentSize() != want)
            {
                scroll.setContentSize(want);
            }
        }

        [[nodiscard]] UiRect regionRect(const Widget& widget) const
        {
            UiRect rect = scope.hierarchy().absoluteRectOf(widget.entity());
            const float scale = std::max(scope.hierarchy().layoutScale(), 1e-3f);
            rect.position = rect.position / scale;
            rect.size = rect.size / scale;
            return rect;
        }

        [[nodiscard]] Widget region(const std::string_view name) const
        {
            if (name == "list")
            {
                return left;
            }
            if (name == "graph")
            {
                return chart;
            }
            if (name == "panes")
            {
                return panes ? panes->tabs() : Widget{};
            }
            if (name == "timeline")
            {
                return timeline->canvas();
            }
            return {};
        }

        std::string scrollTo(const ControlArgs& args, IFormatWriter& out)
        {
            const std::string to = args.string("to").value_or("");
            const std::optional<u64> y = args.integer("y");
            if (y.has_value() == !to.empty())
            {
                return "give y=<pixels> or to=<top|list|graph|panes|timeline|bottom>";
            }
            float offset = y ? static_cast<float>(*y) : 0.0f;
            if (to == "bottom")
            {
                offset = 1.0e9f;
            }
            else if (!to.empty() && to != "top")
            {
                const Widget target = region(to);
                if (!target.isValid())
                {
                    return "no region named '" + profilerSafeText(to, 32) +
                        "' (top, list, graph, panes, timeline, bottom)";
                }
                offset = regionRect(target).position.y - regionRect(scroll.content()).position.y - density.density().xs;
            }
            scroll.setScrollOffset(Vec2{0.0f, offset});
            out.writeF64("y", scroll.scrollOffset().y);
            return {};
        }

        void writePage(IFormatWriter& out) const
        {
            const Vec2 view = scroll.size();
            const Vec2 content = scroll.contentSize();
            out.writeF64("viewportWidth", view.x);
            out.writeF64("viewportHeight", view.y);
            out.writeF64("contentWidth", content.x);
            out.writeF64("contentHeight", content.y);
            out.writeF64("scrollY", scroll.scrollOffset().y);
            out.writeF64("maxScrollY", std::max(0.0f, content.y - view.y));
            out.writeString("pane",
                            panes && panes->active() >= 0 ? PANE_IDS[static_cast<u32>(panes->active())] : "none");
            out.beginObject("regions");
            const auto write = [&](const char* name, const Widget& widget)
            {
                const UiRect rect = widget.isValid() ? regionRect(widget) : UiRect{};
                const float top = regionRect(scroll.content()).position.y;
                out.beginObject(name);
                out.writeF64("y", rect.position.y - top); // in the page, scrolled or not
                out.writeF64("height", rect.size.y);
                out.writeF64("width", rect.size.x);
                out.endObject();
            };
            write("toolbar", bar);
            write("list", left);
            write("graph", chart);
            write("panes", panes ? panes->tabs() : Widget{});
            write("timeline", timeline->canvas());
            out.endObject();
        }

        // -- control ----------------------------------------------------------------------------------------------

        // What profilerState answers from: the store and the worker's finished pages as they are now, not as the last
        // refresh of a panel that may be hidden left them. (Whether a capture runs, and so the live mode, is the status
        // a shown panel reads: a control query cannot run inside this handler.)
        void syncNow()
        {
            checkLocale();
            readStore();
            scanOnce();
            if (data && data->busy())
            {
                pumpData(PAGES_PER_FRAME); // the throttle of a frame: a longer load shows as loading
            }
            if (entriesDirty)
            {
                rebuildEntries();
            }
            if (reportDirty)
            {
                showReport();
            }
            if (summaryDirty)
            {
                showSummary();
                lastSummary = summary.full();
            }
        }

        void writeState(IFormatWriter& out)
        {
            syncNow();
            static constexpr const char* MODES[] = {"none", "live", "stored"};
            out.beginObject("selection");
            writeSelection(out);
            out.endObject();
            out.writeString("mode", MODES[static_cast<u32>(mode)]);
            out.writeBool("maximized", dock.isMaximized(panelName));
            out.beginArray("captures", static_cast<u32>(entries.size()));
            const ProfilerSelectionComponent& sel = selection();
            for (const Entry& entry : entries)
            {
                out.beginObject({});
                out.writeString("kind", entry.inStore ? "store" : "file");
                out.writeU32("id", entry.id);
                out.writeString("name", entry.name);
                out.writeString("path", entry.path);
                out.writeString("state", entry.state);
                out.writeBool("live", entry.live);
                out.writeU64("frames", entry.frames);
                out.writeU64("durationNs", entry.durationNs);
                out.writeU64("bytes", entry.bytes);
                out.writeString("error", entry.error);
                out.writeBool("selected", entry.inStore && entry.id == sel.capture);
                out.writeU64("modifiedMs", static_cast<u64>(std::max<std::int64_t>(0, entry.modifiedMs)));
                const ProfilerListRow text = rowText(entry);
                out.writeString("title", text.title);
                out.writeString("detail", text.detail);
                out.endObject();
            }
            out.endArray();
            out.beginObject("list");
            out.writeBool("showSmall", showSmall);
            out.writeU32("smallFiles", smallFiles);
            out.writeI32("cursor", captureList->cursor());
            out.writeU64("smallBytes", SMALL_BYTES);
            out.endObject();
            out.beginObject("files");
            out.writeBool("listed", filesListed);
            out.writeBool("truncated", filesTruncated);
            out.writeString("error", filesError);
            out.writeU64("count", files.size());
            out.endObject();
            out.beginObject("graph");
            out.writeU32("capture", graph.capture);
            out.writeBool("loading", graph.loading);
            out.writeBool("done", graph.done);
            out.writeString("error", graph.error);
            out.writeU64("frames", graph.frames);
            out.writeU64("loaded", graph.loaded);
            out.writeBool("capped", graph.capped);
            out.writeBool("gpuCalibrated", graph.gpuCalibrated);
            out.writeU64("lanes", graph.lanes.size());
            out.writeU32("series", chart.seriesCount());
            out.beginObject("band"); // the timeline's window, marked on the graph
            const UiChartSelection marked = chart.readout().band;
            out.writeBool("shown", marked.active);
            out.writeBool("pending", bandPending);
            out.writeBool("approximate", marked.active && bandApproximate);
            out.writeU64("first", marked.first);
            out.writeU64("last", marked.last);
            out.endObject();
            out.beginObject("stats");
            out.writeU64("count", graph.stats.count);
            out.writeF64("meanMs", graph.stats.meanMs);
            out.writeF64("minMs", graph.stats.minMs);
            out.writeF64("p50Ms", graph.stats.p50Ms);
            out.writeF64("p99Ms", graph.stats.p99Ms);
            out.writeF64("maxMs", graph.stats.maxMs);
            out.writeU64("overBudget", graph.stats.overBudget);
            out.writeU64("overSlow", graph.stats.overSlow);
            out.endObject();
            out.endObject();
            out.beginObject("live");
            out.writeU64("frames", liveFrames);
            out.writeF64("lastCpuMs", liveCpuMs);
            out.writeF64("lastGpuMs", liveGpuMs);
            out.endObject();
            out.beginObject("page");
            writePage(out);
            out.endObject();
            out.beginObject("status");
            writeStatus(out);
            out.endObject();
            out.writeString("summary", lastSummary);
            out.writeString("error", actionError);
            out.beginObject("stats");
            out.writeU64("refreshes", refreshes);
            out.writeU64("listRebuilds", listRebuilds);
            out.writeU64("scans", scans);
            out.writeU64("storeReads", storeReads);
            out.writeU64("pagesApplied", pagesApplied);
            out.writeU64("selects", selects);
            out.writeBool("workerBusy", data && data->busy());
            writeBoundCells(out, captureList->firstRow(), captureList->slotCount(), captureList->binds,
                            captureList->writes, captureList->visibleLines());
            out.endObject();
            if (timeline)
            {
                timeline->writeState(out);
            }
        }

        void registerCommands()
        {
            control->registerViewCommand(
                "profilerSelect", OWNER,
                "sets the Profiler GUI's selection (a ProfilerSelection component on the profiler panel): the "
                "capture the views show, a frame range in it and a zone. Every value is checked against the capture "
                "store and refused when the capture does not have it; a new capture clears the frames and the zone",
                {optionalArg("capture", ArgKind::Integer,
                             "store id from profileCaptures (a ready capture); 0 selects none"),
                 optionalArg("firstFrame", ArgKind::Integer, "first frame of the range; without lastFrame, one frame"),
                 optionalArg("lastFrame", ArgKind::Integer, "last frame of the range, inclusive", "firstFrame"),
                 optionalArg("zoneLane", ArgKind::Integer,
                             "lane of the zone (profileCaptures capture=<id> lists them)"),
                 optionalArg("zoneIndex", ArgKind::Integer, "the zone's index in its lane, as profileZones reports"),
                 optionalArg("gpuFrame", ArgKind::Integer, "the GPU frame of a GPU pass, as profileGpu reports"),
                 optionalArg("gpuPass", ArgKind::Integer, "the pass's position in its GPU frame (with gpuFrame)"),
                 optionalArg("clear", ArgKind::String, "drops the frames, the zone, the GPU pass or everything first",
                             {}, CLEAR_CHOICES)},
                [this](const ControlArgs& args, IFormatWriter& out) { return select(args, out); });
            control->registerViewCommand(
                "profilerRefresh", OWNER,
                "re-reads the capture store and rescans the artifacts folder for capture sidecars (profileFiles)", {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    seenCommand = ~0ull;
                    storeGeneration = ~0ull;
                    if (data)
                    {
                        data->requestFiles();
                        ++scans;
                    }
                    out.writeBool("queued", true); // the scan runs on the worker; profilerState shows its result
                    return std::string{};
                });
            control->registerViewCommand(
                "profilerMaximize", OWNER,
                "maximizes the Profiler panel's tab group over the dock (the Maximize button), or restores the layout "
                "(Restore): the panel's tab shows first. The same as dock kind=maximize|restore panel=profiler; "
                "profilerState `maximized` says which. Refused for a closed or floating panel",
                {optionalArg("on", ArgKind::Boolean, "true maximizes, false restores", "the opposite of now")},
                [this](const ControlArgs& args, IFormatWriter& out) { return setMaximized(args, out); });
            control->registerViewCommand(
                "profilerCopyPath", OWNER,
                "copies the selected capture's report path (its sidecar, when it has no report) to the clipboard: the "
                "Copy button; the answer is the path",
                {}, [this](const ControlArgs&, IFormatWriter& out) { return copyPath(out); });
            control->registerViewCommand(
                "profilerListFilter", OWNER,
                "shows or hides the stub sidecars (unopened files under 1 MiB) in the capture list: the Show small "
                "button. Hidden by default; profilerState list.smallFiles counts them",
                {optionalArg("showSmall", ArgKind::Boolean, "list them too", "the opposite of now")},
                [this](const ControlArgs& args, IFormatWriter& out) { return setSmall(args, out); });
            control->registerViewCommand(
                "profilerScroll", OWNER,
                "scrolls the Profiler panel's page (it scrolls when its regions do not fit): to a region (top, list, "
                "graph, panes: the view tab bar, timeline, bottom) or to y pixels from the top; the wheel over the "
                "panel "
                "does the same. The answer is the offset reached; profilerState `page` holds the viewport, the page "
                "and the regions' places",
                {optionalArg("to", ArgKind::String, "the region to bring to the top of the viewport", {},
                             SCROLL_TARGETS),
                 optionalArg("y", ArgKind::Integer, "pixels from the top of the page")},
                [this](const ControlArgs& args, IFormatWriter& out) { return scrollTo(args, out); });
            control->registerQuery(
                "profilerState", OWNER,
                "the Profiler panel's selection, mode (none, live, stored), capture list rows, the frame graph's load "
                "state and statistics, and counters",
                {}, [this](const ControlArgs&, IFormatWriter& out) { return writeState(out), std::string{}; });
        }
    };

    EditorProfiler::EditorProfiler(Engine& engine, EditorFeedback& feedback, EditorDialogs& dialogs,
                                   const DockSpace& dock, EditorCaptureFiles& files) :
        m_state(std::make_unique<State>(engine, feedback, dialogs, dock))
    {
        State& state = *m_state;
        state.captureFiles = &files;
        registerComponent<ProfilerSelectionComponent>();
        registerComponent<ProfilerViewComponent>();
        state.root = state.scope.createPanel("editor.panel", columnDesc());
        state.density.apply([&state](const Density& d) { state.placePage(d); });
        state.scope.world().addComponent<ProfilerSelectionComponent>(state.root.entity());
        state.scope.world().addComponent<ProfilerViewComponent>(state.root.entity());
        // the capture bar's actions are icons with their names as tooltips: one row in the list's narrow column
        const auto barButton = [&](const char* id, const std::string_view icon, const char* key)
        {
            Button button = state.density.track(state.scope.createButton(id, "", fitWidthDesc()), HEIGHT_BUTTON);
            bindFitPadding(button, SPACE_SM, SPACE_NONE);
            button.setIcon(icon);
            button.setAccessibleNameKey(profilerKey(key));
            button.setTooltip(profilerKey(key));
            return button;
        };
        state.start = barButton("profiler.start", "record", "start");
        state.start.onClick([&state] { state.send({{"command", "profileStart"}}); });
        state.stop = barButton("profiler.stop", "stop", "stop");
        state.stop.onClick([&state] { state.send({{"command", "profileStop"}}); });
        state.refreshButton = barButton("profiler.refresh", "arrow-clockwise", "refresh");
        state.refreshButton.onClick([&state] { state.send({{"command", "profilerRefresh"}}); });
        state.closeButton = barButton("profiler.close", "x", "close");
        state.closeButton.onClick(
            [&state]
            {
                if (const CaptureId id = state.selection().capture; id != 0)
                {
                    state.send({{"command", "profileClose"}, {"args", {{"capture", id}}}});
                }
            });

        UiRectDesc fitBarDesc = barDesc(Vec2{});
        fitBarDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = false};
        state.maximizeButton =
            state.density.track(state.scope.createButton("profiler.maximize", "", fitBarDesc), HEIGHT_BUTTON);
        bindFitPadding(state.maximizeButton, SPACE_SM, SPACE_NONE);
        state.maximizeButton.setIcon("corners-out");
        state.maximizeButton.setAccessibleNameKey(profilerKey("maximize"));
        state.maximizeButton.setTooltip(profilerKey("maximize"));
        state.maximizeButton.onClick([&state] { state.send({{"command", "profilerMaximize"}}); });

        // The code's content of the document's slots is built here, unparented, and goes into its slot when the view
        // binds it (State::view below).
        state.tips.create(state.scope, "profiler.status.tip");
        state.notice.create(state.scope, "profiler.notice", profilerKey("notice.name"), Widget{}, barDesc(Vec2{}),
                            &state.tips);
        state.density.track(state.notice.box, HEIGHT_BUTTON);
        UiRectDesc pathDesc = barDesc(Vec2{});
        pathDesc.layoutItem = fillItem();
        state.reportPath.create(state.scope, "profiler.report.path", profilerKey("report.path.name"), Widget{},
                                pathDesc, &state.tips, true);
        state.density.track(state.reportPath.box, HEIGHT_BUTTON);
        state.copyButton =
            state.density.track(state.scope.createButton("profiler.copyPath", "", fitBarDesc), HEIGHT_BUTTON);
        bindFitPadding(state.copyButton, SPACE_MD, SPACE_NONE);
        state.copyButton.setLocalizedText(profilerKey("copy"));
        state.copyButton.setAccessibleNameKey(profilerKey("copy.name"));
        state.copyButton.onClick([&state] { state.send({{"command", "profilerCopyPath"}}); });
        state.revealButton =
            state.density.track(state.scope.createButton("profiler.reveal", "", fitBarDesc), HEIGHT_BUTTON);
        bindFitPadding(state.revealButton, SPACE_MD, SPACE_NONE);
        state.revealButton.setLocalizedText(profilerKey("reveal"));
        state.revealButton.setAccessibleNameKey(profilerKey("reveal.name"));
        state.revealButton.onClick([&state] { state.send({{"command", "profilerReveal"}}); });
        // The list's two bars: the stub-file toggle, and Show file / Delete for the cursor row. Fitted to their
        // labels, so a longer translation grows them (two rows: one would not fit the 340 px column at text scale 1.5).
        UiRectDesc fitDesc = barDesc(Vec2{});
        fitDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = false};
        const auto listButton = [&](const char* id, Button& button, const char* nameKey)
        {
            button = state.density.track(state.scope.createButton(id, "", fitDesc), HEIGHT_BUTTON);
            bindFitPadding(button, SPACE_MD, SPACE_NONE);
            button.setAccessibleNameKey(profilerKey(nameKey));
        };
        listButton("profiler.showSmall", state.smallButton, "list.small.name");
        state.smallButton.onClick([&state] { state.send({{"command", "profilerListFilter"}}); });
        listButton("profiler.revealFile", state.revealFileButton, "list.reveal.name");
        state.revealFileButton.setLocalizedText(profilerKey("list.reveal"));
        state.revealFileButton.onClick([&state] { state.listAction(false); });
        listButton("profiler.deleteFile", state.deleteButton, "list.delete.name");
        state.deleteButton.setLocalizedText(profilerKey("list.delete"));
        state.deleteButton.onClick([&state] { state.listAction(true); });
        state.captureList = std::make_unique<ProfilerCaptureList>(ProfilerCaptureList::Host{
            .scope = &state.scope,
            .density = &state.density,
            .read = [&state](const u32 row, ProfilerListRow& out) { out = state.rowText(state.entries[row]); },
            .activate =
                [&state](const u32 row)
            {
                const Entry& entry = state.entries[row];
                if (!entry.inStore)
                {
                    state.send({{"command", "profileOpen"}, {"args", {{"path", entry.path}}}}, true);
                }
                else if (entry.state == "ready")
                {
                    state.send({{"command", "profilerSelect"}, {"args", {{"capture", entry.id}}}});
                }
            },
            .remove = [&state] { state.listAction(true); },
            .cursorMoved = [&state] { state.updateListBar(); }});
        ProfilerCaptureList& captures = *state.captureList;
        captures.list.setAccessibleRole(UiRole::Grid);
        captures.list.setAccessibleNameKey(profilerKey("list.name"));
        state.density.apply(
            [&state](const Density&)
            {
                UiLayoutItemComponent listItem = fillItem();
                listItem.minSize = Vec2{0.0f, state.listMin()};
                state.captureList->list.setLayoutItem(listItem);
            });
        // a list that cannot move gives the wheel to the page
        captures.list.onWheel(
            [&state](const Vec2 wheel)
            {
                const Vec2 delta = Vec2{wheel.x, -wheel.y} * UI_WHEEL_STEP;
                const Vec2 before = state.captureList->list.scrollOffset();
                state.captureList->list.scrollBy(delta);
                if (state.captureList->list.scrollOffset() == before)
                {
                    state.scroll.scrollBy(delta);
                }
            });

        Panel& graphColumn = state.graphStack;
        graphColumn =
            stackPanel(state.scope, "profiler.graphStack", Vec2{}, UiLayoutDirection::Column, UiLayoutAlign::Stretch);
        graphColumn.setLayoutItem(fillItem());
        state.summary.create(state.scope, "profiler.summary", profilerKey("summary.name"), graphColumn, barDesc(Vec2{}),
                             &state.tips);
        state.density.track(state.summary.box, HEIGHT_BUTTON);
        UiChartDesc chartDesc;
        chartDesc.rect = topLeft();
        chartDesc.rect.layoutItem = fillItem();
        chartDesc.rect.layoutItem->minSize = Vec2{0.0f, State::CHART_MIN};
        chartDesc.textSize = 11.0f;
        state.chart = state.scope.createChart("profiler.graph", chartDesc);
        state.chart.setParent(graphColumn);
        state.chart.setVisible(false); // until a capture or the live view fills it
        // the graph is a region of its own and its one stop: arrows move the selected frame
        state.chart.setLandmark(true);
        state.chart.setAccessibleRole(UiRole::Application);
        state.chart.setAccessibleNameKey(profilerKey("graph.name"));
        state.scope.world().getComponent<UiStateComponent>(state.chart.entity()).tabStop = true;
        state.scope.setKeyReaction(state.chart.entity(), [&state](const UiKeyDownEvent& event)
                                   { return state.graphKey(event.scancode, event.modifiers, event.repeat); });
        if (state.store)
        {
            ProfilerViewHost host;
            host.scope = &state.scope;
            host.density = &state.density;
            host.control = state.control;
            host.store = state.store;
            host.data = state.data.get();
            host.locale = &state.locale;
            host.owner = OWNER;
            host.selection = [&state]() -> const ProfilerSelectionComponent& { return state.selection(); };
            host.send = [&state](const Json& line) { state.send(line); };
            host.sync = [&state] { state.syncNow(); };
            state.panes = std::make_unique<ProfilerPanes>(std::move(host));
            state.panes->root().setParent(graphColumn);
            state.panes->root().setLandmark(true);
        }
        state.budgetFast.assign(PROFILER_PAGE_FRAMES, static_cast<float>(PROFILER_BUDGET_MS));
        state.budgetSlow.assign(PROFILER_PAGE_FRAMES, static_cast<float>(PROFILER_BUDGET_SLOW_MS));
        state.chart.onSelect([&state](const UiChartSelection& selection)
                             { state.selectFrames(selection.first, selection.last); });
        state.chart.onPlotClick([&state](const UiChartClick& click)
                                { state.selectFrames(click.sample, click.sample); });
        state.timeline = std::make_unique<ProfilerTimeline>(
            ProfilerTimeline::Host{
                .engine = &engine,
                .control = state.control,
                .scope = &state.scope,
                .density = &state.density,
                .store = state.store,
                .data = state.data.get(),
                .root = state.root.entity(),
                .owner = OWNER,
                .send = [&state](std::string line) { state.pending.push_back({std::move(line), false}); },
                .sendView = [&state](std::string line) { state.pending.push_back({std::move(line), false, true}); },
                .leave = [&state] { (void)state.focusToolbar(); },
                .showFrames = [&state](const std::optional<std::pair<u64, u64>> frames, const bool approximate)
                { state.showBand(frames, approximate); }},
            graphColumn);
        // Escape, and the gamepad's East, from a region back to the toolbar: whatever a region's widgets left
        // unhandled reaches the panel's root
        state.scope.setKeyReaction(state.root.entity(),
                                   [&state](const UiKeyDownEvent& event)
                                   {
                                       constexpr u32 ESCAPE = 41;
                                       return event.scancode == ESCAPE && event.modifiers == 0 && !event.repeat &&
                                           state.focusToolbar();
                                   });
        state.registerCommands();
        files.setView([&state] { return state.selection().capture; }, [&state] { return state.files; },
                      [&state](const std::string& name)
                      {
                          std::erase_if(state.files, [&](const CaptureFile& listed) { return listed.name == name; });
                          state.entriesDirty = true;
                          state.seenCommand = ~0ull;
                          if (state.data)
                          {
                              state.data->requestFiles();
                              ++state.scans;
                          }
                      });
        state.scope.commit();

        // The document (assets/ui/editor/profiler.json) owns the chrome: the scrolling page, the toolbar, the list's
        // column with its status rows and bars, and the graph column. The code owns what goes in the slots. Landmarks
        // are the code's: F6 and the shoulder buttons visit them.
        UiViewContract contract;
        contract.require(InternedString(PAGE_ID), UiWidgetKind::ScrollView,
                         [&state](const Widget& widget)
                         {
                             state.scroll = widget.as<ScrollView>();
                             state.placePage(state.density.density());
                         });
        contract.require(InternedString(LEFT_ID), UiWidgetKind::Panel,
                         [&state](const Widget& widget) { state.left = widget.as<Panel>(); });
        contract.slot(InternedString(BAR_ID),
                      [&state](const Widget& slot)
                      {
                          for (Widget button :
                               std::initializer_list<Widget>{state.start, state.stop, state.refreshButton,
                                                             state.closeButton, state.maximizeButton})
                          {
                              putIn(state.scope, slot, button);
                          }
                          state.bar = slot.as<Panel>();
                      });
        contract.slot(InternedString(NOTICE_ID),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.notice.box);
                          state.noticeRow = slot.as<Panel>();
                          state.noticeRow.setVisible(state.noticeShown);
                      });
        contract.slot(InternedString(REPORT_ID),
                      [&state](const Widget& slot)
                      {
                          for (Widget part : std::initializer_list<Widget>{state.reportPath.box, state.copyButton,
                                                                           state.revealButton})
                          {
                              putIn(state.scope, slot, part);
                          }
                          state.reportRow = slot.as<Panel>();
                          state.reportRow.setVisible(state.reportShown);
                      });
        contract.slot(InternedString(SMALL_BAR_ID),
                      [&state](const Widget& slot) { putIn(state.scope, slot, state.smallButton); });
        contract.slot(InternedString(LIST_BAR_ID),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.revealFileButton);
                          putIn(state.scope, slot, state.deleteButton);
                      });
        contract.slot(InternedString(LIST_ID),
                      [&state](const Widget& slot) { putIn(state.scope, slot, state.captureList->list); });
        contract.slot(InternedString(GRAPH_ID),
                      [&state](const Widget& slot) { putIn(state.scope, slot, state.graphStack); });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                const Density density = Density::read(viewScope);
                UiRectDesc scrollDesc = topLeft();
                scrollDesc.layoutItem = fillItem();
                ScrollView scroll = viewScope.createScrollView(InternedString(PAGE_ID), scrollDesc);
                scroll.setParent(root);
                const auto fitRow = [&](const std::string_view id, const Widget& parent)
                {
                    UiRectDesc desc = barDesc(Vec2{}, stack(UiLayoutDirection::Row, density.xs));
                    desc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                    Panel row = viewScope.createPanel(InternedString(id), desc);
                    bindGap(row, SPACE_XS);
                    row.setParent(parent);
                    return row;
                };
                fitRow(BAR_ID, scroll.content()).setLandmark(true);
                Panel body =
                    stackPanel(viewScope, "profiler.body", Vec2{}, UiLayoutDirection::Row, UiLayoutAlign::Stretch);
                body.setLayoutItem(fillItem());
                body.setParent(scroll.content());
                Panel left = stackPanel(viewScope, LEFT_ID, Vec2{State::LIST_WIDTH, 0.0f}, UiLayoutDirection::Column,
                                        UiLayoutAlign::Stretch);
                left.setLayoutItem(UiLayoutItemComponent{.minSize = Vec2{State::LIST_MIN, 0.0f}});
                left.setLandmark(true);
                left.setParent(body);
                UiRectDesc noticeDesc =
                    barDesc(Vec2{}, stack(UiLayoutDirection::Column, 0.0f, {}, UiLayoutAlign::Stretch));
                noticeDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                viewScope.createPanel(InternedString(NOTICE_ID), noticeDesc).setParent(left);
                fitRow(REPORT_ID, left);
                fitRow(SMALL_BAR_ID, left);
                fitRow(LIST_BAR_ID, left);
                fillSlotPanel(viewScope, LIST_ID, left);
                UiRectDesc graphDesc = columnDesc();
                graphDesc.layoutItem = fillItem();
                graphDesc.layoutItem->minSize = Vec2{State::GRAPH_MIN, 0.0f};
                viewScope.createPanel(InternedString(GRAPH_ID), graphDesc).setParent(body);
            });
        state.view = openView(engine, VIEW_ID, "assets/ui/editor/profiler.json", std::move(contract), state.root);
    }

    EditorProfiler::~EditorProfiler()
    {
        m_state->captureFiles->setView({}, {}, {});
        m_state->control->unregisterOwner(OWNER);
    }

    Widget EditorProfiler::content() const { return m_state->root; }

    u64 EditorProfiler::viewToken() const { return m_state->timeline->viewToken(); }

    u64 EditorProfiler::matchToken() const { return m_state->timeline->matchToken(); }

    void EditorProfiler::injectMatches(const CaptureId capture, const u64 token, const std::string& name,
                                       const u32 count)
    {
        m_state->timeline->injectMatches(capture, token, name, count);
    }

    void EditorProfiler::injectSpans(const CaptureId capture, const u64 token, const u32 lane,
                                     const std::int64_t fromNs, const std::int64_t toNs, const u32 columns,
                                     std::vector<CaptureSpan> spans)
    {
        ProfilerEvent event;
        event.kind = ProfilerEvent::Kind::Spans;
        event.capture = capture;
        event.token = token;
        event.lane = lane;
        event.fromNs = fromNs;
        event.toNs = toNs;
        event.columns = columns;
        event.spans = std::move(spans);
        m_state->timeline->onEvent(event);
    }

    void EditorProfiler::update()
    {
        State& state = *m_state;
        if (!state.pending.empty())
        {
            state.dispatchPending();
        }
        state.feedback->reportJobs("profiler", state.data && state.data->busy() ? 1u : 0u);
        if (state.data && state.data->busy())
        {
            SPITE_ZONE("EditorProfiler.pumpData");
            state.pumpData(PAGES_PER_FRAME); // a load finishes by itself while hidden, 2 pages a frame
        }
        state.readStore(); // one atomic load when nothing changed; a closed capture drops its selection while hidden
        if (!shown(state.scope, state.root))
        {
            state.wasShown = false;
            state.seenCommand = ~0ull;
            SPITE_ZONE("EditorProfiler.timeline");
            state.timeline->update(false);
            return;
        }
        ++state.frames;
        state.updateMaximizeButton();
        if (state.panes)
        {
            state.panes->update();
        }
        state.fitPage();
        for (StatusText* item : state.tips.items())
        {
            item->tick();
        }
        if (state.mode == State::Mode::Live && state.status.armed)
        {
            SPITE_ZONE("EditorProfiler.live");
            const State::Clock::time_point now = State::Clock::now();
            double gpuMs = 0.0;
            if (state.engine->services().contains<IRenderer>())
            {
                gpuMs = state.engine->services().read<IRenderer>()->lastGpuFrameTimings().frameMs;
            }
            if (state.wasShown)
            {
                state.appendLive(std::chrono::duration<double, std::milli>(now - state.lastTick).count(), gpuMs);
            }
            state.lastTick = now; // the first frame after a hide has no frame time of its own
        }
        const u64 command = state.control->commandGeneration();
        const u32 interval = state.status.stopping ? 1
            : state.wasWriting                     ? State::WRITING_REFRESH_FRAMES
                                                   : State::REFRESH_FRAMES;
        if (!state.wasShown || state.frames >= interval || command != state.seenCommand)
        {
            state.seenCommand = command;
            state.refresh();
            state.wasShown = true;
        }
        if (state.entriesDirty)
        {
            SPITE_ZONE("EditorProfiler.rebuildEntries");
            state.rebuildEntries();
        }
        if (state.reportDirty)
        {
            state.showReport();
        }
        if (state.selectionDirty)
        {
            state.selectionDirty = false;
            state.captureList->rebindVisible();
            state.summaryDirty = true;
        }
        if (state.mirrorPending && state.mode == State::Mode::Stored && state.graph.done)
        {
            state.mirrorSelection();
        }
        if (state.summaryDirty)
        {
            SPITE_ZONE("EditorProfiler.showSummary");
            state.showSummary();
            state.lastSummary = state.summary.full();
        }
        SPITE_ZONE("EditorProfiler.timeline");
        state.timeline->update(true);
        if (state.bandPending && (!state.band || (state.mode == State::Mode::Stored && state.graph.done)))
        {
            state.applyBand();
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "profiler",
             .category = "debug",
             .order = 110,
             .dock = {.target = "scene"}, // the wide centre group; the bottom strip is full
             .zone = "EditorProfiler",
             .make = [](EditorPanelContext& c)
             {
                 return makeEditorPanel<EditorProfiler>(c.engine, c.feedback, c.dialogs, c.dock,
                                                        EditorModelAccess::captureFiles(c.model));
             }}};
    } // namespace
} // namespace spite
