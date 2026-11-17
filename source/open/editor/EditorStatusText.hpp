#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "EditorPanelUi.hpp"
#include "ui/UiHierarchyManager.hpp"

// A line of status text that never draws outside its box: the box clips, a text wider than the box ends
// in "..." (a path loses its middle, so its file name stays), and a tooltip on the box carries the full text.
namespace spite::panelui
{
    class StatusTips;

    class StatusText
    {
    public:
        // The text takes `id` and its box (`id`.box) `desc` (the caller's size and layout item) and lays its text out
        // in a row; with `tips` the text joins them. A box with no `parent` waits for one (a document slot). The text
        // is read-only and has no label of its own, so `nameKey` is its accessible name.
        void create(UiScope& scope, InternedString id, InternedString nameKey, const Widget& parent,
                    const UiRectDesc& desc, StatusTips* tips, bool middle = false);

        // Whether what is drawn changed
        bool set(const std::string_view value)
        {
            if (value == m_full && m_fitted == width())
            {
                return false;
            }
            m_full.assign(value);
            return fit();
        }

        // The box got another width (a resize, a scroll bar): fits again
        void tick()
        {
            if (!m_full.empty() && m_fitted != width())
            {
                fit();
            }
        }

        [[nodiscard]] const std::string& full() const { return m_full; }
        [[nodiscard]] const std::string& shown() const { return m_shown; }
        [[nodiscard]] bool truncated() const { return m_truncated; }
        [[nodiscard]] float width() const
        {
            const UiHierarchyManager& hierarchy = m_scope->hierarchy();
            return hierarchy.absoluteRectOf(text.entity()).size.x / std::max(hierarchy.layoutScale(), 1e-3f);
        }

        Panel box;
        TextInput text;

    private:
        // the inner margins of a text input: the run must end this far inside
        static constexpr float SLACK = 10.0f;

        [[nodiscard]] std::string fitted(const float room) const
        {
            const UiTextMeasurer* measurer = m_scope->hierarchy().textMeasurer();
            if (!measurer || room <= 0.0f || measurer->measureText(text.entity(), m_full).x <= room)
            {
                return m_full;
            }
            std::vector<u32> starts; // byte offset of each code point
            for (u32 i = 0; i < m_full.size(); ++i)
            {
                if ((static_cast<unsigned char>(m_full[i]) & 0xC0) != 0x80)
                {
                    starts.push_back(i);
                }
            }
            starts.push_back(static_cast<u32>(m_full.size()));
            const auto build = [&](const u32 kept)
            {
                const u32 head = m_middle ? kept / 3 : kept;
                const u32 tail = kept - head;
                std::string out = m_full.substr(0, starts[head]);
                out += "...";
                out += m_full.substr(starts[starts.size() - 1 - tail]);
                return out;
            };
            u32 low = 0, high = static_cast<u32>(starts.size()) - 1; // code points kept: the most that fit
            while (low < high)
            {
                const u32 mid = (low + high + 1) / 2;
                if (measurer->measureText(text.entity(), build(mid)).x <= room)
                {
                    low = mid;
                }
                else
                {
                    high = mid - 1;
                }
            }
            return build(low);
        }

        bool fit();

        UiScope* m_scope = nullptr;
        StatusTips* m_tips = nullptr;
        std::string m_full;
        std::string m_shown;
        float m_fitted = -1.0f;
        bool m_middle = false;
        bool m_truncated = false;
        bool m_tipped = false;

        friend class StatusTips;
    };

    // The one tooltip of a panel's status texts: a text is attached while it is cut, and the tip shows its full text.
    class StatusTips
    {
    public:
        void create(UiScope& scope, const InternedString id)
        {
            tip = scope.createTooltip(id, UiTooltipDesc{});
            tip.onShow(
                [this](const Entity target)
                {
                    for (const StatusText* item : m_items)
                    {
                        if (item->box.entity() == target)
                        {
                            tip.label().setText(item->full());
                            return;
                        }
                    }
                });
        }

        void add(StatusText& item) { m_items.push_back(&item); }
        [[nodiscard]] const std::vector<StatusText*>& items() const { return m_items; }

        void sync(StatusText& item, const bool cut)
        {
            if (cut != item.m_tipped)
            {
                item.m_tipped = cut;
                cut ? tip.attach(item.box) : tip.detach(item.box);
            }
        }

        Tooltip tip;

    private:
        std::vector<StatusText*> m_items;
    };

    inline void StatusText::create(UiScope& scope, const InternedString id, const InternedString nameKey,
                                   const Widget& parent, const UiRectDesc& desc, StatusTips* tips, const bool middle)
    {
        m_scope = &scope;
        m_tips = tips;
        m_middle = middle;
        box = scope.createPanel(InternedString(std::string(id.toView()) + ".box"), desc);
        if (parent.isValid())
        {
            box.setParent(parent);
        }
        box.setLayout(stack(UiLayoutDirection::Row, 0.0f, {}, UiLayoutAlign::Stretch));
        scope.world().getComponent<UiStateComponent>(box.entity()).clipsChildren = true;
        UiRectDesc textDesc = topLeft();
        textDesc.layoutItem = fillItem();
        text = readOnlyText(scope, id, textDesc);
        text.setParent(box);
        text.setAccessibleNameKey(nameKey);
        if (tips)
        {
            tips->add(*this);
        }
    }

    inline bool StatusText::fit()
    {
        m_fitted = width();
        const std::string next = fitted(m_fitted - SLACK);
        m_truncated = next.size() != m_full.size();
        const bool changed = show(text, next);
        m_shown = next;
        if (m_tips)
        {
            m_tips->sync(*this, m_truncated);
        }
        return changed;
    }
} // namespace spite::panelui
