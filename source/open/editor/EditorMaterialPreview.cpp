#include "EditorMaterialPreview.hpp"

#include "EditorContext.hpp"
#include "EditorFeedback.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorPanelUi.hpp"
#include "EditorProjectMaterial.hpp"
#include "EditorRenderedPreviews.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include "asset/AssetBaker.hpp"
#include "asset/AssetManifest.hpp"
#include "asset/LoadBakedFunctions.hpp"
#include "assetruntime/AssetRuntime.hpp"
#include "control/ControlServer.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"
#include "serialization/JsonFormat.hpp"
#include "ui/UiScope.hpp"
#include "ui/UiEvents.hpp"
#include "uiview/UiFrame.hpp"
#include "uiview/UiViewContext.hpp"

namespace spite
{
    namespace
    {
        using namespace panelui;
        using Json = nlohmann::json;
        constexpr std::string_view OWNER = "editorMaterialPreview";
        constexpr u64 IMAGE_SLOT = 0x4d415450524556ull;
    } // namespace

    struct EditorMaterialPreview::State
    {
        struct Request
        {
            std::string line;
            const char* failedKey = nullptr;
            std::function<void(const std::string&)> refused;
        };

        State(Engine& owner, EditorFeedback& messages) :
            engine(owner), feedback(messages), scope(editorUi(owner).ui()), density(scope), renderer(owner, 4),
            control(owner.control()),
            localization(owner.services().contains<Localization>() ? &owner.services().get<Localization>() : nullptr)
        {
            root = scope.createPanel("materialPreview.root", columnDesc());
            Panel body = stackPanel(scope, "materialPreview.body", {}, UiLayoutDirection::Row, UiLayoutAlign::Stretch);
            body.setLayoutItem(fillItem());
            body.setParent(root);
            image = scope.createImage("materialPreview.image", topLeft());
            image.setLayoutItem(fillItem());
            image.setImageSlot(IMAGE_SLOT);
            image.setAccessibleNameKey("editor-materialPreview.image");
            image.setParent(body);
            auto camera = [this](Vec2 delta, float zoom)
            {
                pending.push_back({Json{{"command", "materialPreviewCamera"},
                                        {"args", {{"yaw", delta.x}, {"pitch", delta.y}, {"zoom", zoom}}}}
                                       .dump()});
            };
            scope.setReaction(image.entity(), UiEventKind::DragStart,
                              [this, camera](const void* payload)
                              {
                                  const Vec2 delta = static_cast<const UiDragStartEvent*>(payload)->delta /
                                      scope.hierarchy().layoutScale();
                                  camera(delta * 0.01f, 0.0f);
                              });
            image.onDrag([this, camera](Vec2 delta) { camera(delta / scope.hierarchy().layoutScale() * 0.01f, 0.0f); });
            image.onWheel([camera](Vec2 delta) { camera({}, delta.y); });
            form = std::make_unique<ProjectMaterialView>(
                scope, density,
                [this](std::string_view key, std::span<const MessageArg> args)
                {
                    if (!localization)
                        return std::string(key);
                    std::string scratch;
                    return std::string(localization->resolve(InternedString(key), args, scratch));
                },
                [this](std::string line, const char* key, std::function<void(const std::string&)> refused)
                { pending.push_back({std::move(line), key, std::move(refused)}); },
                [this]
                {
                    std::vector<std::string> names;
                    for (const auto& [name, entry] : engine.assets().manifest().getTextures())
                        names.emplace_back(name.toView());
                    std::ranges::sort(names);
                    return names;
                },
                [this] { duplicate(); });
            form->root().setParent(body);
            form->root().setVisible(true);
            status = scope.createLabel("materialPreview.status", "", barDesc(Vec2{0.0f, density.density().row}));
            status.bindLayoutToken(UiLayoutField::Basis, InternedString(SIZE_ROW));
            status.setParent(root);
            scope.commit();
            control.registerViewCommand("materialPreviewCamera", OWNER, "Orbit or zoom the material preview camera",
                                        {optionalArg("yaw", ArgKind::Number, "yaw delta in radians", "0"),
                                         optionalArg("pitch", ArgKind::Number, "pitch delta in radians", "0"),
                                         optionalArg("zoom", ArgKind::Number, "wheel delta", "0")},
                                        [this](const ControlArgs& args, IFormatWriter&)
                                        {
                                            auto number = [&](const char* key)
                                            {
                                                auto value = args.json(key);
                                                return value ? std::strtof(value->c_str(), nullptr) : 0.0f;
                                            };
                                            const float yaw = number("yaw"), pitch = number("pitch"),
                                                        zoom = number("zoom");
                                            if (!std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(zoom))
                                                return std::string("Camera values must be finite");
                                            renderer.orbit(yaw, pitch);
                                            renderer.zoom(zoom);
                                            return std::string{};
                                        });
            control.registerViewCommand(
                "openMaterialPreview", OWNER, "opens a material preview",
                {requiredArg("name", ArgKind::String, "material name")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const auto requested = args.string("name");
                    if (!requested || !engine.assets().manifest().getMaterialEntry(InternedString(*requested)))
                        return std::string("material not found");
                    name = *requested;
                    if (running)
                        renderer.cancel();
                    editorUi(engine).uiView().clearImage(IMAGE_SLOT);
                    imageReady = false;
                    running = false;
                    dirty = true;
                    pending.push_back({R"({"command":"dock","args":{"kind":"show","panel":"materialPreview"}})"});
                    pending.push_back(
                        {R"({"command":"dock","args":{"kind":"float","panel":"materialPreview","x":40,"y":40,"w":1000,"h":650,"detached":true}})"});
                    return std::string{};
                });
        }

        ~State()
        {
            control.unregisterOwner(OWNER);
            renderer.cancel();
            editorUi(engine).uiView().clearImage(IMAGE_SLOT);
        }

        void duplicate()
        {
            const auto& manifest = engine.assets().manifest();
            const auto descriptor = readBakedMaterialDescriptor(InternedString(name), manifest);
            if (!descriptor)
                return;
            JsonFragmentWriter writer;
            writeValue(BakedMaterialWriteData::spiteSchema(), &*descriptor, writer, FieldSet::All);
            std::string text;
            writer.dump(text);
            std::string stem = name.substr(0, 51) + "_copy";
            std::string copy = stem;
            for (u32 i = 2; std::ranges::any_of(
                     manifest.getMaterials(), [&](const auto& entry)
                     { return AssetManifest::bakeFileKey(entry.first.toView()) == AssetManifest::bakeFileKey(copy); });
                 ++i)
                copy = stem + "_" + std::to_string(i);
            pending.push_back(
                {Json{{"command", "createMaterial"}, {"args", {{"name", copy}, {"fields", Json::parse(text)}}}}.dump(),
                 "editor-project.material.create.failed"});
        }

        void update()
        {
            std::vector<Request> requests;
            requests.swap(pending);
            for (const Request& request : requests)
            {
                const Json reply = Json::parse(control.handle(request.line), nullptr, false);
                if (!reply.is_object() || !reply.value("ok", false))
                {
                    const std::string error = reply.is_object() ? reply.value("error", std::string{}) : std::string{};
                    if (request.refused)
                        request.refused(error);
                    if (request.failedKey)
                    {
                        const std::vector<MessageArg> args{{"error", std::string_view(error)}};
                        feedback.postToastKey(ToastLevel::Error, request.failedKey, args);
                    }
                    dirty = true;
                }
            }
            if (!shown(scope, root))
            {
                if (running)
                {
                    renderer.cancel();
                    running = false;
                    form->close();
                    editorUi(engine).uiView().clearImage(IMAGE_SLOT);
                    imageReady = false;
                }
                return;
            }
            if (name.empty())
                return;
            if (!running)
            {
                renderer.openMaterial(InternedString(name));
                running = true;
                dirty = true;
            }
            const auto rect = scope.hierarchy().absoluteRectOf(image.entity());
            const Extent2D next{static_cast<u32>(std::max(1l, std::lround(rect.size.x))),
                                static_cast<u32>(std::max(1l, std::lround(rect.size.y)))};
            if (next.width != extent.width || next.height != extent.height)
            {
                extent = next;
                renderer.setViewportExtent(extent);
            }
            renderer.update();
            const auto state = renderer.status();
            const bool ready = state == EditorRenderedPreviews::Status::Ready;
            if (ready != imageReady)
            {
                if (ready)
                    editorUi(engine).uiView().registerImage(IMAGE_SLOT, uiViewTexture(4));
                else
                    editorUi(engine).uiView().clearImage(IMAGE_SLOT);
                imageReady = ready;
            }
            status.setVisible(!ready);
            if (state == EditorRenderedPreviews::Status::Failed || state == EditorRenderedPreviews::Status::Missing)
                status.setLocalizedText("editor-modelPreview.failed");
            else
                status.setLocalizedText("editor-materialPreview.loading");
            const u64 generation = engine.assets().manifest().generation();
            const u64 commands = control.stateGeneration();
            const u64 locale = localization ? localization->revision() : 0;
            if (locale != seenLocale)
            {
                form->relocalize();
                seenLocale = locale;
                dirty = true;
            }
            if (dirty || generation != seenManifest || commands != seenCommands)
            {
                ProjectMaterialModel model;
                model.shown = true;
                model.name = name;
                const auto fields = readBakedMaterialDescriptor(InternedString(name), engine.assets().manifest());
                if (fields)
                    model.values = materialValues(*fields);
                else
                    model.error = std::string(fields.error().message);
                form->show(model);
                seenManifest = generation;
                seenCommands = commands;
                dirty = false;
            }
            form->fit();
            form->tick();
        }

        Engine& engine;
        EditorFeedback& feedback;
        UiScope scope;
        DensityFollower density;
        EditorRenderedPreviews renderer;
        ControlServer& control;
        const Localization* localization;
        Panel root;
        Image image;
        Label status;
        std::unique_ptr<ProjectMaterialView> form;
        std::vector<Request> pending;
        std::string name;
        Extent2D extent{};
        u64 seenManifest = ~0ull, seenCommands = ~0ull, seenLocale = ~0ull;
        bool running = false, dirty = true, imageReady = false;
    };

    EditorMaterialPreview::EditorMaterialPreview(Engine& engine, EditorFeedback& feedback) :
        m_state(std::make_unique<State>(engine, feedback))
    {
    }
    EditorMaterialPreview::~EditorMaterialPreview() = default;
    Widget EditorMaterialPreview::content() const { return m_state->root; }
    void EditorMaterialPreview::update() { m_state->update(); }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "materialPreview",
             .category = "project",
             .order = 205,
             .zone = "EditorMaterialPreview",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<EditorMaterialPreview>(c.engine, c.feedback); },
             .openByDefault = false}};
    } // namespace
} // namespace spite
