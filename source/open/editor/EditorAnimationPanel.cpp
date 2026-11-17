#include "EditorAnimationPanel.hpp"
#include "EditorAnimation.hpp"
#include "EditorContext.hpp"
#include "EditorPanelList.hpp"
#include "EditorPanelRegistry.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/ControlServer.hpp"
#include "editor/EditorModel.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "ui/UiDocument.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        using namespace panelui;

        constexpr std::string_view VIEW = "editor.animation";
        constexpr std::string_view TOOLBAR = "animation.toolbar";
        constexpr std::string_view BAR = "animation.bar";
        constexpr std::string_view SCRUB = "animation.scrub";
        constexpr std::string_view ROWS = "animation.rows";
        constexpr std::string_view STATUS = "animation.status";
        constexpr std::string_view TABLE = "editor-animation.";
        constexpr double SPEED_LIMIT = 100.0;

        // what the clip table shows of a clip
        struct ClipCells
        {
            std::string name;
            std::string duration;
            bool operator==(const ClipCells&) const = default;
            SPITE_OBSERVED_SCHEMA(ClipCells, name, duration)
        };

        UiTableColumn column(const std::string_view field, const float width)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString(std::string(TABLE) + "column." + std::string(field)),
                    .sortable = false};
        }

        // a control as wide as its text plus padding, a control high, centred in its line
        UiRectDesc fitted(const float height)
        {
            UiRectDesc desc = barDesc(Vec2{0.0f, height});
            desc.layoutItem->alignSelf = UiLayoutAlign::Center;
            desc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = false};
            return desc;
        }
    } // namespace

    struct EditorAnimationPanel::State
    {
        State(Engine& engine, const EditorModel& model) :
            engine(&engine), control(&engine.control()), animation(&EditorModelAccess::animation(model)),
            scope(editorUi(engine).ui()), density(scope),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        EditorAnimation* animation; // the model's
        UiScope scope;
        DensityFollower density;
        const Localization* localization;
        Panel root;
        Button play;
        Button pause;
        Button stop;
        Label speedLabel;
        NumberField speed;
        Button preview;
        Button bones;
        Slider scrub;
        Label time;
        Label status;
        Label empty;
        std::unique_ptr<RowTable<ClipCells>> rows;
        std::unique_ptr<UiDocumentView> view;

        std::vector<std::string> pending; // lines to send between frames
        std::optional<double> scrubbed; // the slider's last value this frame
        std::optional<double> typedSpeed;
        u64 seenRevision = ~0ull;
        u64 seenLocale = ~0ull;
        bool wasShown = false;
        // what the widgets hold, so a write happens when it moves
        std::optional<bool> shownCanPlay, shownSpeedEnabled, shownCanPause, shownCanStop, shownCanSeek,
            shownPreviewable;
        std::string shownTime;
        std::string shownStatus;
        std::string shownEmpty;
        u64 builds = 0;

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

        static std::string seconds(const f32 value) { return std::format("{:.2f}", value); }

        void send(std::string line)
        {
            if (!line.empty())
            {
                pending.push_back(std::move(line));
            }
        }

        void enable(Widget widget, std::optional<bool>& held, const bool on)
        {
            if (held != on)
            {
                held = on;
                widget.setInteractable(on);
            }
        }

        static void setLabel(Label& label, std::string& held, const std::string& text)
        {
            if (held != text)
            {
                held = text;
                label.setText(text);
            }
        }

        // the note over the clip list: why there is none to show
        [[nodiscard]] std::string emptyText(const AnimationView& shown) const
        {
            if (!shown.hasAnimator())
            {
                return message(shown.selected ? "empty.notRigged" : "empty.none");
            }
            if (shown.clips.empty())
            {
                const MessageArg args[]{{"skeleton", std::string_view(shown.skeleton)}};
                return message("empty.noClips", args);
            }
            return {};
        }

        [[nodiscard]] std::string statusText(const AnimationView& shown) const
        {
            if (!shown.hasAnimator())
            {
                return {};
            }
            const std::string mode = message(!shown.previewable    ? "mode.play"
                                                 : shown.advancing ? "mode.preview"
                                                                   : "mode.scrub");
            const std::string binding = message("binding." + shown.binding);
            const MessageArg args[]{{"name", std::string_view(shown.name)},
                                    {"binding", std::string_view(binding)},
                                    {"joints", static_cast<std::int64_t>(shown.joints)},
                                    {"mode", std::string_view(mode)}};
            return message("status", args);
        }

        void rebuild()
        {
            SPITE_ZONE("EditorAnimation.rebuild");
            ++builds;
            const AnimationView& shown = animation->view();
            enable(play, shownCanPlay, shown.canPlay());
            enable(pause, shownCanPause, shown.canPause());
            enable(stop, shownCanStop, shown.canStop());
            enable(speed, shownSpeedEnabled, shown.canPlay());
            showSelected(play, shown.playing);
            showSelected(preview, shown.advancing);
            showSelected(bones, shown.bones);
            if (shownPreviewable != shown.previewable)
            {
                shownPreviewable = shown.previewable;
                preview.setVisible(shown.previewable);
            }
            enable(scrub, shownCanSeek, shown.canSeek());
            if (std::abs(scrub.value() - shown.normalized()) > 1e-6)
            {
                scrub.setValue(shown.normalized());
            }
            if (scope.router().focused() != speed.entity() && speed.number() != shown.speed)
            {
                speed.setNumber(shown.speed);
            }
            const MessageArg timeArgs[]{{"time", seconds(shown.time)}, {"duration", seconds(shown.duration)}};
            setLabel(time, shownTime, shown.canSeek() ? message("time", timeArgs) : std::string{});

            std::vector<ClipCells> cells;
            cells.reserve(shown.clips.size());
            for (const AnimationClipRow& clip : shown.clips)
            {
                const MessageArg args[]{{"seconds", seconds(clip.duration)}};
                cells.push_back({clip.name, message("duration", args)});
            }
            rows->set(std::move(cells));
            syncSelection(shown);
            const std::string note = emptyText(shown);
            rows->setHeaderOnly(!note.empty());
            empty.setVisible(!note.empty());
            setLabel(empty, shownEmpty, note);
            setLabel(status, shownStatus, statusText(shown));
            seenLocale = localization ? localization->revision() : 0;
        }

        // the row of the player's clip is the selected one
        void syncSelection(const AnimationView& shown)
        {
            const auto it = std::ranges::find(shown.clips, shown.clip, &AnimationClipRow::name);
            std::vector<Entity> want;
            if (it != shown.clips.end())
            {
                want.emplace_back(static_cast<u32>(it - shown.clips.begin()) + 1u, 1u);
            }
            const std::span<const Entity> held = rows->table().selection();
            if (!std::ranges::equal(held, want))
            {
                rows->table().setSelection(want);
            }
        }

        // a pick by the user: play that clip from its start
        void picked(const std::span<const Entity> keys)
        {
            const AnimationView& shown = animation->view();
            if (keys.empty())
            {
                return;
            }
            const u32 row = keys.back().index() - 1;
            if (row < shown.clips.size() && shown.clips[row].name != shown.clip)
            {
                send(animation->playLine(shown.clips[row].name));
            }
        }

        void flush()
        {
            if (scrubbed)
            {
                send(animation->seekLine(static_cast<f32>(*std::exchange(scrubbed, std::nullopt))));
            }
            if (typedSpeed)
            {
                send(animation->speedLine(static_cast<f32>(*std::exchange(typedSpeed, std::nullopt))));
            }
            dispatch(*control, pending, "EditorAnimationPanel");
        }

        Button makeButton(const char* id, const char* key, std::function<void()> click)
        {
            Button button =
                density.track(scope.createButton(id, "", fitted(density.density().control)), HEIGHT_CONTROL);
            bindFitPadding(button, SPACE_MD, SPACE_NONE);
            button.setLocalizedText(InternedString(std::string(TABLE) + key));
            button.setTooltip(InternedString(std::string(TABLE) + key + ".tip"));
            button.onClick(std::move(click));
            return button;
        }
    };

    EditorAnimationPanel::EditorAnimationPanel(Engine& engine, const EditorModel& model) :
        m_state(std::make_unique<State>(engine, model))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.root = scope.createPanel("editor.panel", columnDesc());

        state.play = state.makeButton("animation.play", "play", [&state] { state.send(state.animation->playLine()); });
        state.pause =
            state.makeButton("animation.pause", "pause", [&state] { state.send(state.animation->pauseLine()); });
        state.stop = state.makeButton("animation.stop", "stop", [&state] { state.send(state.animation->stopLine()); });

        state.speedLabel =
            state.density.track(scope.createLabel("animation.speed.label", "", fitted(0.0f)), HEIGHT_CONTROL);
        state.speedLabel.setLocalizedText(InternedString(std::string(TABLE) + "speed"));
        state.speedLabel.setInteractable(false);
        UiNumberDesc number{.rect = fitted(state.density.density().control),
                            .min = -SPEED_LIMIT,
                            .max = SPEED_LIMIT,
                            .step = 0.05,
                            .decimals = 2};
        number.rect.layoutItem->minSize.x = 2.0f * state.density.density().row;
        state.speed = scope.createNumberField("animation.speed", number);
        bindFitPadding(state.speed, SPACE_XS, SPACE_NONE);
        state.density.track(state.speed, HEIGHT_CONTROL);
        state.speed.setAccessibleNameKey(InternedString(std::string(TABLE) + "speed.tip"));
        state.speed.setTooltip(InternedString(std::string(TABLE) + "speed.tip"));
        state.speed.onNumber([&state](const double value) { state.typedSpeed = value; });
        // Enter commits and lets go, so the field takes the value the command made
        state.speed.onSubmit([&state](std::string_view) { state.scope.router().clearFocus(); });

        state.preview = state.makeButton("animation.preview", "preview",
                                         [&state]
                                         {
                                             const AnimationView& shown = state.animation->view();
                                             state.send(state.animation->previewLine(!shown.advancing));
                                         });
        state.bones = state.makeButton("animation.bones", "bones", [&state]
                                       { state.send(EditorAnimation::bonesLine(!state.animation->bonesShown())); });

        UiSliderDesc slider;
        slider.rect = barDesc(Vec2{0.0f, state.density.density().control});
        slider.rect.layoutItem = fillItem();
        slider.rect.layoutItem->alignSelf = UiLayoutAlign::Center;
        slider.min = 0.0;
        slider.max = 1.0;
        state.scrub = state.density.track(scope.createSlider("animation.scrub.slider", slider), HEIGHT_CONTROL);
        state.scrub.setAccessibleNameKey(InternedString(std::string(TABLE) + "scrub"));
        state.scrub.setTooltip(InternedString(std::string(TABLE) + "scrub.tip"));
        state.scrub.onChange([&state](const double value) { state.scrubbed = value; });
        state.time = state.density.track(scope.createLabel("animation.time", "", fitted(0.0f)), HEIGHT_CONTROL);
        bindFitPadding(state.time, SPACE_XS, SPACE_NONE);
        state.time.setInteractable(false);

        state.rows = std::make_unique<RowTable<ClipCells>>(
            scope, "animation.table", InternedString(std::string(TABLE) + "table"),
            std::vector{column("name", 280.0f), column("duration", 96.0f)});
        state.rows->table().onSelect([&state](const std::span<const Entity> keys) { state.picked(keys); });

        UiRectDesc statusDesc = barDesc(Vec2{});
        statusDesc.layoutItem = fillItem();
        state.status = scope.createLabel("animation.status.text", "", statusDesc);
        state.status.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SIZE_ROW));
        state.status.setStyleClass(InternedString("list.filter.count"));
        state.empty = emptyNote(scope, "animation.empty");
        scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(BAR),
                      [&state](const Widget& slot)
                      {
                          for (Widget item : {static_cast<Widget>(state.play), static_cast<Widget>(state.pause),
                                              static_cast<Widget>(state.stop), static_cast<Widget>(state.speedLabel),
                                              static_cast<Widget>(state.speed), static_cast<Widget>(state.preview),
                                              static_cast<Widget>(state.bones)})
                          {
                              putIn(state.scope, slot, item);
                          }
                          for (Button* button : {&state.play, &state.pause, &state.stop, &state.preview, &state.bones})
                          {
                              setOwnClass(*button, slotFields(state.scope, slot).childClass);
                          }
                      });
        contract.slot(InternedString(SCRUB),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.scrub);
                          putIn(state.scope, slot, state.time);
                      });
        contract.slot(InternedString(ROWS),
                      [&state](const Widget& slot)
                      {
                          state.rows->fill(state.scope, slot);
                          putIn(state.scope, slot, state.empty);
                      });
        contract.slot(InternedString(STATUS), [&state](const Widget& slot) { putIn(state.scope, slot, state.status); });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                wrapBarPanel(viewScope, TOOLBAR, BAR, root);
                toolbarPanel(viewScope, SCRUB, root, SPACE_XS, UiLayoutAlign::Center);
                fillSlotPanel(viewScope, ROWS, root);
                statusPanel(viewScope, STATUS, root);
            });
        state.view = openView(engine, VIEW, "assets/ui/editor/animation.json", std::move(contract), state.root);
    }

    EditorAnimationPanel::~EditorAnimationPanel() = default;

    Widget EditorAnimationPanel::content() const { return m_state->root; }

    void EditorAnimationPanel::update()
    {
        State& state = *m_state;
        state.flush();
        if (!shown(state.scope, state.root))
        {
            state.wasShown = false; // a hidden panel reads nothing; showing it catches up
            return;
        }
        const bool changed = !state.wasShown || state.animation->revision() != state.seenRevision ||
            (state.localization && state.localization->revision() != state.seenLocale);
        state.wasShown = true;
        if (changed)
        {
            state.seenRevision = state.animation->revision();
            state.rebuild();
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "animation",
             .category = "world",
             .order = 196,
             .dock = {.target = "project"},
             .zone = "EditorAnimation",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<EditorAnimationPanel>(c.engine, c.model); }}};
    } // namespace
} // namespace spite
