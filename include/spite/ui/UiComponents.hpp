#pragma once

#include <array>
#include <string>
#include <vector>

#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "base/Platform.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include "ui/UiExport.hpp"

namespace spite
{
    // The retained UI element component set. All plain data: no callbacks, no
    // transient interaction state — behavior lives in the ui module's systems
    // and the presenter layer's side tables, never here.

    // Placement inside the parent rect (ui data): the
    // element's `pivot` (normalized point in its own rect) is placed at
    // `anchor * parentSize + offset`. Roots have no parent; their position is
    // `offset - pivot * size` in surface space. A layout container's children
    // ignore anchor, offset and pivot: the container places them, and `size`
    // is the item's measured size.
    struct UiRectComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiRect";
        Vec2 anchor{0.0f, 0.0f};
        Vec2 offset{0.0f, 0.0f};
        Vec2 size{0.0f, 0.0f};
        Vec2 pivot{0.5f, 0.5f};
        i32 zOffset = 0; // orders siblings (root surfaces too); paint order derives from the tree
        SPITE_SCHEMA(UiRectComponent, anchor, offset, size, pivot, zOffset)
    };

    // Content sizing: layout resolve replaces the fitted axes of
    // UiRectComponent::size with the measured content plus padding on both
    // sides. On a layout container the content is its laid-out items
    // (UiLayoutComponent padding and gaps included) and `padding` insets them
    // on top of the layout padding, on both axes; on any other element the
    // text (label, else value), which needs a text measurer on the hierarchy —
    // without one the rect size stands. A label of a wrapping style fitted in
    // height only wraps at its rect width, or at the width a layout container
    // gives it (stretch, grow, shrink), and the container's height follows
    // (UiTextMeasurer::measureTextBlock).
    struct UiContentSizeComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiContentSize";
        bool fitWidth = true;
        bool fitHeight = true;
        Vec2 padding{0.0f, 0.0f};
        SPITE_SCHEMA(UiContentSizeComponent, fitWidth, fitHeight, padding)
    };

    enum class UiLayoutDirection : u8
    {
        Row, // items left to right
        Column, // items top to bottom
    };

    // Where the items sit along the main axis when they leave space free.
    // SpaceBetween adds the free space between items evenly (Start for one
    // item or none free).
    enum class UiLayoutJustify : u8
    {
        Start,
        Center,
        End,
        SpaceBetween,
    };

    // Cross-axis placement. Stretch sizes the item to the container's inner
    // cross size (within its min/max); Auto on an item defers to the
    // container's `align` (and reads as Start there).
    enum class UiLayoutAlign : u8
    {
        Auto,
        Start,
        Center,
        End,
        Stretch,
    };

    // A layout container: its children are laid out in one line
    // along `direction`, in declared order, `gap` apart, inside `padding`
    // (left, top, right, bottom). Hidden children take no space. Unscaled UI
    // pixels. Add UiContentSizeComponent to size the container to its items.
    // `wrap` breaks the children into lines: an item that does not fit the
    // inner main size starts the next line (a lone item longer than the line
    // shrinks to it). Each line flexes and justifies on its own; lines stack
    // from the cross start `gap` apart, each as tall as its largest item
    // (Stretch fills that line), and never grow to the container's cross
    // size. A wrapping container fitted on its cross axis only is as tall as
    // its lines at the main size its rect authors (an authored main size of 0
    // is one line); fitted on its main axis it never wraps, whatever its
    // parent gives it. The cross fit does not follow a main size a parent
    // gives (stretch, grow, shrink): lines that break at the given size
    // overflow a cross fit measured at the authored one .
    struct UiLayoutComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiLayout";
        UiLayoutDirection direction = UiLayoutDirection::Row;
        float gap = 0.0f;
        Vec4 padding{0.0f, 0.0f, 0.0f, 0.0f};
        UiLayoutJustify justify = UiLayoutJustify::Start;
        UiLayoutAlign align = UiLayoutAlign::Start;
        bool wrap = false;
        SPITE_SCHEMA(UiLayoutComponent, direction, gap, padding, justify, align, wrap)
    };

    // How a container child flexes; a child without one uses these defaults.
    // Its main size starts at `basis` (negative: its measured size) clamped
    // to [minSize, maxSize]; free space grows items by `grow` weight,
    // overflow shrinks them by `shrink` weight times that size. A maxSize
    // axis of 0 is unbounded. Ignored outside a container.
    struct UiLayoutItemComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiLayoutItem";
        float grow = 0.0f;
        float shrink = 1.0f;
        float basis = -1.0f;
        Vec2 minSize{0.0f, 0.0f};
        Vec2 maxSize{0.0f, 0.0f};
        UiLayoutAlign alignSelf = UiLayoutAlign::Auto;
        SPITE_SCHEMA(UiLayoutItemComponent, grow, shrink, basis, minSize, maxSize, alignSelf)
    };

    // The paint-only runtime override tweens and presenters animate; absent = identity. It never
    // changes layout or hit testing. It applies to the element and its whole subtree, nested ones composing:
    // `opacity` multiplies the alpha, `tint` (rgba, 0..1) multiplies the colors, `scale` scales around the
    // element's own rect centre and `translate` then moves (unscaled UI pixels). A clip owned inside the subtree
    // moves with it; a clip above the element stays. Raw writes call UiHierarchyManager::markVisualDirty.
    struct UiVisualComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiVisual";
        float opacity = 1.0f;
        Vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};
        Vec2 translate{0.0f, 0.0f};
        Vec2 scale{1.0f, 1.0f};
        SPITE_SCHEMA(UiVisualComponent, opacity, SPITE_FIELD(tint, COLOR), translate, scale)
    };

    // The entries of a style's shadow stack: the shadow fields of UiStyleAnimComponent are arrays of this
    // many, entry i being entry i of UiStyle::shadows.
    inline constexpr u32 UI_MAX_SHADOWS = 4;

    // The animated copy of an element's transitionable style values, added by the view to an element
    // whose resolved style declares a transition. The unprefixed fields are what paints (rgba 0..1, border and outline
    // width, shadow offset, blur and spread, outline offset, corner radii in unscaled UI pixels, opacity): a tween
    // moves them from their current value to the `target*` fields, which the view sets to the style it last resolved.
    // Settled: the two are equal. Written by the view and the tween system; a raw write to a displayed field repaints
    // the element only. The shadow fields are one value per stack entry (a tween names an entry as `shadowColor[1]`);
    // bit i of shadowInset says entry i is inset, which snaps. An entry the style no longer has fades its color out
    // and keeps its shape.
    struct UiStyleAnimComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiStyleAnim";
        Vec4 backgroundColor{0.0f, 0.0f, 0.0f, 0.0f};
        Vec4 textColor{0.0f, 0.0f, 0.0f, 0.0f};
        Vec4 borderColor{0.0f, 0.0f, 0.0f, 0.0f};
        std::array<Vec4, UI_MAX_SHADOWS> shadowColor{};
        float borderWidth = 0.0f;
        std::array<Vec2, UI_MAX_SHADOWS> shadowOffset{};
        std::array<float, UI_MAX_SHADOWS> shadowBlur{};
        float opacity = 1.0f;
        Vec4 outlineColor{0.0f, 0.0f, 0.0f, 0.0f};
        float outlineWidth = 0.0f;
        float outlineOffset = 0.0f;
        Vec4 cornerRadius{0.0f, 0.0f, 0.0f, 0.0f}; // top-left, top-right, bottom-right, bottom-left
        std::array<float, UI_MAX_SHADOWS> shadowSpread{};
        u32 shadowInset = 0;
        Vec4 targetBackgroundColor{0.0f, 0.0f, 0.0f, 0.0f};
        Vec4 targetTextColor{0.0f, 0.0f, 0.0f, 0.0f};
        Vec4 targetBorderColor{0.0f, 0.0f, 0.0f, 0.0f};
        std::array<Vec4, UI_MAX_SHADOWS> targetShadowColor{};
        float targetBorderWidth = 0.0f;
        std::array<Vec2, UI_MAX_SHADOWS> targetShadowOffset{};
        std::array<float, UI_MAX_SHADOWS> targetShadowBlur{};
        float targetOpacity = 1.0f;
        Vec4 targetOutlineColor{0.0f, 0.0f, 0.0f, 0.0f};
        float targetOutlineWidth = 0.0f;
        float targetOutlineOffset = 0.0f;
        Vec4 targetCornerRadius{0.0f, 0.0f, 0.0f, 0.0f};
        std::array<float, UI_MAX_SHADOWS> targetShadowSpread{};
        u32 targetShadowInset = 0;
        SPITE_SCHEMA(UiStyleAnimComponent, backgroundColor, textColor, borderColor, shadowColor, borderWidth,
                     shadowOffset, shadowBlur, opacity, outlineColor, outlineWidth, outlineOffset, cornerRadius,
                     shadowSpread, shadowInset, targetBackgroundColor, targetTextColor, targetBorderColor,
                     targetShadowColor, targetBorderWidth, targetShadowOffset, targetShadowBlur, targetOpacity,
                     targetOutlineColor, targetOutlineWidth, targetOutlineOffset, targetCornerRadius,
                     targetShadowSpread, targetShadowInset)
    };

    // UI pixels a scroll view moves per wheel notch; a panel that forwards a wheel to a scroll view uses the same step.
    inline constexpr float UI_WHEEL_STEP = 40.0f;

    // A scroll view's state, on its viewport: the content scrolls by `offset`,
    // clamped to [0, contentSize - viewport size]; unscaled UI pixels. A view
    // with `enabled` off stays at offset 0 (wheel, drags and thumbs do
    // nothing, the thumbs hide): its content is clipped to the viewport.
    struct UiScrollComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiScroll";
        Vec2 offset{0.0f, 0.0f};
        Vec2 contentSize{0.0f, 0.0f};
        bool enabled = true;
        SPITE_SCHEMA(UiScrollComponent, offset, contentSize, enabled)
    };

    // A slot: a container a UI document owns whose children code builds and owns. A document apply never
    // lists, creates, destroys, reorders or writes them; the code that fills the slot reads these fields: `scroll`,
    // whether its content scrolls inside the slot, and `childClass`, the style class it gives what it builds there
    // (invalid: its own).
    struct UiSlotComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiSlot";
        bool scroll = true;
        InternedString childClass{};
        bool operator==(const UiSlotComponent& other) const
        {
            return scroll == other.scroll && childClass == other.childClass;
        }
        SPITE_SCHEMA(UiSlotComponent, scroll, childClass)
    };

    // A virtualized list's extent, on its viewport: rowCount rows of
    // rowHeight, of which only the visible ones exist as elements. Rows are
    // contentWidth wide, at least the viewport's width.
    struct UiVirtualListComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiVirtualList";
        u32 rowCount = 0;
        float rowHeight = 0.0f;
        float contentWidth = 0.0f;
        SPITE_SCHEMA(UiVirtualListComponent, rowCount, rowHeight, contentWidth)
    };

    // A tree view's state, on its viewport. `expanded` and `selected` are
    // sorted by entity; `cursor` is the last clicked or navigated node and
    // `anchor` the node Shift ranges start from. During a drag, `dragged`
    // holds the moving nodes and dropParent/dropIndex the target under the
    // pointer (TreeView::drop semantics) while `dropValid`.
    struct UiTreeComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiTree";
        std::vector<Entity> expanded;
        std::vector<Entity> selected;
        Entity cursor = Entity::undefined();
        Entity anchor = Entity::undefined();
        std::vector<Entity> dragged;
        Entity dropParent = Entity::undefined();
        u32 dropIndex = 0;
        bool dropValid = false;
        SPITE_SCHEMA(UiTreeComponent, expanded, selected, cursor, anchor, dragged, dropParent, dropIndex, dropValid)
    };

    // On a tree view's pooled row: the node it shows.
    struct UiTreeRowComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiTreeRow";
        Entity node = Entity::undefined();
        u32 depth = 0;
        SPITE_OBSERVED_SCHEMA(UiTreeRowComponent, node, depth)
    };

    // A table's state, on its root. Rows sort by the `sortColumn` field
    // (none: model order). `selected` holds row keys sorted by entity;
    // `cursor` is the last clicked or navigated key and `anchor` the key
    // Shift ranges start from. The sort is read-only to tools: it changes
    // only by UiTableOp.
    struct UiTableComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiTable";
        InternedString sortColumn{};
        bool descending = false;
        std::vector<Entity> selected;
        Entity cursor = Entity::undefined();
        Entity anchor = Entity::undefined();
        SPITE_SCHEMA(UiTableComponent, SPITE_FIELD(sortColumn, READONLY), SPITE_FIELD(descending, READONLY), selected,
                     cursor, anchor)
    };

    // On a table's header cell: its column, by field, its width and whether a header click sorts by it (the
    // field's kind sorts). Read-only to tools: the width changes only by UiTableOp.
    struct UiTableColumnComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiTableColumn";
        InternedString field{};
        float width = 0.0f;
        bool sortable = false;
        SPITE_SCHEMA(UiTableColumnComponent, SPITE_FIELD(field, READONLY), SPITE_FIELD(width, READONLY),
                     SPITE_FIELD(sortable, READONLY))
    };

    // On a table's pooled row: the model row and key it shows.
    struct UiTableRowComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiTableRow";
        Entity key = Entity::undefined();
        u32 row = 0;
        SPITE_OBSERVED_SCHEMA(UiTableRowComponent, key, row)
    };

    // A dock space's layout, on its root element: `main` fills the space (or
    // undefined when nothing is docked); each `floating` node is an in-window
    // floating window placed by its own UiRect, later ones in front;
    // `maximized` is the tab group of `main` that fills the space while the
    // rest of the tree is hidden (undefined: none). Dock layout fields are
    // read-only to tools: the layout changes only by UiDockOp.
    struct UiDockComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiDock";
        Entity main = Entity::undefined();
        std::vector<Entity> floating;
        Entity maximized = Entity::undefined();
        SPITE_SCHEMA(UiDockComponent, SPITE_FIELD(main, READONLY), SPITE_FIELD(floating, READONLY),
                     SPITE_FIELD(maximized, READONLY))
    };

    enum class UiDockNodeKind : u8
    {
        Tabs,
        Split,
    };

    // A dock layout node, on its element. Tabs: `tabs` are panel hosts
    // (UiDockPanelComponent) in tab order, `active` shows. Split: `first`
    // left of (vertical: above) `second`, a splitter between them; `first`
    // takes `ratio` of the space the splitter leaves.
    struct UiDockNodeComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiDockNode";
        UiDockNodeKind kind = UiDockNodeKind::Tabs;
        std::vector<Entity> tabs;
        u32 active = 0;
        bool vertical = false;
        float ratio = 0.5f;
        Entity first = Entity::undefined();
        Entity second = Entity::undefined();
        SPITE_SCHEMA(UiDockNodeComponent, SPITE_FIELD(kind, READONLY), SPITE_FIELD(tabs, READONLY),
                     SPITE_FIELD(active, READONLY), SPITE_FIELD(vertical, READONLY), SPITE_FIELD(ratio, READONLY),
                     SPITE_FIELD(first, READONLY), SPITE_FIELD(second, READONLY))
    };

    // A dock panel's host element: `name` identifies the panel in layouts and
    // dock ops. A closed panel's host sits hidden under the dock root.
    struct UiDockPanelComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiDockPanel";
        InternedString name{};
        SPITE_SCHEMA(UiDockPanelComponent, name)
    };

    // A popup menu's state, on its root: `path` holds one entry per open
    // nesting level, outermost first: the level's highlighted row (undefined
    // for none), so every entry but the last is the item whose submenu is
    // open. `highlighted` is the deepest highlighted item's id. Read-only to
    // tools: the state changes only through Menu.
    struct UiMenuComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiMenu";
        bool open = false;
        std::vector<Entity> path;
        InternedString highlighted{};
        SPITE_OBSERVED_SCHEMA(UiMenuComponent, SPITE_FIELD(open, READONLY | OBSERVED),
                              SPITE_FIELD(path, READONLY | OBSERVED), SPITE_FIELD(highlighted, READONLY | OBSERVED))
    };

    // On a menu's pooled row: the item it shows (default while unused).
    struct UiMenuItemComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiMenuItem";
        InternedString id{};
        bool enabled = true;
        bool separator = false;
        bool submenu = false;
        bool checkable = false; // a menuitemcheckbox: `checked` is its state
        bool checked = false;
        SPITE_OBSERVED_SCHEMA(UiMenuItemComponent, id, enabled, separator, submenu, checkable, checked)
    };

    // A drop target's state during a drag: the in-flight payload is over it and it accepts or
    // rejects it; None otherwise. Read-only to tools: UiDragDrop owns it.
    enum class UiDropState : u8
    {
        None,
        Accepting,
        Rejecting,
    };

    // On an element registered with UiDragDrop::addTarget.
    struct UiDropTargetComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiDropTarget";
        UiDropState state = UiDropState::None;
        SPITE_SCHEMA(UiDropTargetComponent, SPITE_FIELD(state, READONLY))
    };

    enum class UiOverlayKind : u8
    {
        Tooltip, // hover (or programmatic) popup that never takes input or focus
        Modal, // dialog: input below blocked, focus trapped, stacks
    };

    // The side of its anchor a popup prefers; it flips to the opposite side when it does not fit there.
    enum class UiPlacementSide : u8
    {
        Below,
        Above,
        Right,
        Left,
    };

    // An overlay's state, on its root surface. `target` is the element a tooltip describes
    // (undefined for a point anchor or a modal); `placed` is the side the last placement used, `flipped` and
    // `clamped` say it left the preferred side or moved to stay inside the bounds. `stack` is a modal's depth
    // (1 the bottom one), 0 for a closed overlay or a tooltip. Read-only to tools: the state changes only through
    // Tooltip and Modal.
    struct UiOverlayComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiOverlay";
        UiOverlayKind kind = UiOverlayKind::Tooltip;
        bool open = false;
        Entity target = Entity::undefined();
        UiPlacementSide side = UiPlacementSide::Below;
        UiPlacementSide placed = UiPlacementSide::Below;
        bool flipped = false;
        bool clamped = false;
        bool follow = false;
        bool closeOnEscape = true;
        bool closeOnBackdrop = false;
        u32 stack = 0;
        SPITE_SCHEMA(UiOverlayComponent, SPITE_FIELD(kind, READONLY), SPITE_FIELD(open, READONLY),
                     SPITE_FIELD(target, READONLY), SPITE_FIELD(side, READONLY), SPITE_FIELD(placed, READONLY),
                     SPITE_FIELD(flipped, READONLY), SPITE_FIELD(clamped, READONLY), SPITE_FIELD(follow, READONLY),
                     SPITE_FIELD(closeOnEscape, READONLY), SPITE_FIELD(closeOnBackdrop, READONLY),
                     SPITE_FIELD(stack, READONLY))
    };

    // On an element with a tooltip: hovering it (or a descendant with none of its own) for `delay` seconds of
    // world time shows `overlay`; `shown` while it does. Read-only to tools: Tooltip::attach and detach own it.
    struct UiTooltipComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiTooltip";
        Entity overlay = Entity::undefined();
        float delay = 0.5f;
        bool shown = false;
        // Set by Widget::setTooltip: the message the scope's shared tooltip shows for this element, and the shortcut
        // beside it (a literal, empty: none).
        InternedString key{};
        std::string shortcut;
        SPITE_SCHEMA(UiTooltipComponent, SPITE_FIELD(overlay, READONLY), SPITE_FIELD(delay, READONLY),
                     SPITE_FIELD(shown, READONLY), SPITE_FIELD(key, READONLY), SPITE_FIELD(shortcut, READONLY))
    };

    // Interaction gates for hit testing and keyboard focus. Optional on an
    // element; absence means visible and interactable, neither a tab stop nor
    // a focus scope.
    struct UiStateComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiState";
        bool visible = true;
        bool interactable = true;
        bool clipsChildren = false; // children clip to this element's absolute rect
        // Tab and Shift+Tab stop here while it is visible and interactable.
        bool tabStop = false;
        // Tab traversal from focus inside stays inside (a dock panel's host);
        // without one, the focus's surface.
        bool focusScope = false;
        // A focus scope the landmark cycle (F6, Shift+F6, the shoulder buttons) visits, in document order while it is
        // visible and holds a tab stop; implies focusScope. A document written before landmarks has none.
        bool landmark = false;
        float dragThreshold = 0.0f; // distance from the press in UI units before a drag starts
        SPITE_SCHEMA(UiStateComponent, visible, interactable, clipsChildren, tabStop, focusScope, landmark,
                     dragThreshold)
    };

    // Explicit neighbours of an element for directional navigation (UiRouter::navigate): the element focus moves
    // to when the intent is pressed with focus on this element or inside it, instead of the nearest tab stop in
    // that direction. Undefined: no override. A neighbour that is gone, hidden, not interactable or blocked by an
    // open modal is skipped, as if unset.
    struct UiNavigationComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiNavigation";
        Entity up = Entity::undefined();
        Entity down = Entity::undefined();
        Entity left = Entity::undefined();
        Entity right = Entity::undefined();
        SPITE_SCHEMA(UiNavigationComponent, up, down, left, right)
    };

    // What an element is to assistive tooling, derived from its kind and components (UiAccessibility.hpp);
    // `uiTree` reports it. None leaves the role derived; any other enumerator can be set on an element explicitly.
    enum class UiRole : u8
    {
        None,
        Button,
        TextBox,
        SpinButton,
        CheckBox,
        Switch,
        Slider,
        ProgressBar,
        RadioGroup,
        Radio,
        Image,
        ScrollArea,
        Menu,
        MenuItem,
        Tree,
        TreeItem,
        Table,
        Row,
        Label,
        Canvas,
        ComboBox,
        TabList,
        Tab,
        Chart,
        Option, // a row of a dropdown's list
        Presentation, // decoration (a dropdown's arrow): no name, and not a name source for its parent
        ListBox, // a popup list's panel: its options are the controls, so it needs no name
        Grid, // a table view's rows list: one tab stop, its rows reached with the arrow keys
        ColumnHeader, // a table view's header cell; uiTree adds its sort state
        GridCell, // a table view's cell, named by its text
        Alert, // a message that interrupts (a toast of a warning or error): named by its text, announced at once
        Status, // a message that updates without interrupting (an info toast)
        Dialog, // a modal's dialog window (set by createModal); name it with setAccessibleName(Key)
        AlertDialog, // a modal that interrupts for a decision (UiModalDesc::alert)
        MenuItemCheckBox, // a menu item with a checked state (UiMenuItem::checkable)
        Separator, // a focusable divider the arrow keys move (a dock splitter)
        Application, // a region that handles its own input (an editor view panel's viewport)
    };

    // Whether an element opens and closes content (aria-expanded): None reports nothing.
    enum class UiExpandState : u8
    {
        None,
        Collapsed,
        Expanded,
    };

    // Optional, an explicit accessible name or role over the derived ones: `name` is literal text, `nameKey` a
    // localization key resolved when read (so it follows the locale); with both, the key wins while the table has it.
    // `role` None keeps the derived role. `hasSelected` marks a choice the element is one of (a toolbar mode, a
    // filter), `selected` its state, reported beside the name rather than inside the text; `expand` the state of a row
    // that opens and closes content (a list's section header).
    struct UiAccessibleComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiAccessible";
        std::string name;
        InternedString nameKey{};
        UiRole role = UiRole::None;
        bool hasSelected = false;
        bool selected = false;
        UiExpandState expand = UiExpandState::None;
        SPITE_SCHEMA(UiAccessibleComponent, name, nameKey, role, hasSelected, selected, expand)
    };

    struct UiLabelComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiLabel";
        // owned: label text changes at runtime and must not grow the string interner
        std::string text;
        // The text is rich text markup (UiRichText.hpp): parsed when the text changes, never for a label
        // without it. A text input's value is never markup.
        bool rich = false;
        // A Phosphor icon name (UiIcons.hpp): when set the element draws that icon in place of `text`, in
        // the icons font, coloured and sized by its style; a name the table lacks draws nothing. Empty: the text. A
        // style's own `icon` shows when this is empty.
        std::string icon;
        SPITE_SCHEMA(UiLabelComponent, text, rich, icon)
    };

    // Marks a label or button whose text is a localized message: the layout system resolves `key`
    // through the Localization service with `args` and copies the result into UiLabelComponent::text, only when
    // the locale or a table changed (Localization::revision) or this component was written; a missing key shows
    // the key (also while no Localization service is bound). `args` is one "name=value" per line; a value that
    // parses whole as a number is a number (formatted with the message language's separators), anything else a
    // string; a value starting with ' is a string of the rest ("id='007", written by setLocalizedArgText). A raw write
    // of key or args re-resolves this element alone. Label::setText / Button::setText remove the component (explicit
    // text wins).
    struct UiLocalizedTextComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiLocalizedText";
        InternedString key{};
        std::string args;
        SPITE_SCHEMA(UiLocalizedTextComponent, key, args)
    };

    // Mutable text-input value (presenters bind variables to it).
    struct UiValueComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiValue";
        std::string text;
        SPITE_SCHEMA(UiValueComponent, text)
    };

    // A text input's placeholder (TextInput::setPlaceholder): `text` is drawn in the placeholderColor style while the
    // value is empty and no composition is active. With a valid `key` the text is the localized message (resolved like
    // UiLocalizedTextComponent, without arguments, when the locale or a table changed or the component was added); a
    // raw write of `key` is not picked up, use TextInput::setLocalizedPlaceholder. Absent: no placeholder.
    struct UiPlaceholderComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiPlaceholder";
        std::string text;
        InternedString key{};
        SPITE_SCHEMA(UiPlaceholderComponent, text, key)
    };

    // A text input's editing state, on every text input: byte offsets into
    // UiValueComponent::text on UTF-8 boundaries; the selection spans anchor
    // to caret. `editing` while the input holds focus (the caret shows).
    struct UiTextEditComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiTextEdit";
        u32 caret = 0;
        u32 anchor = 0;
        bool editing = false;
        // The input method's composition at the caret while editing: shown underlined, not part of the
        // value until committed; the selected clause is `compositionSelected` bytes from `compositionCursor`.
        std::string composition;
        u32 compositionCursor = 0;
        u32 compositionSelected = 0;
        SPITE_SCHEMA(UiTextEditComponent, caret, anchor, editing, SPITE_FIELD(composition, OBSERVED | READONLY),
                     SPITE_FIELD(compositionCursor, OBSERVED | READONLY),
                     SPITE_FIELD(compositionSelected, OBSERVED | READONLY))
    };

    // A number field's value. The text is the formatted value until edited;
    // `valid` says whether the edited text parses. A commit parses, clamps to
    // [min, max] and rounds to `decimals`; invalid text reverts. A horizontal
    // drag changes the value by `step` per UI pixel.
    struct UiNumericComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiNumeric";
        double value = 0.0;
        double min = -1.0e300;
        double max = 1.0e300;
        double step = 0.1;
        u8 decimals = 3;
        bool valid = true;
        SPITE_SCHEMA(UiNumericComponent, value, min, max, step, decimals, valid)
    };

    // A checkbox's or toggle's state, on its root. A raw write is applied to the visuals by the next layout update
    // (no onChange: only input and uiActivate fire it).
    struct UiToggleComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiToggle";
        bool checked = false;
        SPITE_SCHEMA(UiToggleComponent, checked)
    };

    // A slider's value and range, on its root. Every write is normalized the next update: a non-finite bound
    // reads as its default (0, 1), min above max swap, a non-finite or negative step is 0 (continuous), the value
    // clamps to [min, max] and a positive step snaps it to min + n * step (the last step clamps to max); a
    // non-finite value keeps the value shown.
    struct UiSliderComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiSlider";
        double value = 0.0;
        double min = 0.0;
        double max = 1.0;
        double step = 0.0;
        SPITE_SCHEMA(UiSliderComponent, value, min, max, step)
    };

    // A progress bar's value and range, on its root; normalized as a slider's (no step).
    struct UiProgressComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiProgress";
        double value = 0.0;
        double min = 0.0;
        double max = 1.0;
        SPITE_SCHEMA(UiProgressComponent, value, min, max)
    };

    // A radio group's selection, on its root: the index of the checked item in creation order, -1 for none.
    // An index with no item reads as the selection shown.
    struct UiRadioGroupComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiRadioGroup";
        i32 selected = -1;
        SPITE_SCHEMA(UiRadioGroupComponent, selected)
    };

    // On a radio item: its position in the group and whether it is the selection. Written by the group only.
    struct UiRadioItemComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiRadioItem";
        u32 index = 0;
        bool checked = false;
        SPITE_SCHEMA(UiRadioItemComponent, SPITE_FIELD(index, READONLY), SPITE_FIELD(checked, READONLY))
    };

    // A dropdown's or combo box's state, on its root: the selected item index (-1 none; a raw write with
    // no such item reads as the selection shown, like a radio group's) and what the widget reports. `open` is the
    // popup list, `highlighted` its keyboard/hover row (-1 none), `count` the items, `filter` a combo's typed text
    // while its list is filtered. Written by the widget only, but `selected`.
    struct UiDropdownComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiDropdown";
        i32 selected = -1;
        bool open = false;
        i32 highlighted = -1;
        u32 count = 0;
        std::string filter;
        SPITE_SCHEMA(UiDropdownComponent, selected, SPITE_FIELD(open, READONLY), SPITE_FIELD(highlighted, READONLY),
                     SPITE_FIELD(count, READONLY), SPITE_FIELD(filter, OBSERVED | READONLY))
    };

    // A tab bar's active tab index on its root (-1 none); a raw write with no such tab reads as the tab shown.
    struct UiTabBarComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiTabBar";
        i32 active = -1;
        SPITE_SCHEMA(UiTabBarComponent, active)
    };

    // On a tab: its position in the bar and whether it is the active one. Written by the bar only.
    struct UiTabComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiTab";
        u32 index = 0;
        bool active = false;
        SPITE_SCHEMA(UiTabComponent, SPITE_FIELD(index, READONLY), SPITE_FIELD(active, READONLY))
    };

    // A canvas's summary, on its element: the primitives and regions a presenter emitted live in the owning
    // scope (UiCanvas.hpp), this is what tools see. `generation` grows with every update (0: never emitted),
    // `primitives` is the list's length, `dropped` how many the last update could not add for `maxPrimitives`.
    struct UiCanvasComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiCanvas";
        u64 generation = 0;
        u32 primitives = 0;
        u32 maxPrimitives = 100000;
        u32 dropped = 0; // primitives the canvas's bound or the context budget refused (an append adds to it)
        u32 droppedRegions = 0; // regions beyond UI_CANVAS_MAX_REGIONS
        SPITE_SCHEMA(UiCanvasComponent, SPITE_FIELD(generation, READONLY), SPITE_FIELD(primitives, READONLY),
                     SPITE_FIELD(maxPrimitives, READONLY), SPITE_FIELD(dropped, READONLY),
                     SPITE_FIELD(droppedRegions, READONLY))
    };

    // A chart's state on its element, written by the chart only: the axes as shown, the window of samples,
    // the hover and the selection. The ticks, per-series hover values and the plot rect are in uiTree's `chart`.
    struct UiChartComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiChart";
        u32 seriesCount = 0;
        bool histogram = false;
        bool xRelative = false; // x labels are offsets from the newest sample
        bool yAuto = true;
        bool aligned = true;
        double xMin = 0.0; // the axes as labelled
        double xMax = 1.0;
        double yMin = 0.0;
        double yMax = 1.0;
        u64 windowFirst = 0; // the first absolute sample shown, and how many
        u32 windowCount = 0;
        bool hovering = false;
        u64 hoverSample = 0; // the sample (histogram: bin) nearest the pointer
        double hoverX = 0.0;
        bool selecting = false; // a drag in progress
        bool selected = false;
        double selectionMin = 0.0;
        double selectionMax = 0.0;
        u64 selectionFirst = 0;
        u64 selectionLast = 0;
        bool banded = false; // Chart::setBand
        double bandMin = 0.0;
        double bandMax = 0.0;
        SPITE_SCHEMA(UiChartComponent, SPITE_FIELD(seriesCount, READONLY), SPITE_FIELD(histogram, READONLY),
                     SPITE_FIELD(xRelative, READONLY), SPITE_FIELD(yAuto, READONLY), SPITE_FIELD(aligned, READONLY),
                     SPITE_FIELD(xMin, READONLY), SPITE_FIELD(xMax, READONLY), SPITE_FIELD(yMin, READONLY),
                     SPITE_FIELD(yMax, READONLY), SPITE_FIELD(windowFirst, READONLY),
                     SPITE_FIELD(windowCount, READONLY), SPITE_FIELD(hovering, READONLY),
                     SPITE_FIELD(hoverSample, READONLY), SPITE_FIELD(hoverX, READONLY),
                     SPITE_FIELD(selecting, READONLY), SPITE_FIELD(selected, READONLY),
                     SPITE_FIELD(selectionMin, READONLY), SPITE_FIELD(selectionMax, READONLY),
                     SPITE_FIELD(selectionFirst, READONLY), SPITE_FIELD(selectionLast, READONLY),
                     SPITE_FIELD(banded, READONLY), SPITE_FIELD(bandMin, READONLY), SPITE_FIELD(bandMax, READONLY))
    };

    // Render-handle placeholder; the future view module types the slot.
    struct UiImageComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiImage";
        u64 imageSlot = 0;
        SPITE_OBSERVED_SCHEMA(UiImageComponent, imageSlot)
    };

    enum class UiWidgetKind : u8
    {
        Panel,
        Button,
        Label,
        TextInput,
        Image,
        ScrollView,
        ScrollThumb,
        NumberField,
        Checkbox,
        Toggle,
        Slider,
        ProgressBar,
        RadioGroup,
        Radio,
        Canvas,
        Dropdown,
        Combo,
        TabBar,
        Tab,
        Chart,
    };

    // Written by the scope builder that created the element (styleClass later
    // by Widget::setStyleClass); the view
    // module keys styles on it (id overrides style class overrides kind).
    struct UiIdentityComponent : IUiComponent
    {
        static constexpr const char* SPITE_COMPONENT_NAME = "UiIdentity";
        InternedString id{};
        UiWidgetKind kind = UiWidgetKind::Panel;
        InternedString styleClass{}; // invalid = none; set through Widget::setStyleClass
        SPITE_SCHEMA(UiIdentityComponent, id, kind, styleClass)
    };
} // namespace spite
