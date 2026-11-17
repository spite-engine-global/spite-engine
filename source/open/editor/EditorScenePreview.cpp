#include "EditorContext.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorPanelUi.hpp"
#include "EditorRenderedPreviews.hpp"
#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "ui/UiEvents.hpp"
#include "uiview/UiFrame.hpp"
#include "uiview/UiViewContext.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <utility>

namespace spite
{
    namespace
    {
        using namespace panelui;
        using Json = nlohmann::json;
        constexpr auto OWNER = "editorScenePreview";
        constexpr auto PANEL = "scenePreview";
        constexpr u64 IMAGE_SLOT = 0x5350495453434e00ull;
        class ScenePreview
        {
        public:
            ScenePreview(Engine& engine, const DockSpace& dock) :
                engine(engine), dock(dock), scope(editorUi(engine).ui()), renderer(engine, 5)
            {
                root = scope.createPanel("scenePreview.root", columnDesc());
                Panel bar = stackPanel(scope, "scenePreview.bar", {}, UiLayoutDirection::Row, UiLayoutAlign::Stretch);
                bar.setParent(root);
                title = scope.createLabel("scenePreview.name", "", barDesc(Vec2{0, 24}));
                title.setLayoutItem(fillItem());
                title.setParent(bar);
                Button fit = scope.createButton("scenePreview.fit", "", barDesc(Vec2{64, 24}));
                fit.setLocalizedText("editor-scenePreview.fit");
                fit.setAccessibleNameKey("editor-scenePreview.fit");
                fit.setTooltip(InternedString("editor-scenePreview.fit"));
                fit.setParent(bar);
                fit.onClick([this] { request({{"reset", true}}); });
                image = scope.createImage("scenePreview.image", topLeft());
                image.setLayoutItem(fillItem());
                image.setImageSlot(IMAGE_SLOT);
                image.setAccessibleNameKey("editor-scenePreview.image");
                image.setParent(root);
                scope.setReaction(image.entity(), UiEventKind::DragStart,
                                  [this](const void* payload)
                                  {
                                      Vec2 delta = static_cast<const UiDragStartEvent*>(payload)->delta /
                                          scope.hierarchy().layoutScale();
                                      request({{"yaw", delta.x * 0.01f}, {"pitch", delta.y * 0.01f}});
                                  });
                image.onDrag(
                    [this](Vec2 delta)
                    {
                        delta = delta / scope.hierarchy().layoutScale();
                        request({{"yaw", delta.x * 0.01f}, {"pitch", delta.y * 0.01f}});
                    });
                image.onWheel([this](Vec2 delta) { request({{"zoom", delta.y}}); });
                status = scope.createLabel("scenePreview.status", "", barDesc(Vec2{0, 24}));
                status.setParent(root);
                editorUi(engine).uiView().registerImage(IMAGE_SLOT, uiViewTexture(5));
                scope.commit();
                engine.control().registerViewCommand(
                    "openScenePreview", OWNER, "Open an isolated scene preview",
                    {requiredArg("name", ArgKind::String, "scene file name")},
                    [this](const ControlArgs& args, IFormatWriter&)
                    {
                        auto value = args.string("name");
                        if (!value || value->empty())
                            return std::string("scene name required");
                        const auto file = pathFromUtf8(*value);
                        if (file.has_parent_path() || (file.extension() != ".json" && file.extension() != ".spsd"))
                            return std::string("invalid scene name");
                        path = this->engine.control().root("scene") / file;
                        std::error_code ec;
                        if (!std::filesystem::is_regular_file(path, ec))
                            return std::string("scene not found");
                        title.setText(*value);
                        if (!this->dock.isDetached(PANEL))
                            this->dock.apply(UiDockOp{.kind = UiDockOpKind::Float,
                                                      .panel = PANEL,
                                                      .position = {80, 60},
                                                      .size = {1100, 700},
                                                      .detached = true});
                        this->dock.apply(UiDockOp{.kind = UiDockOpKind::Activate, .panel = PANEL});
                        renderer.cancel();
                        running = false;
                        return std::string{};
                    });
                engine.control().registerViewCommand(
                    "scenePreviewCamera", OWNER, "Orbit, zoom or reset the scene preview camera",
                    {optionalArg("yaw", ArgKind::Number, "yaw delta", "0"),
                     optionalArg("pitch", ArgKind::Number, "pitch delta", "0"),
                     optionalArg("zoom", ArgKind::Number, "wheel delta", "0"),
                     optionalArg("reset", ArgKind::Boolean, "reframe scene", "false")},
                    [this](const ControlArgs& args, IFormatWriter&)
                    {
                        auto number = [&](const char* key)
                        {
                            auto value = args.json(key);
                            return value ? std::strtof(value->c_str(), nullptr) : 0.0f;
                        };
                        const float yaw = number("yaw"), pitch = number("pitch"), zoom = number("zoom");
                        if (!std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(zoom))
                            return std::string("camera values must be finite");
                        if (args.boolean("reset").value_or(false) && !path.empty())
                        {
                            renderer.openScene(path);
                            running = true;
                        }
                        renderer.orbit(yaw, pitch);
                        renderer.zoom(zoom);
                        return std::string{};
                    });
            }
            ~ScenePreview()
            {
                engine.control().unregisterOwner(OWNER);
                renderer.cancel();
                editorUi(engine).uiView().clearImage(IMAGE_SLOT);
            }
            Widget content() const { return root; }
            void update()
            {
                for (const auto& line : std::exchange(pending, {}))
                    (void)engine.control().handle(line);
                if (!shown(scope, root))
                {
                    if (running)
                    {
                        renderer.cancel();
                        running = false;
                    }
                    return;
                }
                if (path.empty())
                    return;
                if (!running)
                {
                    renderer.openScene(path);
                    running = true;
                }
                const Vec2 size = resolvedSize(scope, image);
                const float scale = scope.hierarchy().layoutScale();
                renderer.setViewportExtent({static_cast<u32>(std::max(1.0f, size.x * scale)),
                                            static_cast<u32>(std::max(1.0f, size.y * scale))});
                renderer.update();
                const auto state = renderer.status();
                const bool ready = state == EditorRenderedPreviews::Status::Ready;
                image.setVisible(ready);
                status.setVisible(!ready);
                status.setLocalizedText(state == EditorRenderedPreviews::Status::Failed
                                            ? "editor-scenePreview.failed"
                                            : "editor-scenePreview.loading");
            }

        private:
            void request(Json args)
            {
                pending.push_back(Json{{"command", "scenePreviewCamera"}, {"args", std::move(args)}}.dump());
            }
            Engine& engine;
            DockSpace dock;
            UiScope scope;
            EditorRenderedPreviews renderer;
            Panel root;
            Image image;
            Label title, status;
            std::filesystem::path path;
            std::vector<std::string> pending;
            bool running = false;
        };
        const EditorPanelRegistrar REGISTRATION{
            {.name = "scenePreview",
             .category = "project",
             .order = 207,
             .zone = "EditorScenePreview",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<ScenePreview>(c.engine, c.dock); },
             .openByDefault = false}};
    } // namespace
} // namespace spite
