#include "EditorSceneHud.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "EditorAgentLayer.hpp"
#include "EditorEntityLabel.hpp"
#include "EditorGizmo.hpp"
#include "editor/EditorModel.hpp"
#include "EditorPanelUi.hpp"
#include "EditorSceneView.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiScope.hpp"
#include "world/TransformComponents.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        constexpr std::string_view OWNER = "editorSceneHud";
        constexpr float SPACING_ROWS = 4.0f; // the room a labelled tick has: its text at every text scale
        constexpr std::int64_t MAX_TICKS = 2048; // minor ticks of one strip, a bound on a degenerate camera
        constexpr sizet MAX_VALUE_CHARS = 16;
        constexpr sizet MAX_FIELD_CHARS = 32;
        constexpr float RULER_MAJOR = 0.4f; // tick heights, of the strip's
        constexpr float RULER_MINOR = 0.2f;

        struct Tick
        {
            float x = 0.0f; // layout units from the strip's left
            bool major = false;
            std::string label; // major ticks only
        };

        struct Ruler
        {
            bool valid = false;
            double spacing = 0.0; // world units between labelled ticks
            double minor = 0.0;
            double left = 0.0; // the coordinate at the strip's left and right edge
            double right = 0.0;
            float worldPerUnit = 0.0f;
            std::vector<Tick> ticks;
        };

        std::string fixed(const double value, const i32 decimals)
        {
            std::string text = std::format("{:.{}f}", value, decimals);
            if (text.starts_with('-') &&
                std::ranges::all_of(text, [](const char c) { return c == '-' || c == '0' || c == '.'; }))
            {
                text.erase(0, 1); // "-0.00" is 0
            }
            return text;
        }

        // the text cut to `limit` bytes at a character boundary, "..." for what was cut (the tail when keepTail)
        std::string shortened(std::string text, const sizet limit, const bool keepTail = false)
        {
            if (text.size() <= limit)
            {
                return text;
            }
            sizet at = keepTail ? text.size() - limit : limit;
            while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
            {
                keepTail ? ++at : --at;
            }
            return keepTail ? "\xE2\x80\xA6" + text.substr(at) : text.substr(0, at) + "\xE2\x80\xA6";
        }

        void appendValue(std::string& out, const Json& value)
        {
            if (value.is_number_float())
            {
                out += std::format("{:g}", static_cast<float>(value.get<double>()));
            }
            else if (value.is_string())
            {
                out += value.get<std::string>();
            }
            else if (value.is_array() || value.is_object())
            {
                out += value.is_array() ? '[' : '{';
                bool first = true;
                for (const auto& [key, item] : value.items())
                {
                    out += first ? "" : ", ";
                    first = false;
                    if (value.is_object())
                    {
                        out += key + ": ";
                    }
                    appendValue(out, item);
                }
                out += value.is_array() ? ']' : '}';
            }
            else
            {
                out += value.dump();
            }
        }

        // a value of the agent layer's JSON text as the callout shows it; empty (too large to carry): an ellipsis
        std::string valueText(const std::string& json)
        {
            const Json value = Json::parse(json, nullptr, false);
            if (json.empty() || value.is_discarded())
            {
                return "\xE2\x80\xA6";
            }
            std::string text;
            appendValue(text, value);
            return shortened(std::move(text), MAX_VALUE_CHARS);
        }
    } // namespace

    struct EditorSceneHud::State
    {
        State(Engine& engineRef, UiScope& scopeRef, const EditorSceneView& sceneViewRef, const EditorModel& modelRef,
              EditorAgentLayer& agentRef) :
            engine(&engineRef), scope(&scopeRef), sceneView(&sceneViewRef), model(&modelRef), agent(&agentRef),
            density(scopeRef), theme(scopeRef, [this] { shownCamera = ~0ull; })
        {
            if (engineRef.services().contains<Localization>())
            {
                localization = &engineRef.services().get<Localization>();
            }
        }

        Engine* engine;
        UiScope* scope;
        const EditorSceneView* sceneView;
        const EditorModel* model;
        EditorAgentLayer* agent;
        const Localization* localization = nullptr;
        panelui::DensityFollower density;

        Panel root;
        Canvas ruler;
        Panel hud;
        Label hudName;
        Label hudPosition;
        Panel callout;
        Label calloutText;
        Vec2 size{};
        bool rulerOn = false; // off until Overlays turns it on: the view keeps its bottom edge
        bool hudOn = true;

        // what the elements show, to write only what moved
        u64 shownCamera = ~0ull;
        Vec2 shownSize{};
        float shownControl = 0.0f;
        bool hudStale = true;
        Entity shownPrimary = Entity::undefined();
        u32 shownCount = ~0u;
        const World* shownWorld = nullptr;
        std::string shownNameKey;
        u64 shownCallout = ~0ull;
        bool calloutStale = true;
        bool calloutShown = false;
        u64 calloutCamera = ~0ull;
        Vec2 calloutSize{};
        float shownLimit = -1.0f;
        Vec2 shownPivot{};
        panelui::ThemeObserver theme; // after shownCamera, which its callback resets: the ruler redraws

        // -- the camera as the view shows it --------------------------------------------

        // the editor camera over the panel's size, in layout units: project() answers in them
        std::optional<GizmoCamera> viewCamera() const
        {
            if (size.x <= 0.0f || size.y <= 0.0f)
            {
                return std::nullopt;
            }
            const EditorCamera& camera = sceneView->camera();
            const f32 tanHalf = std::tan(toRadians(camera.fov) * 0.5f);
            return GizmoCamera{.eye = camera.position(),
                               .rotation = camera.rotation(),
                               .tanHalfFov = tanHalf,
                               .orthographicHeight = camera.orthographic ? 2.0f * camera.distance * tanHalf : 0.0f,
                               .nearPlane = camera.nearPlane,
                               .farPlane = camera.farPlane,
                               .extent = size};
        }

        // -- the ruler ----------------------------------------------------------------

        Ruler rulerNow() const
        {
            Ruler out;
            const std::optional<GizmoCamera> view = viewCamera();
            const float height = density.density().control;
            if (!view || size.x < 2.0f * height)
            {
                return out;
            }
            const EditorCamera& camera = sceneView->camera();
            // the view's height at the pivot's depth over its pixels (an orthographic view shows that height at every
            // depth, also in front of the near plane)
            const double perUnit = 2.0 * camera.distance * std::tan(toRadians(camera.fov) * 0.5f) / size.y;
            if (!(perUnit > 0.0) || !std::isfinite(perUnit))
            {
                return out;
            }
            const Vec3 right = spite::rotate(camera.rotation(), Vec3(1.0f, 0.0f, 0.0f));
            const double middle = dot(camera.pivot, right);
            out.worldPerUnit = static_cast<float>(perUnit);
            out.left = middle - 0.5 * perUnit * size.x;
            out.right = middle + 0.5 * perUnit * size.x;
            const double wanted = perUnit * SPACING_ROWS * density.density().row;
            const double power = std::pow(10.0, std::floor(std::log10(wanted)));
            const double mantissa = wanted / power;
            out.spacing = power * (mantissa <= 1.0 ? 1.0 : mantissa <= 2.0 ? 2.0 : mantissa <= 5.0 ? 5.0 : 10.0);
            out.minor = out.spacing / 5.0;
            const double lowest = std::ceil(out.left / out.minor);
            const double highest = std::floor(out.right / out.minor);
            if (!std::isfinite(lowest) || !std::isfinite(highest) || highest - lowest > MAX_TICKS)
            {
                return out;
            }
            const std::int64_t first = static_cast<std::int64_t>(lowest);
            const std::int64_t last = static_cast<std::int64_t>(highest);
            const i32 decimals = std::max(0, -static_cast<i32>(std::floor(std::log10(out.spacing) + 1.0e-9)));
            for (std::int64_t k = first; k <= last; ++k)
            {
                Tick tick{.x = static_cast<float>((static_cast<double>(k) * out.minor - out.left) / perUnit),
                          .major = k % 5 == 0};
                if (tick.major)
                {
                    tick.label = fixed(static_cast<double>(k) * out.minor, decimals);
                }
                out.ticks.push_back(std::move(tick));
            }
            out.valid = true;
            return out;
        }

        void drawRuler()
        {
            shownCamera = sceneView->cameraRevision();
            shownSize = size;
            shownControl = density.density().control;
            const float height = shownControl;
            if (ruler.size() != Vec2{size.x, height})
            {
                ruler.setSize(Vec2{size.x, height});
            }
            const Ruler now = rulerNow();
            const float box =
                static_cast<float>(now.spacing / std::max(1.0e-30, static_cast<double>(now.worldPerUnit)));
            // the strip is its own plate over the 3D image, whatever the theme: canvas.hud.* keeps it dark
            const u32 plate = panelui::canvasColor(*scope, "canvas.hud.plate");
            const u32 major = panelui::canvasColor(*scope, "canvas.hud.major");
            const u32 minor = panelui::canvasColor(*scope, "canvas.hud.minor");
            const u32 text = panelui::canvasColor(*scope, "canvas.hud.text");
            ruler.update(
                [&](UiCanvasPainter& painter)
                {
                    painter.rect(Vec2{0.0f, 0.0f}, Vec2{size.x, height}, plate);
                    for (const Tick& tick : now.ticks)
                    {
                        const float length = height * (tick.major ? RULER_MAJOR : RULER_MINOR);
                        painter.rect(Vec2{tick.x - 0.5f, 0.0f}, Vec2{1.0f, length}, tick.major ? major : minor);
                        if (tick.major)
                        {
                            painter.text(Vec2{tick.x - 0.5f * box, height * RULER_MAJOR},
                                         Vec2{box, height * (1.0f - RULER_MAJOR)}, tick.label, text,
                                         UiCanvasAlign::Center);
                        }
                    }
                });
        }

        void followRuler()
        {
            const float control = density.density().control;
            const bool wanted = rulerOn && size.x >= 2.0f * control && size.y >= 3.0f * control;
            if (ruler.isVisible() != wanted)
            {
                ruler.setVisible(wanted);
                shownCamera = ~0ull;
            }
            if (wanted && (sceneView->cameraRevision() != shownCamera || size != shownSize || control != shownControl))
            {
                drawRuler();
            }
        }

        // -- the HUD -------------------------------------------------------------------

        static std::optional<Vec3> positionOf(const World& world, const Entity entity)
        {
            if (const auto* matrix = world.tryGetConstComponent<TransformMatrixComponent>(entity))
            {
                const Vec4 column = matrix->matrix.column(3);
                return Vec3(column.x, column.y, column.z);
            }
            if (const auto* transform = world.tryGetConstComponent<TransformComponent>(entity))
            {
                return transform->position;
            }
            return std::nullopt;
        }

        struct Selected
        {
            Entity primary = Entity::undefined();
            u32 count = 0;
        };

        // the first selected entity of the world the view shows, and how many are selected there
        Selected selectedNow() const
        {
            Selected out;
            if (model->selectionWorld() == engine->gameWorldName())
            {
                const std::span<const Entity> selection = model->selection();
                out.count = static_cast<u32>(selection.size());
                if (!selection.empty() && engine->gameWorld().isEntityAlive(selection.front()))
                {
                    out.primary = selection.front();
                }
            }
            return out;
        }

        // whether the entity's name or transform changed since the last frame
        bool moved(const Entity entity) const
        {
            const World& world = engine->gameWorld();
            return world.wasComponentModifiedById(entity, getComponentId<TransformMatrixComponent>()) ||
                world.wasComponentModifiedById(entity, getComponentId<TransformComponent>()) ||
                world.wasComponentModifiedById(entity, getComponentId<EntityNameComponent>());
        }

        void placeHud()
        {
            const panelui::Density& now = density.density();
            const Vec2 offset{now.xs, -(now.xs + (rulerOn ? now.control : 0.0f))};
            if (hud.offset() != offset)
            {
                hud.setOffset(offset);
            }
        }

        void showHud(const Selected& selected)
        {
            hudStale = false;
            shownPrimary = selected.primary;
            shownCount = selected.count;
            shownWorld = &engine->gameWorld();
            const bool visible = hudOn && selected.primary.isValid();
            if (hud.isVisible() != visible)
            {
                hud.setVisible(visible);
            }
            if (!visible)
            {
                return;
            }
            const World& world = engine->gameWorld();
            std::string scratch;
            const std::string_view name = entityLabel(scratch, world, selected.primary);
            const char* key = selected.count > 1 ? "editor-scene.hud.name.more" : "editor-scene.hud.name";
            if (shownNameKey != key)
            {
                shownNameKey = key;
                hudName.setLocalizedText(InternedString(key));
            }
            hudName.setLocalizedArgText("name", name);
            hudName.setLocalizedArgText("id", std::to_string(selected.primary.id()));
            hudName.setLocalizedArgText("more", std::to_string(selected.count - 1));
            const std::optional<Vec3> position = positionOf(world, selected.primary);
            if (hudPosition.isVisible() != position.has_value())
            {
                hudPosition.setVisible(position.has_value());
            }
            if (position)
            {
                hudPosition.setLocalizedArgText("x", fixed(position->x, 2));
                hudPosition.setLocalizedArgText("y", fixed(position->y, 2));
                hudPosition.setLocalizedArgText("z", fixed(position->z, 2));
            }
        }

        void followHud()
        {
            const Selected selected = selectedNow();
            if (hudStale || selected.primary != shownPrimary || selected.count != shownCount ||
                &engine->gameWorld() != shownWorld || (selected.primary.isValid() && moved(selected.primary)))
            {
                showHud(selected);
            }
        }

        // -- the callout ---------------------------------------------------------------

        struct Place
        {
            Vec2 offset;
            Vec2 pivot;
        };

        // Where the callout sits: at the entity's screen position, below it or above where the view is lower, and
        // kept inside the view above the ruler and the HUD. Whatever the plate's size, so a frame in which the plate
        // grows is not one in which it leaves the view: the horizontal anchor slides from the left margin to the right
        // one with the entity, and the plate's pivot with it.
        std::optional<Place> calloutAt(const World& world) const
        {
            const EditorAgentLayer::Callout* note = agent->callout();
            const std::optional<GizmoCamera> view = viewCamera();
            const std::optional<Vec3> position = note ? positionOf(world, note->entity) : std::nullopt;
            const std::optional<Vec2> screen = view && position ? view->project(*position) : std::nullopt;
            if (!screen)
            {
                return std::nullopt;
            }
            const panelui::Density& now = density.density();
            const float room = std::max(1.0f, size.x - 2.0f * now.xs);
            const float slide = std::clamp((screen->x - now.xs) / room, 0.0f, 1.0f);
            const float top = now.xs;
            // above the ruler and the selection HUD at the bottom edge, so the two plates never cover each other
            const float hudRoom = hud.isVisible() ? scope->hierarchy().layoutSizeOf(hud.entity()).y + now.xs : 0.0f;
            const float bottom = std::max(top, size.y - now.xs - (rulerOn ? now.control : 0.0f) - hudRoom);
            const float y = std::clamp(screen->y, top, bottom);
            const bool above = y > 0.5f * (top + bottom);
            return Place{.offset = Vec2{now.xs + slide * room, above ? y - now.xl : y + now.xl},
                         .pivot = Vec2{slide, above ? 1.0f : 0.0f}};
        }

        void showCallout()
        {
            shownCallout = agent->calloutRevision();
            calloutStale = true; // a new callout is always placed
            const EditorAgentLayer::Callout* note = agent->callout();
            if (!note)
            {
                if (calloutShown)
                {
                    calloutShown = false;
                    callout.setVisible(false);
                }
                return;
            }
            const bool set = !note->field.empty();
            const char* key = !set ? "editor-scene.callout.edited"
                : note->fields > 1 ? "editor-scene.callout.set.more"
                                   : "editor-scene.callout.set";
            calloutText.setLocalizedText(InternedString(key));
            std::string client = note->client;
            if (client.empty())
            {
                std::string scratch;
                client = localization
                    ? std::string(localization->resolve(InternedString("editor-scene.callout.client"), scratch))
                    : "client";
            }
            calloutText.setLocalizedArgText("client", client);
            calloutText.setLocalizedArgText("field", shortened(note->field, MAX_FIELD_CHARS, true));
            calloutText.setLocalizedArgText("was", valueText(note->was));
            calloutText.setLocalizedArgText("now", valueText(note->now));
            calloutText.setLocalizedArgText("more", std::to_string(note->fields > 0 ? note->fields - 1 : 0));
            calloutShown = true;
        }

        // the HUD's name and the callout's text take at most the view's width less the margins and the edge (they end
        // with an ellipsis past it)
        void limitText()
        {
            const panelui::Density& now = density.density();
            const float limit = std::max(0.0f, size.x - 2.0f * now.xs - now.xxs);
            if (limit != shownLimit && hudName.isValid() && calloutText.isValid())
            {
                shownLimit = limit;
                UiLayoutItemComponent item = panelui::fixedItem();
                item.maxSize = Vec2{limit, 0.0f};
                calloutText.setLayoutItem(item);
                hudName.setLayoutItem(item);
            }
        }

        void followCallout()
        {
            if (agent->calloutRevision() != shownCallout)
            {
                showCallout();
            }
            if (!calloutShown)
            {
                return;
            }
            const EditorAgentLayer::Callout* note = agent->callout();
            const World& world = engine->gameWorld();
            if (!world.isEntityAlive(note->entity))
            {
                calloutShown = false;
                callout.setVisible(false);
                return;
            }
            if (!calloutStale && sceneView->cameraRevision() == calloutCamera && size == calloutSize &&
                !moved(note->entity))
            {
                return;
            }
            calloutStale = false;
            calloutCamera = sceneView->cameraRevision();
            calloutSize = size;
            const std::optional<Place> at = calloutAt(world);
            if (callout.isVisible() != at.has_value())
            {
                callout.setVisible(at.has_value());
            }
            if (at)
            {
                if (callout.offset() != at->offset)
                {
                    callout.setOffset(at->offset);
                }
                if (shownPivot != at->pivot)
                {
                    shownPivot = at->pivot;
                    callout.setPivot(at->pivot);
                }
            }
        }

        // -- the elements ----------------------------------------------------------------

        Label fittedLabel(const std::string_view id, const Widget& parent)
        {
            UiRectDesc desc = panelui::barDesc(Vec2{});
            desc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
            Label label = scope->createLabel(InternedString(id), "", desc);
            panelui::bindFitPadding(label, panelui::SPACE_SM, panelui::SPACE_XS);
            label.setInteractable(false);
            label.setParent(parent);
            return label;
        }

        // a plate on the root, as wide and high as its content
        Panel plate(const std::string_view id, const UiLayoutDirection direction, const Vec2 corner)
        {
            UiRectDesc desc = panelui::topLeft();
            desc.anchor = corner;
            desc.pivot = corner;
            desc.layout = panelui::stack(direction, 0.0f, {}, UiLayoutAlign::Stretch);
            desc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
            Panel panel = scope->createPanel(InternedString(id), desc);
            panel.setStyleClass(InternedString("scene.bar"));
            panel.setInteractable(false);
            panel.setVisible(false);
            panel.setParent(root);
            return panel;
        }

        void build(const Widget& host)
        {
            root = scope->createPanel("scene.hud.overlay", panelui::topLeft());
            root.setInteractable(false);
            root.setZOffset(1); // over the toolbar's document, which is the host's other child
            root.setParent(host);

            UiRectDesc rulerDesc = panelui::topLeft();
            rulerDesc.anchor = Vec2{0.0f, 1.0f};
            rulerDesc.pivot = Vec2{0.0f, 1.0f};
            ruler = scope->createCanvas("scene.ruler", UiCanvasDesc{.rect = rulerDesc, .maxPrimitives = 8192});
            ruler.setInteractable(false);
            ruler.setAccessibleNameKey(InternedString("editor-scene.ruler.name"));
            ruler.setParent(root);

            hud = plate("scene.hud", UiLayoutDirection::Column, Vec2{0.0f, 1.0f});
            hudName = fittedLabel("scene.hud.name", hud);
            hudName.setStyleClass(InternedString("list.row.text.ellipsis"));
            hudPosition = fittedLabel("scene.hud.position", hud);
            hudPosition.setLocalizedText(InternedString("editor-scene.hud.position"));

            callout = plate("scene.callout", UiLayoutDirection::Row, Vec2{0.0f, 0.0f});
            Panel edge = scope->createPanel("scene.callout.edge", panelui::barDesc(Vec2{}));
            edge.setStyleClass(InternedString("inspectorEdge"));
            edge.setInteractable(false);
            edge.setParent(callout);
            calloutText = fittedLabel("scene.callout.text", callout);
            calloutText.setStyleClass(InternedString("list.row.text.ellipsis"));
            density.apply(
                [this, edge](const panelui::Density& now) mutable
                {
                    if (edge.size().x != now.xxs)
                    {
                        edge.setSize(Vec2{now.xxs, edge.size().y});
                    }
                    placeHud();
                    limitText();
                    calloutStale = true;
                });
        }

        // -- the query --------------------------------------------------------------------

        void write(IFormatWriter& out)
        {
            const Selected selected = selectedNow();
            const World& world = engine->gameWorld();
            out.beginObject("hud");
            out.writeBool("on", hudOn);
            out.writeBool("shown", hudOn && selected.primary.isValid());
            out.writeU32("count", selected.count);
            if (selected.primary.isValid())
            {
                std::string scratch;
                out.writeString("name", entityLabel(scratch, world, selected.primary));
                out.writeU64("id", selected.primary.id());
                if (const std::optional<Vec3> position = positionOf(world, selected.primary))
                {
                    const std::array<f32, 3> values{position->x, position->y, position->z};
                    out.writeFloats("position", values);
                }
            }
            out.endObject();

            const Ruler now = rulerNow();
            out.beginObject("ruler");
            out.writeBool("on", rulerOn);
            out.writeBool("shown", ruler.isVisible() && now.valid);
            out.writeString("mode", sceneView->camera().orthographic ? "orthographic" : "perspective");
            out.writeF64("spacing", now.spacing);
            out.writeF64("minorSpacing", now.minor);
            out.writeF64("left", now.left);
            out.writeF64("right", now.right);
            out.writeU32("ticks", static_cast<u32>(now.ticks.size()));
            out.beginArray("labels", static_cast<u32>(std::ranges::count_if(now.ticks, &Tick::major)));
            for (const Tick& tick : now.ticks)
            {
                if (tick.major)
                {
                    out.writeString("", tick.label);
                }
            }
            out.endArray();
            out.endObject();

            const EditorAgentLayer::Callout* note = agent->callout();
            out.beginObject("callout");
            out.writeBool("shown", calloutShown && callout.isVisible());
            if (note)
            {
                out.writeU64("entity", note->entity.id());
                out.writeString("client", note->client);
                out.writeString("field", note->field);
                out.writeString("was", note->was);
                out.writeString("now", note->now);
                out.writeU32("fields", note->fields);
                const Vec2 at = callout.offset();
                const std::array<f32, 2> where{at.x, at.y};
                out.writeFloats("at", where);
            }
            out.endObject();
        }
    };

    EditorSceneHud::EditorSceneHud(Engine& engine, UiScope& scope, const Widget& host, const EditorSceneView& sceneView,
                                   const EditorModel& model, EditorAgentLayer& agent) :
        m_state(std::make_unique<State>(engine, scope, sceneView, model, agent))
    {
        State& state = *m_state;
        state.build(host);
        engine.control().registerQuery(
            "sceneHud", OWNER,
            "what the Scene view draws over its image. hud: on, shown, count (selected in the shown world) and for the "
            "first selected entity its name, id and position (world x, y, z); ruler: on, shown, mode (perspective or "
            "orthographic), spacing (world units between labelled ticks), minorSpacing, left and right (the "
            "coordinate at the strip's edges), ticks and labels; callout: shown and, for the entity a client edited in "
            "the last 5 s, entity, client, field (the first one its step overwrote), was and now (JSON text), fields "
            "(how many the step overwrote) and at (the plate's offset in the view)",
            {},
            [&state](const ControlArgs&, IFormatWriter& out)
            {
                state.write(out);
                return std::string{};
            });
    }

    EditorSceneHud::~EditorSceneHud() { m_state->engine->control().unregisterOwner(OWNER); }

    void EditorSceneHud::setSize(const Vec2 size)
    {
        State& state = *m_state;
        state.size = size;
        state.limitText();
        if (state.root.isValid() && state.root.size() != size)
        {
            state.root.setSize(size);
        }
    }

    bool EditorSceneHud::rulerShown() const { return m_state->rulerOn; }

    void EditorSceneHud::showRuler(const bool shown)
    {
        State& state = *m_state;
        state.rulerOn = shown;
        state.shownCamera = ~0ull;
        state.calloutStale = true;
        state.placeHud();
    }

    bool EditorSceneHud::hudShown() const { return m_state->hudOn; }

    void EditorSceneHud::showHud(const bool shown)
    {
        m_state->hudOn = shown;
        m_state->hudStale = true;
    }

    void EditorSceneHud::update()
    {
        State& state = *m_state;
        state.followRuler();
        state.followHud();
        state.followCallout();
    }
} // namespace spite
