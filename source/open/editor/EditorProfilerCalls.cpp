#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>

#include "EditorPanelUi.hpp"
#include "ProfilerViews.hpp"
#include "ui/UiCanvas.hpp"
#include "ui/UiInputSnapshot.hpp"

namespace spite
{
    using namespace panelui;

    namespace
    {
        constexpr float LEVEL_HEIGHT = 16.0f;
        constexpr float FLAME_HEIGHT = 116.0f;
        constexpr float MIN_RECT_WIDTH =
            0.5f; // a node narrower than this is culled: primitives follow pixels, not nodes
        constexpr float LABEL_MIN_WIDTH = 48.0f;
        constexpr u32 MAX_LABELS = 400;
        constexpr u32 MAX_TABLE_ROWS = 20000;
        constexpr u32 STATE_ROWS = 64;
        constexpr u32 MAX_ERROR_CHARS = 160;
        constexpr u32 MAX_INDENT = 12;
        constexpr sizet MAX_PATH_NAMES = 4096; // names of a node path kept across ranges
        constexpr u32 NONE = ~0u;
        constexpr std::array<std::string_view, 4> SORT_CHOICES{"inclusive", "self", "count", "name"};
        constexpr std::array<std::string_view, 3> CLEAR_CHOICES{"focus", "select", "all"};

        enum class Sort : u8
        {
            Inclusive,
            Self,
            Count,
            Name,
        };
        // the table's columns: zone, inclusive ms, inclusive %, self ms, calls; the % column has no sort of its own
        constexpr std::array<Sort, 5> SORT_OF_CELL{Sort::Name, Sort::Inclusive, Sort::Inclusive, Sort::Self,
                                                   Sort::Count};
        constexpr i32 CELL_OF_SORT[4] = {1, 3, 4, 0};

        // What the worker aggregated: the tree and, per node, its children in the flame graph's order (inclusive time
        // descending, ties in creation order) as one array with an offset per node
        struct Model
        {
            CaptureId capture = 0;
            std::string key;
            CaptureCallTree tree;
            std::vector<u32> start; // node + 1 entries
            std::vector<u32> kids;
            std::vector<CaptureThread> lanes;
            double aggregateMs = 0.0;
            std::string error;
            std::vector<u32> roots; // lane nodes, in creation order
        };

        // A node named by what survives a new aggregation: its lane and the names from the lane's root down
        struct NodePath
        {
            bool valid = false;
            i32 lane = -1;
            std::vector<std::string> names;
        };

        struct PathResult
        {
            u64 request = 0;
            u32 lane = 0, seq = 0;
            std::vector<std::string> names;
            std::string error;
        };

        struct Payload
        {
            enum class Kind : u8
            {
                Tree,
                Zone,
            };
            Kind kind = Kind::Tree;
            std::shared_ptr<Model> model;
            PathResult zone;
        };

        struct FlameRect
        {
            float x0, x1;
            u32 node;
        };

        class CallsView final : public ProfilerView
        {
        public:
            explicit CallsView(const ProfilerViewHost& host) :
                m_host(host), m_treeGeneration(std::make_shared<std::atomic<u64>>(0)),
                m_zoneGeneration(std::make_shared<std::atomic<u64>>(0)),
                m_theme(*host.scope, [this] { m_dirty = true; })
            {
                UiScope& scope = *m_host.scope;
                DensityFollower& density = *m_host.density;
                m_root = stackPanel(scope, "profiler.calls", Vec2{}, UiLayoutDirection::Column, UiLayoutAlign::Stretch,
                                    true);
                m_root.setLayoutItem(fillItem());
                Panel bar = density.track(stackPanel(scope, "profiler.calls.bar", Vec2{}, UiLayoutDirection::Row),
                                          HEIGHT_CONTROL);
                bar.setParent(m_root);
                UiDropdownDesc dropdown;
                dropdown.rect = barDesc(Vec2{170.0f, 0.0f});
                m_lane = density.track(scope.createDropdown("profiler.calls.lane", dropdown), HEIGHT_ROW);
                m_lane.setParent(bar);
                m_lane.setAccessibleNameKey(profilerKey("calls.lane.name"));
                m_lane.onChange(
                    [this](const u32 index)
                    {
                        m_host.send({{"command", "profilerCalls"},
                                     {"args", index == 0 ? Json{{"allLanes", true}} : Json{{"lane", index - 1}}}});
                    });
                const auto button = [&](Panel& parent, Button& out, const char* textKey, const char* nameKey, Json args)
                {
                    out = density.track(scope.createButton("profiler.calls.button", "", barDesc(Vec2{68.0f, 0.0f})),
                                        HEIGHT_ROW);
                    out.setLocalizedText(profilerKey(textKey));
                    out.setParent(parent);
                    out.setAccessibleNameKey(profilerKey(nameKey));
                    out.onClick([this, args = std::move(args)]
                                { m_host.send({{"command", "profilerCalls"}, {"args", args}}); });
                };
                button(bar, m_focusButton, "calls.focus", "calls.focus.name", Json{{"focusSelected", true}});
                button(bar, m_upButton, "calls.up", "calls.up.name", Json{{"up", true}});
                button(bar, m_resetButton, "calls.reset", "calls.reset.name", Json{{"clear", "all"}});
                m_status = density.track(readOnlyText(scope, "profiler.calls.status", barDesc(Vec2{})), HEIGHT_ROW);
                m_status.setParent(m_root);
                m_status.setAccessibleNameKey(profilerKey("calls.status.name"));
                UiCanvasDesc canvasDesc;
                canvasDesc.rect = barDesc(Vec2{0.0f, FLAME_HEIGHT});
                canvasDesc.maxPrimitives = 40000;
                m_flame = scope.createCanvas("profiler.calls.flame", canvasDesc);
                m_flame.setParent(m_root);
                m_flame.setAccessibleNameKey(profilerKey("calls.flame.name"));
                m_flame.onPointer([this](const UiCanvasPointer& pointer) { pointerEvent(pointer); });
                m_hover = density.track(readOnlyText(scope, "profiler.calls.hover", barDesc(Vec2{})), HEIGHT_ROW);
                m_hover.setParent(m_root);
                m_hover.setAccessibleNameKey(profilerKey("calls.hover.name"));
                m_table = std::make_unique<Table>(
                    scope,
                    TableDesc{
                        .id = "profiler.callTable",
                        .columns = {200.0f, 100.0f, 90.0f, 80.0f},
                        .cells = 5,
                        .buttonWidth = 56.0f,
                        .read =
                            [this](const u32 row, const std::span<std::string> cells, std::string& button)
                        {
                            const Row& r = m_rows[row];
                            const CaptureCallNode& node = m_model->tree.nodes[r.node];
                            cells[0] = std::string(r.node == m_selected ? "> " : "") +
                                std::string(std::min(r.depth, MAX_INDENT) * 2, ' ') + profilerSafeText(node.name, 80);
                            cells[1] = m_host.number(static_cast<double>(node.inclusiveNs) * 1e-6);
                            cells[2] = percentNumber(node.inclusiveNs);
                            cells[3] = m_host.number(static_cast<double>(node.selfNs) * 1e-6);
                            cells[4] = m_host.whole(node.count);
                            button = m_host.text("select");
                        },
                        .press = [this](const u32 row)
                        { m_host.send({{"command", "profilerCalls"}, {"args", {{"select", m_rows[row].node}}}}); },
                        .buttonName =
                            [this](const u32 row)
                        {
                            return m_host.text(
                                "calls.row.select",
                                {{"name", profilerSafeText(m_model->tree.nodes[m_rows[row].node].name, 64)}});
                        },
                        .headers = {{profilerKey("calls.col.name"), true},
                                    {profilerKey("calls.col.inclusive"), true},
                                    {profilerKey("calls.col.percent")},
                                    {profilerKey("calls.col.self"), true},
                                    {profilerKey("calls.col.count"), true}},
                        .sort =
                            [this](const u32 cell)
                        {
                            m_host.send({{"command", "profilerCalls"},
                                         {"args",
                                          {{"sort", std::string(SORT_CHOICES[static_cast<u32>(SORT_OF_CELL[cell])])},
                                           {"toggle", true}}}});
                        },
                        .headerName = [this](u32, const std::string_view title, const std::string_view order)
                        { return m_host.sortName(title, order); },
                        .lastMin = 60.0f});
                Widget list = m_table->widget();
                list.setParent(m_root);
                list.setLayoutItem(fillItem());
                m_table->list.setAccessibleNameKey(profilerKey("calls.list.name"));
            }

            [[nodiscard]] Widget root() const override { return m_root; }
            [[nodiscard]] float preferredHeight() const override { return 430.0f; }

            void activate(const bool active) override
            {
                m_active = active;
                if (!active)
                {
                    m_treeGeneration->fetch_add(1);
                    m_zoneGeneration->fetch_add(1);
                    m_loading = m_zoneLoading = false;
                    m_key.clear();
                    return;
                }
                m_dirty = true;
                refresh();
            }

            void captureChanged(const CaptureId capture, const CaptureInfo& info) override
            {
                m_capture = capture;
                m_frames = capture != 0 ? info.frames : 0;
                m_treeGeneration->fetch_add(1);
                m_zoneGeneration->fetch_add(1);
                m_loading = m_zoneLoading = false;
                m_selection = {};
                m_model.reset();
                m_key.clear();
                m_focus = m_selected = NONE;
                m_focusPath = m_selectedPath = {};
                m_expectedZone.reset();
                m_laneFilter = -1;
                m_error.clear();
                rebuildRows();
                m_dirty = true;
                if (m_active)
                {
                    refresh();
                }
            }

            void selectionChanged(const ProfilerSelectionComponent& selection) override
            {
                const bool window = selection.hasFrames != m_selection.hasFrames ||
                    selection.firstFrame != m_selection.firstFrame || selection.lastFrame != m_selection.lastFrame;
                const bool zone = selection.hasZone &&
                    (selection.zoneLane != m_selection.zoneLane || selection.zoneIndex != m_selection.zoneIndex ||
                     !m_selection.hasZone);
                m_selection = selection;
                if (window && m_active)
                {
                    refresh();
                }
                if (zone && m_active)
                {
                    if (m_expectedZone &&
                        *m_expectedZone == std::pair<u32, u64>(selection.zoneLane, selection.zoneIndex))
                    {
                        m_expectedZone.reset(); // our own selection coming back
                    }
                    else
                    {
                        resolveZone();
                    }
                }
            }

            bool take(ProfilerEvent& event) override
            {
                const auto payload = std::static_pointer_cast<Payload>(event.payload);
                if (!payload)
                {
                    if (event.token != m_treeGeneration->load() && event.token != m_zoneGeneration->load())
                    {
                        return true;
                    }
                    if (!event.error.empty() && event.error != "superseded")
                    {
                        m_loading = m_zoneLoading = false;
                        m_error = profilerSafeText(event.error, MAX_ERROR_CHARS);
                        m_dirty = true;
                    }
                    return true;
                }
                if (payload->kind == Payload::Kind::Zone)
                {
                    if (event.token != m_zoneValue || event.token != m_zoneGeneration->load())
                    {
                        return true;
                    }
                    m_zoneLoading = false;
                    if (payload->zone.error.empty() && m_model)
                    {
                        const u32 node =
                            find(NodePath{true, static_cast<i32>(payload->zone.lane), payload->zone.names});
                        if (node != NONE)
                        {
                            m_selected = node;
                            m_selectedPath = pathOf(node);
                            m_dirty = true;
                        }
                    }
                    return true;
                }
                if (event.token != m_treeValue || event.token != m_treeGeneration->load())
                {
                    return true;
                }
                m_loading = false;
                m_model = payload->model;
                m_error = profilerSafeText(m_model->error, MAX_ERROR_CHARS);
                if (!m_model->error.empty())
                {
                    m_model->tree = {};
                    m_model->start.assign(1, 0);
                }
                // what was focused and selected stays so when the new range still has it
                m_focus = m_focusPath.valid ? find(m_focusPath) : NONE;
                m_selected = m_selectedPath.valid ? find(m_selectedPath) : NONE;
                m_focusLost = m_focusPath.valid && m_focus == NONE;
                updateLanes();
                rebuildRows();
                if (m_selection.hasZone && m_selected == NONE)
                {
                    resolveZone();
                }
                m_dirty = true;
                return true;
            }

            void update() override
            {
                const Vec2 size = m_host.laidOutSize(m_flame);
                if (*m_host.locale != m_localeUsed)
                {
                    m_localeUsed = *m_host.locale;
                    m_laneItems = ~sizet{}; // the unnamed lanes' label is a message
                    if (m_model)
                    {
                        updateLanes();
                    }
                    m_table->rebindVisible();
                    m_dirty = true;
                }
                if (m_dirty || size != m_drawnSize)
                {
                    redraw(size);
                }
            }

            [[nodiscard]] bool busy() const override { return m_loading || m_zoneLoading; }

            void writeState(IFormatWriter& out) override
            {
                out.writeBool("loading", m_loading);
                out.writeString("error", m_error);
                out.writeString("sort", SORT_CHOICES[static_cast<u32>(m_sort)]);
                out.writeBool("descending", m_descending);
                out.writeI32("lane", m_laneFilter);
                out.writeString("window", windowName());
                const bool ready = m_model && m_model->error.empty();
                out.writeBool("ready", ready);
                if (ready)
                {
                    const CaptureCallTree& tree = m_model->tree;
                    out.writeU64("zones", tree.zones);
                    out.writeU64("zonesDropped", tree.zonesDropped);
                    out.writeU64("nodes", tree.nodes.size());
                    out.writeBool("capTruncated", tree.truncated);
                    out.writeF64("aggregateMs", m_model->aggregateMs);
                    out.writeU64("windowStartNs", tree.windowStartNs);
                }
                writeNode(out, "focus", m_focus);
                out.writeBool("focusLost", m_focusLost);
                writeNode(out, "selected", m_selected);
                out.beginObject("table");
                out.writeU64("rows", m_rows.size());
                out.writeBool("truncated", m_rowsTruncated);
                const u64 listed = std::min<u64>(m_rows.size(), STATE_ROWS);
                out.beginArray("first", static_cast<u32>(listed));
                for (u64 i = 0; i < listed; ++i)
                {
                    const CaptureCallNode& node = m_model->tree.nodes[m_rows[i].node];
                    out.beginArray({}, 7);
                    out.writeU32({}, m_rows[i].node);
                    out.writeU32({}, m_rows[i].depth);
                    out.writeString({}, node.name);
                    out.writeU64({}, node.inclusiveNs);
                    out.writeU64({}, node.selfNs);
                    out.writeU64({}, node.count);
                    out.writeU32({}, node.thread);
                    out.endArray();
                }
                out.endArray();
                writeTableStats(out, *m_table);
                out.endObject();
                out.beginObject("flame");
                out.writeU64("rects", m_drawnRects);
                out.writeU64("culled", m_culled);
                out.writeU32("levels", m_levelCount);
                out.writeU64("deeperCut", m_deeperCut);
                out.writeU64("labels", m_labels);
                out.writeU64("primitives", m_flame.primitives());
                out.endObject();
            }

            void registerCommands() override
            {
                m_host.control->registerViewCommand(
                    "profilerCalls", m_host.owner,
                    "drives the Profiler's calls view: the call tree of the selected frames (or the whole capture) as "
                    "a "
                    "flame graph and a sortable table. sort/descending order the table's siblings; lane/allLanes pick "
                    "one thread lane or all; select <node> selects a node (and, through profilerSelect, its longest "
                    "zone, which the timeline highlights); focus <node> narrows the flame graph and the table to a "
                    "subtree, up focuses its parent, clear drops the focus, the selection or both. Node numbers are "
                    "those of profileCallTree for the same frames",
                    {optionalArg("sort", ArgKind::String, "the table's sort key", "inclusive",
                                 std::span<const std::string_view>(SORT_CHOICES)),
                     optionalArg("descending", ArgKind::Boolean, "sort descending", "true except for name"),
                     optionalArg("toggle", ArgKind::Boolean, "with sort: the same key again reverses the order",
                                 "false"),
                     optionalArg("lane", ArgKind::Integer,
                                 "aggregate one thread lane (profileCaptures capture=<id> lists them)"),
                     optionalArg("allLanes", ArgKind::Boolean, "aggregate every lane", "true"),
                     optionalArg("select", ArgKind::Integer, "select this node"),
                     optionalArg("focus", ArgKind::Integer, "focus this node's subtree"),
                     optionalArg("focusSelected", ArgKind::Boolean, "focus the selected node", "false"),
                     optionalArg("up", ArgKind::Boolean, "focus the focused node's parent", "false"),
                     optionalArg("clear", ArgKind::String, "drops the focus, the selection or both", {},
                                 std::span<const std::string_view>(CLEAR_CHOICES))},
                    [this](const ControlArgs& args, IFormatWriter& out) { return command(args, out); });
            }

        private:
            struct Row
            {
                u32 node;
                u32 depth;
            };

            // -- the model --------------------------------------------------------------------------------------------

            [[nodiscard]] std::string keyOf() const
            {
                return std::to_string(m_capture) + ":" + std::to_string(m_selection.hasFrames) + ":" +
                    std::to_string(m_selection.firstFrame) + ":" + std::to_string(m_selection.lastFrame) + ":" +
                    std::to_string(m_laneFilter);
            }

            // the state's descriptor: the same English words under every locale (windowText is the display form)
            [[nodiscard]] std::string windowName() const
            {
                if (!m_selection.hasFrames)
                {
                    return "whole capture";
                }
                return m_selection.firstFrame == m_selection.lastFrame
                    ? "frame " + std::to_string(m_selection.firstFrame)
                    : "frames " + std::to_string(m_selection.firstFrame) + ".." + std::to_string(m_selection.lastFrame);
            }

            [[nodiscard]] std::string windowText() const
            {
                if (!m_selection.hasFrames)
                {
                    return m_host.text("calls.window.whole");
                }
                return m_selection.firstFrame == m_selection.lastFrame
                    ? m_host.text("calls.window.frame", {{"frame", std::to_string(m_selection.firstFrame)}})
                    : m_host.text("calls.window.frames",
                                  {{"first", std::to_string(m_selection.firstFrame)},
                                   {"last", std::to_string(m_selection.lastFrame)}});
            }

            void refresh()
            {
                if (!m_host.data || m_capture == 0)
                {
                    m_dirty = true;
                    return;
                }
                if (const std::string key = keyOf(); key == m_key)
                {
                    return;
                }
                else
                {
                    m_key = key;
                }
                m_loading = true;
                m_since = std::chrono::steady_clock::now();
                m_treeValue = m_treeGeneration->fetch_add(1) + 1;
                CaptureStore* store = m_host.store;
                const CaptureId capture = m_capture;
                CaptureRange range;
                if (m_selection.hasFrames)
                {
                    range.firstFrame = static_cast<std::int64_t>(m_selection.firstFrame);
                    range.lastFrame = static_cast<std::int64_t>(m_selection.lastFrame);
                }
                const i32 lane = m_laneFilter;
                const std::string key = m_key;
                m_host.data->requestTask(
                    static_cast<u32>(ProfilerViewId::Calls), m_treeGeneration, m_treeValue,
                    [store, capture, range, lane, key](const CaptureCancelFn& cancelled,
                                                       std::string&) -> std::shared_ptr<void>
                    {
                        const auto begin = std::chrono::steady_clock::now();
                        auto payload = std::make_shared<Payload>();
                        payload->model = std::make_shared<Model>();
                        Model& model = *payload->model;
                        model.capture = capture;
                        model.key = key;
                        model.error = profilerStoreCall(cancelled,
                                                        [&]
                                                        {
                                                            model.lanes.clear();
                                                            return store->threads(capture, model.lanes);
                                                        });
                        if (model.error.empty())
                        {
                            model.error = store->callTree(capture, range, lane, cancelled, 0, model.tree);
                        }
                        if (model.error.empty())
                        {
                            buildChildren(model);
                        }
                        model.aggregateMs =
                            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
                        return payload;
                    });
            }

            static void buildChildren(Model& model)
            {
                const std::vector<CaptureCallNode>& nodes = model.tree.nodes;
                model.start.assign(nodes.size() + 1, 0);
                for (const CaptureCallNode& node : nodes)
                {
                    if (node.parent != NONE)
                    {
                        ++model.start[node.parent + 1];
                    }
                }
                for (sizet i = 0; i < nodes.size(); ++i)
                {
                    model.start[i + 1] += model.start[i];
                }
                model.kids.assign(model.start.back(), 0);
                std::vector<u32> fill(model.start.begin(), model.start.end() - 1);
                for (u32 i = 0; i < nodes.size(); ++i)
                {
                    if (nodes[i].parent != NONE)
                    {
                        model.kids[fill[nodes[i].parent]++] = i;
                    }
                    else
                    {
                        model.roots.push_back(i);
                    }
                }
                for (sizet i = 0; i < nodes.size(); ++i)
                {
                    std::stable_sort(model.kids.begin() + model.start[i], model.kids.begin() + model.start[i + 1],
                                     [&](const u32 a, const u32 b)
                                     { return nodes[a].inclusiveNs > nodes[b].inclusiveNs; });
                }
            }

            [[nodiscard]] std::span<const u32> childrenOf(const u32 node) const
            {
                return std::span<const u32>(m_model->kids)
                    .subspan(m_model->start[node], m_model->start[node + 1] - m_model->start[node]);
            }

            [[nodiscard]] NodePath pathOf(const u32 node) const
            {
                NodePath path{true, -1, {}};
                if (!m_model || node >= m_model->tree.nodes.size())
                {
                    return {};
                }
                for (u32 at = node; at != NONE; at = m_model->tree.nodes[at].parent)
                {
                    const CaptureCallNode& n = m_model->tree.nodes[at];
                    if (n.lane >= 0)
                    {
                        path.lane = n.lane;
                        break;
                    }
                    if (path.names.size() >= MAX_PATH_NAMES)
                    {
                        break; // the innermost names: a path that begins outside the range matches from a later name
                    }
                    path.names.push_back(n.name);
                }
                std::ranges::reverse(path.names);
                return path;
            }

            // The node a path names: from the lane's root, matching the names from the outermost; a path that begins
            // outside the range (its ancestors began before it) matches from a later name
            [[nodiscard]] u32 find(const NodePath& path) const
            {
                if (!m_model || !path.valid || path.names.size() > MAX_PATH_NAMES)
                {
                    return NONE;
                }
                u32 laneNode = NONE;
                for (const u32 root : m_model->roots)
                {
                    if (m_model->tree.nodes[root].lane == path.lane)
                    {
                        laneNode = root;
                    }
                }
                if (laneNode == NONE)
                {
                    return NONE;
                }
                if (path.names.empty())
                {
                    return laneNode;
                }
                for (sizet from = 0; from < path.names.size(); ++from)
                {
                    u32 at = laneNode;
                    for (sizet i = from; i < path.names.size() && at != NONE; ++i)
                    {
                        u32 next = NONE;
                        for (const u32 child : childrenOf(at))
                        {
                            if (m_model->tree.nodes[child].name == path.names[i])
                            {
                                next = child;
                                break;
                            }
                        }
                        at = next;
                    }
                    if (at != NONE)
                    {
                        return at;
                    }
                }
                return NONE;
            }

            void updateLanes()
            {
                std::vector<UiChoiceItem> items;
                items.push_back({InternedString("all"), "All lanes", profilerKey("calls.lanes.all"), true});
                for (const CaptureThread& lane : m_model->lanes)
                {
                    items.push_back(
                        {InternedString("lane" + std::to_string(lane.index)),
                         std::to_string(lane.index) + " " +
                             (lane.name.empty() ? m_host.text("calls.lane.unnamed") : profilerSafeText(lane.name, 40)),
                         {},
                         true});
                }
                if (m_laneItems != items.size())
                {
                    m_lane.setItems(items);
                    m_laneItems = items.size();
                }
                m_lane.setSelected(m_laneFilter < 0 ? 0 : m_laneFilter + 1);
            }

            // -- selection
            // ---------------------------------------------------------------------------------------------

            void resolveZone()
            {
                if (!m_host.data || m_capture == 0 || !m_selection.hasZone)
                {
                    return;
                }
                m_zoneLoading = true;
                m_zoneValue = m_zoneGeneration->fetch_add(1) + 1;
                CaptureStore* store = m_host.store;
                const CaptureId capture = m_capture;
                const u32 lane = m_selection.zoneLane;
                const u64 seq = m_selection.zoneIndex;
                m_host.data->requestTask(
                    static_cast<u32>(ProfilerViewId::Calls), m_zoneGeneration, m_zoneValue,
                    [store, capture, lane, seq](const CaptureCancelFn& cancelled, std::string&) -> std::shared_ptr<void>
                    {
                        auto payload = std::make_shared<Payload>();
                        payload->kind = Payload::Kind::Zone;
                        payload->zone.lane = lane;
                        payload->zone.error = profilerStoreCall(
                            cancelled, [&] { return store->zonePath(capture, lane, seq, payload->zone.names); });
                        return payload;
                    });
            }

            std::string command(const ControlArgs& args, IFormatWriter& out)
            {
                if (const std::optional<std::string> sort = args.string("sort"))
                {
                    const auto it = std::ranges::find(SORT_CHOICES, std::string_view(*sort));
                    if (it == SORT_CHOICES.end())
                    {
                        return "sort is inclusive, self, count or name";
                    }
                    const auto key = static_cast<Sort>(it - SORT_CHOICES.begin());
                    const bool same = key == m_sort;
                    m_sort = key;
                    m_descending = args.boolean("descending")
                                       .value_or(same && args.boolean("toggle").value_or(false) ? !m_descending
                                                                                                : key != Sort::Name);
                    rebuildRows();
                }
                else if (const std::optional<bool> descending = args.boolean("descending"))
                {
                    m_descending = *descending;
                    rebuildRows();
                }
                if (const std::optional<u64> lane = args.integer("lane"))
                {
                    if (m_model && !m_model->lanes.empty() && *lane >= m_model->lanes.size())
                    {
                        return "lane " + std::to_string(*lane) + " does not exist: the capture has " +
                            std::to_string(m_model->lanes.size()) + " lanes";
                    }
                    setLane(static_cast<i32>(std::min<u64>(*lane, INT32_MAX)));
                }
                else if (args.boolean("allLanes").value_or(false))
                {
                    setLane(-1);
                }
                const std::optional<std::string> clear = args.string("clear");
                if (clear && (*clear == "focus" || *clear == "all"))
                {
                    m_focus = NONE;
                    m_focusPath = {};
                    m_focusLost = false;
                }
                if (clear && (*clear == "select" || *clear == "all"))
                {
                    m_selected = NONE;
                    m_selectedPath = {};
                }
                if (const std::optional<u64> node = args.integer("select"))
                {
                    if (std::string error = checkNode(*node); !error.empty())
                    {
                        return error;
                    }
                    select(static_cast<u32>(*node));
                }
                if (const std::optional<u64> node = args.integer("focus"))
                {
                    if (std::string error = checkNode(*node); !error.empty())
                    {
                        return error;
                    }
                    focus(static_cast<u32>(*node));
                }
                if (args.boolean("focusSelected").value_or(false))
                {
                    if (m_selected == NONE)
                    {
                        return "select a node first (profilerCalls select=<node>)";
                    }
                    focus(m_selected);
                }
                if (args.boolean("up").value_or(false) && m_focus != NONE)
                {
                    const u32 parent = m_model->tree.nodes[m_focus].parent;
                    focus(parent);
                }
                rebuildRows();
                m_dirty = true;
                out.writeString("sort", SORT_CHOICES[static_cast<u32>(m_sort)]);
                out.writeI32("focus", m_focus == NONE ? -1 : static_cast<i32>(m_focus));
                out.writeI32("selected", m_selected == NONE ? -1 : static_cast<i32>(m_selected));
                out.writeI32("lane", m_laneFilter);
                out.writeBool("loading", m_loading);
                return {};
            }

            std::string checkNode(const u64 node) const
            {
                if (!m_model || !m_model->error.empty())
                {
                    return m_loading ? "the call tree is still being aggregated (profilerPanes calls.loading)"
                                     : "there is no call tree (select a capture and show the calls view)";
                }
                if (node >= m_model->tree.nodes.size())
                {
                    return "no node " + std::to_string(node) + ": the call tree has " +
                        std::to_string(m_model->tree.nodes.size()) + " nodes";
                }
                return {};
            }

            void setLane(const i32 lane)
            {
                if (lane != m_laneFilter)
                {
                    m_laneFilter = lane;
                    m_focus = m_selected = NONE;
                    m_focusPath = m_selectedPath = {};
                    refresh();
                }
            }

            void focus(const u32 node)
            {
                m_focus = node;
                m_focusPath = node == NONE ? NodePath{} : pathOf(node);
                m_focusLost = false;
            }

            void select(const u32 node)
            {
                m_selected = node;
                m_selectedPath = pathOf(node);
                const CaptureCallNode& n = m_model->tree.nodes[node];
                if (n.lane < 0 && m_capture != 0)
                {
                    m_expectedZone = std::pair<u32, u64>(n.thread, n.longestSeq);
                    m_host.send(
                        {{"command", "profilerSelect"},
                         {"args", {{"capture", m_capture}, {"zoneLane", n.thread}, {"zoneIndex", n.longestSeq}}}});
                }
            }

            // -- the table
            // ---------------------------------------------------------------------------------------------

            [[nodiscard]] bool before(const u32 a, const u32 b) const
            {
                const CaptureCallNode& x = m_model->tree.nodes[a];
                const CaptureCallNode& y = m_model->tree.nodes[b];
                u64 left = 0, right = 0;
                switch (m_sort)
                {
                case Sort::Inclusive:
                    left = x.inclusiveNs, right = y.inclusiveNs;
                    break;
                case Sort::Self:
                    left = x.selfNs, right = y.selfNs;
                    break;
                case Sort::Count:
                    left = x.count, right = y.count;
                    break;
                case Sort::Name:
                    if (x.name != y.name)
                    {
                        return m_descending ? x.name > y.name : x.name < y.name;
                    }
                    break;
                }
                if (left != right)
                {
                    return m_descending ? left > right : left < right;
                }
                return a < b;
            }

            void rebuildRows()
            {
                m_rows.clear();
                m_rowsTruncated = false;
                if (m_model && m_model->error.empty() && !m_model->tree.nodes.empty())
                {
                    struct Pending
                    {
                        u32 node, depth;
                    };
                    std::vector<Pending> stack;
                    std::vector<u32> siblings;
                    const auto push = [&](std::vector<u32>& group, const u32 depth)
                    {
                        std::ranges::sort(group, [&](const u32 a, const u32 b) { return before(a, b); });
                        for (auto it = group.rbegin(); it != group.rend(); ++it)
                        {
                            stack.push_back({*it, depth});
                        }
                    };
                    siblings = m_focus != NONE ? std::vector<u32>{m_focus} : m_model->roots;
                    push(siblings, 0);
                    while (!stack.empty())
                    {
                        const Pending item = stack.back();
                        stack.pop_back();
                        if (m_rows.size() >= MAX_TABLE_ROWS)
                        {
                            m_rowsTruncated = true;
                            break;
                        }
                        m_rows.push_back({item.node, item.depth});
                        const std::span<const u32> kids = childrenOf(item.node);
                        siblings.assign(kids.begin(), kids.end());
                        push(siblings, item.depth + 1);
                    }
                }
                m_table->setSort(CELL_OF_SORT[static_cast<u32>(m_sort)], m_descending);
                m_table->setRowCount(static_cast<u32>(m_rows.size()));
                m_table->rebindVisible();
            }

            [[nodiscard]] u64 rootInclusive() const
            {
                if (!m_model)
                {
                    return 0;
                }
                if (m_focus != NONE)
                {
                    return m_model->tree.nodes[m_focus].inclusiveNs;
                }
                u64 total = 0;
                for (const u32 root : m_model->roots)
                {
                    total += m_model->tree.nodes[root].inclusiveNs;
                }
                return total;
            }

            // the share of the root's time as a number; the unit is the header's or the sentence's
            [[nodiscard]] std::string percentNumber(const u64 inclusive) const
            {
                const u64 total = rootInclusive();
                return total == 0 ? std::string()
                                  : m_host.number(100.0 * static_cast<double>(inclusive) / static_cast<double>(total));
            }

            [[nodiscard]] std::string percent(const u64 inclusive) const
            {
                const std::string number = percentNumber(inclusive);
                return number.empty() ? number : number + "%";
            }

            void writeNode(IFormatWriter& out, const char* key, const u32 node)
            {
                out.beginObject(key);
                out.writeBool("present", node != NONE && m_model && node < m_model->tree.nodes.size());
                if (node != NONE && m_model && node < m_model->tree.nodes.size())
                {
                    const CaptureCallNode& n = m_model->tree.nodes[node];
                    out.writeU32("node", node);
                    out.writeString("name", n.name);
                    out.writeU64("inclusiveNs", n.inclusiveNs);
                    out.writeU64("selfNs", n.selfNs);
                    out.writeU64("count", n.count);
                    out.writeU32("thread", n.thread);
                    out.writeU64("longestSeq", n.longestSeq);
                }
                out.endObject();
            }

            // -- the flame graph
            // ---------------------------------------------------------------------------------------

            void redraw(const Vec2 size)
            {
                m_dirty = false;
                m_drawnSize = size;
                m_levels.clear();
                m_drawnRects = m_culled = m_deeperCut = m_labels = 0;
                m_levelCount = 0;
                std::string status;
                if (m_capture == 0)
                {
                    status = m_host.text("status.noCapture");
                }
                else if (!m_error.empty())
                {
                    status = m_host.text("status.error", {{"error", m_error}});
                }
                else if (m_loading || !m_model)
                {
                    status = m_host.text("calls.aggregating", {{"window", windowText()}});
                }
                else
                {
                    const CaptureCallTree& tree = m_model->tree;
                    status = m_host.text("calls.status",
                                         {{"window", windowText()},
                                          {"zones", m_host.whole(tree.zones)},
                                          {"nodes", m_host.whole(tree.nodes.size())},
                                          {"ms", m_host.number(m_model->aggregateMs)}});
                    if (tree.truncated)
                    {
                        status += " " + m_host.text("calls.capped", {{"zones", m_host.whole(tree.zonesDropped)}});
                    }
                    if (m_rowsTruncated)
                    {
                        status += " " + m_host.text("calls.tableCapped", {{"rows", m_host.whole(MAX_TABLE_ROWS)}});
                    }
                    if (m_focusLost)
                    {
                        status += " " + m_host.text("calls.focusLost");
                    }
                    if (tree.nodes.empty())
                    {
                        status += " - " + m_host.text("calls.noZones");
                    }
                }
                show(m_status, status);
                updateHover(m_hoverNode);
                if (!m_model || !m_model->error.empty() || m_model->tree.nodes.empty() || size.x < 8.0f ||
                    size.y < LEVEL_HEIGHT)
                {
                    m_flame.clear();
                    return;
                }
                const auto levelsFit = static_cast<u32>(size.y / LEVEL_HEIGHT);
                m_levels.assign(levelsFit, {});
                const UiScope& scope = *m_host.scope;
                const u32 outline = canvasColor(scope, "canvas.outline");
                const u32 onData = canvasColor(scope, "canvas.on-data");
                m_flame.update(
                    [&](UiCanvasPainter& painter)
                    {
                        struct Item
                        {
                            u32 node;
                            float x0, width;
                            u32 level;
                        };
                        std::vector<Item> stack;
                        std::vector<u32> top = m_focus != NONE ? std::vector<u32>{m_focus} : m_model->roots;
                        const u64 total = std::max<u64>(1, rootInclusive());
                        std::vector<Item> start;
                        float x = 0.0f;
                        for (const u32 root : top)
                        {
                            const float w = size.x *
                                static_cast<float>(static_cast<double>(m_model->tree.nodes[root].inclusiveNs) /
                                                   static_cast<double>(total));
                            start.push_back({root, x, w, 0});
                            x += w;
                        }
                        for (auto it = start.rbegin(); it != start.rend(); ++it)
                        {
                            stack.push_back(*it);
                        }
                        while (!stack.empty())
                        {
                            const Item item = stack.back();
                            stack.pop_back();
                            if (item.width < MIN_RECT_WIDTH)
                            {
                                ++m_culled;
                                continue;
                            }
                            const CaptureCallNode& node = m_model->tree.nodes[item.node];
                            const float y = static_cast<float>(item.level) * LEVEL_HEIGHT;
                            const bool picked = item.node == m_selected;
                            const u32 color = profilerNameColor(node.name);
                            painter.rect(Vec2{item.x0, y},
                                         Vec2{std::max(MIN_RECT_WIDTH, item.width - 0.5f), LEVEL_HEIGHT - 1.0f},
                                         picked ? outline : color);
                            m_levels[item.level].push_back({item.x0, item.x0 + item.width, item.node});
                            ++m_drawnRects;
                            m_levelCount = std::max(m_levelCount, item.level + 1);
                            if (item.width >= LABEL_MIN_WIDTH && m_labels < MAX_LABELS)
                            {
                                ++m_labels;
                                painter.text(
                                    Vec2{item.x0 + 2.0f, y}, Vec2{item.width - 3.0f, LEVEL_HEIGHT - 1.0f},
                                    profilerSafeText(node.name, std::max<u32>(4, static_cast<u32>(item.width / 6.5f))),
                                    onData, UiCanvasAlign::Left, 10.0f);
                            }
                            const std::span<const u32> kids = childrenOf(item.node);
                            if (kids.empty())
                            {
                                continue;
                            }
                            if (item.level + 1 >= levelsFit)
                            {
                                m_deeperCut += kids.size();
                                continue;
                            }
                            u64 sum = 0;
                            for (const u32 kid : kids)
                            {
                                sum += m_model->tree.nodes[kid].inclusiveNs;
                            }
                            const double denominator =
                                static_cast<double>(std::max<u64>(1, std::max(node.inclusiveNs, sum)));
                            std::vector<Item> next;
                            float cursor = item.x0;
                            for (const u32 kid : kids)
                            {
                                const float w = item.width *
                                    static_cast<float>(static_cast<double>(m_model->tree.nodes[kid].inclusiveNs) /
                                                       denominator);
                                next.push_back({kid, cursor, w, item.level + 1});
                                cursor += w;
                            }
                            for (auto it = next.rbegin(); it != next.rend(); ++it)
                            {
                                stack.push_back(*it);
                            }
                        }
                    });
            }

            [[nodiscard]] u32 nodeAt(const Vec2 local) const
            {
                if (local.y < 0.0f)
                {
                    return NONE;
                }
                const auto level = static_cast<sizet>(local.y / LEVEL_HEIGHT);
                if (level >= m_levels.size())
                {
                    return NONE;
                }
                const std::vector<FlameRect>& rects = m_levels[level];
                const auto it = std::ranges::upper_bound(rects, local.x, {}, &FlameRect::x0);
                if (it == rects.begin())
                {
                    return NONE;
                }
                const FlameRect& rect = *(it - 1);
                return local.x <= rect.x1 ? rect.node : NONE;
            }

            void pointerEvent(const UiCanvasPointer& pointer)
            {
                switch (pointer.kind)
                {
                case UiCanvasPointerKind::Move:
                case UiCanvasPointerKind::Enter:
                    if (const u32 node = nodeAt(pointer.local); node != m_hoverNode)
                    {
                        m_hoverNode = node;
                        updateHover(node);
                    }
                    break;
                case UiCanvasPointerKind::Leave:
                    if (m_hoverNode != NONE)
                    {
                        m_hoverNode = NONE;
                        updateHover(NONE);
                    }
                    break;
                case UiCanvasPointerKind::Click:
                    if (const u32 node = nodeAt(pointer.local); node != NONE)
                    {
                        m_host.send({{"command", "profilerCalls"},
                                     {"args",
                                      (pointer.modifiers & UiKeyModifier::SHIFT) != 0 ? Json{{"focus", node}}
                                                                                      : Json{{"select", node}}}});
                    }
                    break;
                default:
                    break;
                }
            }

            void updateHover(const u32 hovered)
            {
                const u32 node = hovered != NONE ? hovered : m_selected;
                if (!m_model || node == NONE || node >= m_model->tree.nodes.size())
                {
                    show(m_hover,
                         m_model && m_model->error.empty() && !m_model->tree.nodes.empty() ? m_host.text("calls.hint")
                                                                                           : std::string());
                    return;
                }
                const CaptureCallNode& n = m_model->tree.nodes[node];
                const std::string line = m_host.text("calls.hover",
                                                     {{"name", profilerSafeText(n.name, 64)},
                                                      {"inclusive", m_host.ms(static_cast<double>(n.inclusiveNs))},
                                                      {"percent", percent(n.inclusiveNs)},
                                                      {"self", m_host.ms(static_cast<double>(n.selfNs))},
                                                      {"count", m_host.whole(n.count)}});
                show(m_hover, hovered != NONE ? line : m_host.text("selected", {{"text", line}}));
            }

            void pointerLeaveReset() {}

            ProfilerViewHost m_host;
            Panel m_root;
            Dropdown m_lane;
            Button m_focusButton, m_upButton, m_resetButton;
            TextInput m_status, m_hover;
            Canvas m_flame;
            std::unique_ptr<Table> m_table;
            std::shared_ptr<std::atomic<u64>> m_treeGeneration, m_zoneGeneration;
            u64 m_treeValue = 0, m_zoneValue = 0;
            std::shared_ptr<Model> m_model;
            std::vector<Row> m_rows;
            std::vector<std::vector<FlameRect>> m_levels;
            ProfilerSelectionComponent m_selection;
            std::optional<std::pair<u32, u64>> m_expectedZone;
            std::string m_key, m_error, m_localeUsed;
            std::chrono::steady_clock::time_point m_since;
            NodePath m_focusPath, m_selectedPath;
            Vec2 m_drawnSize{};
            CaptureId m_capture = 0;
            u64 m_frames = 0;
            u64 m_drawnRects = 0, m_culled = 0, m_deeperCut = 0, m_labels = 0;
            u32 m_levelCount = 0;
            u32 m_focus = NONE, m_selected = NONE, m_hoverNode = NONE;
            sizet m_laneItems = 0;
            i32 m_laneFilter = -1;
            Sort m_sort = Sort::Inclusive;
            bool m_descending = true;
            bool m_active = false, m_loading = false, m_zoneLoading = false, m_dirty = true, m_rowsTruncated = false,
                 m_focusLost = false;
            ThemeObserver m_theme; // after m_dirty, which its callback sets
        };
    } // namespace

    std::unique_ptr<ProfilerView> makeCallsView(const ProfilerViewHost& host)
    {
        return std::make_unique<CallsView>(host);
    }
} // namespace spite
