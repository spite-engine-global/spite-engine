#include "EditorSystemsGraph.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "ui/UiCanvas.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiTextMeasurer.hpp"

namespace spite
{
    using namespace panelui;

    namespace
    {
        // HID scancodes of the keys the graph answers
        namespace Key
        {
            constexpr u32 MINUS = 45, EQUALS = 46;
            constexpr u32 HOME = 74, RIGHT = 79, LEFT = 80, DOWN = 81, UP = 82, KP_MINUS = 86, KP_PLUS = 87;
        } // namespace Key

        constexpr float ZOOM_NOTCH = 1.15f;
        constexpr float MAX_NOTCHES = 4.0f;
        constexpr float MIN_TEXT_ZOOM = 0.45f; // below it the nodes are bars without names
        constexpr float LANE_ROWS_WIDE = 12.0f; // the stage choice's width before its labels are measured, in rows
        constexpr float LANE_ROWS_MIN = 4.0f; // and the least it shrinks to
        constexpr float MIN_CANVAS_ROWS = 2.5f; // 4 before the 26 px row: the graph still fits a 1024x600 bottom group
        constexpr u32 MAX_DASHES = 96; // a conflict edge's dashes at most
        constexpr float DISABLED_ALPHA = 0.5f;
        constexpr float EDGE_WIDTH = 1.5f;
        constexpr float THICK = 2.0f; // a border, a highlighted edge
        constexpr u32 LISTED = 6; // names a neighbour or access line lists before "+N"
        constexpr u32 STATE_NODES = 1000; // nodes and edges of the shown lane the query lists
        constexpr u32 STATE_EDGES = 4000;
        constexpr u32 STATE_CONFLICTS = 500;
        constexpr float IMPLIED_ALPHA = 0.45f;
        constexpr std::string_view NONE = "-";

        u32 withAlpha(const u32 color, const float factor)
        {
            const auto alpha = static_cast<u32>(std::lround(static_cast<float>(color >> 24) * factor));
            return (color & 0x00FFFFFFu) | (alpha << 24);
        }

        std::string joined(const std::vector<std::string>& names)
        {
            std::string text;
            for (u32 i = 0; i < names.size() && i < LISTED; ++i)
            {
                text += (i == 0 ? "" : ", ") + names[i];
            }
            if (names.size() > LISTED)
            {
                text += std::format(", +{}", names.size() - LISTED);
            }
            return text.empty() ? std::string(NONE) : text;
        }

        // at most `fit` UTF-8 bytes of the name, ending in "..." when it was cut
        std::string clipped(const std::string& text, const float width, const float fit)
        {
            if (width <= fit)
            {
                return text;
            }
            sizet keep = static_cast<sizet>(static_cast<float>(text.size()) * std::max(fit, 0.0f) / width);
            keep = keep > 3 ? keep - 3 : 0;
            while (keep > 0 && (static_cast<unsigned char>(text[keep]) & 0xC0) == 0x80)
            {
                --keep;
            }
            return keep == 0 ? std::string{} : text.substr(0, keep) + "...";
        }

        std::string laneLabel(const SystemsGraphLane& lane)
        {
            return std::format("{} / {} ({})", SYSTEM_PHASES[lane.phase], lane.stageName, lane.nodeCount);
        }

        void writeStrings(IFormatWriter& out, const std::string_view key, const std::vector<std::string>& names)
        {
            out.beginArray(key, static_cast<u32>(names.size()));
            for (const std::string& name : names)
            {
                out.writeString("", name);
            }
            out.endArray();
        }
    } // namespace

    SystemsGraphView::SystemsGraphView(SystemsGraphHost host, const SystemsGraph& graph) :
        m_host(std::move(host)), m_graph(&graph), m_density(m_host.density->density())
    {
        UiScope& scope = *m_host.scope;
        DensityFollower& density = *m_host.density;
        m_root = stackPanel(scope, "systems.graph", Vec2{}, UiLayoutDirection::Column, UiLayoutAlign::Stretch);
        m_root.setLayoutItem(fillItem());
        m_root.setVisible(false);
        m_bar = density.track(stackPanel(scope, "systems.graph.bar", Vec2{}, UiLayoutDirection::Row), HEIGHT_CONTROL);
        m_bar.setParent(m_root);

        UiDropdownDesc choice;
        choice.rect = barDesc(Vec2{LANE_ROWS_WIDE * m_density.row, 0.0f});
        m_laneChoice = density.track(scope.createDropdown("systems.graph.lane", choice), HEIGHT_CONTROL);
        m_laneChoice.setParent(m_bar);
        m_laneChoice.setAccessibleNameKey(InternedString("editor-systems.graph.lane"));
        m_laneChoice.onChange(
            [this](const u32 index)
            {
                if (index < m_graph->lanes.size())
                {
                    const SystemsGraphLane& chosen = m_graph->lanes[index];
                    m_host.send(Json{
                        {"command", "systemsLane"},
                        {"args", {{"phase", std::string(SYSTEM_PHASES[chosen.phase])}, {"stage", chosen.stageName}}}}
                                    .dump());
                }
            });

        UiRectDesc grow = barDesc(Vec2{});
        grow.layoutItem = fillItem();
        m_conflicts = density.track(scope.createLabel("systems.graph.conflicts", "", grow), HEIGHT_CONTROL);
        m_conflicts.setParent(m_bar);
        m_conflicts.setStyleClass(InternedString("list.filter.count"));
        m_conflicts.setLocalizedText(InternedString("editor-systems.graph.conflicts"));
        m_conflicts.setLocalizedArg("count", std::int64_t{0});

        UiCanvasDesc canvas;
        canvas.rect = topLeft();
        canvas.rect.layoutItem = fillItem();
        canvas.rect.layoutItem->minSize = Vec2{0.0f, MIN_CANVAS_ROWS * m_density.row};
        canvas.maxPrimitives = 60000;
        m_canvas = scope.createCanvas("systems.graph.canvas", canvas);
        m_canvas.setParent(m_root);
        m_canvas.setStyleClass(InternedString("systems.dense.text"));
        m_canvas.setAccessibleNameKey(InternedString("editor-systems.graph"));
        scope.world().getComponent<UiStateComponent>(m_canvas.entity()).tabStop = true;
        m_canvas.onPointer([this](const UiCanvasPointer& pointer) { pointerEvent(pointer); });
        m_canvas.onKeyDown([this](const u32 scancode, const u8, bool) { return keyEvent(scancode); });
        scope.hierarchy().observeSize(m_canvas.entity());

        const auto line = [&](Label& out, const char* id, const char* key)
        {
            out = density.track(scope.createLabel(id, "", barDesc(Vec2{})), HEIGHT_ROW);
            out.setParent(m_root);
            out.setStyleClass(InternedString("list.filter.count"));
            out.setLocalizedText(InternedString(key));
        };
        line(m_order, "systems.graph.order", "editor-systems.graph.hint");
        line(m_access, "systems.graph.access", "editor-systems.graph.legend");
        graphChanged();
    }

    const SystemsGraphLane* SystemsGraphView::lane() const
    {
        return m_lane < m_graph->lanes.size() ? &m_graph->lanes[m_lane] : nullptr;
    }

    float SystemsGraphView::nodeW() const { return 8.0f * m_density.row; }
    float SystemsGraphView::nodeH() const { return std::max(m_density.row, m_textHeight + 2.0f * m_density.xxs); }
    float SystemsGraphView::colStep() const { return nodeW() + 2.0f * m_density.xl; }
    float SystemsGraphView::rowStep() const { return nodeH() + m_density.xs; }

    Vec2 SystemsGraphView::nodePos(const SystemsGraphNode& node) const
    {
        return Vec2{static_cast<float>(node.depth) * colStep(), static_cast<float>(node.row) * rowStep()};
    }

    Vec2 SystemsGraphView::canvasSize() const { return resolvedSize(*m_host.scope, m_canvas); }

    void SystemsGraphView::rebuildLaneChoice()
    {
        std::vector<UiChoiceItem> items;
        items.reserve(m_graph->lanes.size());
        for (const SystemsGraphLane& each : m_graph->lanes)
        {
            items.push_back({.id = InternedString(std::format("{}/{}", SYSTEM_PHASES[each.phase], each.stageName)),
                             .label = laneLabel(each)});
        }
        m_laneChoice.setItems(items);
        m_laneChoice.setSelected(m_lane < items.size() ? static_cast<i32>(m_lane) : -1);
        m_laneChoice.setInteractable(!items.empty());
        fitLaneChoice();
    }

    // The stage names are data: the choice is as wide as its widest label (and a narrow panel may shrink it)
    void SystemsGraphView::fitLaneChoice()
    {
        const UiTextMeasurer* measurer = m_host.scope->hierarchy().textMeasurer();
        float widest = 0.0f;
        for (const SystemsGraphLane& each : m_graph->lanes)
        {
            widest =
                std::max(widest, measurer ? measurer->measureText(m_laneChoice.entity(), laneLabel(each)).x : 0.0f);
        }
        const float width =
            widest > 0.0f ? widest + 2.0f * m_density.xl + m_density.md : LANE_ROWS_WIDE * m_density.row;
        if (width == m_laneFit)
        {
            return;
        }
        m_laneFit = width;
        UiLayoutItemComponent item;
        item.basis = width;
        item.shrink = 1.0f;
        item.minSize = Vec2{LANE_ROWS_MIN * m_density.row, 0.0f};
        m_laneChoice.setLayoutItem(item);
    }

    bool SystemsGraphView::followSelection()
    {
        if (m_selected >= m_graph->nodes.size())
        {
            return false;
        }
        for (u32 i = 0; i < m_graph->lanes.size(); ++i)
        {
            const SystemsGraphLane& each = m_graph->lanes[i];
            if (m_selected >= each.firstNode && m_selected < each.firstNode + each.nodeCount)
            {
                if (i == m_lane)
                {
                    return false;
                }
                m_lane = i;
                m_lanePhase = each.phase;
                m_laneStage = each.stage;
                m_reveal = true;
                return true;
            }
        }
        return false;
    }

    void SystemsGraphView::graphChanged()
    {
        const u32 wasPhase = m_lanePhase;
        const u64 wasStage = m_laneStage;
        m_lane = 0;
        for (u32 i = 0; i < m_graph->lanes.size(); ++i)
        {
            if (m_graph->lanes[i].phase == wasPhase && m_graph->lanes[i].stage == wasStage)
            {
                m_lane = i;
            }
        }
        if (const SystemsGraphLane* current = lane())
        {
            m_lanePhase = current->phase;
            m_laneStage = current->stage;
        }
        const auto found = m_graph->index.find(m_selectedId);
        m_selected = found == m_graph->index.end() ? ~0u : found->second;
        followSelection(); // a selection made before the graph existed, or in a stage the graph kept away from
        rebuildLaneChoice();
        showInfo();
        clampView();
        m_dirty = true;
    }

    void SystemsGraphView::showInfo()
    {
        const SystemsGraphLane* current = lane();
        m_conflicts.setLocalizedArg("count", std::int64_t{current ? current->conflicts : 0});
        if (m_selected >= m_graph->nodes.size())
        {
            m_order.setLocalizedText(InternedString("editor-systems.graph.hint"));
            m_access.setLocalizedText(InternedString("editor-systems.graph.legend"));
            return;
        }
        const SystemsGraphNode& node = m_graph->nodes[m_selected];
        const auto names = [this](const std::vector<u32>& indices)
        {
            std::vector<std::string> out;
            for (const u32 i : indices)
            {
                out.push_back(m_graph->nodes[i].name);
            }
            return out;
        };
        m_order.setLocalizedText(InternedString("editor-systems.graph.order"));
        m_order.setLocalizedArgText("after", joined(names(node.preds)));
        m_order.setLocalizedArgText("before", joined(names(node.succs)));
        m_access.setLocalizedText(InternedString("editor-systems.graph.access"));
        m_access.setLocalizedArgText("reads", joined(node.reads));
        m_access.setLocalizedArgText("writes", joined(node.writes));
    }

    void SystemsGraphView::setSelected(const std::string_view id)
    {
        if (id == m_selectedId)
        {
            return;
        }
        m_selectedId = id;
        const auto found = m_graph->index.find(m_selectedId);
        m_selected = found == m_graph->index.end() ? ~0u : found->second;
        if (m_selected < m_graph->nodes.size())
        {
            if (followSelection())
            {
                m_laneChoice.setSelected(static_cast<i32>(m_lane));
            }
            m_reveal = true;
        }
        showInfo();
        m_dirty = true;
    }

    bool SystemsGraphView::setLane(const i32 phase, const std::string_view stage)
    {
        for (u32 i = 0; i < m_graph->lanes.size(); ++i)
        {
            const SystemsGraphLane& each = m_graph->lanes[i];
            if ((phase < 0 || each.phase == static_cast<u32>(phase)) &&
                (each.stageName == stage || std::to_string(each.stage) == stage))
            {
                m_lane = i;
                m_lanePhase = each.phase;
                m_laneStage = each.stage;
                m_laneChoice.setSelected(static_cast<i32>(i));
                m_origin = Vec2{};
                m_reveal = true;
                showInfo();
                m_dirty = true;
                return true;
            }
        }
        return false;
    }

    std::string SystemsGraphView::lanesText() const
    {
        std::string text;
        for (const SystemsGraphLane& each : m_graph->lanes)
        {
            text += std::format("{}{}/{}", text.empty() ? "" : ", ", SYSTEM_PHASES[each.phase], each.stageName);
        }
        return text;
    }

    void SystemsGraphView::setView(const float zoom, const float x, const float y)
    {
        m_zoom = std::clamp(zoom, SystemsGraphView::ZOOM_MIN, SystemsGraphView::ZOOM_MAX);
        m_origin = Vec2{x, y};
        clampView();
        m_dirty = true;
    }

    void SystemsGraphView::resetView() { setView(1.0f, 0.0f, 0.0f); }

    // The origin stays within the graph, with up to half a view of margin
    void SystemsGraphView::clampView()
    {
        const SystemsGraphLane* current = lane();
        if (!current)
        {
            m_origin = Vec2{};
            return;
        }
        u32 tallest = 0;
        for (u32 c = 0; c < current->columns(); ++c)
        {
            tallest = std::max(tallest, current->columnStart[c + 1] - current->columnStart[c]);
        }
        const Vec2 content{static_cast<float>(current->columns()) * colStep(), static_cast<float>(tallest) * rowStep()};
        const Vec2 span = canvasSize() / m_zoom;
        m_origin.x = std::clamp(m_origin.x, -span.x * 0.5f, std::max(content.x - span.x * 0.5f, 0.0f));
        m_origin.y = std::clamp(m_origin.y, -span.y * 0.5f, std::max(content.y - span.y * 0.5f, 0.0f));
    }

    // Centres the node when any of it is out of view
    void SystemsGraphView::reveal(const u32 node)
    {
        const Vec2 size = canvasSize();
        const SystemsGraphLane* current = lane();
        if (size.x <= 0.0f || size.y <= 0.0f || !current || node < current->firstNode ||
            node >= current->firstNode + current->nodeCount)
        {
            return;
        }
        const Vec2 at = nodePos(m_graph->nodes[node]);
        const Vec2 span = size / m_zoom;
        if (at.x >= m_origin.x && at.y >= m_origin.y && at.x + nodeW() <= m_origin.x + span.x &&
            at.y + nodeH() <= m_origin.y + span.y)
        {
            return;
        }
        m_origin = at + Vec2{nodeW(), nodeH()} * 0.5f - span * 0.5f;
        clampView();
        m_dirty = true;
    }

    void SystemsGraphView::zoomAt(const Vec2 at, const float notches)
    {
        const float zoom = std::clamp(m_zoom * std::pow(ZOOM_NOTCH, std::clamp(notches, -MAX_NOTCHES, MAX_NOTCHES)),
                                      SystemsGraphView::ZOOM_MIN, SystemsGraphView::ZOOM_MAX);
        m_origin = m_origin + at / m_zoom - at / zoom; // the point under the pointer stays
        m_zoom = zoom;
        clampView();
        m_dirty = true;
    }

    void SystemsGraphView::pan(const Vec2 delta)
    {
        m_origin = m_origin - delta / m_zoom;
        clampView();
        m_dirty = true;
    }

    u32 SystemsGraphView::nodeAt(const Vec2 local) const
    {
        const SystemsGraphLane* current = lane();
        if (!current)
        {
            return ~0u;
        }
        const Vec2 g = m_origin + local / m_zoom;
        if (g.x < 0.0f || g.y < 0.0f)
        {
            return ~0u;
        }
        const auto column = static_cast<u32>(g.x / colStep());
        const auto row = static_cast<u32>(g.y / rowStep());
        if (column >= current->columns() || std::fmod(g.x, colStep()) > nodeW() ||
            std::fmod(g.y, rowStep()) > nodeH() ||
            current->columnStart[column] + row >= current->columnStart[column + 1])
        {
            return ~0u;
        }
        return current->order[current->columnStart[column] + row];
    }

    void SystemsGraphView::pick(const u32 node)
    {
        const SystemsGraphNode& picked = m_graph->nodes[node];
        m_host.send(Json{
            {"command", "systemsSelect"},
            {"args",
             {{"name", picked.name}, {"module", picked.module}, {"phase", std::string(SYSTEM_PHASES[picked.phase])}}}}
                        .dump());
    }

    void SystemsGraphView::pointerEvent(const UiCanvasPointer& pointer)
    {
        using K = UiCanvasPointerKind;
        switch (pointer.kind)
        {
        case K::Down:
            m_dragged = false;
            break;
        case K::DragStart:
        case K::DragMove:
            m_dragged = true;
            pan(pointer.delta);
            break;
        case K::Click:
            if (pointer.button == 0 && !m_dragged)
            {
                if (const u32 node = nodeAt(pointer.local); node != ~0u)
                {
                    pick(node);
                }
            }
            break;
        case K::Wheel:
            if (std::isfinite(pointer.wheel.y) && pointer.wheel.y != 0.0f)
            {
                zoomAt(pointer.local, pointer.wheel.y);
            }
            break;
        default:
            break;
        }
    }

    // Left and Right follow the orderings, Up and Down walk the column; with nothing selected the first key selects
    // the first system
    bool SystemsGraphView::keyEvent(const u32 scancode)
    {
        const SystemsGraphLane* current = lane();
        if (!current || current->nodeCount == 0)
        {
            return false;
        }
        const Vec2 centre = canvasSize() * 0.5f;
        switch (scancode)
        {
        case Key::EQUALS:
        case Key::KP_PLUS:
            zoomAt(centre, 1.0f);
            return true;
        case Key::MINUS:
        case Key::KP_MINUS:
            zoomAt(centre, -1.0f);
            return true;
        case Key::HOME:
            resetView();
            return true;
        case Key::LEFT:
        case Key::RIGHT:
        case Key::UP:
        case Key::DOWN:
            break;
        default:
            return false;
        }
        if (m_selected >= m_graph->nodes.size() || m_selected < current->firstNode ||
            m_selected >= current->firstNode + current->nodeCount)
        {
            pick(current->order.front());
            return true;
        }
        const SystemsGraphNode& node = m_graph->nodes[m_selected];
        u32 target = ~0u;
        if (scancode == Key::UP || scancode == Key::DOWN)
        {
            const u32 at = current->columnStart[node.depth] + node.row;
            const u32 next = scancode == Key::UP ? at - 1 : at + 1;
            if (scancode == Key::DOWN ? next < current->columnStart[node.depth + 1]
                                      : at > current->columnStart[node.depth])
            {
                target = current->order[next];
            }
        }
        else
        {
            const std::vector<u32>& neighbours = scancode == Key::LEFT ? node.preds : node.succs;
            const auto gapOf = [&](const u32 index)
            {
                const u32 depth = m_graph->nodes[index].depth;
                return depth > node.depth ? depth - node.depth : node.depth - depth;
            };
            for (const u32 candidate : neighbours) // the nearest column, then the first row
            {
                if (target == ~0u || gapOf(candidate) < gapOf(target))
                {
                    target = candidate;
                }
            }
        }
        if (target != ~0u)
        {
            pick(target);
        }
        return true;
    }

    void SystemsGraphView::refreshPalette()
    {
        const UiStyleSheet& styles = *m_host.styles;
        m_paletteSheet = styles.generation();
        const auto resolve = [&styles](const char* classes)
        {
            return styles.resolve(InternedString("systems.graph.canvas"), UiWidgetKind::Label,
                                  UiInteractionState::Normal, InternedString(classes));
        };
        m_palette.text = resolve("list.row.text.current").textColor;
        m_palette.muted = resolve("list.filter.count").textColor;
        const UiStyle node = resolve("systems.graph.node");
        m_palette.fill = node.backgroundColor;
        m_palette.outline = node.borderColor;
        m_palette.selected = resolve("list.row.selected").backgroundColor;
        m_palette.touched = resolve("systems.graph.edge.touched").textColor;
        m_palette.conflict = resolve("systems.graph.edge.conflict").textColor;
        m_palette.data = resolve("hierarchy.row.detail").textColor;
    }

    void SystemsGraphView::update()
    {
        const u64 layout = m_host.scope->hierarchy().layoutGeneration();
        const u64 sheet = m_host.styles->generation();
        const bool densityMoved = m_host.density->density() != m_density;
        if (layout != m_seenLayout || sheet != m_paletteSheet || densityMoved)
        {
            m_seenLayout = layout;
            m_density = m_host.density->density();
            const UiTextMeasurer* measurer = m_host.scope->hierarchy().textMeasurer();
            const float textHeight = measurer ? measurer->measureText(m_canvas.entity(), "Ag").y : m_density.row;
            if (densityMoved || sheet != m_paletteSheet || textHeight != m_textHeight || canvasSize() != m_drawnSize)
            {
                m_textHeight = textHeight;
                fitLaneChoice();
                m_dirty = true;
            }
        }
        if (m_dirty)
        {
            if (m_reveal && canvasSize().x > 0.0f)
            {
                m_reveal = false;
                if (m_selected < m_graph->nodes.size())
                {
                    reveal(m_selected);
                }
            }
            clampView();
            redraw(canvasSize());
        }
    }

    void SystemsGraphView::redraw(const Vec2 size)
    {
        m_dirty = false;
        ++m_redraws;
        m_drawnSize = size;
        m_drawnNodes = m_drawnEdges = m_drawnDashes = 0;
        refreshPalette();
        const SystemsGraphLane* current = lane();
        if (!current || size.x < 8.0f || size.y < 8.0f)
        {
            m_canvas.clear();
            return;
        }
        const float z = m_zoom;
        const float w = nodeW(), h = nodeH(), cs = colStep(), rs = rowStep();
        const Vec2 lo = m_origin;
        const Vec2 hi = m_origin + size / z;
        const auto screen = [&](const Vec2 g) { return (g - m_origin) * z; };
        const UiTextMeasurer* measurer = m_host.scope->hierarchy().textMeasurer();
        const SystemsGraphNode* chosen = m_selected < m_graph->nodes.size() ? &m_graph->nodes[m_selected] : nullptr;
        m_canvas.update(
            [&](UiCanvasPainter& painter)
            {
                for (u32 e = current->firstEdge; e < current->firstEdge + current->edgeCount; ++e)
                {
                    const SystemsGraphEdge& edge = m_graph->edges[e];
                    const SystemsGraphNode& from = m_graph->nodes[edge.from];
                    const SystemsGraphNode& to = m_graph->nodes[edge.to];
                    const bool touches = chosen && (edge.from == m_selected || edge.to == m_selected);
                    // a chain of implied orders is drawn between neighbouring columns; the rest is the selection's
                    if (!edge.declared && !touches && to.depth != from.depth + 1)
                    {
                        continue;
                    }
                    const Vec2 a = nodePos(from) + Vec2{w, h * 0.5f};
                    const Vec2 b = nodePos(to) + Vec2{0.0f, h * 0.5f};
                    if (std::max(a.x, b.x) < lo.x || std::min(a.x, b.x) > hi.x || std::max(a.y, b.y) < lo.y ||
                        std::min(a.y, b.y) > hi.y)
                    {
                        continue;
                    }
                    u32 color = edge.declared ? m_palette.muted : withAlpha(m_palette.muted, IMPLIED_ALPHA);
                    float width = edge.declared ? EDGE_WIDTH : 1.0f;
                    if (edge.conflict || touches)
                    {
                        color = edge.conflict ? m_palette.conflict : m_palette.touched;
                        width = THICK;
                    }
                    const Vec2 sa = screen(a), sb = screen(b);
                    const Vec2 delta = sb - sa;
                    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
                    if (edge.conflict && length > 1.0f)
                    {
                        // dashes: a conflict reads without its colour
                        const Vec2 d = delta / length;
                        const float on = m_density.sm + m_density.xs;
                        const float step = std::max(on + m_density.xs, length / static_cast<float>(MAX_DASHES));
                        for (float at = 0.0f; at < length; at += step)
                        {
                            painter.line(sa + d * at, sa + d * std::min(at + on, length), width, color);
                            ++m_drawnDashes;
                        }
                    }
                    else
                    {
                        painter.line(sa, sb, width, color);
                    }
                    if (z >= MIN_TEXT_ZOOM && length > 1.0f)
                    {
                        const Vec2 d = delta / length;
                        const Vec2 base = sb - d * (m_density.sm + m_density.xxs);
                        const Vec2 side = Vec2{-d.y, d.x} * (m_density.xs * 0.75f);
                        painter.triangle(sb, base + side, base - side, color);
                    }
                    ++m_drawnEdges;
                }
                const auto first = static_cast<u32>(std::max(0.0f, std::floor(lo.x / cs)));
                for (u32 c = first; c < current->columns() && static_cast<float>(c) * cs <= hi.x; ++c)
                {
                    const u32 begin = current->columnStart[c];
                    const u32 count = current->columnStart[c + 1] - begin;
                    const auto firstRow = static_cast<u32>(std::max(0.0f, std::floor(lo.y / rs)));
                    for (u32 r = firstRow; r < count && static_cast<float>(r) * rs <= hi.y; ++r)
                    {
                        const u32 index = current->order[begin + r];
                        const SystemsGraphNode& node = m_graph->nodes[index];
                        const Vec2 at = screen(nodePos(node));
                        const Vec2 box{w * z, h * z};
                        const bool related = chosen &&
                            (std::ranges::contains(chosen->preds, index) ||
                             std::ranges::contains(chosen->succs, index));
                        const bool picked = index == m_selected;
                        const float shade = node.enabled ? 1.0f : DISABLED_ALPHA;
                        painter.rect(at, box, withAlpha(m_palette.fill, shade));
                        if (picked)
                        {
                            painter.rect(at, box, m_palette.selected);
                        }
                        const bool warned = node.conflict || node.cyclic;
                        const u32 border = picked ? m_palette.touched
                            : related             ? m_palette.data
                            : warned              ? m_palette.conflict
                                                  : m_palette.outline;
                        const float t =
                            std::min(picked || related || warned ? THICK : m_density.hairline, box.y * 0.5f);
                        painter.rect(at, Vec2{box.x, t}, border);
                        painter.rect(at + Vec2{0.0f, box.y - t}, Vec2{box.x, t}, border);
                        painter.rect(at, Vec2{t, box.y}, border);
                        painter.rect(at + Vec2{box.x - t, 0.0f}, Vec2{t, box.y}, border);
                        if (warned)
                        {
                            painter.rect(at, Vec2{std::max(m_density.xs * z, 2.0f), box.y}, m_palette.conflict);
                        }
                        if (z >= MIN_TEXT_ZOOM)
                        {
                            const float pad = m_density.xs + m_density.xxs;
                            const float room = box.x - 2.0f * pad;
                            const float measured =
                                measurer ? measurer->measureText(m_canvas.entity(), node.name).x : room;
                            const std::string shown = clipped(node.name, measured, room);
                            if (!shown.empty())
                            {
                                painter.text(at + Vec2{pad, 0.0f}, Vec2{room, box.y}, shown,
                                             withAlpha(m_palette.text, shade));
                            }
                        }
                        ++m_drawnNodes;
                    }
                }
            });
    }

    void SystemsGraphView::writeState(IFormatWriter& out) const
    {
        const SystemsGraphLane* current = lane();
        out.writeBool("shown", m_root.isVisible());
        out.beginObject("lane");
        out.writeU32("index", m_lane);
        out.writeString("phase", current ? SYSTEM_PHASES[current->phase] : std::string_view{});
        out.writeU64("stage", current ? current->stage : 0);
        out.writeString("stageName", current ? std::string_view(current->stageName) : std::string_view{});
        out.endObject();
        out.beginArray("lanes", static_cast<u32>(m_graph->lanes.size()));
        for (const SystemsGraphLane& each : m_graph->lanes)
        {
            out.beginObject("");
            out.writeString("phase", SYSTEM_PHASES[each.phase]);
            out.writeU64("stage", each.stage);
            out.writeString("stageName", each.stageName);
            out.writeU32("systems", each.nodeCount);
            out.writeU32("edges", each.edgeCount);
            out.writeU32("conflicts", each.conflicts);
            out.writeU32("columns", each.columns());
            out.endObject();
        }
        out.endArray();
        const u32 nodeCount = current ? current->nodeCount : 0;
        const u32 edgeCount = current ? current->edgeCount : 0;
        const u32 nodesListed = std::min(nodeCount, STATE_NODES);
        const auto ids = [this](const std::vector<u32>& indices)
        {
            std::vector<std::string> out;
            for (const u32 i : indices)
            {
                out.push_back(m_graph->nodes[i].id);
            }
            return out;
        };
        out.beginArray("nodes", nodesListed);
        for (u32 i = 0; i < nodesListed; ++i)
        {
            const SystemsGraphNode& node = m_graph->nodes[current->firstNode + i];
            out.beginObject("");
            out.writeString("id", node.id);
            out.writeString("name", node.name);
            out.writeString("module", node.module);
            out.writeU32("depth", node.depth);
            out.writeU32("row", node.row);
            out.writeF32("x", nodePos(node).x);
            out.writeF32("y", nodePos(node).y);
            out.writeBool("conflict", node.conflict);
            out.writeBool("cyclic", node.cyclic);
            out.writeBool("enabled", node.enabled);
            writeStrings(out, "after", node.after);
            writeStrings(out, "succs", ids(node.succs));
            writeStrings(out, "preds", ids(node.preds));
            writeStrings(out, "reads", node.reads);
            writeStrings(out, "writes", node.writes);
            out.endObject();
        }
        out.endArray();
        // the declared orders first, so a long list of implied ones never cuts them off
        std::vector<u32> listed;
        for (const bool declared : {true, false})
        {
            for (u32 e = 0; e < edgeCount && listed.size() < STATE_EDGES; ++e)
            {
                if (m_graph->edges[current->firstEdge + e].declared == declared)
                {
                    listed.push_back(current->firstEdge + e);
                }
            }
        }
        out.beginArray("edges", static_cast<u32>(listed.size()));
        for (const u32 e : listed)
        {
            const SystemsGraphEdge& edge = m_graph->edges[e];
            out.beginObject("");
            out.writeString("from", m_graph->nodes[edge.from].id);
            out.writeString("to", m_graph->nodes[edge.to].id);
            out.writeBool("declared", edge.declared);
            out.writeBool("conflict", edge.conflict);
            writeStrings(out, "components", edge.components);
            out.endObject();
        }
        out.endArray();
        u32 conflictsListed = 0;
        out.beginArray("conflicts", std::min(current ? current->conflicts : 0, STATE_CONFLICTS));
        for (u32 e = 0; e < edgeCount && conflictsListed < STATE_CONFLICTS; ++e)
        {
            const SystemsGraphEdge& edge = m_graph->edges[current->firstEdge + e];
            if (edge.conflict)
            {
                ++conflictsListed;
                out.beginObject("");
                out.writeString("a", m_graph->nodes[edge.from].id);
                out.writeString("b", m_graph->nodes[edge.to].id);
                writeStrings(out, "components", edge.components);
                out.endObject();
            }
        }
        out.endArray();
        out.writeU32("nodeCount", nodeCount);
        out.writeU32("edgeCount", edgeCount);
        out.writeU32("conflictCount", current ? current->conflicts : 0);
        out.writeBool("truncated",
                      nodesListed < nodeCount || listed.size() < edgeCount ||
                          conflictsListed < (current ? current->conflicts : 0));
        out.beginObject("selected");
        out.writeString("id",
                        m_selected < m_graph->nodes.size() ? std::string_view(m_graph->nodes[m_selected].id)
                                                           : std::string_view{});
        if (m_selected < m_graph->nodes.size())
        {
            const SystemsGraphNode& node = m_graph->nodes[m_selected];
            writeStrings(out, "preds", ids(node.preds));
            writeStrings(out, "succs", ids(node.succs));
            writeStrings(out, "reads", node.reads);
            writeStrings(out, "writes", node.writes);
        }
        out.endObject();
        out.beginObject("view");
        out.writeF32("zoom", m_zoom);
        out.writeF32("x", m_origin.x);
        out.writeF32("y", m_origin.y);
        out.writeF32("width", m_drawnSize.x);
        out.writeF32("height", m_drawnSize.y);
        out.writeF32("nodeWidth", nodeW());
        out.writeF32("nodeHeight", nodeH());
        out.writeU32("drawnNodes", m_drawnNodes);
        out.writeU32("drawnEdges", m_drawnEdges);
        out.writeU32("drawnDashes", m_drawnDashes);
        out.writeU64("redraws", m_redraws);
        out.writeU32("primitives", m_canvas.primitives());
        out.writeString("order", m_order.text());
        out.writeString("access", m_access.text());
        out.endObject();
    }
} // namespace spite
