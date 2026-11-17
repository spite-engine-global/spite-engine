#pragma once

// What the Profiler GUI's counter, memory and call-tree views read from a capture: std-only
// value types answered by CaptureStore. The Profiler panel's worker and the control queries call the same store
// methods, so the GUI and an LLM read the same numbers.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace spite
{
    // True when a long store call should stop at its next step (the caller was superseded or went away).
    using CaptureCancelFn = std::function<bool()>;

    struct CaptureCounterName
    {
        std::string name;
        std::uint64_t samples = 0; // frames with a sample
    };

    // One counter over frames [first, first + values.size()): dense, 0 where the counter had no sample (as the zones
    // report's counters.<name>.samples).
    struct CaptureCounterSeries
    {
        std::uint64_t first = 0;
        std::vector<std::uint64_t> values;
        std::uint64_t total = 0;
        std::uint64_t max = 0;
    };

    // A memory space's totals over the session; the same numbers as the zones report's memory.<space>.
    struct CaptureMemorySpace
    {
        bool present = false;
        std::uint64_t allocCount = 0, allocBytes = 0, freeCount = 0, freeBytes = 0, netEndBytes = 0, netPeakBytes = 0;
    };

    struct CaptureMemoryTag
    {
        std::uint8_t space = 0; // 0 ram, 1 gpu
        std::string tag;
        std::uint64_t count = 0, bytes = 0, maxBytes = 0;
    };

    // A big-allocation event (RAM >= 256 KB, GPU >= 64 KB); kind 0 alloc, 1 free, 2 grow (prevBytes: the capacity
    // before it).
    struct CaptureMemoryEvent
    {
        std::int64_t tNs = 0;
        std::uint8_t space = 0, kind = 0;
        std::uint64_t bytes = 0, prevBytes = 0;
        std::string tag;
    };

    struct CaptureMemory
    {
        bool hasTotals = false; // false: a capture without the memory section (or a sidecar from before)
        CaptureMemorySpace spaces[2];
        std::vector<CaptureMemoryTag> tags; // by space, then bytes descending (the report's order)
        std::vector<CaptureMemoryEvent> events; // in time order, the window's, at most the caller's cap
        std::uint64_t eventsInWindow = 0; // events the window holds (>= events.size(): the largest are kept)
        std::uint64_t eventsDropped = 0; // events the profiler's ring refused during the capture
    };

    // One node of an aggregated call tree: a zone name under one parent path. A node's parent comes before it
    // (nodes are in creation order), so a parent index is always smaller. lane >= 0 marks a thread-lane node (the
    // roots: `name` is the thread's name, inclusive is the sum of its children, self 0, count the zones of its
    // direct children); other nodes have lane -1.
    struct CaptureCallNode
    {
        std::uint32_t parent = ~0u; // ~0u: a root
        std::int32_t lane = -1;
        std::uint32_t depth = 0; // roots 0
        std::string name;
        std::uint64_t inclusiveNs = 0, selfNs = 0, count = 0;
        // the longest single zone merged into the node (an exemplar the views select): its seq in the node's lane
        std::uint64_t longestNs = 0, longestSeq = 0;
        std::uint32_t thread = 0; // the lane its zones ran on (for every node, not only the lane nodes)
    };

    // The zones that began inside a window, merged by name along their parent chains: inclusive = the sum of their
    // durations, self = inclusive less the inclusive of the node's children, count = the zones merged. A zone whose
    // parent began before the window (or is absent) hangs under its lane's node.
    struct CaptureCallTree
    {
        std::vector<CaptureCallNode> nodes;
        std::uint64_t zones = 0; // zones aggregated
        std::uint64_t zonesDropped =
            0; // zones left out because the node cap was reached (their time stays in the parent's self)
        bool truncated = false; // a cap (nodes or zones) stopped the aggregation short
        std::uint64_t windowStartNs = 0, windowEndNs = 0;
    };
} // namespace spite
