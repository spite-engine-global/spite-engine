#include "base/WritableLayout.hpp"
#include "EditorAssetPreviews.hpp"
#include "EditorContext.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorPanelUi.hpp"
#include "asset/AssetManifest.hpp"
#include "assetruntime/AssetRuntime.hpp"
#include "base/ContentRoot.hpp"
#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "ui/UiEvents.hpp"
#include "uiview/UiViewContext.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <nlohmann/json.hpp>

namespace spite
{
    namespace
    {
        using namespace panelui;
        using Json = nlohmann::json;
        constexpr auto OWNER = "editorImagePreview";
        constexpr u64 IMAGE_SLOT = 0x53504954494d4700ull;

        class ImagePreview
        {
        public:
            ImagePreview(Engine& engine, const DockSpace&) :
                engine(engine), scope(editorUi(engine).ui()),
                previews((writableDirectory(WritableFolder::Cache, {}, false) / "previews"), 4096)
            {
                root = scope.createPanel("imagePreview.root", columnDesc());
                Panel bar = stackPanel(scope, "imagePreview.bar", {}, UiLayoutDirection::Row, UiLayoutAlign::Stretch);
                bar.setParent(root);
                title = scope.createLabel("imagePreview.name", "", barDesc(Vec2{0.0f, 24.0f}));
                title.setLayoutItem(fillItem());
                title.setParent(bar);
                Button fit = scope.createButton("imagePreview.fit", "", barDesc(Vec2{64.0f, 24.0f}));
                fit.setLocalizedText("editor-imagePreview.fit");
                fit.setAccessibleNameKey("editor-imagePreview.fit");
                fit.setTooltip(InternedString("editor-imagePreview.fit"));
                fit.setParent(bar);
                fit.onClick([this] { request({{"reset", true}}); });
                viewport = scope.createPanel("imagePreview.viewport", topLeft());
                viewport.setLayoutItem(fillItem());
                viewport.setParent(root);
                scope.world().getComponent<UiStateComponent>(viewport.entity()).clipsChildren = true;
                image = scope.createImage("imagePreview.image", topLeft());
                image.setImageSlot(IMAGE_SLOT);
                image.setAccessibleNameKey("editor-imagePreview.image");
                image.setParent(viewport);
                scope.setReaction(image.entity(), UiEventKind::DragStart,
                                  [this](const void* payload)
                                  {
                                      const Vec2 delta = static_cast<const UiDragStartEvent*>(payload)->delta /
                                          scope.hierarchy().layoutScale();
                                      request({{"x", delta.x}, {"y", delta.y}});
                                  });
                image.onDrag(
                    [this](Vec2 delta)
                    {
                        delta = delta / scope.hierarchy().layoutScale();
                        request({{"x", delta.x}, {"y", delta.y}});
                    });
                image.onWheel([this](Vec2 delta) { request({{"zoom", delta.y}}); });
                status = scope.createLabel("imagePreview.status", "", barDesc(Vec2{0.0f, 24.0f}));
                status.setParent(root);
                scope.commit();
                engine.control().registerViewCommand(
                    "imagePreviewView", OWNER, "Pan, zoom or reset the image preview",
                    {optionalArg("x", ArgKind::Number, "horizontal pan delta", "0"),
                     optionalArg("y", ArgKind::Number, "vertical pan delta", "0"),
                     optionalArg("zoom", ArgKind::Number, "wheel delta", "0"),
                     optionalArg("reset", ArgKind::Boolean, "fit the image", "false")},
                    [this](const ControlArgs& args, IFormatWriter&)
                    {
                        auto number = [&](const char* key)
                        {
                            auto value = args.json(key);
                            return value ? std::strtof(value->c_str(), nullptr) : 0.0f;
                        };
                        const float x = number("x"), y = number("y"), wheel = number("zoom");
                        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(wheel))
                            return std::string("view values must be finite");
                        if (args.boolean("reset").value_or(false))
                        {
                            pan = {};
                            zoom = 1.0f;
                        }
                        pan = pan + Vec2{x, y};
                        zoom = std::clamp(zoom * std::pow(1.15f, std::clamp(wheel, -100.0f, 100.0f)), 0.1f, 32.0f);
                        return std::string{};
                    });
                engine.control().registerViewCommand(
                    "openImagePreview", OWNER, "Open a texture or atlas preview",
                    {requiredArg("name", ArgKind::String, "asset name"),
                     requiredArg("type", ArgKind::String, "texture or atlas")},
                    [this](const ControlArgs& args, IFormatWriter&)
                    {
                        const auto requested = args.string("name");
                        const auto type = args.string("type");
                        if (!requested || !type || (*type != "texture" && *type != "atlas"))
                            return std::string("expected texture or atlas name and type");
                        const auto& manifest = this->engine.assets().manifest();
                        InternedString texture(*requested);
                        if (*type == "atlas")
                        {
                            const auto atlas = manifest.getSpriteAtlasEntry(texture);
                            if (!atlas)
                                return std::string("atlas not found");
                            texture = atlas->texture;
                        }
                        const auto entry = manifest.getTextureEntry(texture);
                        if (!entry)
                            return std::string("texture not found");
                        previews.clear();
                        editorUi(this->engine).uiView().clearImage(IMAGE_SLOT);
                        name = *requested;
                        title.setText(name);
                        registered = false;
                        zoom = 1.0f;
                        pan = {};
                        previews.request(name, resolveContentPath(pathFromUtf8(entry->baked.toView())));
                        pending = true;
                        return std::string{};
                    });
            }
            ~ImagePreview()
            {
                engine.control().unregisterOwner(OWNER);
                previews.clear();
                editorUi(engine).uiView().clearImage(IMAGE_SLOT);
            }
            Widget content() const { return root; }
            void update()
            {
                for (const auto& line : std::exchange(requests, {}))
                    (void)engine.control().handle(line);
                if (pending)
                {
                    pending = false;
                    engine.control().handle(R"({"command":"dock","args":{"kind":"show","panel":"imagePreview"}})");
                    engine.control().handle(
                        R"({"command":"dock","args":{"kind":"float","panel":"imagePreview","x":40,"y":40,"w":1000,"h":650,"detached":true}})");
                }
                if (!shown(scope, root) || name.empty())
                    return;
                previews.update();
                const auto result = previews.status(name);
                if (!registered && result.status == EditorAssetPreviews::Status::Ready)
                {
                    editorUi(engine).uiView().registerImageFile(IMAGE_SLOT, utf8Path(result.path));
                    registered = true;
                }
                const auto info = editorUi(engine).uiView().imageInfo(IMAGE_SLOT);
                const bool ready = info && info->state == UiImageState::Ready && info->width && info->height;
                status.setVisible(!ready);
                status.setLocalizedText(result.status == EditorAssetPreviews::Status::Error ||
                                                (info && info->state == UiImageState::Failed)
                                            ? "editor-imagePreview.failed"
                                            : "editor-imagePreview.loading");
                image.setVisible(ready);
                if (!ready)
                    return;
                const Vec2 available = resolvedSize(scope, viewport);
                const Vec2 pixels{static_cast<float>(info->width), static_cast<float>(info->height)};
                const float fit = std::min(available.x / pixels.x, available.y / pixels.y);
                const Vec2 size = pixels * (fit * zoom);
                image.setSize(size);
                image.setOffset((available - size) * 0.5f + pan);
            }

        private:
            void request(Json args)
            {
                requests.push_back(Json{{"command", "imagePreviewView"}, {"args", std::move(args)}}.dump());
            }
            Engine& engine;
            UiScope scope;
            EditorAssetPreviews previews;
            Panel root, viewport;
            Image image;
            Label title, status;
            std::string name;
            std::vector<std::string> requests;
            Vec2 pan{};
            float zoom = 1.0f;
            bool registered = false, pending = false;
        };
        const EditorPanelRegistrar REGISTRATION{
            {.name = "imagePreview",
             .category = "project",
             .order = 206,
             .zone = "EditorImagePreview",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<ImagePreview>(c.engine, c.dock); },
             .openByDefault = false}};
    } // namespace
} // namespace spite
