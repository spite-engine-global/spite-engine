#include "engine/UiInspectCommands.hpp"

#include "control/NumberArg.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/ServiceLocator.hpp"
#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "ecs/World.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiAccessibility.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiDragDrop.hpp"
#include "ui/UiOverlay.hpp"
#include "ui/UiRichText.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiTextMeasurer.hpp"
#include "ui/UiTween.hpp"
#include "uiview/UiStyleSheet.hpp"
#include "uiview/UiViewContext.hpp"

namespace spite
{
    namespace
    {
        constexpr ArgDesc UI_WORLD_ARG =
            optionalArg("world", ArgKind::String, "the UI's world: a UI world, scene, or play while playing",
                        "the first UI world, else scene");
        constexpr ArgDesc ID_ARG =
            optionalArg("id", ArgKind::String,
                        "an element id, or an id path 'ancestor/.../id' (ancestors in order, not necessarily parents)");
        constexpr u32 DEFAULT_DEPTH = 8;
        constexpr u32 DEFAULT_MAX_NODES = 400;
        constexpr float TEXT_FIT_SLACK = 0.5f; // UI pixels a text may exceed its rect by (rounding of the measure)

        std::string_view uiPlacementSideName(const UiPlacementSide side)
        {
            return side == UiPlacementSide::Below ? "below"
                : side == UiPlacementSide::Above  ? "above"
                : side == UiPlacementSide::Right  ? "right"
                                                  : "left";
        }

        std::string_view uiDropStateName(const UiDropState state)
        {
            return state == UiDropState::Accepting ? "accepting"
                : state == UiDropState::Rejecting  ? "rejecting"
                                                   : "none";
        }

        std::string_view idOf(const World& world, const Entity element)
        {
            // a list taken in the frame a command destroyed elements can still name them
            const auto* identity =
                world.isEntityAlive(element) ? world.tryGetConstComponent<UiIdentityComponent>(element) : nullptr;
            return identity ? identity->id.toView() : std::string_view{};
        }

        void writeRect(IFormatWriter& out, const UiRect& rect, const std::string_view key = "rect")
        {
            out.beginObject(key);
            out.writeF32("x", rect.position.x);
            out.writeF32("y", rect.position.y);
            out.writeF32("w", rect.size.x);
            out.writeF32("h", rect.size.y);
            out.endObject();
        }

        // The drag in flight: its payload, source, the target under the pointer and whether it takes it.
        void writeDrag(IFormatWriter& out, const UiContext& ui)
        {
            const UiDragDrop::Drag* drag = ui.dragDrop().current();
            if (!drag)
            {
                return;
            }
            const World& world = ui.hierarchy().world();
            const auto element = [&](const char* key, const Entity entity)
            {
                out.beginObject(key);
                out.writeU64("entity", entity == Entity::undefined() ? 0 : entity.id());
                out.writeString("id", entity == Entity::undefined() ? std::string_view{} : idOf(world, entity));
                out.endObject();
            };
            out.beginObject("drag");
            out.writeString("type", drag->payload.type.isValid() ? drag->payload.type.toView() : std::string_view{});
            out.writeU64("payloadEntity", drag->payload.entity == Entity::undefined() ? 0 : drag->payload.entity.id());
            out.writeU64("data", drag->payload.data);
            out.writeString("text", drag->payload.text);
            out.writeString("label", drag->payload.label);
            element("source", drag->source);
            element("target", drag->target);
            element("hit", drag->hit);
            out.writeBool("accepting", drag->accepting);
            out.writeF32("x", drag->position.x);
            out.writeF32("y", drag->position.y);
            out.endObject();
        }

        // Empty `error` and an undefined element: no element named
        struct Target
        {
            Entity element = Entity::undefined();
            std::string error;
        };

        // Whether the ancestors of `element` hold `segments` (all but the last, outermost first) in order.
        bool ancestorsMatch(const UiHierarchyManager& hierarchy, const World& world, Entity element,
                            const std::span<const std::string_view> segments)
        {
            sizet next = segments.size();
            for (element = hierarchy.parentOf(element); next > 0 && element != Entity::undefined();
                 element = hierarchy.parentOf(element))
            {
                if (idOf(world, element) == segments[next - 1])
                {
                    --next;
                }
            }
            return next == 0;
        }

        Target findById(const UiContext& ui, const std::string_view path)
        {
            std::vector<std::string_view> segments;
            for (sizet begin = 0; begin <= path.size();)
            {
                const sizet end = std::min(path.find('/', begin), path.size());
                segments.push_back(path.substr(begin, end - begin));
                begin = end + 1;
            }
            const UiHierarchyManager& hierarchy = ui.hierarchy();
            const World& world = hierarchy.world();
            const std::string_view last = segments.back();
            const std::span<const std::string_view> ancestors(segments.data(), segments.size() - 1);
            Target target;
            u32 matches = 0;
            std::string listed;
            for (const Entity element : hierarchy.paintOrderOf())
            {
                if (!world.isEntityAlive(element) || idOf(world, element) != last ||
                    !ancestorsMatch(hierarchy, world, element, ancestors))
                {
                    continue;
                }
                if (matches++ == 0)
                {
                    target.element = element;
                }
                if (matches <= 5)
                {
                    listed += (listed.empty() ? "" : ", ") + std::to_string(element.id());
                }
            }
            if (matches == 0)
            {
                target.error = "no UI element with id '" + std::string(path) + "'";
            }
            else if (matches > 1)
            {
                target.element = Entity::undefined();
                target.error = "id '" + std::string(path) + "' matches " + std::to_string(matches) +
                    " elements (entities " + listed + (matches > 5 ? ", ..." : "") + "); name an id path or the entity";
            }
            return target;
        }

        Target targetOf(const UiContext& ui, const ControlArgs& args, const std::string_view idKey,
                        const std::string_view entityKey)
        {
            Target target;
            if (const std::optional<u64> entity = args.integer(entityKey))
            {
                target.element = Entity(*entity);
                if (!ui.hierarchy().isElement(target.element))
                {
                    target.element = Entity::undefined();
                    target.error = "no UI element " + std::to_string(*entity);
                }
                return target;
            }
            if (const std::optional<std::string> id = args.string(idKey); id && !id->empty())
            {
                return findById(ui, *id);
            }
            return target;
        }

        // "to"/"from" of uiTween: a number or an array of 1 to 4 numbers. Empty `error` and the values on success.
        struct LaneValues
        {
            Vec4 value{0.0f, 0.0f, 0.0f, 0.0f};
            u32 count = 0;
            std::string error;
        };

        LaneValues laneValues(const ControlArgs& args, const std::string_view key)
        {
            LaneValues out;
            std::string text = args.json(key).value_or(std::string{});
            std::erase_if(text, [](const char c) { return c == '[' || c == ']' || c == ' ' || c == '"'; });
            for (sizet begin = 0; begin <= text.size() && !text.empty();)
            {
                const sizet end = std::min(text.find(',', begin), text.size());
                float lane = 0.0f;
                const auto [stop, error] = std::from_chars(text.data() + begin, text.data() + end, lane);
                if (error != std::errc{} || stop != text.data() + end || out.count == 4)
                {
                    out.error = std::string(key) + " is a number or an array of up to 4 numbers";
                    return out;
                }
                (out.count == 0       ? out.value.x
                     : out.count == 1 ? out.value.y
                     : out.count == 2 ? out.value.z
                                      : out.value.w) = lane;
                ++out.count;
                begin = end + 1;
            }
            return out;
        }

        // The elements a uiTree reply holds, planned first because an array is written with its count.
        struct TreePlan
        {
            struct Node
            {
                Entity element;
                u32 childCount; // before depth and budget cut them
                std::vector<u32> kids; // indices into nodes
            };

            const UiHierarchyManager& hierarchy;
            const UiViewContext* view; // null: the world has no view module
            bool visibleOnly;
            u32 maxDepth;
            u32 budget; // nodes still allowed
            bool truncated = false;
            std::vector<Node> nodes;
            const UiSeriesTable* series = nullptr; // the canvases' series buffers
            const UiContext* ui = nullptr; // for the charts' readouts
            mutable u32 seriesLeft = 0; // series entries still allowed (the node budget again)

            // -1 when the budget is spent
            i32 collect(const Entity element, const u32 depth)
            {
                if (budget == 0)
                {
                    truncated = true;
                    return -1;
                }
                --budget;
                const auto index = static_cast<u32>(nodes.size());
                const std::span<const Entity> all = hierarchy.childrenOf(element);
                std::vector<Entity> children(all.begin(), all.end());
                if (visibleOnly)
                {
                    std::erase_if(children, [this](const Entity child) { return !hierarchy.isVisibleOf(child); });
                }
                nodes.push_back({element, static_cast<u32>(children.size()), {}});
                if (depth >= maxDepth)
                {
                    truncated |= !children.empty();
                    return index;
                }
                for (const Entity child : children)
                {
                    const i32 kid = collect(child, depth + 1);
                    if (kid < 0)
                    {
                        break;
                    }
                    nodes[index].kids.push_back(static_cast<u32>(kid));
                }
                truncated |= nodes[index].kids.size() < children.size();
                return index;
            }

            void write(IFormatWriter& out, const UiRouter& router, const std::string_view key, const u32 index) const
            {
                const Node& node = nodes[index];
                const Entity element = node.element;
                const World& world = hierarchy.world();
                out.beginObject(key);
                out.writeU64("entity", element.id());
                const auto* identity = world.tryGetConstComponent<UiIdentityComponent>(element);
                out.writeString("id", identity ? identity->id.toView() : std::string_view{});
                if (identity)
                {
                    out.writeString("kind", uiWidgetKindName(identity->kind));
                    if (identity->styleClass.isValid())
                    {
                        out.writeString("styleClass", identity->styleClass.toView());
                    }
                }
                writeRect(out, hierarchy.absoluteRectOf(element));
                if (hierarchy.isClippedOf(element))
                {
                    // the rect an ancestor clips this element to (what shows is rect intersected with clip) and
                    // the ancestor that clips
                    writeRect(out, hierarchy.clipRectOf(element), "clip");
                    out.writeU64("clipOwner", hierarchy.clipOwnerOf(element).id());
                }
                // accessibility: what the element is and is called; an interactive role with no name
                // is the gap to fill
                if (const UiAccessible accessible = uiAccessibleOf(hierarchy, element); accessible.role != UiRole::None)
                {
                    out.writeString("role", uiRoleName(accessible.role));
                    out.writeString("name", accessible.name);
                    if (accessible.hasSelected)
                    {
                        out.writeBool("selected", accessible.selected);
                    }
                    if (accessible.expand != UiExpandState::None)
                    {
                        out.writeBool("expanded", accessible.expand == UiExpandState::Expanded);
                    }
                    if (accessible.sort != UiSortState::NotSortable)
                    {
                        out.writeString("sort", uiSortStateName(accessible.sort));
                    }
                    if (accessible.nameKey.isValid())
                    {
                        out.writeString("nameKey", accessible.nameKey.toView());
                    }
                }
                const auto* state = world.tryGetConstComponent<UiStateComponent>(element);
                const bool interactable = !state || state->interactable;
                out.writeBool("visible", hierarchy.isVisibleOf(element));
                out.writeBool("interactable", interactable);
                if (state && state->landmark)
                {
                    out.writeBool("landmark", true);
                }
                if (const auto* tokens = world.tryGetConstComponent<UiLayoutTokensComponent>(element))
                {
                    // each bound layout field: its token, value and the table's (tokenValue, as applied: a length
                    // never below 0); unresolved says why ("token": the table lacks it, "component": the element
                    // lacks the field's component) and the field keeps its value; held: a tween or raw write moved
                    // the field off the table value until the table next changes, or the table changed since the
                    // last frame applied it
                    out.beginObject("layoutTokens");
                    for (u8 slot = 0; slot < static_cast<u8>(UiLayoutField::Count); ++slot)
                    {
                        const auto field = static_cast<UiLayoutField>(slot);
                        if (!(*tokens)[field].isValid())
                        {
                            continue;
                        }
                        const float* value = uiLayoutFieldValue(world, element, field);
                        std::optional<float> table = ui->layoutTokens().find((*tokens)[field]);
                        if (table && field != UiLayoutField::Basis)
                        {
                            table = std::max(*table, 0.0f);
                        }
                        out.beginObject(UI_LAYOUT_FIELD_NAMES[slot]);
                        out.writeString("token", (*tokens)[field].toView());
                        out.writeF64("value", value ? *value : 0.0);
                        if (table)
                        {
                            out.writeF64("tokenValue", *table);
                        }
                        out.writeBool("resolved", value && table);
                        if (!value || !table)
                        {
                            out.writeString("unresolved", !table ? "token" : "component");
                        }
                        else
                        {
                            out.writeBool("held", *value != *table);
                        }
                        out.endObject();
                    }
                    out.endObject();
                }
                if (const auto* menu = world.tryGetConstComponent<UiMenuComponent>(element))
                {
                    // an open popup menu: the item the keys highlight (empty: none), by its id
                    out.writeBool("open", menu->open);
                    out.writeString("highlighted", menu->highlighted.toView());
                }
                if (const auto* menuItem = world.tryGetConstComponent<UiMenuItemComponent>(element);
                    menuItem && !menuItem->separator && menuItem->id.isValid())
                {
                    out.writeString("item", menuItem->id.toView());
                    if (menuItem->checkable)
                    {
                        out.writeBool("checked", menuItem->checked);
                    }
                }
                const auto* dropTarget = world.tryGetConstComponent<UiDropTargetComponent>(element);
                out.writeString("state",
                                dropTarget && dropTarget->state == UiDropState::Accepting       ? "dropAccept"
                                    : dropTarget && dropTarget->state == UiDropState::Rejecting ? "dropReject"
                                    : !interactable                                             ? "disabled"
                                    : router.pressed() == element                               ? "pressed"
                                    : router.focused() == element                               ? "focused"
                                    : router.hovered() == element                               ? "hover"
                                                                                                : "normal");
                if (const auto* label = world.tryGetConstComponent<UiLabelComponent>(element))
                {
                    out.writeString("text", label->text);
                    if (const UiTextMeasurer* measurer = hierarchy.textMeasurer(); measurer && !label->text.empty())
                    {
                        // the text as measured at the element's own width, in the units of rect (scaled); a text
                        // wider than its rect (a wrapping one: taller) is cut or ellipsized by whatever clips it
                        const UiRect rect = hierarchy.absoluteRectOf(element);
                        const float scale = hierarchy.layoutScale();
                        const float width = rect.size.x > 0.0f ? rect.size.x / scale : -1.0f;
                        const UiTextMeasure block = label->rich
                            ? measurer->measureRichTextBlock(element, label->text, width)
                            : measurer->measureTextBlock(element, label->text, width);
                        const float room = (block.widthDependent ? rect.size.y : rect.size.x) / scale + TEXT_FIT_SLACK;
                        out.writeF32("textWidth", block.size.x * scale);
                        out.writeF32("textHeight", block.size.y * scale);
                        out.writeBool("textClipped",
                                      rect.size.x <= 0.0f || rect.size.y <= 0.0f ||
                                          (block.widthDependent ? block.size.y : block.size.x) > room);
                    }
                    // the icon it shows by name: its own, else its style's in the state it paints in
                    std::string_view icon = label->icon;
                    if (icon.empty() && view)
                    {
                        icon = view->styleIconOf(element,
                                                 uiInteractionState(interactable, router.pressed() == element,
                                                                    router.hovered() == element,
                                                                    router.focused() == element))
                                   .toView();
                    }
                    if (!icon.empty())
                    {
                        out.writeString("icon", icon);
                    }
                    if (label->rich)
                    {
                        // the markup above; the text without tags and its runs, parsed here: an
                        // inspection leaves the view's cache and its parse count alone
                        UiRichText rich;
                        std::string error;
                        parseUiRichText(label->text, rich, error);
                        out.writeBool("rich", true);
                        out.writeString("plainText", rich.plain);
                        if (rich.literal)
                        {
                            out.writeString("richError", error);
                        }
                        out.beginArray("spans", static_cast<u32>(rich.runs.size()));
                        for (const UiRichRun& run : rich.runs)
                        {
                            out.beginObject("");
                            out.writeU32("begin", run.begin);
                            out.writeU32("end", run.end);
                            if (run.hasColor)
                            {
                                out.writeString("color", std::format("#{:08x}", std::byteswap(run.color)));
                            }
                            if (run.sizePx > 0.0f)
                            {
                                out.writeF32("size", run.sizePx);
                            }
                            else if (run.sizeScale != 1.0f)
                            {
                                out.writeF32("sizeScale", run.sizeScale);
                            }
                            if (run.weight != 0)
                            {
                                out.writeU32("weight", run.weight);
                            }
                            if (!run.sprite.empty())
                            {
                                out.writeString("sprite", run.sprite);
                            }
                            out.endObject();
                        }
                        out.endArray();
                    }
                }
                else if (const auto* value = world.tryGetConstComponent<UiValueComponent>(element))
                {
                    out.writeString("text", value->text);
                    if (const auto* edit = world.tryGetConstComponent<UiTextEditComponent>(element);
                        edit && !edit->composition.empty())
                    {
                        // an input method's uncommitted text at the caret
                        out.writeString("composition", edit->composition);
                    }
                    if (const auto* placeholder = world.tryGetConstComponent<UiPlaceholderComponent>(element))
                    {
                        // drawn in place of an empty text; the message when localized, with its key
                        out.writeString("placeholder", placeholder->text);
                        if (placeholder->key.isValid())
                        {
                            out.writeString("placeholderKey", placeholder->key.toView());
                        }
                    }
                }
                if (const auto* localized = world.tryGetConstComponent<UiLocalizedTextComponent>(element))
                {
                    // text above is the resolved message; a key the tables lack shows as the key itself
                    out.writeString("locKey", localized->key.toView());
                    if (!localized->args.empty())
                    {
                        out.writeString("locArgs", localized->args);
                    }
                    const Localization* localization = hierarchy.localization();
                    out.writeBool("locMissing", localization && !localization->contains(localized->key));
                }
                if (const std::optional<UiShownImage> image = view ? view->shownImage(element) : std::nullopt)
                {
                    // the name as authored ("atlas/sprite" or a texture asset; none for a bindless index registered
                    // from code) and whether it resolved to a sprite
                    if (image->name.isValid())
                    {
                        out.writeString("image", image->name.toView());
                    }
                    out.writeString("imageState",
                                    image->state == UiImageState::Ready        ? "ready"
                                        : image->state == UiImageState::Failed ? "failed"
                                                                               : "pending");
                    if (image->state == UiImageState::Ready)
                    {
                        out.writeString("imageKind", image->sprite ? "sprite" : "texture");
                    }
                }
                if (const auto* numeric = world.tryGetConstComponent<UiNumericComponent>(element))
                {
                    out.writeF64("number", numeric->value);
                }
                if (const auto* toggle = world.tryGetConstComponent<UiToggleComponent>(element))
                {
                    out.writeBool("checked", toggle->checked);
                }
                if (const auto* slider = world.tryGetConstComponent<UiSliderComponent>(element))
                {
                    out.writeF64("value", slider->value);
                    out.writeF64("min", slider->min);
                    out.writeF64("max", slider->max);
                    out.writeF64("step", slider->step);
                }
                if (const auto* progress = world.tryGetConstComponent<UiProgressComponent>(element))
                {
                    out.writeF64("value", progress->value);
                    out.writeF64("min", progress->min);
                    out.writeF64("max", progress->max);
                }
                if (const auto* group = world.tryGetConstComponent<UiRadioGroupComponent>(element))
                {
                    out.writeI32("selected", group->selected);
                }
                if (const auto* bar = world.tryGetConstComponent<UiTabBarComponent>(element))
                {
                    out.writeI32("active", bar->active);
                }
                if (const auto* tab = world.tryGetConstComponent<UiTabComponent>(element))
                {
                    out.writeBool("active", tab->active);
                    out.writeU32("index", tab->index);
                }
                if (const auto* dropdown = world.tryGetConstComponent<UiDropdownComponent>(element))
                {
                    out.writeI32("selected", dropdown->selected);
                    out.writeBool("open", dropdown->open);
                    out.writeU32("count", dropdown->count);
                    if (dropdown->open)
                    {
                        out.writeI32("highlighted", dropdown->highlighted);
                    }
                    if (!dropdown->filter.empty())
                    {
                        out.writeString("filter", dropdown->filter);
                    }
                }
                if (const auto* radio = world.tryGetConstComponent<UiRadioItemComponent>(element))
                {
                    out.writeBool("checked", radio->checked);
                    out.writeU32("index", radio->index);
                }
                if (const auto* canvas = world.tryGetConstComponent<UiCanvasComponent>(element))
                {
                    out.beginObject("canvas");
                    out.writeU64("generation", canvas->generation);
                    out.writeU32("primitives", canvas->primitives);
                    out.writeU32("maxPrimitives", canvas->maxPrimitives);
                    out.writeU32("dropped", canvas->dropped);
                    out.writeU32("droppedRegions", canvas->droppedRegions);
                    // its series buffers: held samples and what the series primitives show
                    u32 seriesCount = 0;
                    for (u32 slot = 0; series && slot < series->slotCount(); ++slot)
                    {
                        seriesCount += series->at(slot) && series->at(slot)->owner == element.id() ? 1 : 0;
                    }
                    const u32 written = std::min(seriesCount, seriesLeft);
                    seriesLeft -= written;
                    out.writeU32("seriesCount", seriesCount);
                    out.beginArray("series", written);
                    u32 left = written;
                    for (u32 slot = 0; left != 0 && slot < series->slotCount(); ++slot)
                    {
                        const UiSeries* held = series->at(slot);
                        if (!held || held->owner != element.id())
                        {
                            continue;
                        }
                        --left;
                        const auto [first, count] = held->visible();
                        out.beginObject("");
                        out.writeU32("slot", slot);
                        out.writeU32("capacity", held->capacity());
                        out.writeU32("count", held->count);
                        out.writeU64("appended", held->appended);
                        out.writeU32("shownFirst", first);
                        out.writeU32("shown", count);
                        out.writeBool("tail", held->range.tail);
                        out.writeF32("min", held->range.min);
                        out.writeF32("max", held->range.max);
                        out.endObject();
                    }
                    out.endArray();
                    out.endObject();
                }
                if (const auto* chart = world.tryGetConstComponent<UiChartComponent>(element))
                {
                    // the axes as labelled, the plot's place (unscaled UI pixels from the chart's top-left, where a
                    // pointer injection lands), the ticks, the hover per series and the selection
                    UiChartReadout readout;
                    out.beginObject("chart");
                    out.writeU32("seriesCount", chart->seriesCount);
                    out.writeBool("histogram", chart->histogram);
                    out.writeBool("xRelative", chart->xRelative);
                    out.writeBool("yAuto", chart->yAuto);
                    out.writeBool("aligned", chart->aligned);
                    out.writeF64("xMin", chart->xMin);
                    out.writeF64("xMax", chart->xMax);
                    out.writeF64("yMin", chart->yMin);
                    out.writeF64("yMax", chart->yMax);
                    out.writeU64("windowFirst", chart->windowFirst);
                    out.writeU32("windowCount", chart->windowCount);
                    if (ui && ui->chartReadout(element, readout))
                    {
                        out.writeF32("plotX", readout.plotPosition.x);
                        out.writeF32("plotY", readout.plotPosition.y);
                        out.writeF32("plotWidth", readout.plotSize.x);
                        out.writeF32("plotHeight", readout.plotSize.y);
                        out.writeU64("outOfRange", readout.outOfRange);
                        const auto writeTicks =
                            [&out](const std::string_view key, const std::vector<UiChartTick>& ticks)
                        {
                            out.beginArray(key, static_cast<u32>(ticks.size()));
                            for (const UiChartTick& tick : ticks)
                            {
                                out.beginObject("");
                                out.writeF64("value", tick.value);
                                out.writeString("label", tick.label);
                                out.endObject();
                            }
                            out.endArray();
                        };
                        writeTicks("xTicks", readout.xTicks);
                        writeTicks("yTicks", readout.yTicks);
                        out.writeBool("hovering", readout.hovering);
                        if (readout.hovering)
                        {
                            out.beginArray("hover", static_cast<u32>(readout.hover.size()));
                            for (const UiChartHoverValue& value : readout.hover)
                            {
                                out.beginObject("");
                                out.writeString("series", value.series);
                                out.writeU64("sample", value.sample);
                                out.writeF64("x", value.x);
                                out.writeF64("value", value.value);
                                out.endObject();
                            }
                            out.endArray();
                        }
                        out.writeBool("selecting", chart->selecting);
                        if (readout.selection.active)
                        {
                            out.beginObject("selection");
                            out.writeF64("xMin", readout.selection.xMin);
                            out.writeF64("xMax", readout.selection.xMax);
                            out.writeU64("first", readout.selection.first);
                            out.writeU64("last", readout.selection.last);
                            out.endObject();
                        }
                        if (readout.band.active)
                        {
                            out.beginObject("band");
                            out.writeF64("xMin", readout.band.xMin);
                            out.writeF64("xMax", readout.band.xMax);
                            out.writeU64("first", readout.band.first);
                            out.writeU64("last", readout.band.last);
                            out.endObject();
                        }
                    }
                    out.endObject();
                }
                if (const auto* visual = world.tryGetConstComponent<UiVisualComponent>(element))
                {
                    out.beginObject("visual");
                    out.writeF32("opacity", visual->opacity);
                    out.writeF32("tintR", visual->tint.x);
                    out.writeF32("tintG", visual->tint.y);
                    out.writeF32("tintB", visual->tint.z);
                    out.writeF32("tintA", visual->tint.w);
                    out.writeF32("translateX", visual->translate.x);
                    out.writeF32("translateY", visual->translate.y);
                    out.writeF32("scaleX", visual->scale.x);
                    out.writeF32("scaleY", visual->scale.y);
                    out.endObject();
                }
                if (const auto* anim = world.tryGetConstComponent<UiStyleAnimComponent>(element))
                {
                    // the style properties whose animated value has not reached the resolved one
                    const bool moving[] = {anim->backgroundColor != anim->targetBackgroundColor,
                                           anim->textColor != anim->targetTextColor,
                                           anim->borderColor != anim->targetBorderColor,
                                           anim->shadowColor != anim->targetShadowColor,
                                           anim->borderWidth != anim->targetBorderWidth,
                                           anim->shadowOffset != anim->targetShadowOffset,
                                           anim->shadowBlur != anim->targetShadowBlur,
                                           anim->opacity != anim->targetOpacity,
                                           anim->outlineColor != anim->targetOutlineColor,
                                           anim->outlineWidth != anim->targetOutlineWidth,
                                           anim->outlineOffset != anim->targetOutlineOffset,
                                           anim->cornerRadius != anim->targetCornerRadius,
                                           anim->shadowSpread != anim->targetShadowSpread};
                    static_assert(std::size(moving) == UI_TRANSITION_PROPERTY_NAMES.size());
                    out.beginArray("transitions", static_cast<u32>(std::ranges::count(moving, true)));
                    for (sizet i = 0; i < std::size(moving); ++i)
                    {
                        if (moving[i])
                        {
                            out.writeString("", UI_TRANSITION_PROPERTY_NAMES[i]);
                        }
                    }
                    out.endArray();
                }
                if (dropTarget)
                {
                    out.writeString("dropTarget", uiDropStateName(dropTarget->state));
                }
                if (router.blockedByModal(element))
                {
                    out.writeBool("blocked", true); // below the open modal: it gets no hits, wheel or focus
                }
                if (const auto* overlay = world.tryGetConstComponent<UiOverlayComponent>(element))
                {
                    out.beginObject("overlay");
                    out.writeString("kind", overlay->kind == UiOverlayKind::Modal ? "modal" : "tooltip");
                    out.writeBool("open", overlay->open);
                    if (overlay->target != Entity::undefined())
                    {
                        out.writeU64("target", overlay->target.id());
                    }
                    out.writeString("side", uiPlacementSideName(overlay->side));
                    out.writeString("placed", uiPlacementSideName(overlay->placed));
                    out.writeBool("flipped", overlay->flipped);
                    out.writeBool("clamped", overlay->clamped);
                    out.writeBool("follow", overlay->follow);
                    out.writeBool("closeOnEscape", overlay->closeOnEscape);
                    out.writeBool("closeOnBackdrop", overlay->closeOnBackdrop);
                    out.writeU32("stack", overlay->stack);
                    out.endObject();
                }
                if (const auto* tooltip = world.tryGetConstComponent<UiTooltipComponent>(element))
                {
                    out.beginObject("tooltip");
                    out.writeU64("overlay", tooltip->overlay.id());
                    out.writeF32("delay", tooltip->delay);
                    out.writeBool("shown", tooltip->shown);
                    if (tooltip->key.isValid())
                    {
                        out.writeString("key", tooltip->key.toView());
                        out.writeString("shortcut", tooltip->shortcut);
                    }
                    out.endObject();
                }
                if (const auto* navigation = world.tryGetConstComponent<UiNavigationComponent>(element))
                {
                    // explicit neighbours of directional navigation, by entity (0: none)
                    out.beginObject("neighbors");
                    out.writeU64("up", navigation->up.id());
                    out.writeU64("down", navigation->down.id());
                    out.writeU64("left", navigation->left.id());
                    out.writeU64("right", navigation->right.id());
                    out.endObject();
                }
                if (const auto* scroll = world.tryGetConstComponent<UiScrollComponent>(element))
                {
                    out.beginObject("scroll");
                    out.writeF32("x", scroll->offset.x);
                    out.writeF32("y", scroll->offset.y);
                    out.endObject();
                }
                out.writeU32("childCount", node.childCount);
                if (!node.kids.empty())
                {
                    out.beginArray("children", static_cast<u32>(node.kids.size()));
                    for (const u32 kid : node.kids)
                    {
                        write(out, router, "", kid);
                    }
                    out.endArray();
                }
                out.endObject();
            }
        };

        using Json = nlohmann::ordered_json;

        // A JSON value into a reply, as nested members.
        void writeJson(IFormatWriter& out, const std::string_view name, const Json& value)
        {
            if (value.is_object())
            {
                out.beginObject(name);
                for (const auto& [key, member] : value.items())
                {
                    writeJson(out, key, member);
                }
                out.endObject();
            }
            else if (value.is_array())
            {
                out.beginArray(name, static_cast<u32>(value.size()));
                for (const Json& member : value)
                {
                    writeJson(out, "", member);
                }
                out.endArray();
            }
            else if (value.is_string())
            {
                out.writeString(name, value.get_ref<const std::string&>());
            }
            else if (value.is_boolean())
            {
                out.writeBool(name, value.get<bool>());
            }
            else if (value.is_number_unsigned())
            {
                out.writeU64(name, value.get<u64>());
            }
            else if (value.is_number_integer() && value.get<std::int64_t>() >= std::numeric_limits<i32>::min())
            {
                out.writeI32(name, static_cast<i32>(value.get<std::int64_t>()));
            }
            else if (value.is_number())
            {
                out.writeF64(name, value.get<double>());
            }
            else
            {
                out.writeNull(name);
            }
        }

        // A .json file under the ui root. Empty plus an error when refused.
        std::filesystem::path documentPath(const ControlServer& control, const std::string& name, std::string& error)
        {
            std::filesystem::path file;
            error = control.resolveFile("ui", name, file);
            if (error.empty() && file.extension() != ".json")
            {
                error = "path '" + name + "' must name a .json file under assets/ui";
                return {};
            }
            return file;
        }

        void writeProblems(IFormatWriter& out, const UiDocumentView& view)
        {
            out.beginArray("problems", static_cast<u32>(view.problems().size()));
            for (const UiDocumentProblem& problem : view.problems())
            {
                out.beginObject("");
                out.writeString("path", problem.path);
                out.writeString("message", problem.message);
                out.writeBool("error", problem.error);
                out.endObject();
            }
            out.endArray();
            out.beginArray("codeOnly", static_cast<u32>(view.codeOnly().size()));
            for (const InternedString id : view.codeOnly())
            {
                out.writeString("", id.toView());
            }
            out.endArray();
        }

        void writeStats(IFormatWriter& out, const UiDocumentStats& stats)
        {
            out.beginObject("stats");
            out.writeU64("applies", stats.applies);
            out.writeU64("refused", stats.refused);
            out.writeU32("nodes", stats.nodes);
            out.writeU32("created", stats.created);
            out.writeU32("updated", stats.updated);
            out.writeU32("unchanged", stats.unchanged);
            out.writeU32("destroyed", stats.destroyed);
            out.writeU32("fieldsWritten", stats.fieldsWritten);
            out.writeF64("parseMs", stats.parseMs);
            out.writeF64("applyMs", stats.applyMs);
            out.endObject();
        }
    } // namespace

    void registerUiInspectCommands(ControlServer& control, UiOfFn uiOf)
    {
        const auto resolve = [uiOf](const ControlArgs& args, UiContext*& ui)
        {
            const std::string world = args.string("world").value_or(std::string{});
            ui = uiOf(world);
            return ui ? std::string{} : "no UI for world '" + world + "' (a UI world, scene, or play while playing)";
        };

        control.registerQuery(
            "uiTree", "engine",
            "a UI's element tree as nested JSON: id, kind, resolved rect (UI pixels) and the clip rect an ancestor "
            "gives it (clip and its clipOwner entity, when clipped), visibility, interaction state, role, name, "
            "selected, "
            "sort and expanded, "
            "style class, text (a label also textWidth and textHeight, its text measured at its own width, in rect "
            "units, and textClipped: the text exceeds its rect, so it is cut or ellipsized; a text input also its "
            "placeholder, the hint drawn while its text is empty, and placeholderKey when localized), the image or "
            "sprite it "
            "shows, running style transitions and the layout fields bound to "
            "theme tokens (layoutTokens: token, value, resolved) per element; layoutTokenPasses counts the rebinds a "
            "token table change ran; bounded by "
            "depth and maxNodes; canvasPrimitives and canvasBudget give the primitives all canvases hold together and "
            "the most they may; seriesSamples, seriesBudget and seriesRefused the same for series buffers; "
            "a canvas lists at most maxNodes series in all (seriesCount is its full number)",
            {optionalArg("root", ArgKind::String, "the subtree's root: an entity id, an element id or an id path",
                         "the surfaces"),
             optionalArg("depth", ArgKind::Integer, "levels below the root to include (0: the root alone)", "8"),
             optionalArg("visibleOnly", ArgKind::Boolean, "drop hidden elements and their subtrees", "false"),
             optionalArg("maxNodes", ArgKind::Integer, "elements to write at most; truncated says when cut", "400"),
             UI_WORLD_ARG},
            [resolve, uiOf](const ControlArgs& args, IFormatWriter& out)
            {
                UiContext* ui = nullptr;
                if (std::string error = resolve(args, ui); !error.empty())
                {
                    return error;
                }
                const UiHierarchyManager& hierarchy = ui->hierarchy();
                std::vector<Entity> roots;
                if (const auto root = args.string("root"))
                {
                    const Target target = targetOf(*ui, args, "root", "root");
                    if (!target.error.empty())
                    {
                        std::string world = args.string("world").value_or("");
                        if (world.empty())
                            world = ui == uiOf("editor") ? "editor" : ui == uiOf("scene") ? "scene" : "default UI";
                        const UiContext* play = uiOf("play");
                        const bool foundInPlay =
                            play && play != ui && !args.integer("root") && findById(*play, *root).element.isValid();
                        const std::string_view hint = foundInPlay ? "; use world=play to inspect the game HUD" : "";
                        return target.error + " in world '" + world + "'" + std::string(hint);
                    }
                    roots.push_back(target.element);
                }
                else
                {
                    for (const Entity element : hierarchy.paintOrderOf())
                    {
                        if (hierarchy.world().isEntityAlive(element) &&
                            hierarchy.parentOf(element) == Entity::undefined())
                        {
                            roots.push_back(element);
                        }
                    }
                }
                ServiceLocator& services = hierarchy.world().services();
                const auto viewService =
                    services.contains<UiViewContext>() ? std::optional(services.read<UiViewContext>()) : std::nullopt;
                TreePlan plan{hierarchy, viewService ? &viewService->get() : nullptr,
                              args.boolean("visibleOnly").value_or(false),
                              static_cast<u32>(args.integer("depth").value_or(DEFAULT_DEPTH)),
                              std::max<u32>(1, static_cast<u32>(args.integer("maxNodes").value_or(DEFAULT_MAX_NODES)))};
                plan.series = &ui->series();
                plan.ui = ui;
                plan.seriesLeft = plan.budget;
                std::vector<u32> rootNodes;
                for (const Entity root : roots)
                {
                    if (plan.visibleOnly && !hierarchy.isVisibleOf(root))
                    {
                        continue;
                    }
                    const i32 node = plan.collect(root, 0);
                    if (node < 0)
                    {
                        break;
                    }
                    rootNodes.push_back(static_cast<u32>(node));
                }
                out.beginArray("tree", static_cast<u32>(rootNodes.size()));
                for (const u32 node : rootNodes)
                {
                    plan.write(out, ui->router(), "", node);
                }
                out.endArray();
                out.writeU32("nodes", static_cast<u32>(plan.nodes.size()));
                out.writeBool("truncated", plan.truncated);
                // primitives held by all canvases together and the most they may hold
                out.writeU64("canvasPrimitives", ui->canvasPrimitives());
                out.writeU32("canvasBudget", ui->canvasBudget());
                // samples held by all series together, the most they may hold, and the creates it refused
                out.writeU64("seriesSamples", ui->series().samples());
                out.writeU64("seriesBudget", ui->series().budget());
                out.writeU64("seriesRefused", ui->series().refused());
                // layout token passes so far: a quiet UI adds none
                out.writeU64("layoutTokenPasses", ui->layoutTokenPasses());
                // the open modal (the topmost): everything painted below it is blocked
                const Entity modal = hierarchy.modalRoot();
                out.writeU64("modal", hierarchy.isElement(modal) ? modal.id() : 0);
                if (hierarchy.isElement(modal))
                {
                    out.writeString("modalId", idOf(hierarchy.world(), modal));
                }
                writeDrag(out, *ui);
                return std::string{};
            });

        control.registerQuery(
            "uiHit", "engine", "the frontmost visible, interactable element under a UI-pixel point and its id path",
            {requiredArg("x", ArgKind::Number, "UI pixels from the left"),
             requiredArg("y", ArgKind::Number, "UI pixels from the top"), UI_WORLD_ARG},
            [resolve](const ControlArgs& args, IFormatWriter& out)
            {
                UiContext* ui = nullptr;
                if (std::string error = resolve(args, ui); !error.empty())
                {
                    return error;
                }
                const std::optional<double> x = numberArg(args, "x");
                const std::optional<double> y = numberArg(args, "y");
                if (!x || !y)
                {
                    return std::string("uiHit needs x and y numbers");
                }
                const UiHierarchyManager& hierarchy = ui->hierarchy();
                const World& world = hierarchy.world();
                const Entity hit = ui->hitTest({static_cast<float>(*x), static_cast<float>(*y)});
                out.writeU64("entity", hit == Entity::undefined() ? 0 : hit.id());
                if (hit == Entity::undefined())
                {
                    return std::string{};
                }
                const auto* identity = world.tryGetConstComponent<UiIdentityComponent>(hit);
                out.writeString("id", identity ? identity->id.toView() : std::string_view{});
                out.writeString("kind", identity ? uiWidgetKindName(identity->kind) : std::string_view{});
                std::vector<std::string_view> chain;
                for (Entity current = hit; current != Entity::undefined(); current = hierarchy.parentOf(current))
                {
                    chain.push_back(idOf(world, current));
                }
                std::string path;
                for (auto it = chain.rbegin(); it != chain.rend(); ++it)
                {
                    path += (path.empty() ? "" : "/");
                    path += *it;
                }
                out.writeString("path", path);
                writeRect(out, hierarchy.absoluteRectOf(hit));
                if (hierarchy.isElement(hierarchy.modalRoot()))
                {
                    // everything below the modal is unreachable, so a point over it hits the modal's own surface
                    out.writeU64("modal", hierarchy.modalRoot().id());
                }
                return std::string{};
            });

        static constexpr std::array<std::string_view, 4> ACTIONS = {"click", "focus", "setValue", "scrollIntoView"};
        static_assert(ACTIONS.size() == static_cast<sizet>(UiContext::Action::ScrollIntoView) + 1);
        control.registerCommand(
            "uiActivate", "engine",
            "acts on a UI element as a person's input would: click (queued, fires at the next frame), focus, setValue "
            "(a text input or number field: select-all, insert, commit; a checkbox or toggle: true or false; a slider "
            "or progress bar: a number; a radio group, tab bar, dropdown or combo: an item index, id or (dropdown, "
            "combo) label; a chart: hover <x>, select <x0> <x1> or clear, in x units) or scrollIntoView",
            {optionalArg("entity", ArgKind::Entity, "the UI element"), ID_ARG,
             requiredArg("action", ArgKind::String, "what to do", ACTIONS),
             optionalArg("value", ArgKind::String, "setValue's text, or the number, boolean or index as text"),
             UI_WORLD_ARG},
            [resolve](const ControlArgs& args, IFormatWriter& out)
            {
                UiContext* ui = nullptr;
                if (std::string error = resolve(args, ui); !error.empty())
                {
                    return error;
                }
                const Target target = targetOf(*ui, args, "id", "entity");
                if (!target.error.empty())
                {
                    return target.error;
                }
                if (target.element == Entity::undefined())
                {
                    return std::string("uiActivate needs entity or id");
                }
                const std::string name = args.string("action").value_or(std::string{});
                const auto action = std::ranges::find(ACTIONS, name);
                if (action == ACTIONS.end())
                {
                    return std::string("action is click, focus, setValue or scrollIntoView");
                }
                const auto kind = static_cast<UiContext::Action>(action - ACTIONS.begin());
                const std::optional<std::string> value = args.string("value");
                if (kind == UiContext::Action::SetValue && !value)
                {
                    return std::string("setValue needs value");
                }
                if (std::string error = ui->activate(target.element, kind, value.value_or(std::string{}));
                    !error.empty())
                {
                    return error;
                }
                const World& world = ui->hierarchy().world();
                out.writeU64("entity", target.element.id());
                out.writeString("id", idOf(world, target.element));
                out.writeString("action", name);
                if (kind == UiContext::Action::Click)
                {
                    out.writeBool("queued", true);
                }
                else if (kind == UiContext::Action::SetValue)
                {
                    if (const auto* text = world.tryGetConstComponent<UiValueComponent>(target.element))
                    {
                        out.writeString("text", text->text);
                    }
                }
                return std::string{};
            });

        constexpr sizet MAX_DRAG_TEXT = 4096;
        static constexpr std::array<std::string_view, 5> DRAG_PHASES = {"all", "begin", "over", "drop", "cancel"};
        control.registerCommand(
            "uiDrag", "engine",
            "drives a drag and drop as a person's pointer would: begin at a source element (its payload, "
            "or "
            "the one given by type), hover a target element or point, drop (an accepting target gets its drop event) "
            "or "
            "cancel; phase all does begin, over and drop at once. uiTree's drag shows the one in flight",
            {optionalArg("from", ArgKind::String, "the source: an element id or id path", "none"),
             optionalArg("fromEntity", ArgKind::Entity, "the source element, instead of from"),
             optionalArg("to", ArgKind::String, "the element to hover (its nearest drop target takes the drop)",
                         "none"),
             optionalArg("toEntity", ArgKind::Entity, "the element to hover, instead of to"),
             optionalArg("x", ArgKind::Number, "hover this UI-pixel point instead of an element", "none"),
             optionalArg("y", ArgKind::Number, "with x", "none"),
             optionalArg("phase", ArgKind::String, "which part of the drag to do", "all", DRAG_PHASES),
             optionalArg("type", ArgKind::String, "begin with this payload type instead of the source's own payload",
                         "the source's payload"),
             optionalArg("text", ArgKind::String, "the payload's text (names or ids, newline-separated)", "empty"),
             optionalArg("label", ArgKind::String, "the payload's preview label", "the text"),
             optionalArg("data", ArgKind::Integer, "the payload's number", "0"),
             optionalArg("payloadEntity", ArgKind::Entity, "the payload's entity", "none"), UI_WORLD_ARG},
            [resolve](const ControlArgs& args, IFormatWriter& out)
            {
                UiContext* ui = nullptr;
                if (std::string error = resolve(args, ui); !error.empty())
                {
                    return error;
                }
                const std::string phase = args.string("phase").value_or("all");
                if (std::ranges::find(DRAG_PHASES, phase) == DRAG_PHASES.end())
                {
                    return std::string("phase is all, begin, over, drop or cancel");
                }
                UiDragDrop& dragDrop = ui->dragDrop();
                const UiHierarchyManager& hierarchy = ui->hierarchy();
                const bool begins = phase == "all" || phase == "begin";
                const bool hovers = phase == "all" || phase == "over";
                Target from = targetOf(*ui, args, "from", "fromEntity");
                Target to = targetOf(*ui, args, "to", "toEntity");
                for (const Target* target : {&from, &to})
                {
                    if (!target->error.empty())
                    {
                        return target->error;
                    }
                }
                const std::optional<double> x = numberArg(args, "x");
                const std::optional<double> y = numberArg(args, "y");
                if (x.has_value() != y.has_value())
                {
                    return std::string("x and y go together");
                }
                const bool hasPoint = x.has_value();
                const bool hasTarget = to.element != Entity::undefined();
                const bool hoversNow = hasTarget || hasPoint;
                if (begins && from.element == Entity::undefined())
                {
                    return std::string("uiDrag needs from (or fromEntity) to begin");
                }
                if (begins && !hierarchy.isVisibleOf(from.element))
                {
                    return std::string("the source is hidden or not laid out yet");
                }
                if (begins && ui->router().blockedByModal(from.element))
                {
                    return std::string("the source is below the open modal");
                }
                if (begins)
                {
                    const auto* sourceState = hierarchy.world().tryGetConstComponent<UiStateComponent>(from.element);
                    if (sourceState && !sourceState->interactable)
                    {
                        return std::string("the source is not interactable");
                    }
                    for (const char* key : {"type", "text", "label"})
                    {
                        if (args.string(key).value_or(std::string{}).size() > MAX_DRAG_TEXT)
                        {
                            return std::string("uiDrag ") + key + " is limited to 4096 bytes";
                        }
                    }
                }
                if (hovers && !hoversNow)
                {
                    return std::string("uiDrag needs to, toEntity or x and y to hover");
                }
                if (hasTarget && !hierarchy.isVisibleOf(to.element))
                {
                    return std::string("the target is hidden or not laid out yet");
                }
                if (phase != "begin" && phase != "all" && !dragDrop.active() && phase != "cancel")
                {
                    return std::string("no drag is in flight");
                }
                if (begins && dragDrop.active())
                {
                    return std::string("a drag is in flight; phase cancel ends it");
                }
                if (begins)
                {
                    const UiRect rect = hierarchy.absoluteRectOf(from.element);
                    const Vec2 start = rect.position + rect.size * 0.5f;
                    bool started = false;
                    if (const std::optional<std::string> type = args.string("type"))
                    {
                        UiDragPayload payload;
                        payload.type = InternedString(*type);
                        payload.text = args.string("text").value_or(std::string{});
                        payload.label = args.string("label").value_or(payload.text);
                        payload.data = args.integer("data").value_or(0);
                        if (const std::optional<u64> entity = args.integer("payloadEntity"))
                        {
                            payload.entity = Entity(*entity);
                        }
                        started = dragDrop.begin(from.element, std::move(payload), start);
                    }
                    else
                    {
                        started = dragDrop.begin(from.element, start);
                    }
                    if (!started)
                    {
                        return std::string("the element is no drag source, or its payload was refused");
                    }
                }
                if (hoversNow && phase != "begin" && phase != "cancel")
                {
                    if (hasTarget)
                    {
                        dragDrop.moveTo(to.element);
                    }
                    else
                    {
                        dragDrop.move({static_cast<float>(*x), static_cast<float>(*y)});
                    }
                }
                bool dropped = false;
                if (phase == "all" || phase == "drop")
                {
                    dropped = dragDrop.drop();
                    out.writeBool("dropped", dropped);
                }
                else if (phase == "cancel")
                {
                    dragDrop.cancel();
                }
                out.writeBool("active", dragDrop.active());
                writeDrag(out, *ui);
                return std::string{};
            });

        static constexpr std::array<std::string_view, 8> EASES = {"linear",  "inQuad",   "outQuad",    "inOutQuad",
                                                                  "inCubic", "outCubic", "inOutCubic", "outBack"};
        static_assert(EASES.size() == static_cast<sizet>(UiEase::Count));
        control.registerCommand(
            "uiTween", "engine",
            "starts a tween of one float or vector field of a UI element's UiRect, UiVisual, UiStyleAnim, UiContent"
            "Size, UiLayout or UiLayoutItem component (a missing UiVisual is added); the "
            "uiTweens table lists it",
            {optionalArg("entity", ArgKind::Entity, "the UI element"), ID_ARG,
             requiredArg("component", ArgKind::String, "the component, e.g. UiVisual"),
             requiredArg("field", ArgKind::String,
                         "the field, e.g. opacity; an array field names its entry, e.g. shadowBlur[1]"),
             requiredArg("to", ArgKind::Json, "the end value: a number, or an array with one number per field lane"),
             optionalArg("from", ArgKind::Json, "the start value, same shape", "the field's value when the tween runs"),
             optionalArg("duration", ArgKind::Number, "seconds of one play; 0 snaps", "0.25"),
             optionalArg("delay", ArgKind::Number, "seconds before the first play", "0"),
             optionalArg("ease", ArgKind::String, "the easing curve", "outCubic", EASES),
             optionalArg("repeat", ArgKind::Integer, "plays; 0 repeats until cancelled", "1"),
             optionalArg("yoyo", ArgKind::Boolean, "every second play runs backwards", "false"),
             optionalArg("after", ArgKind::Integer, "a tween id: wait until it finished or was cancelled", "none"),
             UI_WORLD_ARG},
            [resolve](const ControlArgs& args, IFormatWriter& out)
            {
                UiContext* ui = nullptr;
                if (std::string error = resolve(args, ui); !error.empty())
                {
                    return error;
                }
                const Target target = targetOf(*ui, args, "id", "entity");
                if (!target.error.empty())
                {
                    return target.error;
                }
                if (target.element == Entity::undefined())
                {
                    return std::string("uiTween needs entity or id");
                }
                const std::string component = args.string("component").value_or(std::string{});
                const std::string field = args.string("field").value_or(std::string{});
                UiTweenDesc desc;
                desc.element = target.element;
                desc.component = component;
                desc.field = field;
                const LaneValues to = laneValues(args, "to");
                if (!to.error.empty() || to.count == 0)
                {
                    return to.error.empty() ? std::string("uiTween needs to") : to.error;
                }
                desc.to = to.value;
                desc.lanes = to.count;
                if (args.json("from"))
                {
                    const LaneValues from = laneValues(args, "from");
                    if (!from.error.empty() || from.count != to.count)
                    {
                        return from.error.empty() ? std::string("from needs as many values as to") : from.error;
                    }
                    desc.from = from.value;
                }
                if (args.json("duration"))
                {
                    const std::optional<double> duration = numberArg(args, "duration");
                    if (!duration)
                    {
                        return std::string("duration is a number of seconds");
                    }
                    desc.duration = static_cast<float>(*duration);
                }
                if (args.json("delay"))
                {
                    const std::optional<double> delay = numberArg(args, "delay");
                    if (!delay)
                    {
                        return std::string("delay is a number of seconds");
                    }
                    desc.delay = static_cast<float>(*delay);
                }
                if (const std::optional<std::string> ease = args.string("ease"))
                {
                    const std::optional<UiEase> parsed = uiEaseFromName(*ease);
                    if (!parsed)
                    {
                        return "unknown ease '" + *ease + "'";
                    }
                    desc.ease = *parsed;
                }
                const u64 repeat = args.integer("repeat").value_or(1);
                const u64 after = args.integer("after").value_or(NO_UI_TWEEN);
                if (repeat > std::numeric_limits<u32>::max() || after > std::numeric_limits<UiTweenId>::max())
                {
                    return std::string("repeat and after must fit 32 bits");
                }
                desc.repeat = static_cast<u32>(repeat);
                desc.yoyo = args.boolean("yoyo").value_or(false);
                desc.after = static_cast<UiTweenId>(after);
                const Expected<UiTweenId> id = ui->tweens().start(desc);
                if (!id)
                {
                    return id.error().message;
                }
                out.writeU32("tween", *id);
                out.writeU64("entity", target.element.id());
                return std::string{};
            });

        control.registerCommand(
            "uiTweenCancel", "engine",
            "cancels one tween by id, or every tween on an element; a cancelled tween keeps the value it reached and "
            "fires no completion event",
            {optionalArg("tween", ArgKind::Integer, "the tween id from uiTween or the uiTweens table"),
             optionalArg("entity", ArgKind::Entity, "the UI element whose tweens to cancel"), ID_ARG, UI_WORLD_ARG},
            [resolve](const ControlArgs& args, IFormatWriter& out)
            {
                UiContext* ui = nullptr;
                if (std::string error = resolve(args, ui); !error.empty())
                {
                    return error;
                }
                if (const std::optional<u64> tween = args.integer("tween"))
                {
                    if (*tween > std::numeric_limits<UiTweenId>::max() ||
                        !ui->tweens().cancel(static_cast<UiTweenId>(*tween)))
                    {
                        return "no active tween " + std::to_string(*tween);
                    }
                    out.writeU32("cancelled", 1);
                    return std::string{};
                }
                const Target target = targetOf(*ui, args, "id", "entity");
                if (!target.error.empty())
                {
                    return target.error;
                }
                if (target.element == Entity::undefined())
                {
                    return std::string("uiTweenCancel needs tween, entity or id");
                }
                out.writeU32("cancelled", ui->tweens().cancelElement(target.element));
                return std::string{};
            });

        control.registerCommand(
            "uiDismiss", "engine",
            "closes the topmost open modal as Escape would (also when it does not close on Escape), else hides the "
            "shown tooltip, and reports what closed; the uiOverlays table lists what is open",
            {UI_WORLD_ARG},
            [resolve](const ControlArgs& args, IFormatWriter& out)
            {
                UiContext* ui = nullptr;
                if (std::string error = resolve(args, ui); !error.empty())
                {
                    return error;
                }
                const World& world = ui->hierarchy().world();
                const Entity closed = ui->overlays().dismiss();
                out.writeBool("dismissed", closed != Entity::undefined());
                if (closed != Entity::undefined())
                {
                    out.writeU64("entity", closed.id());
                    out.writeString("id", idOf(world, closed));
                }
                out.writeU32("modals", static_cast<u32>(ui->overlays().modals().size()));
                return std::string{};
            });

        control.registerCommand(
            "uiReducedMotion", "engine",
            "sets the UI's reduced-motion switch (tweens snap to their end value and finish at the next update) and "
            "reports it; without enabled it only reports",
            {optionalArg("enabled", ArgKind::Boolean, "the new setting", "unchanged"), UI_WORLD_ARG},
            [resolve](const ControlArgs& args, IFormatWriter& out)
            {
                UiContext* ui = nullptr;
                if (std::string error = resolve(args, ui); !error.empty())
                {
                    return error;
                }
                if (const std::optional<bool> enabled = args.boolean("enabled"))
                {
                    ui->tweens().setReducedMotion(*enabled);
                }
                out.writeBool("reducedMotion", ui->tweens().reducedMotion());
                out.writeU32("activeTweens", ui->tweens().activeCount());
                return std::string{};
            });

        // UI documents
        control.registerQuery(
            "uiDocuments", "engine",
            "the UI's document views: name, file, root, the last apply's counts and problems, the contract's ids with "
            "the element each bound, the contract ids no element answers (codeOnly) and the document ids the contract "
            "does not name (documentOnly)",
            {optionalArg("view", ArgKind::String, "only this view", "every view"), UI_WORLD_ARG},
            [resolve](const ControlArgs& args, IFormatWriter& out)
            {
                UiContext* ui = nullptr;
                if (std::string error = resolve(args, ui); !error.empty())
                {
                    return error;
                }
                const World& world = ui->hierarchy().world();
                const std::string only = args.string("view").value_or(std::string{});
                std::vector<const UiDocumentView*> views;
                for (const UiDocumentView* view : ui->documents().views())
                {
                    if (view->adopted() && !view->rootAlive())
                    {
                        continue; // a stale view over a destroyed root
                    }
                    if (only.empty() || view->name() == only)
                    {
                        views.push_back(view);
                    }
                }
                if (!only.empty() && views.empty())
                {
                    return "no document view '" + only + "'";
                }
                out.beginArray("views", static_cast<u32>(views.size()));
                for (const UiDocumentView* view : views)
                {
                    out.beginObject("");
                    out.writeString("name", view->name());
                    out.writeString("path", utf8Path(view->path()));
                    out.writeBool("watching", view->watching());
                    out.writeBool("adopted", view->adopted());
                    const bool rooted = ui->hierarchy().isElement(view->root());
                    out.writeU64("root", rooted ? view->root().id() : 0);
                    out.writeString("rootId", rooted ? idOf(world, view->root()) : std::string_view{});
                    out.writeBool("fromDocument", !view->documentText().empty());
                    writeStats(out, view->stats());
                    const std::span<const UiViewContract::Element> contract = view->contract().elements();
                    out.beginArray("contract", static_cast<u32>(contract.size()));
                    for (const UiViewContract::Element& element : contract)
                    {
                        out.beginObject("");
                        out.writeString("id", element.id.toView());
                        out.writeString("kind", uiWidgetKindName(element.kind));
                        out.writeBool("required", element.required);
                        out.writeBool("attaches", static_cast<bool>(element.attach));
                        out.writeU64("entity", view->element(element.id).entity().id());
                        out.endObject();
                    }
                    out.endArray();
                    writeProblems(out, *view);
                    out.writeU32("documentOnlyCount", view->documentOnlyCount());
                    out.beginArray("documentOnly", static_cast<u32>(view->documentOnly().size()));
                    for (const std::string& id : view->documentOnly())
                    {
                        out.writeString("", id);
                    }
                    out.endArray();
                    out.endObject();
                }
                out.endArray();
                return std::string{};
            });

        // the view a request names: a document view by name, else the view adopting the root element
        const auto documentView = [](UiContext& ui, const ControlArgs& args, UiDocumentView*& view) -> std::string
        {
            if (const std::optional<std::string> name = args.string("view"))
            {
                view = ui.documents().find(*name);
                return view ? std::string{} : "no document view '" + *name + "'";
            }
            const Target target = targetOf(ui, args, "id", "root");
            if (!target.error.empty())
            {
                return target.error;
            }
            if (target.element == Entity::undefined())
            {
                return "name a view, or the root element by root (entity) or id";
            }
            if (!ui.hierarchy().world().hasComponent<UiIdentityComponent>(target.element))
            {
                return "the root has no id";
            }
            // the root of a document view is that view's
            const std::span<UiDocumentView* const> views = ui.documents().views();
            const auto owner = std::ranges::find(views, target.element, &UiDocumentView::root);
            view = owner != views.end() ? *owner : &ui.documents().adopt(target.element);
            return {};
        };

        control.registerCommand(
            "uiExport", "engine",
            "writes a live UI tree as a UI document: a document view's tree, or any element's subtree; "
            "every element by id with its kind and the fields that differ from the kind's defaults, a composite "
            "widget's parts left out and what a document cannot build marked opaque; with path, also saved under "
            "assets/ui",
            {optionalArg("view", ArgKind::String, "a document view (uiDocuments)", "root or id"),
             optionalArg("root", ArgKind::Entity, "the subtree's root element"), ID_ARG,
             optionalPath("path", "ui", "save it as this .json file under assets/ui", "not saved"),
             optionalArg("overwrite", ArgKind::Boolean, "replace an existing file at path", "false"), UI_WORLD_ARG},
            [resolve, &control](const ControlArgs& args, IFormatWriter& out)
            {
                UiContext* ui = nullptr;
                if (std::string error = resolve(args, ui); !error.empty())
                {
                    return error;
                }
                Entity root = Entity::undefined();
                if (const std::optional<std::string> name = args.string("view"))
                {
                    const UiDocumentView* view = ui->documents().find(*name);
                    if (!view)
                    {
                        return "no document view '" + *name + "'";
                    }
                    root = view->root();
                }
                else
                {
                    const Target target = targetOf(*ui, args, "id", "root");
                    if (!target.error.empty())
                    {
                        return target.error;
                    }
                    root = target.element;
                }
                std::string unwritable;
                const std::string text = exportUiDocument(*ui, root, &unwritable);
                if (text.empty())
                {
                    return unwritable.empty() ? std::string("name a view, or a root element with an id, by root or id")
                                              : "the tree cannot be exported as a document: " + unwritable;
                }
                if (const std::optional<std::string> name = args.string("path"))
                {
                    std::string error;
                    const std::filesystem::path path = documentPath(control, *name, error);
                    if (path.empty())
                    {
                        return error;
                    }
                    std::error_code exists;
                    const bool present = std::filesystem::exists(path, exists);
                    if (exists)
                    {
                        return "cannot check '" + *name + "': " + exists.message();
                    }
                    if (present && !args.boolean("overwrite").value_or(false))
                    {
                        return "'" + *name + "' exists: pass overwrite=true to replace it";
                    }
                    if (const Expected<void> written =
                            ensureDirectory(path.parent_path())
                                .and_then([&] { return writeTextFileAtomically(path, text); });
                        !written)
                    {
                        return "cannot write '" + *name + "'";
                    }
                    out.writeString("path", utf8Path(path));
                }
                out.writeU64("root", root.id());
                out.writeU64("bytes", text.size());
                writeJson(out, "document", Json::parse(text));
                return std::string{};
            });

        control.registerUndoableViewCommand(
            "uiImport", "engine",
            "applies a UI document to a document view, or to the code-built tree under a root element (adopted: its "
            "elements are matched by id and written, the document's own added, never the code's destroyed), by id-path "
            "diff: unchanged elements keep entity, focus, scroll and tweens. A refused document (limits, malformed, "
            "unknown kind, wrong type, missing contract id) leaves the tree and reports why. Undo applies the "
            "previous document",
            {optionalArg("view", ArgKind::String, "a document view (uiDocuments)", "root or id"),
             optionalArg("root", ArgKind::Entity, "the root element of a code-built tree"), ID_ARG,
             optionalArg("document", ArgKind::Json,
                         "the document as a JSON string (kept byte for byte: duplicate keys are refused). An object "
                         "argument is parsed by the control layer first, where the last duplicate key wins",
                         "path"),
             optionalPath("path", "ui",
                          "a .json file under assets/ui; without document and path, a view re-reads its own file",
                          "the view's file")},
            [resolve, documentView, &control](const ControlArgs& args, IFormatWriter& out)
            {
                UiContext* ui = nullptr;
                if (std::string error = resolve(args, ui); !error.empty())
                {
                    return error;
                }
                UiDocumentView* view = nullptr;
                if (std::string error = documentView(*ui, args, view); !error.empty())
                {
                    return error;
                }
                std::string text;
                if (const std::optional<std::string> document = args.json("document"))
                {
                    const Json parsed = Json::parse(*document, nullptr, false);
                    text = parsed.is_string() ? parsed.get<std::string>() : *document;
                }
                else
                {
                    std::filesystem::path path = view->path();
                    if (const std::optional<std::string> name = args.string("path"))
                    {
                        std::string error;
                        path = documentPath(control, *name, error);
                        if (path.empty())
                        {
                            return error;
                        }
                    }
                    if (path.empty())
                    {
                        return std::string("uiImport needs document or path (the view has no file)");
                    }
                    std::string why;
                    if (!readUiDocumentFile(path, text, why))
                    {
                        return "cannot read '" + utf8Path(path) + "': " + why;
                    }
                }
                const std::string previous = view->documentText().empty() ? view->exportText() : view->documentText();
                if (previous.empty())
                {
                    return std::string("the tree cannot be exported, so the import could not be undone: refused");
                }
                const std::string name = view->name();
                const Entity root = view->root();
                const bool adopted = view->adopted();
                if (!view->apply(text))
                {
                    writeProblems(out, *view);
                    std::string why;
                    for (const UiDocumentProblem& problem : view->problems())
                    {
                        if (problem.error)
                        {
                            why = (problem.path.empty() ? "" : problem.path + ": ") + problem.message;
                            break;
                        }
                    }
                    return "the document was refused: " + why;
                }
                // undo applies what was there; redo and replay apply this text, whatever the file holds later
                const auto request = [&](const std::string& document)
                {
                    Json json = Json::object();
                    if (adopted)
                    {
                        json["root"] = root.id();
                    }
                    else
                    {
                        json["view"] = name;
                    }
                    // the text as a JSON string: kept byte for byte (an object would be re-parsed, its keys reordered,
                    // and the history entry sized by a DOM)
                    json["document"] = document;
                    return json.dump(-1, ' ', false, Json::error_handler_t::replace);
                };
                if (!previous.empty())
                {
                    control.recordInverse("uiImport", request(previous));
                    control.recordForward(request(text));
                }
                out.writeString("view", view->name());
                out.writeU64("root", view->root().id());
                writeStats(out, view->stats());
                writeProblems(out, *view);
                return std::string{};
            });
    }
} // namespace spite
