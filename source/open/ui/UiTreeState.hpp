#pragma once

#include <functional>
#include <span>
#include <vector>

#include "ui/UiComponents.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class UiScope;

    // A tree view's runtime half, owned by its scope: the model, the visible
    // nodes in row order (the list's rows) and the row pool's parts.
    // Expansion, selection and the drag target live in UiTreeComponent.
    class UiTreeState
    {
    public:
        static constexpr u32 NO_ROW = static_cast<u32>(-1);

        struct Row
        {
            Entity node = Entity::undefined();
            u32 depth = 0;
            u32 sibling = 0; // index among its parent's children
        };

        struct Place
        {
            Entity parent = Entity::undefined();
            u32 sibling = 0;
        };

        struct Slot
        {
            Panel root;
            Panel wash; // invalid without UiTreeDesc::mark: the first part, under the highlight
            Panel highlight;
            Label toggle;
            Panel chip; // invalid without UiTreeDesc::chip
            Label icon; // invalid without UiTreeDesc::icon
            Panel bar; // the readout's track and fill: invalid without UiTreeDesc::barSize
            Panel barFill;
            bool selected = false; // icon and text wear their .selected classes
            Label text;
            Label mark; // invalid without UiTreeDesc::mark
            Label detail; // invalid without UiTreeDesc::detail
            bool detailWarn = false; // detail wears <rowClass>.detail.warn
            bool dim = false; // text wears <rowClass>.text.inactive, or .inactive.selected on a selected row
            bool dimSelected = false;
            InternedString tooltip{}; // the key the row root's tooltip shows
            std::string tooltipName; // the literal beside it: the whole label of a row that clips it
            std::string markName; // the tag's explicit accessible name: the full text while it shows its short form
            Entity node = Entity::undefined();
        };

        struct DropTarget
        {
            Entity parent = Entity::undefined();
            u32 index = 0; // in the parent's current children
            bool into = false;
            Vec2 marker{0.0f, 0.0f}; // content-space indicator origin
        };

        [[nodiscard]] const UiTreeComponent& tree() const;
        [[nodiscard]] UiTreeComponent& writableTree() const;
        [[nodiscard]] bool hasChildren(Entity node) const { return !desc.children(node).empty(); }
        [[nodiscard]] bool isExpanded(Entity node) const;
        [[nodiscard]] u32 rowOf(Entity node) const;

        Widget createRow(InternedString id);
        // the row root takes rowClass, its parts <rowClass>.selected, .toggle, .chip and .text
        void styleRow(Slot& slot) const;
        void bindRow(u32 slot, u32 row);
        void appendVisible(Entity node, u32 depth, std::vector<Row>& out) const;
        void refresh();
        void setExpanded(Entity node, bool expanded);
        void select(Entity node, UiTreeSelect mode);
        void setSelection(std::span<const Entity> nodes);
        void apply(UiTreeCommand command, bool extend);
        void scrollTo(Entity node);
        void click(u32 slot, Vec2 position, u8 modifiers);
        void contextMenu(Entity node, Vec2 position);
        void beginDrag();
        void updateDrag(Vec2 position);
        void endDrag(Vec2 position);
        // keepOpen: the drop's parent, whose auto-opened ancestors stay open; none: a cancel closes them all
        void cancelDrag(Entity keepOpen = Entity::undefined());
        void advance(double seconds);
        void placeDragged();
        void showDropTarget(Entity node);
        void hideDropTarget();
        [[nodiscard]] DropTarget targetAt(Vec2 position) const;
        bool drop(std::span<const Entity> nodes, Entity parent, u32 index);

        UiScope* scope = nullptr;
        UiTreeDesc desc;
        VirtualList list;
        Panel indicator;
        std::vector<Row> rows;
        std::vector<Slot> slots;
        Entity pressed = Entity::undefined();
        std::vector<Entity> dragSet; // the dragged nodes, sorted
        std::vector<Place> dragPlaces; // where the dragged nodes sit, for the drop index
        Vec2 dragPosition{0.0f, 0.0f}; // where the pointer last was in the drag
        bool externalDrop = false; // the indicator shows another panel's drag (showDropTarget)
        Vec2 externalOffset{0.0f, 0.0f};
        Vec2 externalSize{0.0f, 0.0f};
        bool dropRefused = false; // the target is a dragged node or inside one
        std::vector<Entity> dragOpened; // the nodes this drag opened, closed again unless the drop is inside them
        Entity dragHover = Entity::undefined(); // the collapsed node the drag rests on
        double dragHoverSince = 0.0;
        double time = 0.0; // the presenter's clock (advance)
        Entity lastClicked = Entity::undefined(); // a plain click's node, for the slow second click
        double lastClickAt = 0.0;
        std::function<void(std::span<const Entity>)> onSelect;
        std::function<void(std::span<const Entity>, Entity, u32)> onDrop;
        std::function<void(Entity)> onRename;
        std::function<void()> onDropRefused;
        std::function<void(Entity, Vec2)> onContextMenu;
    };
} // namespace spite
