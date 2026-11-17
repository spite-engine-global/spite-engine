#include "EditorSystemsGraph.hpp"

#include <algorithm>
#include <numeric>
#include <queue>

namespace spite
{
    namespace
    {
        struct Prerequisite
        {
            std::string_view module; // empty: any module
            std::string_view name;
        };

        Prerequisite parsePrerequisite(const std::string_view text)
        {
            const sizet split = text.rfind("::");
            return split == std::string_view::npos ? Prerequisite{{}, text}
                                                   : Prerequisite{text.substr(0, split), text.substr(split + 2)};
        }

        // SystemManager's matchesPrerequisite
        bool satisfies(const std::vector<Prerequisite>& prerequisites, const SystemsGraphInput& candidate)
        {
            return std::ranges::any_of(
                prerequisites, [&candidate](const Prerequisite& p)
                { return p.name == candidate.name && (p.module.empty() || p.module == candidate.module); });
        }

        void sortedUnique(std::vector<std::string> names, std::vector<std::string>& out)
        {
            std::ranges::sort(names);
            names.erase(std::unique(names.begin(), names.end()), names.end());
            out = std::move(names);
        }

        void intersect(const std::vector<std::string>& a, const std::vector<std::string>& b,
                       std::vector<std::string>& out)
        {
            std::ranges::set_intersection(a, b, std::back_inserter(out));
        }

        // SystemManager's hasConflict: the components one writes that the other reads or writes
        bool conflicting(const SystemsGraphNode& a, const SystemsGraphNode& b, std::vector<std::string>& components,
                         bool& writeWrite)
        {
            std::vector<std::string> shared;
            intersect(a.writes, b.writes, shared);
            writeWrite = !shared.empty();
            intersect(a.writes, b.reads, shared);
            intersect(a.reads, b.writes, shared);
            std::ranges::sort(shared);
            shared.erase(std::unique(shared.begin(), shared.end()), shared.end());
            components = std::move(shared);
            return !components.empty();
        }

        bool reaches(const std::vector<std::vector<u32>>& declared, const u32 from, const u32 to, std::vector<u8>& seen)
        {
            std::ranges::fill(seen, u8{0});
            std::vector<u32> stack{from};
            seen[from] = 1;
            while (!stack.empty())
            {
                const u32 at = stack.back();
                stack.pop_back();
                if (at == to)
                {
                    return true;
                }
                for (const u32 next : declared[at])
                {
                    if (!seen[next])
                    {
                        seen[next] = 1;
                        stack.push_back(next);
                    }
                }
            }
            return false;
        }

        void buildLane(SystemsGraph& graph, SystemsGraphLane& lane, const std::span<const SystemsGraphInput> inputs)
        {
            const u32 count = static_cast<u32>(inputs.size());
            lane.firstNode = static_cast<u32>(graph.nodes.size());
            lane.nodeCount = count;
            lane.firstEdge = static_cast<u32>(graph.edges.size());
            std::vector<std::vector<Prerequisite>> prerequisites(count);
            for (u32 i = 0; i < count; ++i)
            {
                const SystemsGraphInput& in = inputs[i];
                SystemsGraphNode node;
                node.id = in.module + "::" + in.name + "/" + std::string(SYSTEM_PHASES[in.phase]);
                node.module = in.module;
                node.name = in.name;
                node.phase = in.phase;
                node.enabled = in.enabled;
                node.after = in.runsAfter;
                sortedUnique(in.reads, node.reads);
                sortedUnique(in.writes, node.writes);
                for (const std::string& text : in.runsAfter)
                {
                    prerequisites[i].push_back(parsePrerequisite(text));
                }
                graph.index[node.id] = lane.firstNode + i;
                graph.nodes.push_back(std::move(node));
            }
            std::vector<std::vector<u32>> declared(count); // local successors over declared edges
            std::vector<u32> writeWrite; // indices into graph.edges of implied write/write edges
            for (u32 i = 0; i < count; ++i)
            {
                for (u32 j = i + 1; j < count; ++j)
                {
                    SystemsGraphEdge edge;
                    if (satisfies(prerequisites[j], inputs[i]))
                    {
                        edge = {lane.firstNode + i, lane.firstNode + j, true};
                        declared[i].push_back(j);
                    }
                    else if (satisfies(prerequisites[i], inputs[j]))
                    {
                        edge = {lane.firstNode + j, lane.firstNode + i, true};
                        declared[j].push_back(i);
                    }
                    else if (bool both = false; conflicting(graph.nodes[lane.firstNode + i],
                                                            graph.nodes[lane.firstNode + j], edge.components, both))
                    {
                        edge.from = lane.firstNode + i;
                        edge.to = lane.firstNode + j;
                        if (both)
                        {
                            writeWrite.push_back(static_cast<u32>(graph.edges.size()));
                        }
                    }
                    else
                    {
                        continue;
                    }
                    graph.edges.push_back(std::move(edge));
                }
            }
            // an implied write/write order is a conflict unless declared orders join the two systems anyway
            std::vector<u8> seen(count);
            const bool anyDeclared = std::ranges::any_of(declared, [](const auto& next) { return !next.empty(); });
            for (const u32 e : writeWrite)
            {
                SystemsGraphEdge& edge = graph.edges[e];
                const u32 a = edge.from - lane.firstNode;
                const u32 b = edge.to - lane.firstNode;
                if (!anyDeclared || (!reaches(declared, a, b, seen) && !reaches(declared, b, a, seen)))
                {
                    edge.conflict = true;
                    graph.nodes[edge.from].conflict = graph.nodes[edge.to].conflict = true;
                    ++lane.conflicts;
                }
            }
            lane.edgeCount = static_cast<u32>(graph.edges.size()) - lane.firstEdge;

            // columns: the longest chain before a node; what a cycle keeps from the queue goes after the last column
            std::vector<u32> pending(count, 0);
            for (u32 e = lane.firstEdge; e < graph.edges.size(); ++e)
            {
                const SystemsGraphEdge& edge = graph.edges[e];
                graph.nodes[edge.from].succs.push_back(edge.to);
                graph.nodes[edge.to].preds.push_back(edge.from);
                ++pending[edge.to - lane.firstNode];
            }
            std::queue<u32> ready;
            for (u32 i = 0; i < count; ++i)
            {
                if (pending[i] == 0)
                {
                    ready.push(i);
                }
            }
            u32 settled = 0;
            u32 deepest = 0;
            while (!ready.empty())
            {
                const u32 at = ready.front();
                ready.pop();
                ++settled;
                SystemsGraphNode& node = graph.nodes[lane.firstNode + at];
                deepest = std::max(deepest, node.depth);
                for (const u32 next : node.succs)
                {
                    SystemsGraphNode& after = graph.nodes[next];
                    after.depth = std::max(after.depth, node.depth + 1);
                    if (--pending[next - lane.firstNode] == 0)
                    {
                        ready.push(next - lane.firstNode);
                    }
                }
            }
            if (settled < count)
            {
                for (u32 i = 0; i < count; ++i)
                {
                    if (pending[i] != 0)
                    {
                        graph.nodes[lane.firstNode + i].cyclic = true;
                        graph.nodes[lane.firstNode + i].depth = deepest + 1;
                    }
                }
                ++deepest;
            }
            std::vector<u32> columnSize(count == 0 ? 0 : deepest + 1, 0);
            for (u32 i = 0; i < count; ++i)
            {
                SystemsGraphNode& node = graph.nodes[lane.firstNode + i];
                node.row = columnSize[node.depth]++;
            }
            lane.columnStart.assign(columnSize.size() + 1, 0);
            std::partial_sum(columnSize.begin(), columnSize.end(), lane.columnStart.begin() + 1);
            lane.order.assign(count, 0);
            for (u32 i = 0; i < count; ++i)
            {
                const SystemsGraphNode& node = graph.nodes[lane.firstNode + i];
                lane.order[lane.columnStart[node.depth] + node.row] = lane.firstNode + i;
            }
        }
    } // namespace

    SystemsGraph buildSystemsGraph(const std::span<const SystemsGraphInput> systems)
    {
        std::vector<u32> sorted(systems.size());
        std::iota(sorted.begin(), sorted.end(), 0u);
        std::ranges::stable_sort(sorted, {},
                                 [&systems](const u32 i) { return std::pair(systems[i].phase, systems[i].stage); });
        SystemsGraph graph;
        graph.nodes.reserve(systems.size());
        std::vector<SystemsGraphInput> laneInputs;
        for (sizet begin = 0; begin < sorted.size();)
        {
            const SystemsGraphInput& first = systems[sorted[begin]];
            laneInputs.clear();
            sizet end = begin;
            while (end < sorted.size() && systems[sorted[end]].phase == first.phase &&
                   systems[sorted[end]].stage == first.stage)
            {
                laneInputs.push_back(systems[sorted[end++]]);
            }
            SystemsGraphLane lane;
            lane.phase = first.phase;
            lane.stage = first.stage;
            lane.stageName = first.stageName.empty() ? std::to_string(first.stage) : first.stageName;
            buildLane(graph, lane, laneInputs);
            graph.lanes.push_back(std::move(lane));
            begin = end;
        }
        return graph;
    }
} // namespace spite
