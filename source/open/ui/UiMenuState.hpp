#pragma once

#include <functional>
#include <optional>
#include <string_view>
#include <vector>

#include "ui/UiComponents.hpp"
#include "ui/UiLayout.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class UiScope;

    // A menu's runtime half, owned by its scope: the open model and, per
    // nesting level, the popup panel, its highlight and its pooled rows.
    // Open state mirrors into UiMenuComponent.
    class UiMenuState
    {
    public:
        static constexpr u32 NONE = static_cast<u32>(-1);

        struct Slot
        {
            Label row;
            Label arrow;
            Label check; // the check icon of a checked item, at the right edge like the submenu arrow
            Panel line;
        };

        struct Level
        {
            Panel panel;
            Panel highlight;
            std::vector<Slot> slots;
            const std::vector<UiMenuItem>* items = nullptr;
            u32 highlighted = NONE;
            float width = 0.0f; // unscaled
            UiRect rect; // as placed, layout pixels
        };

        [[nodiscard]] UiMenuComponent& menu() const;
        [[nodiscard]] bool contains(Entity element) const;
        [[nodiscard]] float rowTop(const Level& level, u32 index) const;
        [[nodiscard]] std::optional<UiRect> bounds() const;
        [[nodiscard]] UiRect place(Vec2 size, const UiRect& anchor, bool beside) const;

        void open(std::span<const UiMenuItem> items, const UiRect& anchor, Entity from);
        void close();
        bool apply(UiMenuCommand command);
        bool activate(InternedString item);
        // Highlights the next enabled item of the innermost level whose label starts with the first character of the
        // UTF-8 `text` (case-folded: ASCII, Latin-1, Greek, Cyrillic).
        bool typeAhead(std::string_view text);
        // Whether onCycle took the move to the neighbouring menu.
        bool cycle(i32 direction);

        Level& level(u32 index);
        void addSlot(u32 levelIndex);
        void openLevel(u32 levelIndex, const std::vector<UiMenuItem>& items, const UiRect& anchor, bool beside);
        // Opens the highlighted item's submenu beside its row.
        void openSubmenu(u32 levelIndex);
        void closeFrom(u32 levelIndex);
        void highlight(u32 levelIndex, u32 index);
        [[nodiscard]] u32 nextSelectable(u32 levelIndex, u32 from, bool forward) const;
        void hover(u32 levelIndex, u32 slot);
        void click(u32 levelIndex, u32 slot);
        void fire(InternedString item);
        void focusLost();
        void sync();

        UiScope* scope = nullptr;
        InternedString id{};
        UiMenuDesc desc;
        Panel root;
        std::vector<UiMenuItem> model;
        std::vector<Level> levels;
        u32 depth = 0; // open levels
        Entity returnFocus = Entity::undefined();
        Entity dismissedBy = Entity::undefined(); // the element whose press last closed the menu
        Entity openedFrom = Entity::undefined(); // the anchor of the last open; undefined for point opens
        std::function<void(InternedString)> onActivate;
        std::function<void(i32)> onCycle;
    };
} // namespace spite
