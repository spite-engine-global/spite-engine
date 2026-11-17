#include "EditorSceneToolbar.hpp"

#include <array>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "EditorActions.hpp"
#include "EditorAnimation.hpp"
#include "EditorGizmoSettings.hpp"
#include "EditorGizmo.hpp"
#include "editor/EditorModel.hpp"
#include "EditorPanelUi.hpp"
#include "EditorSceneHud.hpp"
#include "EditorSceneView.hpp"
#include "EditorSettings.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "engine/ViewModeCommands.hpp"
#include "localization/Localization.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        constexpr std::string_view VIEW = "editor.scene";
        constexpr std::string_view DOCUMENT = "assets/ui/editor/scene.json";
        constexpr std::string_view OVERLAY = "scene.overlay";
        constexpr std::string_view TOOLBAR_SLOT = "scene.toolbar";
        constexpr std::string_view TRAY_SLOT = "scene.tray";
        constexpr std::string_view HINT_SLOT = "scene.hint";
        constexpr std::string_view OWNER = "editorSceneToolbar";
        constexpr std::string_view GROUP_CLASS = "scene.bar"; // an opaque bar: the controls sit on the 3D view
        constexpr float FIELD_MIN_ROWS = 2.0f; // a number field grows with its text, never under this many row heights

        constexpr u32 FRAME_ROW =
            static_cast<u32>(std::ranges::find(SCENE_VIEW_GESTURES, "frame") - SCENE_VIEW_GESTURES.begin());

        // the overlays the Overlays tray toggles, in order: what each shows, as the sceneOverlay command names them
        // (the last is the toolbar itself)
        constexpr std::array<std::string_view, 12> OVERLAYS{"grid",      "gizmo",   "outline", "ruler",
                                                            "hud",       "bones",   "toolbar", "lights",
                                                            "colliders", "cameras", "custom",  "componentSelected"};
        enum Overlay : u32
        {
            GRID,
            GIZMO,
            OUTLINE,
            RULER,
            HUD,
            BONES,
            TOOLBAR,
            LIGHTS,
            COLLIDERS,
            CAMERAS,
            CUSTOM,
            COMPONENT_SELECTED
        };
        // the view dropdown's width, in row heights: the longest view name of every locale fits
        constexpr float VIEW_ROWS = 4.5f;
        // the shading dropdown's width, in row heights (its text is the same at every control height): the longest
        // mode name of every locale fits
        constexpr float SHADING_ROWS = 6.0f;

        // the number fields, in order: the value each edits (the first is an editor setting, the others camera fields)
        enum Field : u32
        {
            FLY_SPEED,
            FOV,
            NEAR_PLANE,
            FAR_PLANE,
            FIELD_COUNT
        };
        constexpr std::array<std::string_view, FIELD_COUNT> FIELD_NAMES{"flySpeed", "fov", "near", "far"};

        // A control that is as wide as its text plus `padding` a side, a control high, centred in its line.
        UiRectDesc fitted(const float height, const float padding)
        {
            UiRectDesc desc = panelui::barDesc(Vec2{0.0f, height});
            desc.layoutItem->alignSelf = UiLayoutAlign::Center;
            desc.contentSize =
                UiContentSizeComponent{.fitWidth = true, .fitHeight = false, .padding = Vec2{padding, 0.0f}};
            return desc;
        }
    } // namespace

    struct EditorSceneToolbar::State
    {
        State(Engine& engineRef, UiScope& scopeRef, EditorSceneView& sceneViewRef, EditorSceneHud& hudRef,
              const EditorModel& editorModelRef, EditorSettingsService& settingsRef, const EditorActions& actionsRef,
              EditorGizmo& gizmoRef) :
            engine(&engineRef), scope(&scopeRef), sceneView(&sceneViewRef), hud(&hudRef), editorModel(&editorModelRef),
            settings(&settingsRef), actions(&actionsRef), gizmo(&gizmoRef), density(scopeRef)
        {
            if (engineRef.services().contains<Localization>())
            {
                localization = &engineRef.services().get<Localization>();
            }
        }

        Engine* engine;
        UiScope* scope;
        EditorSceneView* sceneView;
        EditorSceneHud* hud;
        const EditorModel* editorModel;
        EditorSettingsService* settings;
        u32 applied = 0; // the settings listener, removed with the toolbar
        const EditorActions* actions;
        EditorGizmo* gizmo;
        const Localization* localization = nullptr;
        panelui::DensityFollower density;
        std::unique_ptr<UiDocumentView> view;
        Widget overlay;
        std::vector<Widget> items; // the toolbar slot's content, in order
        std::array<Widget, 7> gizmoControls;
        std::vector<Widget> trayItems; // the tray slot's: the overlay toggles, then the camera fields

        Widget bar; // the scene.toolbar slot
        Widget tray; // the scene.tray slot under the bar: the overlay toggles or the camera fields, while one is open
        Button frame;
        Dropdown viewChoice; // the camera's view: an editorCamera view= client
        Button camera; // opens the tray on the camera fields
        bool cameraOpen = false;
        Button collapse; // hides the toolbar (editorSettings sceneToolbar=false)
        Button expand; // all a hidden toolbar shows: brings it back
        std::array<Widget, FIELD_COUNT> fieldBoxes;
        Button help;
        Widget hint; // the scene.hint slot
        std::unique_ptr<panelui::Table> hintTable; // a line per gesture: its input and what it does
        std::string frameKey; // what Frame is bound to, shown as its row's input
        bool hintOpen = false;
        Dropdown shading; // the shading mode: a setViewMode client
        Button overlays; // opens the tray on the overlay toggles
        Button componentGizmos;
        std::array<Button, OVERLAYS.size()> overlayButtons;
        bool overlaysOpen = false;
        bool componentGizmosOpen = false;
        std::array<NumberField, FIELD_COUNT> fields;
        std::array<std::optional<double>, FIELD_COUNT> typed; // the latest committed value; one command per frame
        std::vector<std::string> pending;

        // what the controls show: empty until written (a refused edit empties it so the next refresh rewrites); not a
        // NaN, which the Release build's fast floating point does not compare reliably
        std::array<std::optional<float>, FIELD_COUNT> shown;
        std::optional<ViewModeStatus> shownShading;
        std::array<std::optional<bool>, OVERLAYS.size()> shownOverlay;
        std::optional<bool> shownOverlaysOpen;
        std::optional<bool> shownCameraOpen;
        std::optional<bool> shownComponentGizmosOpen;
        std::optional<bool> shownToolbar;
        u32 shownView = ~0u;
        u64 shownLocale = ~0ull;
        u64 shownRevision = ~0ull;
        u64 shownBindings = ~0ull;
        bool settingsMoved = true;

        Panel group(const std::string_view id)
        {
            const panelui::Density& now = density.density();
            UiRectDesc desc =
                panelui::barDesc(Vec2{},
                                 panelui::stack(UiLayoutDirection::Row, now.xs, Vec4{now.xs, now.xs, now.xs, now.xs},
                                                UiLayoutAlign::Center));
            desc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
            Panel panel = scope->createPanel(InternedString(id), desc);
            panelui::bindGap(panel, panelui::SPACE_XS);
            panelui::bindPadding(panel, panelui::SPACE_XS, panelui::SPACE_XS, panelui::SPACE_XS, panelui::SPACE_XS);
            return panel;
        }

        // A text button as wide as its label, a control high.
        Button textButton(const std::string_view id, const std::string_view textKey, const std::string_view tipKey)
        {
            Button button =
                scope->createButton(InternedString(id), "", fitted(density.density().control, density.density().md));
            panelui::bindFitPadding(button, panelui::SPACE_MD, panelui::SPACE_NONE);
            density.track(button, panelui::HEIGHT_CONTROL);
            button.setLocalizedText(InternedString(textKey));
            button.setTooltip(InternedString(tipKey));
            return button;
        }

        // A caption and its number field in a bar of their own; "<key>.tip" names and explains the field.
        void numberField(const Field index, const std::string_view id, const std::string_view key, const double min,
                         const double max, const double step, const u8 decimals)
        {
            const Panel box = group(std::string(id) + ".box");
            Label caption = scope->createLabel(InternedString(std::string(id) + ".label"), "",
                                               fitted(density.density().control, 0.0f));
            density.track(caption, panelui::HEIGHT_CONTROL);
            caption.setLocalizedText(InternedString(key));
            caption.setInteractable(false);
            caption.setParent(box);
            UiNumberDesc desc{.rect = fitted(density.density().control, density.density().xs),
                              .min = min,
                              .max = max,
                              .step = step,
                              .decimals = decimals};
            NumberField field = scope->createNumberField(InternedString(id), desc);
            panelui::bindFitPadding(field, panelui::SPACE_XS, panelui::SPACE_NONE);
            density.track(field, panelui::HEIGHT_CONTROL);
            const InternedString tip(std::string(key) + ".tip");
            field.setAccessibleNameKey(tip);
            field.setTooltip(tip);
            field.setParent(box);
            field.onNumber([this, index](const double value) { typed[index] = value; });
            fields[index] = field;
            fieldBoxes[index] = box;
            trayItems.push_back(box);
        }

        // The shading mode as a dropdown (the modes the renderer shows now are enabled): a choice is setViewMode.
        void buildShading()
        {
            UiDropdownDesc desc;
            desc.rect = panelui::barDesc(Vec2{0.0f, density.density().control});
            desc.rect.layoutItem->alignSelf = UiLayoutAlign::Center;
            shading = scope->createDropdown("scene.shading", desc);
            density.apply(
                [target = static_cast<Widget>(shading)](const panelui::Density& now) mutable
                {
                    const Vec2 size{SHADING_ROWS * now.row, now.control};
                    if (target.size() != size)
                    {
                        target.setSize(size);
                    }
                });
            shading.setAccessibleNameKey(InternedString("editor-scene.shading.name"));
            shading.onChange(
                [this](const u32 index)
                {
                    if (index < VIEW_MODES.size())
                    {
                        pending.push_back(
                            Json{{"command", "setViewMode"}, {"args", {{"mode", VIEW_MODES[index]}}}}.dump());
                    }
                });
            items.push_back(shading);
        }

        // The camera's view as a dropdown of SCENE_VIEW_MODES: a choice is editorCamera view=.
        void buildView()
        {
            UiDropdownDesc desc;
            desc.rect = panelui::barDesc(Vec2{0.0f, density.density().control});
            desc.rect.layoutItem->alignSelf = UiLayoutAlign::Center;
            viewChoice = scope->createDropdown("scene.view", desc);
            density.apply(
                [target = static_cast<Widget>(viewChoice)](const panelui::Density& now) mutable
                {
                    const Vec2 size{VIEW_ROWS * now.row, now.control};
                    if (target.size() != size)
                    {
                        target.setSize(size);
                    }
                });
            std::vector<UiChoiceItem> choices;
            for (const std::string_view mode : SCENE_VIEW_MODES)
            {
                choices.push_back(
                    {.id = InternedString(mode), .key = InternedString("editor-scene.view." + std::string(mode))});
            }
            viewChoice.setItems(choices);
            viewChoice.setAccessibleNameKey(InternedString("editor-scene.view.name"));
            viewChoice.setTooltip(InternedString("editor-scene.view.tip"));
            viewChoice.onChange(
                [this](const u32 index)
                {
                    if (index < SCENE_VIEW_MODES.size())
                    {
                        pending.push_back(
                            Json{{"command", "editorCamera"}, {"args", {{"view", SCENE_VIEW_MODES[index]}}}}.dump());
                    }
                });
            items.push_back(viewChoice);
        }

        // Overlays opens the tray on the overlay toggles (each a sceneOverlay client), Camera on the camera fields.
        void buildOverlays()
        {
            overlays = textButton("scene.overlays", "editor-scene.overlays", "editor-scene.overlays.tip");
            overlays.setAccessibleNameKey(InternedString("editor-scene.overlays"));
            overlays.onClick(
                [this]
                { pending.push_back(Json{{"command", "sceneOverlay"}, {"args", {{"open", !overlaysOpen}}}}.dump()); });
            items.push_back(overlays);
            componentGizmos =
                textButton("scene.componentGizmos", "editor-scene.componentGizmos", "editor-scene.componentGizmos.tip");
            componentGizmos.setAccessibleNameKey(InternedString("editor-scene.componentGizmos"));
            componentGizmos.onClick([this] { pending.push_back(Json{{"command", "sceneComponentGizmos"}}.dump()); });
            items.push_back(componentGizmos);
            camera = textButton("scene.camera", "editor-scene.camera", "editor-scene.camera.tip");
            camera.setAccessibleNameKey(InternedString("editor-scene.camera"));
            camera.onClick(
                [this]
                { pending.push_back(Json{{"command", "sceneCamera"}, {"args", {{"open", !cameraOpen}}}}.dump()); });
            items.push_back(camera);
            for (u32 i = 0; i < OVERLAYS.size(); ++i)
            {
                const std::string name(OVERLAYS[i]);
                const std::string key = "editor-scene.overlay." + name;
                overlayButtons[i] = textButton("scene.overlay." + name, key, key + ".tip");
                overlayButtons[i].setAccessibleNameKey(InternedString(key));
                trayItems.push_back(overlayButtons[i]);
                overlayButtons[i].onClick(
                    [this, i, name]
                    {
                        pending.push_back(
                            Json{{"command", "sceneOverlay"}, {"args", {{"name", name}, {"on", !overlayOn(i)}}}}
                                .dump());
                    });
            }
        }

        bool overlayOn(const u32 overlay) const
        {
            if (overlay == COMPONENT_SELECTED)
                return gizmo->selectedComponentGizmosEnabled();
            if (overlay >= LIGHTS)
                return gizmo->componentCategoryEnabled(static_cast<ComponentGizmoCategory>(overlay - LIGHTS));
            return overlay == GRID   ? settings->settings().gridEnabled
                : overlay == GIZMO   ? EditorModelAccess::gizmo(*editorModel).enabled
                : overlay == OUTLINE ? sceneView->outlineShown()
                : overlay == RULER   ? hud->rulerShown()
                : overlay == HUD     ? hud->hudShown()
                : overlay == BONES   ? EditorModelAccess::animation(*editorModel).bonesShown()
                                     : settings->settings().sceneToolbar;
        }

        void build()
        {
            Panel gizmoGroup = group("scene.gizmo.controls");
            for (u32 i = 0; i < gizmoControls.size(); ++i)
            {
                panelui::putIn(*scope, gizmoGroup, gizmoControls[i]);
                if (i < 5)
                    panelui::setOwnClass(gizmoControls[i], InternedString("toolbar.icon"));
            }
            items.push_back(gizmoGroup);
            frame = textButton("scene.frame", "editor-scene.frame", "editor-scene.frame.tip");
            frame.onClick(
                [this]
                {
                    pending.push_back(Json{{"command", "editorCamera"},
                                           {"args", {{"frame", EditorSceneView::frameTarget(*engine, *editorModel)}}}}
                                          .dump());
                });
            items.push_back(frame);
            buildView();
            buildShading();
            buildOverlays();
            numberField(FLY_SPEED, "scene.speed", "editor-scene.speed", 0.01, 100.0, 0.01, 2);
            numberField(FOV, "scene.fov", "editor-scene.fov", 1.0, 179.0, 0.5, 1);
            numberField(NEAR_PLANE, "scene.near", "editor-scene.near", 0.001, 1.0e6, 0.01, 3);
            numberField(FAR_PLANE, "scene.far", "editor-scene.far", 0.01, 1.0e7, 1.0, 1);
            density.apply(
                [this](const panelui::Density& now)
                {
                    UiLayoutItemComponent item = panelui::fixedItem();
                    item.minSize = Vec2{FIELD_MIN_ROWS * now.row, 0.0f};
                    item.alignSelf = UiLayoutAlign::Center;
                    for (NumberField& field : fields)
                    {
                        field.setLayoutItem(item);
                    }
                });
            help = textButton("scene.help", "editor-scene.help", "editor-scene.help.tip");
            help.setAccessibleNameKey(InternedString("editor-scene.help.name"));
            help.onClick([this] { pending.push_back(Json{{"command", "sceneHint"}}.dump()); });
            items.push_back(help);
            collapse = textButton("scene.collapse", "editor-scene.collapse", "editor-scene.collapse.tip");
            collapse.setAccessibleNameKey(InternedString("editor-scene.collapse"));
            collapse.setIcon("caret-double-left");
            collapse.onClick([this] { pending.push_back(toolbarRequest(false)); });
            items.push_back(collapse);
            expand = textButton("scene.expand", "editor-scene.expand", "editor-scene.expand.tip");
            expand.setAccessibleNameKey(InternedString("editor-scene.expand"));
            expand.setIcon("caret-double-right");
            expand.onClick([this] { pending.push_back(toolbarRequest(true)); });
            items.push_back(expand);
        }

        static std::string toolbarRequest(const bool shown)
        {
            return Json{{"command", "editorSettings"}, {"args", {{"sceneToolbar", shown}}}}.dump();
        }

        // the gesture's input (the Frame row's: its key now) and what it does, in the locale now
        std::pair<std::string, std::string> hintRow(const u32 row) const
        {
            const std::string key = "editor-scene.gesture." + std::string(SCENE_VIEW_GESTURES[row]);
            return {row == FRAME_ROW ? frameKey : resolve(key), resolve(key + ".action")};
        }

        // The hint's table, a line "input: what it does" for each gesture, goes into its slot once, as tall as its rows
        // (less where the panel has no room: it scrolls); it shows while hintOpen.
        void fillHint(const Widget& slot)
        {
            hint = slot;
            if (!hintTable)
            {
                panelui::TableDesc desc;
                desc.id = "scene.hint.list";
                desc.ellipsizeLast = true;
                desc.read = [this](const u32 row, const std::span<std::string> cells, std::string&)
                {
                    const auto [input, action] = hintRow(row);
                    cells[0] = std::format("{}: {}", input, action);
                };
                hintTable = std::make_unique<panelui::Table>(*scope, std::move(desc));
                hintTable->setRowCount(static_cast<u32>(SCENE_VIEW_GESTURES.size()));
                hintTable->list.setLayoutItem(panelui::fillItem());
                hintTable->list.setAccessibleNameKey(InternedString("editor-scene.help.name"));
                density.apply(
                    [this](const panelui::Density& now)
                    {
                        const Vec2 size{0.0f, static_cast<float>(SCENE_VIEW_GESTURES.size()) * now.row};
                        if (hint.isValid() && hint.size() != size)
                        {
                            hint.setSize(size);
                        }
                    });
            }
            panelui::fillList(*scope, slot, *hintTable);
            hint.setVisible(hintOpen && settings->settings().sceneToolbar);
        }

        // The ? button is selected while the hint shows, as the chosen view's button is.
        void showHelp()
        {
            help.setLocalizedText(InternedString("editor-scene.help"));
            panelui::showSelected(help, hintOpen);
        }

        void showHint(const bool open)
        {
            if (open == hintOpen)
            {
                return;
            }
            hintOpen = open;
            if (hint.isValid())
            {
                hint.setVisible(open && settings->settings().sceneToolbar);
            }
            showHelp();
        }

        // The bar shows its controls, or only Expand while the toolbar is hidden (the sceneToolbar setting); the tray
        // under it shows the overlay toggles or the camera fields while one of them is open, and the hint hides with
        // the toolbar.
        void reveal()
        {
            const bool shown = settings->settings().sceneToolbar;
            shownToolbar = shown;
            shownOverlaysOpen = overlaysOpen;
            shownCameraOpen = cameraOpen;
            shownComponentGizmosOpen = componentGizmosOpen;
            for (Widget& item : items)
            {
                item.setVisible(shown);
            }
            expand.setVisible(!shown);
            for (u32 i = 0; i < overlayButtons.size(); ++i)
                overlayButtons[i].setVisible(i >= LIGHTS ? componentGizmosOpen : overlaysOpen);
            for (Widget& box : fieldBoxes)
            {
                box.setVisible(cameraOpen);
            }
            if (tray.isValid())
            {
                tray.setVisible(shown && (overlaysOpen || cameraOpen || componentGizmosOpen));
            }
            if (hint.isValid())
            {
                hint.setVisible(shown && hintOpen);
            }
            panelui::showSelected(overlays, overlaysOpen);
            panelui::showSelected(componentGizmos, componentGizmosOpen);
            panelui::showSelected(camera, cameraOpen);
        }

        // The slot's content goes in once; a document that recreates the slot gets it again.
        void fillToolbar(const Widget& slot)
        {
            bar = slot;
            if (items.empty())
            {
                build();
            }
            for (const Widget& item : items)
            {
                panelui::putIn(*scope, slot, item);
            }
            reveal();
        }

        void fillTray(const Widget& slot)
        {
            tray = slot;
            if (items.empty())
            {
                build();
            }
            for (const Widget& item : trayItems)
            {
                panelui::putIn(*scope, slot, item);
            }
            reveal();
        }

        std::string resolve(const std::string_view key) const
        {
            std::string scratch;
            return localization ? std::string(localization->resolve(InternedString(key), scratch)) : std::string(key);
        }

        void showBindings()
        {
            shownBindings = actions->bindingRevision();
            std::string chords;
            for (const std::string& chord : actions->shortcuts(EditorActions::Action::FrameSelected))
            {
                chords += (chords.empty() ? "" : ", ") + chord;
            }
            frame.setTooltip(InternedString("editor-scene.frame.tip"), chords);
            frameKey = chords.empty() ? resolve("editor-scene.hint.unbound") : chords;
            if (hintTable)
            {
                hintTable->rebindRow(FRAME_ROW);
            }
        }

        // The view dropdown shows the camera's view.
        void showView(const u32 chosen, const u64 locale)
        {
            const bool localeMoved = locale != shownLocale;
            shownView = chosen;
            shownLocale = locale;
            viewChoice.setSelected(chosen < SCENE_VIEW_MODES.size() ? static_cast<i32>(chosen) : -1);
            showHelp();
            showOverlayText();
            if (localeMoved)
            {
                showBindings(); // the unbound Frame key reads in the locale
                if (hintTable)
                {
                    hintTable->rebindVisible();
                }
            }
        }

        // The dropdown lists the modes with the ones the renderer cannot show now disabled (the tooltip says why), the
        // current one chosen; a mode set one pass at a time is "custom", shown disabled as the last item.
        void showShading(const ViewModeStatus status)
        {
            shownShading = status;
            std::vector<UiChoiceItem> choices;
            for (u32 i = 0; i < VIEW_MODES.size(); ++i)
            {
                choices.push_back({.id = InternedString(VIEW_MODES[i]),
                                   .key = InternedString("editor-scene.shading." + std::string(VIEW_MODES[i])),
                                   .enabled = (status.available >> i & 1u) != 0});
            }
            if (status.mode >= VIEW_MODES.size())
            {
                choices.push_back({.id = InternedString("custom"),
                                   .key = InternedString("editor-scene.shading.custom"),
                                   .enabled = false});
            }
            shading.setItems(choices);
            shading.setSelected(static_cast<i32>(std::min<u32>(status.mode, VIEW_MODES.size())));
            const u32 all = (1u << VIEW_MODES.size()) - 1;
            shading.setTooltip(InternedString((status.available & all) == all ? "editor-scene.shading.tip"
                                                                              : "editor-scene.shading.tip.limited"));
        }

        void showOverlays()
        {
            for (u32 i = 0; i < OVERLAYS.size(); ++i)
            {
                if (const bool on = overlayOn(i); shownOverlay[i] != on)
                {
                    shownOverlay[i] = on;
                    panelui::showSelected(overlayButtons[i], on);
                }
            }
            if (shownOverlaysOpen != overlaysOpen || shownCameraOpen != cameraOpen ||
                shownComponentGizmosOpen != componentGizmosOpen || shownToolbar != settings->settings().sceneToolbar)
            {
                reveal();
            }
        }

        // the text of a button follows the locale; its selected state does not live in it
        void showOverlayText()
        {
            overlays.setLocalizedText(InternedString("editor-scene.overlays"));
            camera.setLocalizedText(InternedString("editor-scene.camera"));
            for (u32 i = 0; i < OVERLAYS.size(); ++i)
            {
                overlayButtons[i].setLocalizedText(InternedString("editor-scene.overlay." + std::string(OVERLAYS[i])));
            }
        }

        // an overlay set: the grid and the toolbar are editor settings (saved), the gizmo's visibility is the gizmo's,
        // the outline the scene view's, the ruler and the HUD the Scene HUD's; the error, empty when it landed
        std::string setOverlay(const u32 overlay, const bool on)
        {
            if (overlay == COMPONENT_SELECTED)
            {
                gizmo->setSelectedComponentGizmosEnabled(on);
                return {};
            }
            if (overlay >= LIGHTS)
            {
                gizmo->setComponentCategoryEnabled(static_cast<ComponentGizmoCategory>(overlay - LIGHTS), on);
                return {};
            }
            if (overlay == OUTLINE || overlay == RULER || overlay == HUD || overlay == BONES)
            {
                overlay == OUTLINE     ? sceneView->showOutline(on)
                    : overlay == RULER ? hud->showRuler(on)
                    : overlay == HUD   ? hud->showHud(on)
                                       : EditorModelAccess::animation(*editorModel).showBones(on);
                return {};
            }
            const Json request = overlay == GIZMO
                ? Json{{"command", "gizmoMode"}, {"args", {{"enabled", on}}}}
                : Json{{"command", "editorSettings"},
                       {"args", {{overlay == GRID ? "gridEnabled" : "sceneToolbar", on}}}};
            const Json reply = Json::parse(engine->control().handle(request.dump()), nullptr, false);
            return reply.is_object() && reply.value("ok", false) ? std::string{} : reply.value("error", reply.dump());
        }

        void writeOverlays(IFormatWriter& out) const
        {
            out.writeBool("overlaysOpen", overlaysOpen);
            out.writeBool("componentGizmosOpen", componentGizmosOpen);
            out.beginArray("overlays", static_cast<u32>(OVERLAYS.size()));
            for (u32 i = 0; i < OVERLAYS.size(); ++i)
            {
                out.beginObject("");
                out.writeString("name", OVERLAYS[i]);
                out.writeBool("on", overlayOn(i));
                out.endObject();
            }
            out.endArray();
        }

        void writeShading(IFormatWriter& out) const
        {
            const ViewModeStatus status = viewModeStatus(engine->services());
            out.writeString("shading", status.mode < VIEW_MODES.size() ? VIEW_MODES[status.mode] : "custom");
            out.beginArray("shadingModes", static_cast<u32>(VIEW_MODES.size()));
            for (u32 i = 0; i < VIEW_MODES.size(); ++i)
            {
                const bool enabled = (status.available >> i & 1u) != 0;
                out.beginObject("");
                out.writeString("mode", VIEW_MODES[i]);
                out.writeBool("enabled", enabled);
                out.writeString("reason", enabled ? std::string_view{} : viewModeUnavailableReason(VIEW_MODES[i]));
                out.endObject();
            }
            out.endArray();
        }

        void refresh()
        {
            const EditorCamera& camera = sceneView->camera();
            const std::array<float, FIELD_COUNT> values{settings->settings().flySpeed, camera.fov, camera.nearPlane,
                                                        camera.farPlane};
            for (u32 i = 0; i < FIELD_COUNT; ++i)
            {
                if (shown[i] != values[i])
                {
                    shown[i] = values[i];
                    fields[i].setNumber(values[i]);
                }
            }
            const std::string_view mode = sceneView->viewMode();
            const u32 chosen = static_cast<u32>(std::ranges::find(SCENE_VIEW_MODES, mode) - SCENE_VIEW_MODES.begin());
            const u64 locale = localization ? localization->revision() : 0;
            if (chosen != shownView || locale != shownLocale)
            {
                showView(chosen, locale);
            }
            shownRevision = sceneView->cameraRevision();
            settingsMoved = false;
        }

        void dispatch()
        {
            for (u32 i = 0; i < FIELD_COUNT; ++i)
            {
                if (typed[i])
                {
                    pending.push_back(Json{{"command", i == FLY_SPEED ? "editorSettings" : "editorCamera"},
                                           {"args", {{std::string(FIELD_NAMES[i]), *typed[i]}}}}
                                          .dump());
                    typed[i].reset();
                }
            }
            if (pending.empty())
            {
                return;
            }
            panelui::dispatch(engine->control(), pending, "scene toolbar");
            // a refused value is not in the camera or the settings: the fields show what they hold again
            shown.fill(std::nullopt);
            shownShading.reset();
            shownOverlay.fill(std::nullopt);
            settingsMoved = true;
        }
    };

    EditorSceneToolbar::EditorSceneToolbar(Engine& engine, UiScope& scope, const Widget& host,
                                           EditorSceneView& sceneView, EditorSceneHud& hud,
                                           const EditorModel& editorModel, EditorSettingsService& settings,
                                           const EditorActions& actions, EditorGizmo& gizmo,
                                           std::array<Widget, 7> gizmoControls) :
        m_state(std::make_unique<State>(engine, scope, sceneView, hud, editorModel, settings, actions, gizmo))
    {
        State& state = *m_state;
        state.gizmoControls = gizmoControls;
        state.applied = settings.onApplied([&state](const EditorSettings&) { state.settingsMoved = true; });
        UiViewContract contract;
        contract.optional(InternedString(OVERLAY), UiWidgetKind::Panel,
                          [&state](const Widget& root) { state.overlay = root; });
        contract.slot(InternedString(TOOLBAR_SLOT), [&state](const Widget& slot) { state.fillToolbar(slot); });
        contract.slot(InternedString(TRAY_SLOT), [&state](const Widget& slot) { state.fillTray(slot); });
        contract.slot(InternedString(HINT_SLOT), [&state](const Widget& slot) { state.fillHint(slot); });
        contract.fallback(
            [&state](UiScope& viewScope, const Widget& root)
            {
                const panelui::Density now = panelui::Density::read(viewScope);
                state.overlay = root;
                Widget overlay = root;
                overlay.setPivot(Vec2{0.0f, 0.0f});
                overlay.setInteractable(false);
                overlay.setLayout(panelui::stack(UiLayoutDirection::Column, now.xs,
                                                 Vec4{now.xs, now.xs, now.xs, now.xs}, UiLayoutAlign::Stretch));
                UiRectDesc desc =
                    panelui::barDesc(Vec2{}, panelui::stack(UiLayoutDirection::Row, now.xs, {}, UiLayoutAlign::Center));
                desc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                desc.layout->wrap = true;
                Panel bar = viewScope.createPanel(InternedString(TOOLBAR_SLOT), desc);
                bar.setInteractable(false);
                panelui::bindGap(bar, panelui::SPACE_XS);
                bar.setParent(root);
                UiRectDesc trayDesc =
                    panelui::barDesc(Vec2{},
                                     panelui::stack(UiLayoutDirection::Row, now.xs,
                                                    Vec4{now.xs, now.xs, now.xs, now.xs}, UiLayoutAlign::Center));
                trayDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
                trayDesc.layoutItem->alignSelf = UiLayoutAlign::Start;
                Panel trayBox = viewScope.createPanel(InternedString(TRAY_SLOT), trayDesc);
                panelui::bindGap(trayBox, panelui::SPACE_XS);
                panelui::bindPadding(trayBox, panelui::SPACE_XS, panelui::SPACE_XS, panelui::SPACE_XS,
                                     panelui::SPACE_XS);
                trayBox.setStyleClass(InternedString(GROUP_CLASS));
                trayBox.setVisible(false);
                trayBox.setParent(root);
                UiRectDesc hintDesc = panelui::barDesc(
                    Vec2{}, panelui::stack(UiLayoutDirection::Column, 0.0f, {}, UiLayoutAlign::Stretch));
                hintDesc.layoutItem->shrink = 1.0f;
                Panel box = viewScope.createPanel(InternedString(HINT_SLOT), hintDesc);
                box.setStyleClass(InternedString(GROUP_CLASS));
                box.setVisible(false);
                box.setParent(root);
            });
        state.view = panelui::openView(engine, VIEW, DOCUMENT, std::move(contract), host);
        state.showBindings();

        // state of the toolbar, not of the scene: no undo step, and it works with the panel hidden
        engine.control().registerViewCommand(
            "sceneHint", OWNER,
            "shows or hides the Scene view's controls hint, the mouse and key gestures it answers; no open: toggles",
            {optionalArg("open", ArgKind::Boolean, "true shows, false hides")},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                const std::optional<bool> open = args.boolean("open");
                state.showHint(open ? *open : !state.hintOpen);
                out.writeBool("open", state.hintOpen);
                return std::string{};
            });
        engine.control().registerViewCommand(
            "sceneCamera", OWNER,
            "shows or hides the Scene toolbar's camera fields (Speed, FOV, Near and Far) in the tray under it, which "
            "closes the overlay toggles; no open: toggles",
            {optionalArg("open", ArgKind::Boolean, "true shows, false hides")},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                state.cameraOpen = args.boolean("open").value_or(!state.cameraOpen);
                if (state.cameraOpen)
                {
                    state.overlaysOpen = false;
                    state.componentGizmosOpen = false;
                }
                out.writeBool("open", state.cameraOpen);
                return std::string{};
            });
        engine.control().registerViewCommand(
            "sceneComponentGizmos", OWNER,
            "opens or closes the Scene component-gizmo category controls; no open toggles them",
            {optionalArg("open", ArgKind::Boolean, "true shows, false hides")},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                state.componentGizmosOpen = args.boolean("open").value_or(!state.componentGizmosOpen);
                if (state.componentGizmosOpen)
                {
                    state.overlaysOpen = false;
                    state.cameraOpen = false;
                }
                out.writeBool("open", state.componentGizmosOpen);
                return std::string{};
            });
        engine.control().registerViewCommand(
            "sceneOverlay", OWNER,
            "shows, hides or toggles one of the Scene view's overlays (grid: the editorSettings gridEnabled, gizmo: "
            "the gizmoMode enabled, outline: the selection's outline, ruler: the strip of world coordinates along the "
            "bottom edge, hud: the selection's name, id and position, bones: the skeleton of the selected animator, "
            "toolbar: the Scene toolbar itself; lights, colliders, cameras and custom: scene-wide component gizmos; "
            "componentSelected: all component gizmos of selected objects) "
            "and/or opens or closes the toolbar's "
            "overlay toggles (closing its camera "
            "fields); replies componentGizmosOpen as whether the component-gizmo controls tray is open; "
            "the sceneToolbar query tells their state, sceneHud what the ruler and the HUD show",
            {optionalArg("name", ArgKind::String, "the overlay", {}, OVERLAYS),
             optionalArg("on", ArgKind::Boolean, "true shows, false hides; omitted: toggles"),
             optionalArg("open", ArgKind::Boolean, "true shows the toolbar's overlay toggles, false hides them")},
            [&state](const ControlArgs& args, IFormatWriter& out)
            {
                const std::optional<std::string> name = args.string("name");
                const std::optional<bool> open = args.boolean("open");
                if (!name && !open)
                {
                    return std::string(
                        "sceneOverlay needs a name (grid, gizmo, outline, ruler, hud, bones, toolbar, lights, "
                        "colliders, cameras, custom or componentSelected) and/or open");
                }
                if (name)
                {
                    const u32 overlay = static_cast<u32>(std::ranges::find(OVERLAYS, *name) - OVERLAYS.begin());
                    if (const std::string error =
                            state.setOverlay(overlay, args.boolean("on").value_or(!state.overlayOn(overlay)));
                        !error.empty())
                    {
                        return error;
                    }
                }
                if (open)
                {
                    state.overlaysOpen = *open;
                    if (*open)
                    {
                        state.cameraOpen = false;
                        state.componentGizmosOpen = false;
                    }
                }
                state.writeOverlays(out);
                return std::string{};
            });
        engine.control().registerQuery("sceneToolbar", OWNER,
                                       "the Scene view's controls hint (whether it is open and its lines as shown), "
                                       "its overlay toggles (open, each overlay's state; toolbar: whether it shows), "
                                       "componentGizmosOpen (whether the component-gizmo controls tray is open), "
                                       "whether its camera fields are open (cameraOpen) and its shading dropdown "
                                       "(the mode and each mode with whether it is enabled and why not)",
                                       {},
                                       [&state](const ControlArgs&, IFormatWriter& out)
                                       {
                                           state.writeOverlays(out);
                                           state.writeShading(out);
                                           out.writeBool("hintOpen", state.hintOpen);
                                           out.writeBool("cameraOpen", state.cameraOpen);
                                           out.beginArray("gestures", static_cast<u32>(SCENE_VIEW_GESTURES.size()));
                                           for (u32 row = 0; row < SCENE_VIEW_GESTURES.size(); ++row)
                                           {
                                               const auto [input, action] = state.hintRow(row);
                                               out.beginObject({});
                                               out.writeString("gesture", SCENE_VIEW_GESTURES[row]);
                                               out.writeString("input", input);
                                               out.writeString("action", action);
                                               out.endObject();
                                           }
                                           out.endArray();
                                           if (state.hintTable)
                                           {
                                               out.beginObject("list");
                                               panelui::writeTableStats(out, *state.hintTable);
                                               out.endObject();
                                           }
                                           return std::string{};
                                       });
    }

    EditorSceneToolbar::~EditorSceneToolbar()
    {
        m_state->settings->removeApplied(m_state->applied);
        m_state->engine->control().unregisterOwner(OWNER);
    }

    void EditorSceneToolbar::setSize(const Vec2 size)
    {
        State& state = *m_state;
        if (state.overlay.isValid() && state.overlay.size() != size)
        {
            state.overlay.setSize(size);
        }
    }

    void EditorSceneToolbar::update()
    {
        State& state = *m_state;
        state.dispatch();
        if (state.actions->bindingRevision() != state.shownBindings)
        {
            state.showBindings();
        }
        if (state.settingsMoved || state.sceneView->cameraRevision() != state.shownRevision ||
            (state.localization && state.localization->revision() != state.shownLocale))
        {
            state.refresh();
        }
        if (const ViewModeStatus status = viewModeStatus(state.engine->services()); state.shownShading != status)
        {
            state.showShading(status);
        }
        state.showOverlays();
    }
} // namespace spite
