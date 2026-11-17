#pragma once

#include <array>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "EditorPanelUi.hpp"
#include "ui/UiScope.hpp"
#include "uiview/UiStyleSheet.hpp"

namespace spite
{
    inline constexpr std::array<std::string_view, 2> SYSTEM_PHASES{"update", "fixed"};

    // One phase of a registered system, as the `systems` query lists it.
    struct SystemsGraphInput
    {
        std::string module;
        std::string name;
        u32 phase = 0; // index into SYSTEM_PHASES
        u64 stage = 0;
        std::string stageName;
        bool enabled =
            true; // drawn dimmed when off; its orderings and conflicts stay (the graph is the schedule's shape)
        std::vector<std::string> runsAfter; // "Module::Name", or "Name" for any module
        std::vector<std::string> reads;
        std::vector<std::string> writes;
    };

    struct SystemsGraphNode
    {
        std::string id; // "module::name/phase", the Systems panel's row identity
        std::string module;
        std::string name;
        u32 phase = 0;
        bool enabled = true;
        u32 depth = 0; // the column: the longest chain of systems that must run before it in its stage
        u32 row = 0; // its place in the column, in registration order
        bool conflict = false; // one end of a write/write edge nothing orders
        bool cyclic = false; // on or behind a cycle of orderings (the scheduler would never finish the stage)
        std::vector<u32> preds; // direct neighbours over every edge, node indices
        std::vector<u32> succs;
        std::vector<std::string> after; // the runsAfter it declares
        std::vector<std::string> reads;
        std::vector<std::string> writes;
    };

    // Two systems of one stage and phase the scheduler runs in this order (SystemManager::buildDependencyGraph): a
    // declared one (runsAfter), or an implied one, registration order, between systems whose component access
    // conflicts and that nothing declares an order for.
    struct SystemsGraphEdge
    {
        u32 from = 0;
        u32 to = 0;
        bool declared = false;
        bool conflict = false; // implied, both write a component, and no chain of declared orders joins them
        std::vector<std::string> components; // the components that conflict (empty when declared)
    };

    // The systems of one (phase, stage): the scheduler runs a stage's graph as one unit, stages in sequence.
    struct SystemsGraphLane
    {
        u32 phase = 0;
        u64 stage = 0;
        std::string stageName;
        u32 firstNode = 0;
        u32 nodeCount = 0;
        u32 firstEdge = 0;
        u32 edgeCount = 0;
        u32 conflicts = 0;
        std::vector<u32> order; // the lane's nodes by (column, row)
        std::vector<u32> columnStart; // offsets into `order`; one more than the columns
        [[nodiscard]] u32 columns() const { return columnStart.empty() ? 0 : static_cast<u32>(columnStart.size() - 1); }
    };

    struct SystemsGraph
    {
        std::vector<SystemsGraphNode> nodes; // lane by lane
        std::vector<SystemsGraphEdge> edges;
        std::vector<SystemsGraphLane> lanes; // (phase, stage) ascending
        std::unordered_map<std::string, u32> index; // node id -> node
    };

    // The graph the scheduler builds, per lane. The input order within a lane is the registration order.
    [[nodiscard]] SystemsGraph buildSystemsGraph(std::span<const SystemsGraphInput> systems);

    struct SystemsGraphHost
    {
        UiScope* scope = nullptr;
        panelui::DensityFollower* density = nullptr;
        const UiStyleSheet* styles = nullptr;
        std::function<void(std::string)> send; // queues a control request line
    };

    // The graph tab of the Systems panel: a toolbar (the stage shown), a canvas drawing the stage's systems as nodes
    // in columns by dependency depth with an edge per ordering, and two lines listing the selected system's
    // neighbours and component access. Wheel zooms, a drag pans, a click or the arrow keys select (always through
    // the panel's systemsSelect command). Only the nodes and edges in view are emitted; nothing is drawn again while
    // the view, the selection, the graph, the size and the style stay.
    class SystemsGraphView
    {
    public:
        static constexpr float ZOOM_MIN = 0.1f;
        static constexpr float ZOOM_MAX = 2.5f;

        SystemsGraphView(SystemsGraphHost host, const SystemsGraph& graph);

        [[nodiscard]] Widget root() const { return m_root; }

        // The panel's graph was rebuilt in place: node indices changed, the lane and selection are kept by identity.
        void graphChanged();
        void setSelected(std::string_view id); // "" for none; shows the lane and the node
        void nodesChanged() { m_dirty = true; } // a node's enabled state moved: the graph is the same, the paint is not
        // The stage by name or number, in `phase` (-1: the first phase that has it); false, changing nothing, for a
        // stage the graph has not.
        bool setLane(i32 phase, std::string_view stage);
        void setView(float zoom, float x, float y);
        void resetView();
        void update(); // every frame while shown
        void shownChanged()
        {
            m_dirty = true;
            m_reveal = true;
        }

        [[nodiscard]] std::string lanesText() const; // "update/UPDATE, fixed/FIXED..." for a refusal
        void writeState(IFormatWriter& out) const;
        [[nodiscard]] float zoom() const { return m_zoom; }
        [[nodiscard]] Vec2 origin() const { return m_origin; }

    private:
        struct Palette
        {
            u32 text = 0, muted = 0, fill = 0, outline = 0, selected = 0, touched = 0, conflict = 0, data = 0;
        };

        void rebuildLaneChoice();
        void fitLaneChoice();
        bool followSelection(); // switches to the selected node's lane when the shown one lacks it
        void refreshPalette();
        void showInfo();
        void redraw(Vec2 size);
        void pointerEvent(const UiCanvasPointer& pointer);
        bool keyEvent(u32 scancode);
        void pick(u32 node);
        void reveal(u32 node);
        void zoomAt(Vec2 at, float notches);
        void pan(Vec2 delta);
        void clampView();
        [[nodiscard]] float nodeW() const;
        [[nodiscard]] float nodeH() const;
        [[nodiscard]] float colStep() const;
        [[nodiscard]] float rowStep() const;
        [[nodiscard]] Vec2 nodePos(const SystemsGraphNode& node) const;
        [[nodiscard]] u32 nodeAt(Vec2 local) const;
        [[nodiscard]] const SystemsGraphLane* lane() const;
        [[nodiscard]] Vec2 canvasSize() const;

        SystemsGraphHost m_host;
        const SystemsGraph* m_graph = nullptr;
        Panel m_root;
        Panel m_bar;
        Dropdown m_laneChoice;
        Label m_conflicts;
        Canvas m_canvas;
        Label m_order;
        Label m_access;
        u32 m_lane = 0;
        u32 m_lanePhase = 0; // the lane shown by identity, kept across a rebuild
        u64 m_laneStage = 0;
        std::string m_selectedId;
        u32 m_selected = ~0u; // node index of m_selectedId, ~0u for none
        float m_zoom = 1.0f;
        Vec2 m_origin{}; // the graph-space point at the canvas's top-left
        bool m_dirty = true;
        Palette m_palette;
        u64 m_paletteSheet = ~0ull;
        u64 m_seenLayout = ~0ull;
        panelui::Density m_density;
        Vec2 m_drawnSize{};
        u32 m_drawnNodes = 0;
        u32 m_drawnEdges = 0;
        u32 m_drawnDashes = 0;
        float m_laneFit = 0.0f; // the width the stage choice was last given
        u64 m_redraws = 0;
        float m_textHeight = 0.0f;
        bool m_dragged = false;
        bool m_reveal = false; // the selected node is brought into view at the next update
    };
} // namespace spite
