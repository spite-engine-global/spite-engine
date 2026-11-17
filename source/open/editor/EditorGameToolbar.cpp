#include "EditorGameToolbar.hpp"
#include "EditorContext.hpp"
#include "EditorEntityLabel.hpp"
#include "EditorFeedback.hpp"
#include "EditorPanelUi.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ecs/World.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include "renderruntime/RenderRuntime.hpp"
#include "renderruntime/RenderingComponents.hpp"
#include "ui/UiHierarchyManager.hpp"

namespace spite
{
    namespace
    {
        using namespace panelui;

        constexpr std::string_view GAME_VIEW_ID = "editor.game";
        constexpr std::string_view GAME_TOOLBAR = "game.toolbar";
        constexpr std::string_view GAME_ROOM = "game.view";
        constexpr std::string_view OWNER = "editorGameView";
        constexpr std::string_view TABLE = "editor-game.";
        constexpr u32 MAX_CAMERAS = 64; // the chooser lists this many; the rest is counted and said
        constexpr u32 REFRESH_FRAMES = 10; // a stream of changes (a game spawning entities) is read this often at most
        constexpr u32 ASPECT_MAX = 64;
        constexpr u32 SIDE_MIN = 16;
        constexpr u32 SIDE_MAX = 8192;
        // The views allocate about 18 graph textures at the target's size and an allocation failure is an assert, so a
        // command refuses what a 4K display shows at most.
        constexpr u64 PIXELS_MAX = 3840ull * 2160ull;
        constexpr float STATS_WINDOW_SECONDS = 0.5f;
        constexpr float CAMERA_WIDTH = 150.0f;
        constexpr float FIT_WIDTH = 120.0f;
        constexpr std::array<std::string_view, 9> FIT_PRESETS{"free",     "16:9",      "16:10",     "4:3",      "1:1",
                                                              "1280x720", "1920x1080", "2560x1440", "3840x2160"};

        // How the game view's target takes the room of the panel: all of it, the largest rect of one aspect in it, or
        // a fixed resolution (the picture is then fitted in the room).
        struct Fit
        {
            enum class Kind : u8
            {
                Free,
                Aspect,
                Resolution
            };
            Kind kind = Kind::Free;
            u32 a = 0;
            u32 b = 0;
            std::string text = "free";
        };

        std::optional<u32> whole(const std::string_view text)
        {
            u32 value = 0;
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{} && end == text.data() + text.size() ? std::optional(value) : std::nullopt;
        }

        // free | W:H | WxH; the refusal otherwise, naming the valid forms and ranges
        std::string parseFit(std::string text, Fit& out)
        {
            std::ranges::transform(text, text.begin(), [](const char c) { return static_cast<char>(std::tolower(c)); });
            if (text == "free")
            {
                out = Fit{};
                return {};
            }
            const sizet at = text.find_first_of(":x");
            const std::optional<u32> first = at == std::string::npos ? std::nullopt : whole(text.substr(0, at));
            const std::optional<u32> second = at == std::string::npos ? std::nullopt : whole(text.substr(at + 1));
            const bool aspect = at != std::string::npos && text[at] == ':';
            if (first && second && aspect && std::min(*first, *second) >= 1 && std::max(*first, *second) <= ASPECT_MAX)
            {
                out = Fit{Fit::Kind::Aspect, *first, *second, std::format("{}:{}", *first, *second)};
                return {};
            }
            if (first && second && !aspect && std::min(*first, *second) >= SIDE_MIN &&
                std::max(*first, *second) <= SIDE_MAX && u64{*first} * *second <= PIXELS_MAX)
            {
                out = Fit{Fit::Kind::Resolution, *first, *second, std::format("{}x{}", *first, *second)};
                return {};
            }
            return std::format("gameViewFit takes free, an aspect W:H (each side 1..{}) or a resolution WxH (each side "
                               "{}..{} pixels, at most {} pixels in all, the size of 3840x2160), such as 16:9 or "
                               "1920x1080; got '{}'",
                               ASPECT_MAX, SIDE_MIN, SIDE_MAX, PIXELS_MAX, text);
        }

        struct CameraRow
        {
            u64 id = 0;
            std::string name;
        };

        // The camera of the game view: bound and alive, none, or removed (bound to an entity that is gone or no longer
        // a camera, which the view draws as its fallback camera). The name is of the last camera seen alive, a view
        // of State::boundName that stays valid until the next cameraState().
        struct CameraState
        {
            Entity camera = Entity::undefined();
            bool alive = false;
            bool removed = false;
            std::string_view name;
        };

        // the stats the overlay shows: the mean frame time of its last window, the drawn world's entities, the target
        struct StatsSample
        {
            double ms = 0.0;
            std::string extent;
            std::int64_t entities = 0;
            bool valid = false;

            [[nodiscard]] std::int64_t fps() const { return ms > 0.0 ? std::lround(1000.0 / ms) : 0; }
        };

        // the cameras of the world a game view draws, as the chooser lists them
        struct CameraList
        {
            std::vector<CameraRow> rows;
            u64 total = 0;
            const World* world = nullptr;
        };

        UiRectDesc textLineDesc()
        {
            UiRectDesc desc = barDesc(Vec2{});
            desc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
            return desc;
        }
    } // namespace

    struct EditorGameToolbar::State
    {
        State(Engine& engine, EditorFeedback& feedback, const u32 view, const Widget& host, const Widget& image) :
            engine(&engine), control(&engine.control()), feedback(&feedback), view(view), host(host), image(image),
            scope(editorUi(engine).ui()), density(scope),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        EditorFeedback* feedback;
        u32 view;
        Widget host;
        Widget image;
        UiScope scope;
        DensityFollower density;
        const Localization* localization; // not owned: the engine's service
        Panel root;
        Dropdown cameraChoice;
        Dropdown fitChoice;
        Checkbox statsBox;
        Panel statsPanel;
        Panel statsHolder; // the picture's width less the offsets: the overlay shrinks into it
        Panel inputHint;
        Label statsFrame;
        Label statsScene;
        std::unique_ptr<UiDocumentView> docView;
        std::vector<std::function<void()>> pending;

        Fit fit;
        u64 fitRevision = 0;
        u64 shownFit = ~0ull;
        bool stats = false;
        u64 shownStats = ~0ull;
        float statsSeconds = 0.0f;
        u32 statsFrames = 0;
        StatsSample sample;

        // the cameras of the drawn world as the chooser lists them, and the counters they were read at
        CameraList cameras;
        u64 seenState = ~0ull;
        u32 seenEntities = ~0u;
        u32 sinceRead = REFRESH_FRAMES; // shown frames since the cameras were read
        u64 reads = 0; // the shown panel's reads of the cameras
        u64 seenCamera = ~0ull;
        u64 seenLocale = ~0ull;
        bool listDirty = true;

        // the camera the view was last seen drawing through, for the note of its removal
        Entity bound = Entity::undefined();
        std::string boundName;
        const World* boundWorld = nullptr;

        i32 inputShown = -1;
        std::string noteShown;
        u64 seenNote = ~0ull; // the camera key and locale the note was built at
        u64 seenNoteLocale = ~0ull;

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

        [[nodiscard]] bool isShown() const { return scope.hierarchy().isVisibleOf(image.entity()); }
        [[nodiscard]] World& world() const { return engine->gameWorld(); }
        [[nodiscard]] Entity viewCamera() const { return engine->render().view(view).camera; }

        [[nodiscard]] bool isCamera(const Entity camera) const
        {
            return camera.isValid() && world().isEntityAlive(camera) && world().hasComponent<CameraComponent>(camera);
        }

        // a read of the world that writes nothing of the panel's: the query reads through it too
        [[nodiscard]] CameraList readCameras() const
        {
            CameraList list;
            list.world = &world();
            const Json found = request(*control,
                                       Json{{"query", "find"},
                                            {"args",
                                             {{"with", "CameraComponent"},
                                              {"world", std::string(engine->gameWorldName())},
                                              {"limit", MAX_CAMERAS}}}});
            const Json result = found.contains("result") ? found["result"] : Json::object();
            list.total = result.value("total", u64{0});
            std::string scratch;
            for (const Json& row : result.value("entities", Json::array()))
            {
                const u64 id = row.value("id", u64{0});
                std::string name = row.value("name", std::string{});
                list.rows.push_back({id, name.empty() ? std::string(entityLabel(scratch, world(), Entity(id))) : name});
            }
            for (CameraRow& row : list.rows)
            {
                if (std::ranges::count(list.rows, row.name, &CameraRow::name) > 1)
                {
                    row.name += std::format(" #{}", Entity(row.id).index());
                }
            }
            return list;
        }

        CameraState cameraState()
        {
            CameraState state;
            state.camera = viewCamera();
            state.alive = isCamera(state.camera);
            if (const World* now = &world(); boundWorld != now)
            {
                boundWorld = now;
                bound = Entity::undefined();
                boundName.clear();
            }
            if (state.alive)
            {
                if (bound != state.camera)
                {
                    bound = state.camera;
                    std::string scratch;
                    boundName = std::string(entityLabel(scratch, world(), state.camera));
                }
                state.name = boundName;
            }
            else
            {
                state.removed = state.camera.isValid();
                state.name = state.removed && bound == state.camera ? std::string_view(boundName) : std::string_view{};
            }
            return state;
        }

        [[nodiscard]] std::string removedNote(const CameraState& camera) const
        {
            if (!camera.removed)
            {
                return {};
            }
            const std::string name =
                camera.name.empty() ? std::format("#{}", camera.camera.index()) : std::string(camera.name);
            const MessageArg args[]{{"name", std::string_view(name)}};
            return message("removed", args);
        }

        void sendBind(const u64 id)
        {
            pending.push_back(
                [this, id]
                {
                    const Json reply = request(
                        *control,
                        Json{{"command", "bindCamera"},
                             {"args", {{"view", view}, {"id", id}, {"world", std::string(engine->gameWorldName())}}}});
                    if (!reply.value("ok", false))
                    {
                        const std::string error = reply.value("error", std::string());
                        const MessageArg args[]{{"error", std::string_view(error)}};
                        feedback->postToastKey(ToastLevel::Warn, std::string(TABLE) + "bindFailed", args);
                    }
                });
        }

        void sendCommand(Json line)
        {
            pending.push_back([this, line = std::move(line)] { (void)request(*control, line); });
        }

        [[nodiscard]] StatsSample sampleAt(const double ms) const
        {
            const RenderView& drawn = engine->render().view(view);
            return {ms, std::format("{}x{}", drawn.extent.width, drawn.extent.height),
                    static_cast<std::int64_t>(world().activeEntityCount()), true};
        }

        // the overlay's two lines as one text: the shown sample, or a fresh one when none was taken (the sample is
        // not stored: a query writes nothing of the panel's)
        [[nodiscard]] std::string statsLine() const
        {
            const StatsSample fresh = sample.valid ? StatsSample{} : sampleAt(engine->lastFrameSeconds() * 1000.0);
            const StatsSample& shown = sample.valid ? sample : fresh;
            const std::string ms = std::format("{:.2f}", shown.ms);
            const MessageArg args[]{{"ms", std::string_view(ms)},
                                    {"fps", shown.fps()},
                                    {"entities", shown.entities},
                                    {"extent", std::string_view(shown.extent)}};
            return message("stats.frame", args) + "  " + message("stats.scene", args);
        }

        // The overlay's reading: the mean frame time of the last half second, written when the window closes.
        void followStats(const float seconds)
        {
            statsSeconds += seconds;
            ++statsFrames;
            if (statsSeconds < STATS_WINDOW_SECONDS && sample.valid)
            {
                return;
            }
            sample = sampleAt(statsSeconds * 1000.0 / static_cast<double>(statsFrames));
            statsSeconds = 0.0f;
            statsFrames = 0;
            statsFrame.setLocalizedArgText("ms", std::format("{:.2f}", sample.ms));
            statsFrame.setLocalizedArg("fps", sample.fps());
            statsScene.setLocalizedArg("entities", sample.entities);
            statsScene.setLocalizedArgText("extent", sample.extent);
        }

        // The overlay is as wide as its lines, never wider than the picture: its lines clip with an ellipsis
        void fitStats()
        {
            const float width = std::max(0.0f, image.size().x - 2.0f * density.density().xs);
            if (statsHolder.size().x != width)
            {
                statsHolder.setSize(Vec2{width, 0.0f});
            }
        }

        void showStats()
        {
            shownStats = stats;
            statsBox.setChecked(stats);
            statsPanel.setVisible(stats);
            fitStats();
            sample.valid = false;
            statsSeconds = 0.0f;
            statsFrames = 0;
        }

        void showFit()
        {
            std::vector<UiChoiceItem> items;
            for (const std::string_view preset : FIT_PRESETS)
            {
                items.push_back(preset == "free"
                                    ? UiChoiceItem{.id = InternedString(preset),
                                                   .key = InternedString(std::string(TABLE) + "fit.free")}
                                    : UiChoiceItem{.id = InternedString(preset), .label = std::string(preset)});
            }
            const auto at = std::ranges::find(FIT_PRESETS, std::string_view(fit.text));
            if (at == FIT_PRESETS.end())
            {
                items.push_back({.id = InternedString(fit.text), .label = fit.text});
            }
            fitChoice.setItems(items);
            fitChoice.setSelected(at == FIT_PRESETS.end() ? static_cast<i32>(FIT_PRESETS.size())
                                                          : static_cast<i32>(at - FIT_PRESETS.begin()));
            shownFit = fitRevision;
        }

        void showCameras(const CameraState& camera)
        {
            std::vector<UiChoiceItem> items;
            items.push_back({.id = InternedString("none"), .key = InternedString(std::string(TABLE) + "camera.none")});
            for (const CameraRow& row : cameras.rows)
            {
                items.push_back({.id = InternedString(std::to_string(row.id)), .label = row.name});
            }
            if (cameras.total > cameras.rows.size())
            {
                const MessageArg args[]{{"shown", static_cast<std::int64_t>(cameras.rows.size())},
                                        {"total", static_cast<std::int64_t>(cameras.total)}};
                items.push_back({.label = message("camera.truncated", args), .enabled = false});
            }
            cameraChoice.setItems(items);
            const auto at = std::ranges::find(cameras.rows, camera.camera.id(), &CameraRow::id);
            cameraChoice.setSelected(!camera.alive                  ? 0
                                         : at == cameras.rows.end() ? -1
                                                                    : static_cast<i32>(at - cameras.rows.begin()) + 1);
            listDirty = false;
        }

        // Keep the release hint visible while the game owns input.
        void showInput()
        {
            const bool on = engine->gameInputFocused();
            if (inputShown == static_cast<i32>(on))
            {
                return;
            }
            inputShown = on;
            inputHint.setVisible(on);
        }

        // A removed camera is announced once as a toast; the note is rebuilt (a localization resolve and a format)
        // only when the camera or the locale changed.
        void showNote(const CameraState& camera, const u64 cameraKey, const u64 locale)
        {
            const u64 key = cameraKey * 2 + (camera.name.empty() ? 0 : 1);
            if (key == seenNote && locale == seenNoteLocale)
            {
                return;
            }
            seenNote = key;
            seenNoteLocale = locale;
            if (std::string text = removedNote(camera); text != noteShown)
            {
                if (!text.empty())
                {
                    feedback->postToast(ToastLevel::Warn, text);
                }
                noteShown = std::move(text);
            }
        }

        void registerEntries()
        {
            control->registerViewCommand(
                "gameViewFit", OWNER,
                "keeps the Game view's target at one aspect, letterboxed in the panel (W:H, each side 1..64), at a "
                "fixed resolution (WxH, each side 16..8192 pixels and at most 8294400 pixels in all, the size of "
                "3840x2160: the views allocate their textures at it), or fills the panel (free); acts while the panel "
                "is hidden",
                {requiredArg("fit", ArgKind::String, "free, W:H such as 16:9, or WxH such as 1920x1080")},
                [this](const ControlArgs& args, IFormatWriter& out)
                {
                    Fit next;
                    if (std::string error = parseFit(args.string("fit").value_or(std::string{}), next); !error.empty())
                    {
                        return error;
                    }
                    if (next.text != fit.text)
                    {
                        fit = next;
                        ++fitRevision;
                    }
                    out.writeString("fit", fit.text);
                    return std::string{};
                });
            control->registerViewCommand("gameViewStats", OWNER,
                                         "shows or hides the stats over the Game view: the frame time, the entities of "
                                         "the drawn world and the target's size; acts while the panel is hidden",
                                         {requiredArg("on", ArgKind::Boolean, "true shows them")},
                                         [this](const ControlArgs& args, IFormatWriter& out)
                                         {
                                             stats = args.boolean("on").value_or(false);
                                             out.writeBool("on", stats);
                                             return std::string{};
                                         });
            control->registerQuery(
                "gamePanel", OWNER,
                "the Game panel's toolbar: the camera the game view draws through (id, name, whether it was removed "
                "and the note), the cameras of the drawn world the chooser lists, the aspect or resolution fit with "
                "the target extent and drawn rect it gives, the stats box and its text, and the input state with how "
                "to release it; answered from the engine and reads only, current while the panel is hidden except "
                "width, height and windowRect, which are the view's and follow a fit when the panel is next shown",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    const CameraState camera = cameraState();
                    const CameraList list = readCameras();
                    const RenderView& drawn = engine->render().view(view);
                    const bool focused = engine->gameInputFocused();
                    out.writeU32("view", view);
                    out.writeString("world", engine->gameWorldName());
                    out.writeBool("shown", isShown());
                    out.writeU64("camera", camera.alive ? camera.camera.id() : 0);
                    out.writeString("cameraName", camera.name);
                    out.writeBool("cameraRemoved", camera.removed);
                    out.writeString("cameraNote", removedNote(camera));
                    out.writeU64("camerasTotal", list.total);
                    out.writeBool("camerasTruncated", list.total > list.rows.size());
                    out.beginArray("cameras", static_cast<u32>(list.rows.size()));
                    for (const CameraRow& row : list.rows)
                    {
                        out.beginObject("");
                        out.writeU64("id", row.id);
                        out.writeString("name", row.name);
                        out.endObject();
                    }
                    out.endArray();
                    out.writeString("fit", fit.text);
                    out.writeString("fitKind",
                                    fit.kind == Fit::Kind::Free         ? "free"
                                        : fit.kind == Fit::Kind::Aspect ? "aspect"
                                                                        : "resolution");
                    out.writeU32("width", drawn.extent.width);
                    out.writeU32("height", drawn.extent.height);
                    out.beginObject("windowRect");
                    out.writeI32("x", drawn.windowRect.offset.x);
                    out.writeI32("y", drawn.windowRect.offset.y);
                    out.writeU32("w", drawn.windowRect.extent.width);
                    out.writeU32("h", drawn.windowRect.extent.height);
                    out.endObject();
                    out.writeBool("stats", stats);
                    out.writeString("statsText", stats ? statsLine() : std::string{});
                    out.writeU64("reads", reads);
                    out.writeBool("inputFocused", focused);
                    // the status bar's input item says who has the input; its tooltip is the hint
                    std::string scratch;
                    out.writeString("inputHint",
                                    localization ? localization->resolve(
                                                       InternedString(focused ? "editor-statusbar.tip.input.game"
                                                                              : "editor-statusbar.tip.input.editor"),
                                                       scratch)
                                                 : std::string_view{});
                    return std::string{};
                });
        }
    };

    EditorGameToolbar::EditorGameToolbar(Engine& engine, EditorFeedback& feedback, const u32 gameView,
                                         const Widget& host, const Widget& image) :
        m_state(std::make_unique<State>(engine, feedback, gameView, host, image))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.root = scope.createPanel("editor.game.panel", columnDesc());
        state.root.setInteractable(false); // what the toolbar does not cover is the view's: the game's presses

        const auto named = [&](Widget widget, const char* key)
        {
            widget.setAccessibleNameKey(InternedString(std::string(TABLE) + key));
            widget.setTooltip(InternedString(std::string(TABLE) + key + ".tip"));
        };
        UiDropdownDesc cameraDesc;
        cameraDesc.rect = barDesc(Vec2{CAMERA_WIDTH, 0.0f});
        state.cameraChoice = state.density.track(scope.createDropdown("game.camera", cameraDesc), HEIGHT_CONTROL);
        named(state.cameraChoice, "camera");
        state.cameraChoice.onChange(
            [&state](const u32 index)
            {
                if (index == 0)
                {
                    state.sendBind(0);
                }
                else if (index <= state.cameras.rows.size())
                {
                    state.sendBind(state.cameras.rows[index - 1].id);
                }
            });
        UiDropdownDesc fitDesc;
        fitDesc.rect = barDesc(Vec2{FIT_WIDTH, 0.0f});
        state.fitChoice = state.density.track(scope.createDropdown("game.fit", fitDesc), HEIGHT_CONTROL);
        named(state.fitChoice, "fit");
        state.fitChoice.onChange(
            [&state](const u32 index)
            {
                const std::string text = index < FIT_PRESETS.size() ? std::string(FIT_PRESETS[index]) : state.fit.text;
                state.sendCommand(Json{{"command", "gameViewFit"}, {"args", {{"fit", text}}}});
            });
        state.statsBox = scope.createCheckbox("game.stats", "", barDesc(Vec2{}), false);
        state.statsBox.label().setLocalizedText(InternedString(std::string(TABLE) + "stats"));
        state.statsBox.setTooltip(InternedString(std::string(TABLE) + "stats.tip"));
        state.statsBox.onChange(
            [&state](const bool checked)
            { state.sendCommand(Json{{"command", "gameViewStats"}, {"args", {{"on", checked}}}}); });

        const Density& spaces = state.density.density();
        UiRectDesc statsDesc = topLeft({}, {});
        statsDesc.layout = stack(UiLayoutDirection::Column, 0.0f, Vec4{spaces.sm, spaces.xxs, spaces.sm, spaces.xxs},
                                 UiLayoutAlign::Stretch);
        statsDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
        state.statsPanel = scope.createPanel("game.stats.overlay", statsDesc);
        bindPadding(state.statsPanel, SPACE_SM, SPACE_XXS, SPACE_SM, SPACE_XXS);
        state.statsPanel.setStyleClass(InternedString("toast.info"));
        state.statsPanel.setInteractable(false);
        state.statsPanel.setVisible(false);
        UiRectDesc holderDesc = topLeft({}, {});
        holderDesc.layout = stack(UiLayoutDirection::Row, 0.0f);
        holderDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        state.statsHolder = scope.createPanel("game.stats.holder", holderDesc);
        state.statsHolder.setInteractable(false);
        state.statsHolder.setParent(image);
        state.statsPanel.setParent(state.statsHolder);
        UiRectDesc hintDesc = topLeft({}, {});
        hintDesc.layout = stack(UiLayoutDirection::Row, 0.0f, Vec4{spaces.sm, spaces.xxs, spaces.sm, spaces.xxs});
        hintDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
        state.inputHint = scope.createPanel("game.input.hint", hintDesc);

        state.inputHint.setStyleClass(InternedString("toast.info"));
        state.inputHint.setInteractable(false);
        state.inputHint.setVisible(false);
        UiRectDesc hintTextDesc = topLeft();
        hintTextDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
        Label hintText = scope.createLabel("game.input.hint.text", "", hintTextDesc);
        hintText.setParent(state.inputHint);
        hintText.setLocalizedText(InternedString(std::string(TABLE) + "input.on.hint"));
        hintText.setStyleClass(InternedString("toast.text"));
        hintText.setInteractable(false);
        UiRectDesc lineDesc = topLeft();
        lineDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
        for (const auto& [label, key] :
             {std::pair{&state.statsFrame, "stats.frame"}, {&state.statsScene, "stats.scene"}})
        {
            *label = scope.createLabel(InternedString(std::string("game.") + key), "", lineDesc);
            label->setStyleClass(InternedString("list.row.text.ellipsis"));
            label->setLocalizedText(InternedString(std::string(TABLE) + key));
            label->setInteractable(false);
            label->setParent(state.statsPanel);
        }
        state.density.apply([&state](const Density& density)
                            { state.statsHolder.setOffset(Vec2{density.xs, density.xs}); });
        state.registerEntries();
        scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(GAME_TOOLBAR),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.cameraChoice);
                          putIn(state.scope, slot, state.fitChoice);
                          putIn(state.scope, slot, state.statsBox);
                          putIn(state.scope, slot, state.inputHint);
                      });
        contract.slot(InternedString(GAME_ROOM),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.host);
                          state.host.setLayoutItem(fillItem());
                      });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                Widget(root).setInteractable(false);
                toolbarPanel(viewScope, GAME_TOOLBAR, root, SPACE_XS, UiLayoutAlign::Center);
                fillSlotPanel(viewScope, GAME_ROOM, root).setInteractable(false);
            });
        state.docView = openView(engine, GAME_VIEW_ID, "assets/ui/editor/game.json", std::move(contract), state.root);
    }

    EditorGameToolbar::~EditorGameToolbar() { m_state->control->unregisterOwner(OWNER); }

    Widget EditorGameToolbar::content() const { return m_state->root; }

    Extent2D EditorGameToolbar::targetIn(const Vec2 room) const
    {
        const Fit& fit = m_state->fit;
        const auto pixels = [](const float value)
        { return std::max(1u, static_cast<u32>(std::max(0l, std::lround(value)))); };
        if (fit.kind == Fit::Kind::Resolution)
        {
            return {fit.a, fit.b};
        }
        if (fit.kind == Fit::Kind::Aspect)
        {
            const float ratio = static_cast<float>(fit.a) / static_cast<float>(fit.b);
            const float width = std::min(room.x, room.y * ratio);
            return {pixels(width), pixels(width / ratio)};
        }
        return {pixels(room.x), pixels(room.y)};
    }

    void EditorGameToolbar::update()
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
            state.listDirty = true; // a refused bind returns the chooser to the camera the view draws through
        }
        if (!state.isShown())
        {
            return;
        }
        const CameraState camera = state.cameraState();
        const u64 generation = state.control->stateGeneration();
        const u32 entities = state.world().activeEntityCount();
        ++state.sinceRead;
        if (&state.world() != state.cameras.world ||
            ((generation != state.seenState || entities != state.seenEntities) && state.sinceRead >= REFRESH_FRAMES))
        {
            state.seenState = generation;
            state.seenEntities = entities;
            state.sinceRead = 0;
            ++state.reads;
            state.cameras = state.readCameras();
            state.listDirty = true;
        }
        const u64 locale = state.localization ? state.localization->revision() : 0;
        const u64 cameraKey = camera.camera.id() * 2 + (camera.alive ? 1 : 0);
        if (state.listDirty || cameraKey != state.seenCamera || locale != state.seenLocale)
        {
            state.seenCamera = cameraKey;
            state.seenLocale = locale;
            state.showCameras(camera);
        }
        if (state.shownFit != state.fitRevision)
        {
            state.showFit();
        }
        if (state.shownStats != static_cast<u64>(state.stats))
        {
            state.showStats();
        }
        if (state.stats)
        {
            state.fitStats();
            state.followStats(static_cast<float>(state.engine->lastFrameSeconds()));
        }
        state.showInput();
        state.showNote(camera, cameraKey, locale);
    }
} // namespace spite
