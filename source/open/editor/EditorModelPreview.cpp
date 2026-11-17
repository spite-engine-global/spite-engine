#include "EditorModelPreview.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorMaterialLinks.hpp"
#include "asset/LoadBakedFunctions.hpp"
#include "EditorPanelUi.hpp"
#include "EditorRenderedPreviews.hpp"
#include "assetruntime/AssetRuntime.hpp"
#include "engine/Engine.hpp"
#include "engine/ViewModeCommands.hpp"
#include "uiview/UiFrame.hpp"
#include "uiview/UiViewContext.hpp"
#include "ui/UiEvents.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

namespace spite
{
    namespace
    {
        using namespace panelui;
        constexpr auto OWNER = "editorModelPreview";
        constexpr u64 IMAGE_SLOT = 0x535049544d4f4400ull;
        constexpr std::string_view PANEL = "modelPreview";
        const char* statusName(EditorRenderedPreviews::Status status)
        {
            switch (status)
            {
            case EditorRenderedPreviews::Status::Idle:
                return "idle";
            case EditorRenderedPreviews::Status::Loading:
                return "loading";
            case EditorRenderedPreviews::Status::Capturing:
                return "capturing";
            case EditorRenderedPreviews::Status::Ready:
                return "ready";
            case EditorRenderedPreviews::Status::Missing:
                return "missing";
            case EditorRenderedPreviews::Status::Failed:
                return "failed";
            }
            return "failed";
        }
    } // namespace
    struct EditorModelPreview::State
    {
        State(Engine& engine, const DockSpace& dock) :
            engine(engine), dock(dock), scope(editorUi(engine).ui()), renderer(engine, 3)
        {
        }
        Engine& engine;
        DockSpace dock;
        UiScope scope;
        EditorRenderedPreviews renderer;
        Panel root, viewport, details;
        Dropdown shading;
        ViewModeStatus shownShading{.mode = ~0u, .available = ~0u};
        DockSpace innerDock;
        std::unique_ptr<Table> facts;
        std::unique_ptr<EditorMaterialLinks> links;
        std::vector<std::pair<std::string, std::string>> rows;
        u64 manifestRevision = ~0ull, localeRevision = ~0ull;
        float ratio = 0.725f;

        void refreshDetails()
        {
            const auto& manifest = engine.assets().manifest();
            auto* loc = engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr;
            const u64 revision = loc ? loc->revision() : 0;
            if (manifestRevision == manifest.generation() && localeRevision == revision)
                return;
            const bool reread = manifestRevision != manifest.generation();
            localeRevision = revision;
            if (reread)
            {
                manifestRevision = manifest.generation();
                rows.clear();
                std::vector<std::string> materials;
                if (name.isValid())
                    if (const auto entry = manifest.getModelEntry(name))
                    {
                        auto add = [this](const char* key, std::string value)
                        { rows.emplace_back(key, std::move(value)); };
                        add("name", std::string(name.toView()));
                        add("source", displayPath(entry->source.toView()));
                        if (const auto descriptor = loadBakedModelDescriptor(name, manifest))
                        {
                            add("nodes", std::to_string((*descriptor)->nodes.size()));
                            add("submeshes", std::to_string((*descriptor)->submeshes.size()));
                        }
                        add("vertices", std::to_string(entry->gpuCosts.vertexCount));
                        add("triangles", std::to_string(entry->gpuCosts.meshletTriangleIndexCount / 3));
                        add("materials", std::to_string(entry->materials.size()));
                        for (const auto& material : entry->materials)
                            materials.emplace_back(material.toView());
                    }
                links->setMaterials(materials);
                facts->setRowCount(static_cast<u32>(rows.size()));
            }
            facts->rebindVisible();
        }
        Image image;
        InternedString name;
        Extent2D extent{};
        bool running = false;
        float yaw = 0, pitch = 0, zoom = 0;
        std::vector<std::string> pending;

        void requestCamera(nlohmann::json args)
        {
            pending.push_back(nlohmann::json{{"command", "modelPreviewCamera"}, {"args", std::move(args)}}.dump());
        }

        void reset()
        {
            if (!name.isValid())
                return;
            renderer.openModel(name);
            running = true;
            yaw = pitch = zoom = 0;
        }
        std::string open(std::string_view value)
        {
            const InternedString requested(value);
            if (value.empty() || !engine.assets().manifest().getModelEntry(requested))
                return "Unknown model";
            name = requested;
            manifestRevision = ~0ull;
            if (!dock.isDetached(PANEL))
                dock.apply(UiDockOp{.kind = UiDockOpKind::Float,
                                    .panel = PANEL,
                                    .position = {80, 60},
                                    .size = {1100, 700},
                                    .detached = true});
            dock.apply(UiDockOp{.kind = UiDockOpKind::Activate, .panel = PANEL});
            if (running)
            {
                renderer.cancel();
                running = false;
            }
            return {};
        }
        void orbit(float x, float y)
        {
            if (!running || renderer.status() != EditorRenderedPreviews::Status::Ready)
                return;
            renderer.orbit(x, y);
            yaw += x;
            pitch = std::clamp(pitch + y, -1.5f, 1.5f);
        }
        void wheel(float value)
        {
            if (!running || renderer.status() != EditorRenderedPreviews::Status::Ready)
                return;
            renderer.zoom(value);
            zoom += value;
        }
        void commands()
        {
            auto& control = engine.control();
            control.registerViewCommand("openModelPreview", OWNER, "Open a model in the preview window",
                                        {requiredArg("name", ArgKind::String, "model name")},
                                        [this](const ControlArgs& args, IFormatWriter&)
                                        { return open(args.string("name").value_or("")); });
            control.registerViewCommand("modelPreviewCamera", OWNER, "Orbit, zoom or reset the model preview camera",
                                        {optionalArg("yaw", ArgKind::Number, "yaw delta in radians", "0"),
                                         optionalArg("pitch", ArgKind::Number, "pitch delta in radians", "0"),
                                         optionalArg("zoom", ArgKind::Number, "wheel delta", "0"),
                                         optionalArg("reset", ArgKind::Boolean, "frame the model", "false")},
                                        [this](const ControlArgs& args, IFormatWriter&)
                                        {
                                            auto number = [&](const char* key)
                                            {
                                                auto v = args.json(key);
                                                return v ? std::strtof(v->c_str(), nullptr) : 0.0f;
                                            };
                                            const float x = number("yaw"), y = number("pitch"), z = number("zoom");
                                            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
                                                return std::string("Camera values must be finite");
                                            if (args.boolean("reset").value_or(false) && running)
                                                reset();
                                            orbit(x, y);
                                            wheel(z);
                                            return std::string{};
                                        });
            control.registerViewCommand("modelPreviewSplit", OWNER, "Resize the model preview details pane",
                                        {requiredArg("ratio", ArgKind::Number, "viewport share")},
                                        [this](const ControlArgs& args, IFormatWriter&)
                                        {
                                            auto value = args.json("ratio");
                                            const float next = value ? std::strtof(value->c_str(), nullptr) : 0.0f;
                                            if (!std::isfinite(next) || next <= 0.0f || next >= 1.0f)
                                                return std::string("Invalid split ratio");
                                            innerDock.apply(UiDockOp{.kind = UiDockOpKind::Resize,
                                                                     .panel = InternedString("modelPreview.viewport"),
                                                                     .target = InternedString("modelPreview.details"),
                                                                     .ratio = next});
                                            ratio = next;
                                            return std::string{};
                                        });
            control.registerQuery(
                "modelPreview", OWNER,
                "Model preview: name is the selected model; status is idle, loading, capturing, ready, "
                "missing or failed; error is the failure message; visible means the panel is shown; "
                "running means its renderer is allocated. viewportWidth and detailsWidth are pane "
                "widths in UI units; ratio is the requested viewport share. yaw and pitch are accumulated "
                "orbit radians, zoom is accumulated wheel input; all three reset when framing the model.",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    out.writeString("name", name.isValid() ? name.toView() : std::string_view{});
                    out.writeString("status", statusName(renderer.status()));
                    out.writeString("error", renderer.error());
                    out.writeBool("visible", shown(scope, root));
                    out.writeBool("running", running);
                    out.writeF32("viewportWidth", viewport.size().x);
                    out.writeF32("detailsWidth", details.size().x);
                    out.writeF32("ratio", ratio);
                    out.writeF64("yaw", yaw);
                    out.writeF64("pitch", pitch);
                    out.writeF64("zoom", zoom);
                    return std::string{};
                });
        }
    };
    EditorModelPreview::EditorModelPreview(Engine& engine, const DockSpace& dock) :
        m_state(std::make_unique<State>(engine, dock))
    {
        auto& s = *m_state;
        s.root = s.scope.createPanel("modelPreview.root", columnDesc());
        UiDockDesc dockDesc;
        dockDesc.rect = topLeft();
        dockDesc.rect.layoutItem = fillItem();
        dockDesc.showChrome = false;
        dockDesc.splitterSize = Density::read(s.scope).md;
        s.innerDock = s.scope.createDock("modelPreview.dock", dockDesc);
        s.innerDock.setParent(s.root);
        s.viewport = s.scope.createPanel("modelPreview.viewport.host", columnDesc());
        s.details = s.scope.createPanel("modelPreview.details.host", columnDesc(0.0f, Vec4{8, 8, 8, 8}));
        s.innerDock.addPanel(InternedString("modelPreview.viewport"), "Viewport", s.viewport);
        s.innerDock.addPanel(InternedString("modelPreview.details"), "Details", s.details);
        s.innerDock.setPanelMinSize(InternedString("modelPreview.viewport"), Vec2{240, 0});
        s.innerDock.setPanelMinSize(InternedString("modelPreview.details"), Vec2{220, 0});
        s.innerDock.apply(UiDockOp{.kind = UiDockOpKind::Dock, .panel = InternedString("modelPreview.viewport")});
        s.innerDock.apply(UiDockOp{.kind = UiDockOpKind::Dock,
                                   .panel = InternedString("modelPreview.details"),
                                   .target = InternedString("modelPreview.viewport"),
                                   .zone = UiDockZone::Right,
                                   .ratio = 1.0f - s.ratio});
        s.innerDock.onRequest(
            [&s](const UiDockOp& op)
            {
                if (op.kind == UiDockOpKind::Resize)
                    s.pending.push_back(Json{{"command", "modelPreviewSplit"}, {"args", {{"ratio", op.ratio}}}}.dump());
            });
        s.facts = std::make_unique<Table>(s.scope,
                                          TableDesc{.id = "modelPreview.facts",
                                                    .columns = {100.0f},
                                                    .cells = 2,
                                                    .read = [&s](u32 row, std::span<std::string> cells, std::string&)
                                                    {
                                                        if (row >= s.rows.size())
                                                            return;
                                                        const std::string key =
                                                            "editor-project.details." + s.rows[row].first;
                                                        if (s.engine.services().contains<Localization>())
                                                        {
                                                            std::string scratch;
                                                            cells[0] = s.engine.services().get<Localization>().resolve(
                                                                InternedString(key), {}, scratch);
                                                        }
                                                        else
                                                            cells[0] = key;
                                                        cells[1] = s.rows[row].second;
                                                    }});
        const float factsHeight = Density::read(s.scope).row * 7.0f;
        s.facts->list.setLayoutItem(UiLayoutItemComponent{.shrink = 0.0f, .basis = factsHeight});
        s.facts->list.setParent(s.details);
        s.links =
            std::make_unique<EditorMaterialLinks>(engine, s.scope, "modelPreview.materials", 0x53504d4d00000000ull);
        s.links->root().setLayoutItem(fillItem());
        s.links->root().setParent(s.details);
        s.links->onOpen(
            [&s](std::string_view name)
            { s.pending.push_back(Json{{"command", "openMaterialPreview"}, {"args", {{"name", name}}}}.dump()); });
        UiDropdownDesc shadingDesc;
        const auto density = Density::read(s.scope);
        shadingDesc.rect = barDesc(Vec2{density.row * 6.0f, density.control});
        s.shading = s.scope.createDropdown("modelPreview.shading", shadingDesc);
        s.shading.setParent(s.viewport);
        s.shading.setAccessibleNameKey("editor-modelPreview.shading.name");
        s.shading.onChange(
            [&s](u32 index)
            {
                if (index < VIEW_MODES.size())
                    s.pending.push_back(
                        Json{{"command", "setViewMode"}, {"args", {{"mode", VIEW_MODES[index]}, {"view", 3}}}}.dump());
            });
        UiRectDesc imageDesc = topLeft();
        imageDesc.layoutItem = fillItem();
        s.image = s.scope.createImage("modelPreview.image", imageDesc);
        s.image.setParent(s.viewport);
        s.image.setImageSlot(IMAGE_SLOT);
        s.image.setAccessibleNameKey("editor-modelPreview.image");
        s.scope.setReaction(s.image.entity(), UiEventKind::DragStart,
                            [&s](const void* payload)
                            {
                                const Vec2 delta = static_cast<const UiDragStartEvent*>(payload)->delta /
                                    s.scope.hierarchy().layoutScale();
                                s.requestCamera({{"yaw", delta.x * 0.01f}, {"pitch", delta.y * 0.01f}});
                            });
        s.image.onDrag(
            [&s](Vec2 delta)
            {
                delta = delta / s.scope.hierarchy().layoutScale();
                s.requestCamera({{"yaw", delta.x * 0.01f}, {"pitch", delta.y * 0.01f}});
            });
        s.image.onWheel([&s](Vec2 delta) { s.requestCamera({{"zoom", delta.y}}); });
        editorUi(engine).uiView().registerImage(IMAGE_SLOT, uiViewTexture(3));
        s.commands();
        s.scope.commit();
    }
    EditorModelPreview::~EditorModelPreview()
    {
        m_state->engine.control().unregisterOwner(OWNER);
        m_state->renderer.cancel();
        editorUi(m_state->engine).uiView().clearImage(IMAGE_SLOT);
    }
    Widget EditorModelPreview::content() const { return m_state->root; }
    void EditorModelPreview::update()
    {
        auto& s = *m_state;
        for (const auto& line : std::exchange(s.pending, {}))
        {
            (void)s.engine.control().handle(line);
        }
        if (!shown(s.scope, s.root))
        {
            if (s.running)
            {
                s.renderer.cancel();
                s.running = false;
            }
            return;
        }
        s.refreshDetails();
        s.links->update();
        const auto shading = viewModeStatus(s.engine.services(), 3);
        if (shading != s.shownShading)
        {
            std::vector<UiChoiceItem> choices;
            for (u32 i = 0; i < VIEW_MODES.size(); ++i)
                choices.push_back({.id = InternedString(VIEW_MODES[i]),
                                   .key = InternedString("editor-scene.shading." + std::string(VIEW_MODES[i])),
                                   .enabled = (shading.available >> i & 1u) != 0});
            if (shading.mode >= VIEW_MODES.size())
                choices.push_back({.id = InternedString("custom"),
                                   .key = InternedString("editor-scene.shading.custom"),
                                   .enabled = false});
            s.shading.setItems(choices);
            s.shading.setSelected(static_cast<i32>(std::min<u32>(shading.mode, VIEW_MODES.size())));
            const u32 all = (1u << VIEW_MODES.size()) - 1;
            s.shading.setTooltip((shading.available & all) == all ? "editor-modelPreview.shading.tip"
                                                                  : "editor-modelPreview.shading.tip.limited");
            s.shownShading = shading;
        }
        if (!s.running && s.name.isValid())
            s.reset();
        if (!s.running)
            return;
        const Vec2 size = resolvedSize(s.scope, s.image);
        const float scale = s.scope.hierarchy().layoutScale();
        const Extent2D next{static_cast<u32>(std::max(0.0f, size.x * scale)),
                            static_cast<u32>(std::max(0.0f, size.y * scale))};
        if (next.width && next.height && (next.width != s.extent.width || next.height != s.extent.height))
        {
            s.renderer.setViewportExtent(next);
            s.extent = next;
        }
        s.renderer.update();
    }
    namespace
    {
        const EditorPanelRegistrar REGISTRATION{
            {.name = "modelPreview",
             .category = "project",
             .order = 1000,
             .dock = {.target = "project"},
             .zone = "EditorModelPreview",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<EditorModelPreview>(c.engine, c.dock); },
             .openByDefault = false}};
    } // namespace
} // namespace spite
