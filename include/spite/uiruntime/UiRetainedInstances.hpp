#pragma once

#include <memory>
#include <span>
#include "uiview/UiFrame.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "uiruntime/UiPaintRow.hpp"

namespace spite
{
    class SPITE_UI_RUNTIME_API UiRetainedInstances
    {
    public:
        UiRetainedInstances();
        ~UiRetainedInstances();
        UiRetainedInstances(const UiRetainedInstances&) = delete;
        UiRetainedInstances& operator=(const UiRetainedInstances&) = delete;
        UiFrameGeometry& frame();
        std::span<const UiPaintRow> paintRows(const World& world, const UiHierarchyManager& hierarchy, u32 first,
                                              u32 last);
        std::span<const UiPaintRecord> records() const;
        std::span<const UiPaintRecord> recordsOf(u32 position) const;
        bool hasElements(u32 count) const;
        void resetRecords(u32 elements, std::vector<std::string>& spareTexts);
        void appendRecords(u32 first, u32 last, std::vector<UiPaintRecord>& records, std::span<const u32> ends,
                           std::vector<std::string>& spareTexts);
        std::span<const UiPaintRange> collectDirty(const UiHierarchyManager& hierarchy,
                                                   std::span<const UiPaintRange> damage,
                                                   std::span<const Entity> changedStates);
        u32 offset(u32 position) const;
        void reset(u32 elements);
        void begin();
        void append(u32 first, u32 last, u64 generation, bool exact, std::span<const UiInstance> instances,
                    std::span<const u32> ends);
        void finish(u64 generation, bool partial);

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
