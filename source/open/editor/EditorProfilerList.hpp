#pragma once

#include <functional>
#include <string>
#include <vector>

#include "EditorPanelUi.hpp"

namespace spite
{
    // One row of the Profiler's capture list: two lines, the time and tag over the duration, frames and
    // size. `warn` paints the second line as a failure.
    struct ProfilerListRow
    {
        std::string title;
        std::string detail;
        bool warn = false;
    };

    // The capture list: a virtual list of two-line rows that is ONE Tab stop. The list takes the focus, Up / Down /
    // Home / End / Page Up / Page Down move the cursor row (highlighted), Enter and Space activate it, Delete asks for
    // its removal; a click moves the cursor to the row and activates it. The cursor is a row index the panel keeps
    // pointing at the same capture across rebuilds (setCursor). Rows are read through `read` only for visible slots.
    class ProfilerCaptureList
    {
    public:
        struct Host
        {
            UiScope* scope = nullptr;
            panelui::DensityFollower* density = nullptr;
            std::function<void(u32 row, ProfilerListRow& out)> read;
            std::function<void(u32 row)> activate;
            std::function<void()> remove; // Delete on the cursor row
            std::function<void()> cursorMoved;
        };

        // The row's height at a density: two text rows
        [[nodiscard]] static float rowHeight(const panelui::Density& density) { return 2.0f * density.row; }

        explicit ProfilerCaptureList(Host host);
        ~ProfilerCaptureList() = default;
        ProfilerCaptureList(const ProfilerCaptureList&) = delete;
        ProfilerCaptureList& operator=(const ProfilerCaptureList&) = delete;

        VirtualList list;
        u64 binds = 0; // slot binds that read a row
        u64 writes = 0; // line texts rewritten

        void setRowCount(u32 count);
        void rebindVisible();
        // -1: none. Clamped to the rows; scrolls the row into view when `reveal`.
        void setCursor(i32 row, bool reveal = true);
        [[nodiscard]] i32 cursor() const { return m_cursor; }
        [[nodiscard]] u32 firstRow() const { return static_cast<u32>(list.scrollOffset().y / list.rowHeight()); }
        [[nodiscard]] u32 slotCount() const { return static_cast<u32>(m_slots.size()); }
        // The shown rows' two lines, top to bottom
        [[nodiscard]] std::vector<std::vector<std::string>> visibleLines() const;
        // The key handling (Up, Down, ...): true when handled
        bool key(u32 scancode, u8 modifiers, bool repeat);

    private:
        struct Slot
        {
            Panel root;
            Panel highlight;
            Panel clips[2];
            Panel lines[2]; // plain text: a value on a panel, as the table view's cells (a text input would paint a
                            // field)
            std::string shown[2];
            bool warn = false;
            u32 row = panelui::NO_ROW;
            float width = -1.0f;
        };

        Widget createSlot(u32 index);
        void layout(Slot& slot);
        void bind(u32 index);

        Host m_host;
        std::vector<Slot> m_slots;
        i32 m_cursor = -1;
        ProfilerListRow m_scratch;
    };
} // namespace spite
