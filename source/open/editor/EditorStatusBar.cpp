#include "EditorStatusBar.hpp"
#include "EditorBuildId.hpp"
#include "EditorContext.hpp"
#include "EditorEntityLabel.hpp"
#include "EditorFeedback.hpp"
#include "editor/EditorModel.hpp"
#include "EditorPanelUi.hpp"
#include "EditorPanelList.hpp"
#include "EditorSettings.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "application/Window.hpp"
#include "control/ControlServer.hpp"
#include "control/LogHistory.hpp"
#include "engine/Engine.hpp"
#include "engine/FrameQuiescence.hpp"
#include "engine/PlayWorld.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "rendering/IRenderer.hpp"
#include "rendering/ITransferManager.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiScope.hpp"
#include "ui/UiTextMeasurer.hpp"
#include "uiview/UiStyleSheet.hpp"
#include "uiview/UiViewContext.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using namespace panelui;

        constexpr std::string_view OWNER = "editorStatusBar";
        constexpr std::string_view VIEW = "editor.statusbar";
        constexpr std::string_view LEFT_SLOT = "statusbar.left";
        constexpr std::string_view RIGHT_SLOT = "statusbar.right";
        constexpr std::string_view END_SLOT = "statusbar.end";
        constexpr std::string_view THEME_DARK = "forge-dark";
        constexpr std::string_view THEME_LIGHT = "forge-light";
        constexpr std::string_view TABLE = "editor-statusbar.";

        constexpr float BAR_PAD = 8.0f;
        constexpr float ITEM_GAP = 8.0f;
        constexpr float ITEM_HEIGHT = 20.0f;
        constexpr float RIGHT_WIDTH =
            560.0f; // jobs, warnings, errors, quiescent lamp, upload rate, entities, GPU (full), FPS, frame
        constexpr float LEFT_MIN_WIDTH = 184.0f; // the play state and a clipped scene name
        constexpr float LEFT_KEEP_WIDTH =
            402.0f; // what the end groups leave the left slot: state, scene, some selection
        constexpr float AGENT_MAX_WIDTH = 220.0f; // a long client name clips
        constexpr u32 AGENT_BUCKETS = 60; // one second each: the commands-per-minute window
        constexpr float ITEM_TEXT_PAD = 6.0f; // what a fitted item holds beside its text
        constexpr float LAMP_WIDTH = 84.0f;
        constexpr float UPLOAD_WIDTH = 76.0f;
        constexpr float FPS_WIDTH = 64.0f;
        constexpr float ENTITIES_WIDTH = 92.0f;
        constexpr float GPU_WIDTH = 84.0f;
        constexpr double FPS_BAND = 1.5; // the shown FPS keeps its value until the reading is this far from it
        constexpr float TOAST_WIDTH = 340.0f;
        constexpr float TOAST_HEIGHT = 44.0f;
        constexpr float TOAST_GAP = 6.0f;
        constexpr float TOAST_MARGIN = 10.0f;
        constexpr float TOAST_PAD = 8.0f;
        constexpr double FRAME_WINDOW = 0.25; // seconds of frames per frame-time reading: at most 4 Hz
        constexpr double FRAME_OVER =
            1.05; // a frame time is over its budget past this multiple: a vsynced 16.67 ms frame is not
        constexpr u32 BUSY_WINDOWS = 4; // windows in a row mostly busy before the QUIESCENT lamp goes out (one second)
        constexpr u64 SAMPLE_MIN_FRAMES = 4; // engine frames a lamp sample covers at least
        constexpr double JOB_DELAY = 0.5; // a job shows once it has run this long
        constexpr u32 ESCAPE_KEY = 41; // HID

        enum class Play : u8
        {
            Edit,
            Playing,
            Paused
        };
        constexpr std::array<std::string_view, 3> PLAY_NAMES{"edit", "playing", "paused"};

        // the class of a toast of a level, and the accessible role it announces itself with
        constexpr std::array<std::string_view, 3> TOAST_CLASSES{"toast.info", "toast.warn", "toast.error"};

        std::string toolbarTint(const Play play)
        {
            return "toolbar.tint." + std::string(PLAY_NAMES[static_cast<u8>(play)]);
        }
    } // namespace

    struct EditorStatusBar::State
    {
        struct ToastSlot
        {
            Panel panel;
            Label text;
            u64 id = 0; // the toast shown, 0: none
        };

        State(Engine& engine, const DockSpace& dock, const EditorModel& editorModel, EditorFeedback& feedback) :
            engine(&engine), control(&engine.control()), scope(editorUi(engine).ui()), dock(dock),
            editorModel(&editorModel), feedback(&feedback),
            history(engine.services().contains<LogHistory>() ? &engine.services().get<LogHistory>() : nullptr),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
            primeSample();
        }

        Engine* engine;
        ControlServer* control;
        UiScope scope;
        DockSpace dock;
        const EditorModel* editorModel;
        EditorFeedback* feedback;
        LogHistory* history;
        const Localization* localization;

        Label jobsLabel;
        // the end slot: THEME and VIEW toggle groups, the agent client and its rate, the build id
        std::array<ToastSlot, EditorFeedback::MAX_VISIBLE> toasts;
        InternedString leftClass{"statusbar.item"}, rightClass, endClass;
        // per styled item: state, scene, selection, jobs, frame, warn, error, quiescent, upload, then the end slot's
        // theme caption, view caption, dark, light, quiet, data, agent toggle, swatch, agent and dev, then fps,
        // entities and GPU
        std::array<InternedString, 23> shownClass{};
        std::optional<bool> shownInput;
        std::vector<std::string> dataPresets; // the presets of editorSettingsState
        std::string dataColor, dataColorShown, themeName;
        bool agentLayerOn = true;
        bool full = false; // the telemetry setting is full
        float frameBudgetMs = 16.6f;
        u64 settingsSeen = ~0ull;
        std::vector<std::string> pending; // request lines update() dispatches

        // -- the end slot: the groups and what shows of them --
        // The groups in the priority they keep their place in (a narrow bar sheds from the end of the list) and their
        // items in slot order.
        enum Group : u8
        {
            View,
            Theme,
            Agent,
            Dev,
            GROUPS
        };
        enum End : u8
        {
            ThemeCaption,
            ThemeDark,
            ThemeLight,
            ViewCaption,
            ViewQuiet,
            ViewData,
            ViewAgent,
            Swatch,
            AgentText,
            DevText,
            ITEMS
        };
        static constexpr std::array<Group, ITEMS> GROUP_OF{Theme, Theme, Theme, View,  View,
                                                           View,  View,  View,  Agent, Dev};
        // the theme switch and the telemetry and agent-layer toggles live in Preferences: the bar keeps the data
        // swatch, the agent's activity and the build
        static constexpr bool inPreferences(const u32 item) { return item < Swatch; }
        std::array<std::string, ITEMS> endText;
        std::array<float, ITEMS> endWidth{};
        std::optional<ControlServer::AttachedClient> attached;
        u64 attachedSeen = ~0ull;
        std::string agentClient;
        std::string agentText;
        u32 agentRate = 0; // commands a client reported over the last minute
        u64 commandsSeen = 0;
        u64 rateSecond = 0;
        std::array<u32, AGENT_BUCKETS> buckets{};
        bool agentFit = true; // the agent item's width is measured again
        bool endDirty = true; // the groups are decided again
        u64 endLayout = ~0ull;

        // -- the model: what the bar shows, refreshed from the engine --
        Play play = Play::Edit;
        std::string stateText;
        std::string sceneName = "";
        std::string scenePath;
        bool sceneDirty = false;
        bool sceneRead = false;
        u64 sceneGeneration = 0;
        std::string sceneText;
        std::string title;
        u32 selectionCount = 0;
        u64 selectionPrimary = 0;
        u64 selectionStamp = ~0ull;
        std::string selectionPrimaryName;
        std::string selectionText;
        u32 jobsRunning = 0;
        u32 jobsShown = 0;
        double jobsSince = -1.0;
        std::string jobsText;
        u64 logSeen = 0;
        u32 warnings = 0;
        u32 errors = 0;
        std::string warnText, errorText;
        double frameSum = 0.0;
        u32 frameCount = 0;
        double frameMs = 0.0;
        double windowMs = 0.0; // the last window's mean, unrounded
        bool frameKnown = false;
        bool frameOver = false;
        std::string frameText, frameOkText;
        bool frameHovered = false;
        std::function<void(double, bool)> frameListener;
        // the engine's counters at the last lamp sample: lit until BUSY_WINDOWS busy windows in a row
        u64 framesSeen = 0, busySeen = 0, stagedSeen = 0;
        double sampleSeconds = 0.0;
        bool quiescentLit = true;
        u32 busyWindows = 0;
        double uploadRate = 0.0; // bytes per second over the last sample
        std::string quiescentText, uploadText, uploadShownRate, frameNameShown;
        // FPS follows every frame window; the entity count and the GPU time are read at the window close in full only
        std::int64_t fps = 0;
        u32 entityCount = 0;
        double gpuMs = 0.0;
        std::string fpsText, entitiesText, gpuText;
        // the widths the lamps are fitted to are measured under these (the text scale, the stylesheet, the locale)
        u64 fitSheet = ~0ull, fitRevision = ~0ull;
        float fitTextScale = 0.0f;
        float lampMin = 0.0f, frameMin = 0.0f, fpsMin = 0.0f, gpuMin = 0.0f,
              entitiesMin = 0.0f; // the fitted minimum widths
        bool readoutsDirty = true; // the entities or GPU text changed: their minimum is measured again
        u64 shownRevision = ~0ull;
        bool localeChanged = true;
        bool restyle = true;
        u32 classKey = 0;
        Vec2 size{};
        float toastsTop = 0.0f; // the bar top the toasts were placed above
        std::vector<u64> visible; // scratch
        Entity lastFocus = Entity::undefined(); // the focus as the last frame ended: before any press of this one

        [[nodiscard]] std::string tr(const std::string_view key, const std::vector<MessageArg>& args = {}) const
        {
            const std::string full = std::string(TABLE) + std::string(key);
            if (!localization)
            {
                return full;
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(full), args, scratch));
        }

        // -- layout --

        void placeToasts()
        {
            toastsTop = size.y;
            for (u32 k = 0; k < toasts.size(); ++k)
            {
                panelui::place(toasts[k].panel,
                               Vec2{size.x - TOAST_WIDTH - TOAST_MARGIN,
                                    toastsTop - TOAST_MARGIN - static_cast<float>(k + 1) * TOAST_HEIGHT -
                                        static_cast<float>(k) * TOAST_GAP},
                               Vec2{TOAST_WIDTH, TOAST_HEIGHT});
            }
        }

        void place(const Vec2 shell)
        {
            size = shell;
            placeToasts();
        }

        // -- model --

        void refreshLocale()
        {
            localeChanged = localization && localization->revision() != shownRevision;
            if (localeChanged)
            {
                shownRevision = localization->revision();
            }
        }

        void refreshPlay()
        {
            PlayWorld* world = engine->playWorld();
            const Play now = !world                       ? Play::Edit
                : world->world().simulationState().paused ? Play::Paused
                                                          : Play::Playing;
            if (now != play || stateText.empty() || localeChanged)
            {
                play = now;
                stateText = tr(std::string("state.") + std::string(PLAY_NAMES[static_cast<u8>(now)]));
            }
        }

        void refreshScene(const bool force)
        {
            const u64 generation = control->stateGeneration();
            if (sceneRead && !force && generation == sceneGeneration)
            {
                return;
            }
            sceneRead = true;
            sceneGeneration = generation;
            const Json reply = request(*control, Json{{"query", "sceneInfo"}, {"args", {{"counts", false}}}});
            if (!reply.value("ok", false) || !reply.contains("result"))
            {
                sceneName.clear();
                scenePath.clear();
                sceneDirty = false;
                sceneText.clear();
                title.clear();
                return;
            }
            const Json& info = reply["result"];
            scenePath = info.value("activePath", std::string{});
            sceneName = scenePath.empty() ? tr("untitled") : info.value("activeName", std::string{});
            sceneDirty = info.value("workspaceDirty", false);
            sceneText = sceneName + (sceneDirty ? "*" : "");
            std::string app = "SPITE";
            if (localization && localization->contains(InternedString("engine.app.title")))
            {
                std::string scratch;
                app = std::string(localization->resolve(InternedString("engine.app.title"), scratch));
            }
            title = sceneText + " - " + app;
        }

        void refreshSelection()
        {
            const std::span<const Entity> selected = editorModel->selection();
            const u32 count = static_cast<u32>(selected.size());
            const u64 primary = selected.empty() ? 0 : selected.front().id();
            const u64 stamp = (static_cast<u64>(count) << 40) ^ (primary << 8) ^ control->stateGeneration();
            if (stamp == selectionStamp && !localeChanged)
            {
                return;
            }
            selectionStamp = stamp;
            selectionCount = count;
            selectionPrimary = primary;
            selectionPrimaryName.clear();
            if (count > 0)
            {
                const World* world = control->world(editorModel->selectionWorld());
                std::string buffer;
                const Entity first = selected.front();
                selectionPrimaryName = world ? entityLabel(buffer, *world, first)
                                             : unnamedEntityLabel(buffer, INVALID_COMPONENT_ID, first);
            }
            selectionText = count == 0 ? tr("selection.none")
                : count == 1
                ? selectionPrimaryName
                : tr("selection.many",
                     {{"count", static_cast<std::int64_t>(count)}, {"name", std::string_view(selectionPrimaryName)}});
        }

        void refreshJobs()
        {
            jobsRunning = feedback->jobCount();
            if (jobsRunning == 0)
            {
                jobsSince = -1.0;
            }
            else if (jobsSince < 0.0)
            {
                jobsSince = feedback->now();
            }
            const u32 shownNow = jobsRunning > 0 && feedback->now() - jobsSince >= JOB_DELAY ? jobsRunning : 0;
            if (shownNow != jobsShown || localeChanged)
            {
                jobsShown = shownNow;
                jobsText = shownNow == 0 ? std::string{} : tr("jobs", {{"count", static_cast<std::int64_t>(shownNow)}});
            }
        }

        void refreshLog()
        {
            const u64 recorded = history ? history->recorded() : 0;
            bool changed = localeChanged;
            if (recorded != logSeen)
            {
                const u64 from = std::max(logSeen, history->oldest());
                history->read(from, recorded - from,
                              [this](u64, const LogEntryRow& row)
                              {
                                  warnings += row.level == "warning";
                                  errors += row.level == "error";
                              });
                logSeen = recorded;
                changed = true;
            }
            if (changed || warnText.empty())
            {
                warnText = tr("warnings", {{"count", static_cast<std::int64_t>(warnings)}});
                errorText = tr("errors", {{"count", static_cast<std::int64_t>(errors)}});
            }
        }

        // The mean frame time of the last window of frames, a reading about four times a second; the first reading
        // comes with the first frame that has a time (a frame of no time is none). A reading that rounds to the text on
        // screen changes nothing.
        void refreshFrame(const double seconds)
        {
            if (seconds <= 0.0)
            {
                return; // no frame has run yet
            }
            frameSum += seconds;
            ++frameCount;
            if (frameKnown && frameSum < FRAME_WINDOW)
            {
                return;
            }
            const double mean = frameSum / static_cast<double>(frameCount) * 1000.0;
            sampleSeconds += frameSum;
            frameSum = 0.0;
            frameCount = 0;
            windowMs = mean;
            frameOver = isOver(mean);
            if (!frameKnown || msText(mean) != msText(frameMs))
            {
                frameKnown = true;
                frameMs = mean;
                frameText = tr("frame", {{"ms", std::string_view(msText(mean))}});
            }
            // a mean wobbling across a rounding boundary must not repaint a quiescent bar: the shown rate keeps its
            // value until the reading is FPS_BAND away from it
            const double rate = 1000.0 / mean;
            if (fpsText.empty() || std::abs(rate - static_cast<double>(fps)) >= FPS_BAND)
            {
                fps = std::llround(rate);
                fpsText = tr("fps", {{"fps", fps}});
            }
            sampleEngine();
            sampleReadouts();
            if (frameListener)
            {
                frameListener(mean, frameOver);
            }
        }

        [[nodiscard]] bool isOver(const double ms) const
        {
            return frameBudgetMs > 0.0f && ms > frameBudgetMs * FRAME_OVER;
        }

        // What the frame item shows: in quiet the value over budget or while hovered (the FRAME OK lamp otherwise), in
        // full always.
        [[nodiscard]] const std::string& frameShown() const
        {
            return !frameKnown || full || frameOver || frameHovered ? frameText : frameOkText;
        }

        [[nodiscard]] u64 stagedBytes() const
        {
            const auto& services = engine->services();
            return services.contains<ITransferManager>() ? services.read<ITransferManager>()->stats().bytesStaged : 0;
        }

        void primeSample()
        {
            if (engine->services().contains<FrameQuiescence>())
            {
                const auto quiet = engine->services().read<FrameQuiescence>();
                framesSeen = quiet->frames;
                busySeen = quiet->busyFrames;
            }
            stagedSeen = stagedBytes();
        }

        // The QUIESCENT lamp and the upload rate, once per frame window (4 Hz, no per-frame read): the lamp is lit
        // unless BUSY_WINDOWS windows in a row had more than a quarter of their frames busy, so the stats panel's own
        // refresh (one frame in thirty), an edit and its settle frame, or a load or a resize do not make it flicker (a
        // change of its class relays the bar out), and a change that goes on for a second unlights it. A window with
        // fewer than SAMPLE_MIN_FRAMES engine frames is left to the next one.
        void sampleEngine()
        {
            if (!engine->services().contains<FrameQuiescence>())
            {
                return;
            }
            u64 frames = 0;
            u64 busy = 0;
            {
                const auto quiet = engine->services().read<FrameQuiescence>();
                frames = quiet->frames;
                busy = quiet->busyFrames;
            }
            if (frames - framesSeen < SAMPLE_MIN_FRAMES)
            {
                return;
            }
            busyWindows = (busy - busySeen) * 4 > frames - framesSeen ? busyWindows + 1 : 0;
            const bool lit = busyWindows < BUSY_WINDOWS;
            const u64 staged = stagedBytes();
            uploadRate = staged >= stagedSeen ? static_cast<double>(staged - stagedSeen) / sampleSeconds : 0.0;
            framesSeen = frames;
            busySeen = busy;
            stagedSeen = staged;
            sampleSeconds = 0.0;
            if (lit != quiescentLit)
            {
                quiescentLit = lit;
            }
            updateUploadText();
        }

        // The entity count and the GPU time, read once per frame window and only in full: quiet costs neither a read
        // nor a format.
        void sampleReadouts()
        {
            if (!full)
            {
                return;
            }
            const u32 entities = engine->gameWorld().activeEntityCount();
            if (entities != entityCount || entitiesText.empty() || localeChanged)
            {
                entityCount = entities;
                entitiesText = tr("entities", {{"count", static_cast<std::int64_t>(entities)}});
                readoutsDirty = true;
            }
            const double gpu = engine->services().contains<IRenderer>()
                ? engine->services().read<IRenderer>()->lastGpuFrameTimings().frameMs
                : 0.0;
            if (gpuText.empty() || msText(gpu) != msText(gpuMs) || localeChanged)
            {
                gpuMs = gpu;
                gpuText = tr("gpu", {{"ms", std::string_view(msText(gpu))}});
                readoutsDirty = true;
            }
        }

        // The upload item's text, built only while it is shown (full): in quiet a changing rate costs no formatting.
        void updateUploadText()
        {
            if (!full)
            {
                return;
            }
            const std::string rate = rateText(uploadRate);
            if (uploadText.empty() || rate != uploadShownRate || localeChanged)
            {
                uploadShownRate = rate;
                uploadText = tr("upload", {{"rate", std::string_view(rate)}});
            }
        }

        [[nodiscard]] static std::string rateText(const double bytes)
        {
            return bytes < 1024.0   ? std::format("{:.0f} B", bytes)
                : bytes < 1048576.0 ? std::format("{:.1f} KB", bytes / 1024.0)
                                    : std::format("{:.1f} MB", bytes / 1048576.0);
        }

        [[nodiscard]] static std::string msText(const double ms) { return std::format("{:.1f}", ms); }
        // The settings the bar shows follow editorSettingsState whenever a command ran: the theme, the data colour
        // (a colour outside the presets, a hex, shows as itself), the agent layer, the telemetry density and the frame
        // budget.
        void refreshSettings()
        {
            const u64 command = control->commandGeneration();
            if (command == settingsSeen && !localeChanged)
            {
                return;
            }
            const Json reply = request(*control, Json{{"query", "editorSettingsState"}});
            if (!reply.value("ok", false))
            {
                return; // asked again with the next frame
            }
            settingsSeen = command;
            const Json& settings = reply["result"];
            dataPresets = settings.value("dataColors", std::vector<std::string>{});
            dataColor = settings.value("dataColor", std::string{});
            themeName = settings.value("theme", std::string{});
            if (const bool layer = settings.value("agentLayer", true); layer != agentLayerOn)
            {
                agentLayerOn = layer;
                endDirty = true;
                refreshAgentText();
            }
            if (dataColor != dataColorShown || localeChanged)
            {
                dataColorShown = dataColor;
            }
            applyTelemetry(settings.value("telemetry", std::string(TELEMETRY_QUIET)) == TELEMETRY_FULL,
                           settings.value("frameBudgetMs", frameBudgetMs));
        }

        // a preset by its name in the language, any other colour as it is written
        [[nodiscard]] std::string dataColorName() const
        {
            const InternedString key(std::string(TABLE) + "data." + dataColor);
            return localization && localization->contains(key) ? tr("data." + dataColor) : dataColor;
        }

        // the next preset after the one in force, the first from a colour outside them
        [[nodiscard]] std::string nextDataColor() const
        {
            if (dataPresets.empty())
            {
                return {};
            }
            const auto at = std::ranges::find(dataPresets, dataColor);
            return dataPresets[at == dataPresets.end()
                                   ? 0
                                   : static_cast<sizet>(at - dataPresets.begin() + 1) % dataPresets.size()];
        }

        // -- the agent segment: the attached client and the commands clients reported over the last minute --

        void refreshAgent()
        {
            bool changed = localeChanged;
            if (const u64 epoch = control->attachedEpoch(); epoch != attachedSeen)
            {
                attachedSeen = epoch;
                attached = control->attachedClient();
                agentClient = !attached                        ? std::string{}
                    : !attached->name.empty()                  ? attached->name
                    : attached->source == RequestSource::Stdio ? tr("agent.stdio")
                                                               : tr("agent.pipe");
                agentFit = true;
                changed = true;
            }
            const u64 count = control->clientCommandCount();
            const u64 second = static_cast<u64>(feedback->now());
            if (count != commandsSeen || (agentRate > 0 && second != rateSecond))
            {
                for (u64 step = 1; step <= std::min<u64>(second - rateSecond, AGENT_BUCKETS); ++step)
                {
                    buckets[(rateSecond + step) % AGENT_BUCKETS] = 0;
                }
                rateSecond = second;
                buckets[second % AGENT_BUCKETS] += static_cast<u32>(count - commandsSeen);
                commandsSeen = count;
                u32 total = 0;
                for (const u32 bucket : buckets)
                {
                    total += bucket;
                }
                changed |= total != agentRate;
                agentRate = total;
            }
            if (changed)
            {
                refreshAgentText();
            }
        }

        void refreshAgentText()
        {
            const bool wanted = attached.has_value() && agentLayerOn;
            std::string text;
            if (wanted)
            {
                text = agentRate > 0
                    ? tr("agent.rate",
                         {{"client", std::string_view(agentClient)}, {"rate", static_cast<std::int64_t>(agentRate)}})
                    : tr("agent", {{"client", std::string_view(agentClient)}});
            }
            if (text != endText[AgentText])
            {
                endText[AgentText] = std::move(text);
                endDirty = true;
            }
        }

        void applyTelemetry(const bool fullNow, const float budget)
        {
            if (full != fullNow)
            {
                full = fullNow;
                endDirty = true; // the optional items' room is decided again
                if (!full)
                {
                    // the texts built for the full readouts are not kept: coming back builds them in the language in
                    // force then
                    uploadText.clear();
                    entitiesText.clear();
                    gpuText.clear();
                }
                updateUploadText();
                sampleReadouts();
            }
            frameBudgetMs = budget;
            frameOver = isOver(windowMs);
        }

        // An editorSettings that landed: the readouts follow before its reply.
        void settle(const bool fullNow, const float budget) { applyTelemetry(fullNow, budget); }

        void setMinWidth(Widget widget, const float width)
        {
            UiLayoutItemComponent item = scope.world().getConstComponent<UiLayoutItemComponent>(widget.entity());
            if (item.minSize.x != width)
            {
                item.minSize.x = width;
                widget.setLayoutItem(item);
            }
        }

        void setFixedWidth(Widget widget, const float width)
        {
            if (widget.size().x != width)
            {
                widget.setSize(Vec2{width, widget.size().y});
            }
        }

        // everything the query and the widgets read, as of now
        void refresh(const double frameSeconds, const bool frameStep)
        {
            refreshLocale();
            refreshSettings();
            refreshAgent();
            refreshPlay();
            refreshScene(localeChanged);
            refreshSelection();
            refreshJobs();
            refreshLog();
            if (frameStep)
            {
                refreshFrame(frameSeconds);
            }
            if (localeChanged)
            {
                frameOkText = tr("frame.ok");
                if (frameKnown)
                {
                    frameText = tr("frame", {{"ms", std::string_view(msText(frameMs))}});
                }
                quiescentText = tr("quiescent.lit");
                if (frameKnown)
                {
                    fpsText = tr("fps", {{"fps", fps}});
                }
                refreshEndTexts();
                updateUploadText();
                sampleReadouts();
                frameNameShown.clear();
            }
        }

        void refreshEndTexts()
        {
            endText[ThemeCaption] = tr("theme");
            endText[ThemeDark] = tr("theme.dark");
            endText[ThemeLight] = tr("theme.light");
            endText[ViewCaption] = tr("view");
            endText[ViewQuiet] = tr("view.quiet");
            endText[ViewData] = tr("view.data");
            endText[ViewAgent] = tr("view.agent");
            endText[DevText] = tr("dev", {{"id", editorBuildId()}});
            refreshAgentText();
        }

        // -- widgets --

        template <class W>
        static void showText(W& widget, const std::string_view text)
        {
            if (widget.text() != text)
            {
                widget.setText(text);
            }
        }

        void styleItem(const u32 index, Widget widget, const InternedString styleClass)
        {
            if (shownClass[index] != styleClass)
            {
                shownClass[index] = styleClass;
                widget.setStyleClass(styleClass);
            }
        }

        [[nodiscard]] InternedString stateClass() const
        {
            return classPart(leftClass, std::string(".") + std::string(PLAY_NAMES[static_cast<u8>(play)]));
        }
        [[nodiscard]] InternedString warnClass() const
        {
            return classPart(rightClass, warnings > 0 ? ".warn" : ".button");
        }
        [[nodiscard]] InternedString errorClass() const
        {
            return classPart(rightClass, errors > 0 ? ".error" : ".button");
        }
        // the lamps and the quiet frame item are muted until they have something to say
        [[nodiscard]] InternedString frameClass() const
        {
            return full ? rightClass : classPart(rightClass, frameOver ? ".warn" : ".button");
        }
        // the lamp's text never changes (a text change relays the bar out); its class dims it and its accessible name
        // says it is off
        [[nodiscard]] InternedString quiescentNameKey() const
        {
            return InternedString(std::string(TABLE) + (quiescentLit ? "quiescent.lit" : "quiescent.off"));
        }
        [[nodiscard]] InternedString quiescentClass() const
        {
            return quiescentLit ? rightClass : classPart(rightClass, ".button");
        }

        void applyToasts()
        {
            feedback->visibleToasts(visible);
            for (u32 k = 0; k < toasts.size(); ++k)
            {
                ToastSlot& slot = toasts[k];
                const u64 id = k < visible.size() ? visible[visible.size() - 1 - k] : 0;
                if (slot.id == id)
                {
                    continue;
                }
                slot.id = id;
                if (id == 0)
                {
                    slot.panel.setVisible(false);
                    continue;
                }
                const ToastRecord& toast = *feedback->toast(id);
                const u8 level = static_cast<u8>(toast.level);
                slot.panel.setStyleClass(InternedString(TOAST_CLASSES[level]));
                slot.panel.setAccessibleRole(toast.level == ToastLevel::Info ? UiRole::Status : UiRole::Alert);
                slot.panel.setAccessibleName(toast.text);
                showText(slot.text, toast.text);
                slot.panel.setVisible(true);
            }
        }

        void syncWindowTitle()
        {
            if (Window* window = engine->window(); window && !title.empty() && title != shownTitle)
            {
                shownTitle = title;
                window->setTitle(InternedString(title));
            }
        }
        std::string shownTitle;

        // a click on a count shows the Log panel at that level
        void openLog(const std::string_view level)
        {
            pending.push_back(Json{{"command", "logLevel"}, {"args", {{"level", std::string(level)}}}}.dump());
            Json dockArgs{{"kind", dock.isOpen(InternedString("log")) ? "activate" : "float"}, {"panel", "log"}};
            if (!dock.isOpen(InternedString("log")))
            {
                dockArgs["x"] = 120.0f;
                dockArgs["y"] = 80.0f;
                dockArgs["w"] = 520.0f;
                dockArgs["h"] = 320.0f;
            }
            pending.push_back(Json{{"command", "dock"}, {"args", std::move(dockArgs)}}.dump());
        }

        // -- control --

        void writeBar(IFormatWriter& out)
        {
            out.writeString("state", PLAY_NAMES[static_cast<u8>(play)]);
            out.writeString("stateText", stateText);
            out.writeString("tint", stateClass().isValid() ? stateClass().toView() : std::string_view{});
            out.writeString("toolbarTint", toolbarTint(play));
            out.beginObject("scene");
            out.writeString("name", sceneName);
            out.writeString("path", scenePath);
            out.writeBool("dirty", sceneDirty);
            out.writeString("text", sceneText);
            out.endObject();
            out.writeString("windowTitle", title);
            out.beginObject("selection");
            out.writeU32("count", selectionCount);
            out.writeString("primary", selectionPrimaryName);
            out.writeString("text", selectionText);
            out.endObject();
            out.beginObject("jobs");
            out.writeU32("running", jobsRunning);
            out.writeU32("shown", jobsShown);
            out.writeString("text", jobsText);
            out.beginObject("sources");
            for (const auto& [source, count] : feedback->jobSources())
            {
                out.writeU32(source, count);
            }
            out.endObject();
            out.endObject();
            out.writeU32("warnings", warnings);
            out.writeU32("errors", errors);
            out.writeString("warningsText", warnText);
            out.writeString("errorsText", errorText);
            out.writeF64("frameMs", frameMs);
            out.writeString("frameText", frameText);
            out.writeString("telemetry", full ? TELEMETRY_FULL : TELEMETRY_QUIET);
            out.writeString("theme", themeName);
            out.writeString("dataColor", dataColor);
            out.writeBool("agentLayer", agentLayerOn);
            out.beginObject("agent");
            out.writeBool("shown", false);
            out.writeString("client", agentClient);
            out.writeU32("commandsPerMinute", agentRate);
            out.writeString("text", endText[AgentText]);
            out.endObject();
            out.beginObject("end");
            out.writeBool("view", false);
            out.writeBool("theme", false);
            out.writeBool("agent", false);
            out.writeBool("build", false);
            out.endObject();
            out.writeString("build", editorBuildId());
            out.writeF32("frameBudgetMs", frameBudgetMs);
            out.writeBool("frameOver", frameOver);
            out.writeString("frameShown", "");
            out.beginObject("quiescent");
            out.writeBool("lit", quiescentLit);
            out.writeString("text", quiescentText);
            out.endObject();
            out.beginObject("upload");
            out.writeF64("bytesPerSecond", uploadRate);
            out.writeBool("shown", false);
            out.writeString("text", uploadText);
            out.endObject();
            out.writeI32("fps", static_cast<i32>(fps));
            out.writeString("fpsText", fpsText);
            out.writeBool("fpsShown", false);
            out.beginObject("entities");
            out.writeBool("shown", false);
            out.writeU32("count", entityCount);
            out.writeString("text", entitiesText);
            out.endObject();
            out.beginObject("gpu");
            out.writeBool("shown", false);
            out.writeF64("ms", gpuMs);
            out.writeString("text", gpuText);
            out.endObject();
        }

        void writeToasts(IFormatWriter& out) const
        {
            out.beginObject("toasts");
            feedback->writeLog(out);
            out.endObject();
        }

        void registerCommands()
        {
            control->registerQuery("statusBar", OWNER,
                                   "the status bar's contents (play state and tint class, scene, selection, jobs, "
                                   "warning and error counts, frame time and what the frame item shows under the "
                                   "telemetry density, the QUIESCENT lamp, the upload rate, fps with its fpsText and "
                                   "fpsShown and, in full, entities "
                                   "{shown,count,text} and gpu {shown,ms,text} (shown is false for an item a narrow "
                                   "bar sheds); the theme, data colour, "
                                   "agentLayer and telemetry settings its toggles show; the attached agent client "
                                   "with the commands per minute clients reported, and which end groups fit the bar; "
                                   "the build id) and the last 50 toasts",
                                   {},
                                   [this](const ControlArgs&, IFormatWriter& out)
                                   {
                                       refresh(0.0, false);
                                       writeBar(out);
                                       writeToasts(out);
                                       return std::string{};
                                   });
        }
    };

    EditorStatusBar::EditorStatusBar(Engine& engine, const Widget& parent, const DockSpace& dock,
                                     const EditorModel& editorModel, EditorFeedback& feedback) :
        m_state(std::make_unique<State>(engine, dock, editorModel, feedback))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.jobsLabel = scope.createLabel("statusbar.jobs", "", fittedText(ITEM_HEIGHT, 6.0f));
        state.jobsLabel.setStyleClass(InternedString("statusbar.item"));
        state.jobsLabel.setVisible(false);
        for (State::ToastSlot& slot : state.toasts)
        {
            slot.panel = scope.createPanel("statusbar.toast", topLeft({}, Vec2{TOAST_WIDTH, TOAST_HEIGHT}));
            slot.panel.setParent(parent);
            slot.panel.setVisible(false);
            slot.text = scope.createLabel(
                "statusbar.toast.text", "",
                topLeft(Vec2{TOAST_PAD, 4.0f}, Vec2{TOAST_WIDTH - 2.0f * TOAST_PAD, TOAST_HEIGHT - 8.0f}));
            slot.text.setParent(slot.panel);
            slot.text.setInteractable(false);
            slot.text.setStyleClass(InternedString("toast.text"));
            State::ToastSlot* raw = &slot;
            // a press focuses what it hits; a toast gives the focus back, so reading one never costs an edit its place
            slot.panel.onPress(
                [&state](const u8 button, Vec2)
                {
                    if (button != 0)
                    {
                        return;
                    }
                    UiRouter& router = state.scope.router();
                    if (state.lastFocus != Entity::undefined() && state.scope.hierarchy().isElement(state.lastFocus))
                    {
                        router.requestFocus(state.lastFocus);
                    }
                    else
                    {
                        router.clearFocus();
                    }
                });
            slot.panel.onClick([&state, raw] { state.feedback->dismissToast(raw->id); });
        }
        // Esc, from any focus inside the editor (the UI would clear the focus with it): dismisses the toasts on screen;
        // with no focus the editor.dismissToast hotkey does the same
        Widget shellRoot = parent;
        shellRoot.onKeyDown(
            [&state](const u32 scancode, const u8 modifiers, const bool repeat)
            { return scancode == ESCAPE_KEY && modifiers == 0 && !repeat && state.feedback->dismissVisible() > 0; });
        state.registerCommands();
        scope.commit();

        state.refresh(0.0, false);
    }

    EditorStatusBar::~EditorStatusBar() { m_state->control->unregisterOwner(OWNER); }

    void EditorStatusBar::onFrameWindow(std::function<void(double, bool)> listener)
    {
        m_state->frameListener = std::move(listener);
    }

    bool EditorStatusBar::frameOver() const { return m_state->frameOver; }

    void EditorStatusBar::setSize(const Vec2 size) { m_state->place(size); }

    void EditorStatusBar::settle(const bool fullTelemetry, const float frameBudgetMs)
    {
        m_state->settle(fullTelemetry, frameBudgetMs);
    }

    Widget EditorStatusBar::jobs() const { return m_state->jobsLabel; }

    const std::string& EditorStatusBar::scenePath() const { return m_state->scenePath; }

    bool EditorStatusBar::sceneDirty() const { return m_state->sceneDirty; }

    void EditorStatusBar::update()
    {
        SPITE_ZONE("EditorStatusBar");
        State& state = *m_state;
        if (!state.pending.empty())
        {
            dispatch(*state.control, state.pending, "statusbar");
        }
        const double seconds = state.engine->lastFrameSeconds();
        state.feedback->advance(seconds);
        state.frameHovered = false;
        state.refresh(seconds, true);
        state.jobsLabel.setVisible(state.jobsShown > 0);
        State::showText(state.jobsLabel, state.jobsText);
        state.applyToasts();
        state.syncWindowTitle();
        state.lastFocus = state.scope.router().focused();
    }
} // namespace spite
