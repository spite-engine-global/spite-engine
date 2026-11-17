#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <typeinfo>
#include <vector>

#include "base/ImageRange.hpp"
#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "ecs/EcsTypes.hpp"
#include "ui/UiCanvas.hpp"
#include "ui/UiChart.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiEvents.hpp"
#include "ui/UiExport.hpp"
#include "ui/UiInputSnapshot.hpp"
#include "ui/UiLayout.hpp"
#include "ui/UiLayoutTokens.hpp"

namespace spite
{
    class UiScope;
    class UiTreeState;
    class UiDockState;
    class UiMenuState;
    class UiTableState;
    class UiChartState;

    // Builder parameter blocks: per-widget-type "default parameters", written
    // into components at creation. Theme/style systems are out of scope.
    struct UiRectDesc
    {
        Vec2 anchor{0.0f, 0.0f};
        Vec2 offset{0.0f, 0.0f};
        Vec2 size{0.0f, 0.0f};
        Vec2 pivot{0.5f, 0.5f};
        i32 zOffset = 0;
        // Set: the element carries it and sizes the fitted axes to its text
        // (a layout container: to its items).
        std::optional<UiContentSizeComponent> contentSize;
        // Set: the element is a layout container placing its children.
        std::optional<UiLayoutComponent> layout;
        // Set: how the element flexes inside its layout container.
        std::optional<UiLayoutItemComponent> layoutItem;
    };

    // Non-owning, generation-checked view over a scope-owned element. Copies
    // are cheap; caching is for use (setters, state), never for keeping the
    // element alive. Stale-handle use asserts in dev/Test and no-ops with a
    // log in release. Destruction follows ownership (the scope), never
    // parentage; there are no destructors on handles.
    class SPITE_UI_API Widget
    {
    public:
        Widget() = default;
        Widget(UiScope* scope, Entity entity, u32 generation);

        [[nodiscard]] bool isValid() const;
        [[nodiscard]] Entity entity() const { return m_entity; }
        [[nodiscard]] UiScope* scope() const { return m_scope; }
        // The same element through a typed handle (a UiViewContract attach function's Button, Slider...); the
        // caller knows the kind.
        template <std::derived_from<Widget> T>
        [[nodiscard]] T as() const
        {
#if SPITE_DEBUG_BUILD
            checkAs(typeid(T));
#endif
            return T(m_scope, m_entity, m_generation);
        }

        void setParent(const Widget& parent);
        void setVisible(bool visible);
        [[nodiscard]] bool isVisible() const;
        void setInteractable(bool interactable);
        // The explicit neighbour directional navigation moves to from this element (or from focus inside it) in
        // `direction` (Up, Down, Left or Right), instead of the nearest tab stop that way; an invalid widget clears
        // it. Any element may be the target; one that is hidden, disabled or gone when the intent comes is skipped.
        void setNeighbor(UiNavIntent direction, const Widget& target);
        // Marks the element a landmark: a focus scope (Tab stays inside it) the landmark intents cycle, F6 and
        // Shift+F6 or a pad's shoulder buttons, in document order; focus enters at its first tab stop. A landmark
        // that is hidden, behind a modal or without a tab stop is skipped. uiTree reports `landmark`.
        void setLandmark(bool landmark);
        // Names the stylesheet class this element styles by, between its id and its kind.
        void setStyleClass(InternedString styleClass);
        // A tooltip for the element, shown by the scope's own shared tooltip: the message `key` of the Localization
        // service (re-resolved with the locale), with `shortcut` (a literal such as "Ctrl+Z") beside it. It shows
        // after the hover delay (0.5 s of world time) or when keyboard focus (Tab, F6, an arrow) rests on the element
        // for it, anchored beside the element so it never covers it; a press, the pointer leaving, the focus moving
        // and Escape hide it. Calling again changes the text; an invalid key removes the tooltip. Documents set it
        // with the "tooltip" attribute (a key). uiTree reports it as `tooltip.key` and `tooltip.shortcut`.
        void setTooltip(InternedString key, std::string_view shortcut = {});
        // The accessible name and role uiTree reports, over the derived ones (UiAccessibility.hpp): a
        // literal name, or a localization key resolved when read so it follows the locale. An input, slider or
        // number field has no text of its own and takes one; an icon-only button too.
        void setAccessibleName(std::string_view name);
        void setAccessibleNameKey(InternedString key);
        void setAccessibleRole(UiRole role);
        // Marks the element one of a set of choices and whether it is the chosen one, for uiTree's "selected": the
        // state belongs here, not in the text (no "[Move]").
        void setAccessibleSelected(bool selected);
        // The expanded or collapsed state a row that opens content reports; None clears it. Equal state writes nothing.
        void setAccessibleExpanded(UiExpandState state);
        [[nodiscard]] bool isInteractable() const;
        void setAnchor(Vec2 anchor);
        void setOffset(Vec2 offset);
        [[nodiscard]] Vec2 offset() const;
        // Through any handle, also resyncs what the size drives: a scroll
        // view's offset and list pool, a dock space's layout. A raw UiRect
        // write skips that.
        void setSize(Vec2 size);
        // The rect size; for a scroll view, list, table or dock space a layout container places, the size the
        // container gave it at the last resolve (what those resync to).
        [[nodiscard]] Vec2 size() const;
        void setPivot(Vec2 pivot);
        void setZOffset(i32 zOffset);
        // Makes the element a layout container (or updates it): it places its
        // children from then on.
        void setLayout(const UiLayoutComponent& layout);
        // How the element flexes inside its layout container.
        void setLayoutItem(const UiLayoutItemComponent& item);
        // Binds one layout field to a token of the context's layout token table (UiContext::layoutTokens):
        // the field takes the token's value now and whenever the table changes; an invalid token unbinds it (the
        // value stays). setLayout and setLayoutItem re-apply the bound fields, so the call order does not matter; a
        // tween or raw component write to a bound field holds until the table next changes, or until a write to the
        // element's UiLayoutTokensComponent, which re-applies all of its bindings in the next frame.
        void bindLayoutToken(UiLayoutField field, InternedString token);

        // Reaction slots: one per kind, a new registration replaces the
        // previous. Unhandled kinds remain plain ECS events any system can
        // subscribe to.
        void onClick(std::function<void()> fn);
        // Presses of any button over the element (0 primary, 1 secondary,
        // 2 middle) at the pointer position: context menus open on a
        // secondary press, dropdowns on a primary one. Like onClick and
        // onDrag, it replaces the widget's own reaction of that kind
        // (TextInput/NumberField place the caret on PointerDown).
        void onPress(std::function<void(u8 button, Vec2 position)> fn);
        void onDrag(std::function<void(Vec2 delta)> fn);
        void onDragEnd(std::function<void()> fn);
        void onWheel(std::function<void(Vec2 wheelDelta)> fn);
        // Presses and OS repeats (flagged) bubbling from focus; modifiers are
        // UiKeyModifier bits. Return whether the key was handled: only handled
        // keys are claimed from gameplay, the rest bubble on.
        void onKeyDown(std::function<bool(u32 scancode, u8 modifiers, bool repeat)> fn);
        void onTextInput(std::function<void(std::string_view text)> fn);
        void onFocus(std::function<void(bool gained)> fn);

    protected:
        void react(UiEventKind kind, std::function<void(const void*)> fn, CodeOrigin origin = {});
        // Debug: asserts the element's kind fits the typed handle as<T>() makes (typed handles of the common kinds).
        void checkAs(const std::type_info& type) const;

        UiScope* m_scope = nullptr;
        Entity m_entity = Entity::undefined();
        u32 m_generation = 0;
    };

    struct SPITE_UI_API Panel : Widget
    {
        using Widget::Widget;
        Panel() = default;
    };

    // A widget showing text (label, button): literal through setText, or a localized message.
    struct SPITE_UI_API TextWidget : Widget
    {
        using Widget::Widget;
        TextWidget() = default;

        // Shows the message `key` of the Localization service, re-resolved when the locale or a table changes
        // (UiLocalizedTextComponent); a missing key shows the key. Arguments set so far stay. An invalid key
        // returns to literal text. setText replaces a localized message with literal text.
        void setLocalizedText(InternedString key);
        // Sets one message argument of a localized text, creating it if absent; only this element re-resolves,
        // and only when the value changed. False when the widget is not localized or the name/value cannot be
        // stored (empty name, a name containing '=' or a newline, a value containing a newline).
        // The string_view overload stores the value as a number when it parses WHOLE as one ("12" is 12, "1e5" is
        // 100000, "007" is 7), else as a string; setLocalizedArgText always stores a string ("007" stays "007").
        // The integral and double overloads store numbers (formatted with the message language's separators).
        bool setLocalizedArg(std::string_view name, std::string_view value);
        bool setLocalizedArgText(std::string_view name, std::string_view value);
        bool setLocalizedArg(std::string_view name, std::int64_t value);
        bool setLocalizedArg(std::string_view name, double value);
        template <std::integral T>
        bool setLocalizedArg(const std::string_view name, const T value)
        {
            return setLocalizedArg(name, static_cast<std::int64_t>(value));
        }
        [[nodiscard]] bool isLocalized() const;
        // The text is rich text markup (UiRichText.hpp,: color, size, bold and inline sprites), parsed when
        // it changes; a localized message is formatted first and its string arguments are escaped. Off by default:
        // a plain label never runs the parser.
        void setRichText(bool rich);
        [[nodiscard]] bool isRichText() const;
        // Shows the Phosphor icon `name` ("eye", "eye-slash", UiIcons.hpp) in place of the text, in the icons font,
        // sized and coloured by the element's style (its weight: the style's fontWeight, 900 is Fill). An empty name
        // returns to the text. A name the table lacks draws nothing and logs once; false then (the name is kept, so
        // the tree shows what was asked for). The icon is not accessible text: a button that shows only one is named
        // by its text, which the icon hides, or by setAccessibleName.
        bool setIcon(std::string_view name);
        [[nodiscard]] std::string_view icon() const;

    private:
        bool storeLocalizedArg(std::string_view name, std::string_view encoded);
    };

    struct SPITE_UI_API Button : TextWidget
    {
        using TextWidget::TextWidget;
        Button() = default;

        void setText(std::string_view text);
        [[nodiscard]] std::string_view text() const;
    };

    struct SPITE_UI_API Label : TextWidget
    {
        using TextWidget::TextWidget;
        Label() = default;

        void setText(std::string_view text);
        [[nodiscard]] std::string_view text() const;
    };

    // A text input's editing vocabulary: keys map onto it, and presenters or
    // control commands apply it directly. Moves collapse the selection
    // unless they extend it. Commit ends an edit (onCommit when the text
    // changed since the last commit, then onSubmit); Cancel restores the text the edit
    // started from and drops focus.
    enum class UiTextCommand : u8
    {
        Left,
        Right,
        WordLeft,
        WordRight,
        Home,
        End,
        DeleteBackward,
        DeleteForward,
        DeleteWordBackward,
        DeleteWordForward,
        SelectAll,
        Copy,
        Cut,
        Paste,
        Undo,
        Redo,
        Commit,
        Cancel,
    };

    // Single-line editable text: caret and selection (UiTextEditComponent),
    // clipboard, undo within the edit, commit and cancel. Focus starts an
    // edit; Enter commits, Escape cancels, focus loss commits; a press places
    // the caret and a drag selects. Keys: arrows, Home/End (Shift extends,
    // Ctrl by word), Backspace/Delete (Ctrl by word), Ctrl+A/C/X/V/Z/Y,
    // Ctrl+Shift+Z. bind() pushes UI -> variable on every change;
    // variable -> UI is explicit setText. The input holds its own key, text,
    // focus, press and drag reactions: Widget's on* for those replace editing.
    struct SPITE_UI_API TextInput : Widget
    {
        using Widget::Widget;
        TextInput() = default;

        // Puts the caret at the end and starts a fresh edit from the text
        // (commit baseline, undo history); fires no change.
        void setText(std::string_view text);
        [[nodiscard]] std::string_view value() const;
        void bind(std::string* variable);
        // A hint drawn in the style's placeholderColor while the value is empty and no composition is active, also
        // while the input has focus (the caret shows at its start). Literal text; an empty text clears it, and it
        // replaces a localized one.
        void setPlaceholder(std::string_view text);
        // The placeholder is the message under `key` (a missing key shows the key) and follows the locale; an invalid
        // key clears it.
        void setLocalizedPlaceholder(InternedString key);
        // The placeholder as shown (the resolved message when localized).
        [[nodiscard]] std::string_view placeholder() const;
        void onChange(std::function<void(std::string_view text)> fn);
        // When a commit (Enter, focus loss) changed the text.
        void onCommit(std::function<void(std::string_view text)> fn);
        // Every Enter / Commit command, changed or not (a console's line).
        void onSubmit(std::function<void(std::string_view text)> fn);
        // Every Cancel command (Escape), before the edit reverts and the focus clears: a dialog's field that closes
        // the dialog instead of only leaving the edit.
        void onCancel(std::function<void()> fn);
        // Escape asks the owner first: true means it took the key (a completion list it closed), so the edit does
        // not revert, the focus stays and onCancel does not fire; false is a Cancel as before.
        void onCancelRequest(std::function<bool()> fn);
        // Key presses (and repeats) editing does not map, while the input has
        // focus: Up, Down, Tab, Page Up/Down, unmapped Ctrl chords; printable
        // keys too, which also type as text. A console's history and
        // completion. The input claims every key either way, except the
        // unmapped Ctrl and Alt chords: they stay editor hotkeys and gameplay,
        // and Tab without an onKey: it moves focus (UiRouter::moveFocus).
        void onKey(std::function<void(u32 scancode, u8 modifiers)> fn);

        // Replaces the selection, as typing does.
        void insert(std::string_view text);
        void apply(UiTextCommand command, bool extendSelection = false);
        // Byte offsets, snapped to UTF-8 boundaries within the text.
        void select(u32 anchor, u32 caret);
        [[nodiscard]] u32 caret() const;
        [[nodiscard]] u32 anchor() const;
    };

    struct UiNumberDesc
    {
        UiRectDesc rect;
        double value = 0.0;
        double min = -1.0e300;
        double max = 1.0e300;
        double step = 0.1; // per UI pixel of horizontal drag
        u8 decimals = 3;
    };

    // A text input over a number (UiNumericComponent): typing validates as
    // it goes, a commit parses, clamps and rounds or reverts invalid text,
    // and a horizontal drag changes the value without entering the text. A
    // press without a drag focuses with the whole text selected.
    struct SPITE_UI_API NumberField : TextInput
    {
        using TextInput::TextInput;
        NumberField() = default;

        // Clamps, rounds and formats; starts a fresh edit as setText; fires nothing.
        void setNumber(double value);
        [[nodiscard]] double number() const;
        // Each committed change and each drag step.
        void onNumber(std::function<void(double value)> fn);
        // A commit (Enter, focus loss) of text that is not a number, with that text, before the field goes back to
        // its last value: the owner shows why.
        void onInvalid(std::function<void(std::string_view text)> fn);
    };

    struct SPITE_UI_API Image : Widget
    {
        using Widget::Widget;
        Image() = default;

        void setImageSlot(u64 slot);
        [[nodiscard]] u64 imageSlot() const;
    };

    // A clipping viewport over a content panel that scrolls by the viewport's
    // UiScrollComponent: the wheel scrolls, dragging the content background
    // pans, dragging the vertical thumb scrolls proportionally. Destroying the
    // content, a thumb or a pooled row destroys the whole view. The viewport
    // holds the wheel reaction; onWheel on this handle replaces it.
    struct SPITE_UI_API ScrollView : Widget
    {
        using Widget::Widget;
        ScrollView() = default;

        // Parent scrolled children here.
        [[nodiscard]] Panel content() const;
        // Ignored on a VirtualList: its content size is derived from its rows.
        void setContentSize(Vec2 size);
        [[nodiscard]] Vec2 contentSize() const;
        // Clamped to [0, contentSize - viewport size].
        void setScrollOffset(Vec2 offset);
        [[nodiscard]] Vec2 scrollOffset() const;
        void scrollBy(Vec2 delta);
        // Off: the view rests at offset 0, the wheel, drags and thumbs do nothing (UiScrollComponent::enabled).
        void setScrollEnabled(bool enabled);
        [[nodiscard]] bool scrollEnabled() const;
    };

    struct UiVirtualListDesc
    {
        UiRectDesc rect;
        float rowHeight = 20.0f;
        u32 rowCount = 0;
        // Builds the row root of a pool slot, once per slot, in the list's
        // scope; the list parents, sizes and positions it.
        std::function<Widget(u32 slot)> createRow;
        // Shows `row` in the slot's row; called only when the slot's row
        // changes, or for every visible slot on refreshRows/resize.
        std::function<void(u32 slot, u32 row)> bindRow;
    };

    // A scroll view over fixed-height rows that creates elements only for
    // the rows on screen: a pool of ceil(viewport / rowHeight) + 1 slots, row
    // i shown by slot i % pool, so a scroll rebinds only the slots whose row
    // changed. The content size follows the row count.
    struct SPITE_UI_API VirtualList : ScrollView
    {
        using ScrollView::ScrollView;
        VirtualList() = default;

        void setRowCount(u32 count);
        [[nodiscard]] u32 rowCount() const;
        // Rows wider than the viewport scroll horizontally (a bar shows);
        // narrower ones take the viewport's width.
        void setContentWidth(float width);
        // The rows' height changed (a density): re-pools and rebinds the visible rows. Positive.
        void setRowHeight(float height);
        [[nodiscard]] float rowHeight() const;
        // Rebinds every visible slot: the rows' data changed.
        void refreshRows();
    };

    // How a selection gesture combines with the selection: a click replaces,
    // Ctrl toggles, Shift selects the visible rows from the anchor to the
    // node, Ctrl+Shift adds that range. Without a visible anchor Range
    // replaces and AddRange adds the node. All but a range move the anchor.
    enum class UiTreeSelect : u8
    {
        Replace,
        Toggle,
        Range,
        AddRange,
    };

    // A tree's keyboard vocabulary: moves the cursor over the visible rows
    // and selects it (extend: the range from the anchor). Left collapses an
    // expanded node or goes to its parent; Right expands a collapsed node or
    // goes to its first child.
    enum class UiTreeCommand : u8
    {
        Up,
        Down,
        Left,
        Right,
        Home,
        End,
    };

    // A row's tag (UiTreeDesc::mark): `text` shows at the row's end (empty: no tag), `wash` also fills the whole row
    // behind its selection highlight. `shortText` is what the tag shrinks to, in markShortSpace, when the label
    // would not fit whole beside the full tag (empty: the tag never shrinks).
    struct UiTreeMark
    {
        std::string_view text;
        bool wash = false;
        std::string_view shortText{};
    };

    // A row's readout (UiTreeDesc::detail): `text` shows left of the tag (empty: none), `warn` swaps the label's class
    // for <rowClass>.detail.warn. `bar` (0..1, negative: none) is the share a mini bar left of the readout fills; it
    // shows with the readout and only while UiTreeDesc::barSize is set and the row has the room.
    struct UiTreeDetail
    {
        std::string_view text;
        bool warn = false;
        float bar = -1.0f;
    };

    // A row's state look (UiTreeDesc::look): `dim` swaps the label's class for <rowClass>.text.inactive (.selected
    // too while the row is selected, over the selection fill); `tooltip` is the message key shown over the row
    // (invalid: none).
    struct UiTreeLook
    {
        bool dim = false;
        InternedString tooltip{};
    };

    struct UiTreeDesc
    {
        UiRectDesc rect;
        float rowHeight = 20.0f;
        float indent = 16.0f; // per depth; also the expander's width
        InternedString rowClass{}; // style class of every generated row's root (TreeView::setRowClass); invalid = none
        // The model, queried only for visible nodes. children(undefined) are
        // the roots; spans stay valid until the model changes (refresh).
        std::function<std::span<const Entity>(Entity node)> children;
        std::function<Entity(Entity node)> parent; // undefined for roots
        std::function<std::string_view(Entity node)>
            label; // read when a row binds; the view need only outlive the bind
        // Optional colour chip between the expander and the label: the node's rgba (alpha 0: no chip), read when a
        // row binds. The chip is a panel styled <rowClass>.chip whose UiVisualComponent tint carries the colour (the
        // class paints it white). chipSpace is the width reserved for it before the label; null chip: none.
        std::function<Vec4(Entity node)> chip;
        float chipSpace = 14.0f;
        // Optional icon rows (the Forge look): the expander is a caret icon ("caret-right", "caret-down") instead of
        // "+" and "-", and the node's Phosphor icon name (empty: none) shows between the expander and the label in a
        // label styled <rowClass>.icon, <rowClass>.icon.inactive while UiTreeDesc::look dims the row or
        // <rowClass>.icon.selected while it is selected; a selected row's text takes <rowClass>.text.selected. The
        // icon takes `indent` before the label. Null: the plain expander and no icon.
        std::function<std::string_view(Entity node)> icon;
        // Optional tag at the row's end (the agent layer's AGENT and EDITED), read when a row binds. A tag is a label
        // styled <rowClass>.mark in the last markSpace of the row, which the label then leaves free; a wash is a panel
        // styled <rowClass>.wash under the row's selection highlight. The name keeps priority: a tag with a shortText
        // shows it in markShortSpace instead whenever the label would keep less than its own text width (at least
        // markMinLabel) beside the full tag. Null: rows have neither part.
        std::function<UiTreeMark(Entity node)> mark;
        float markSpace = 64.0f;
        float markShortSpace = 24.0f;
        float markMinLabel = 40.0f; // the label's least width beside the full tag
        float markInset = 0.0f; // kept clear after the tag (a scroll thumb)
        // Optional readout left of the tag (the hierarchy's subtree cost), read when a row binds: a label styled
        // <rowClass>.detail, or <rowClass>.detail.warn, in the last detailSpace before the tag, which the label then
        // leaves free. A row too narrow to hold detailSpace beside its tag and the label's whole text (at least
        // detailMinLabel) shows no readout: the name and the tag win. Null: rows have none.
        std::function<UiTreeDetail(Entity node)> detail;
        float detailSpace = 88.0f;
        float detailMinLabel = 40.0f; // the label's least width beside the readout
        // The readout's mini bar (UiTreeDetail::bar): a track panel <rowClass>.bar of barSize with a fill panel
        // <rowClass>.bar.fill, or <rowClass>.bar.fill.warn on a warning readout, of its share of the width, a gap of
        // barGap left of the readout. A row too narrow for the bar beside detailSpace and detailMinLabel shows the
        // readout alone. Zero barSize: no bars.
        Vec2 barSize{0.0f, 0.0f};
        float barGap = 4.0f;
        // Optional state look (the hierarchy's inactive rows), read when a row binds; a change of it is a refreshRows
        // away. Null: rows have the plain look.
        std::function<UiTreeLook(Entity node)> look;
        // false: the model keeps no sibling order, so a drop that leaves every dragged node under its parent changes
        // nothing: it shows no indicator and reports no drop (the invalid state, not a refusal)
        bool ordered = true;
        // a drag resting this long (TreeView::advance's seconds) on a collapsed node that has children opens it
        float dragOpenSeconds = 0.6f;
    };

    // A virtualized tree over a presenter-owned model, keyed by Entity
    // (scene entities, or any ids wrapped in one). It shows the expanded
    // nodes as rows of a VirtualList, so elements and per-change work scale
    // with the visible rows. State is UiTreeComponent on the viewport. A click
    // on the expander toggles; elsewhere it selects (Ctrl, Shift as
    // UiTreeSelect); arrows, Home and End navigate (Shift extends). Dragging
    // rows reports a move through onDrop and UiTreeDropEvent: the upper or
    // lower quarter of a row drops before or after it, the middle into it as
    // the last child. Dragging a selected node moves the selection (without
    // nodes whose ancestor is selected), otherwise just the node. Changing
    // the model is the presenter's: the tree only reports.
    struct SPITE_UI_API TreeView : ScrollView
    {
        using ScrollView::ScrollView;
        TreeView() = default;

        // The model changed: re-reads the visible nodes. The tree cannot tell
        // a deleted node from a hidden one, so after deleting nodes the
        // presenter re-syncs with setSelection (expanded entries of deleted
        // nodes are inert). Refresh forgets a cursor, anchor or pressed node
        // that no longer shows, and cancels an in-flight drag unless all its
        // nodes still show.
        void refresh();
        // What a row shows changed but the model did not (a tag, a readout): rebinds the visible rows in place.
        void refreshRows();
        // The style class of every row root (UiTreeDesc::rowClass); the row parts take <rowClass>.selected,
        // <rowClass>.toggle, <rowClass>.chip (with UiTreeDesc::chip), <rowClass>.wash and <rowClass>.mark (with
        // UiTreeDesc::mark), <rowClass>.detail (with UiTreeDesc::detail), <rowClass>.icon, <rowClass>.bar and
        // <rowClass>.bar.fill (with UiTreeDesc::icon and barSize; the variants are on those descriptions) and
        // <rowClass>.text, or <rowClass>.text.inactive (.selected) while UiTreeDesc::look dims the row (none while
        // rowClass is invalid).
        void setRowClass(InternedString rowClass);
        [[nodiscard]] InternedString rowClass() const;
        // The rows' height and the indent per depth changed (a density): re-pools and rebinds the visible rows in
        // place, keeping the scroll position (as VirtualList::setRowHeight), selection, cursor and expansion. Both
        // positive; an unchanged pair does nothing.
        void setMetrics(float rowHeight, float indent);
        // The readout bars' size and gap changed (a density): the parts resize and the rows rebind; needs bars at
        // creation (a positive UiTreeDesc::barSize) and a positive width, else does nothing.
        void setBar(Vec2 size, float gap);
        [[nodiscard]] float rowHeight() const;
        [[nodiscard]] float indent() const;
        void setExpanded(Entity node, bool expanded);
        [[nodiscard]] bool isExpanded(Entity node) const;
        // Collapses every node, shown or not (a node reopened later shows its children collapsed), and re-reads the
        // visible nodes; the selection stays, the cursor moves to a visible row.
        void collapseAll();
        // Input vocabulary (pointer and keys map onto it): fires onSelect.
        void select(Entity node, UiTreeSelect mode = UiTreeSelect::Replace);
        void apply(UiTreeCommand command, bool extend = false);
        // Syncs from outside (editor selection); fires nothing. The last node
        // becomes cursor and anchor (none for an empty selection).
        void setSelection(std::span<const Entity> nodes);
        [[nodiscard]] std::span<const Entity> selection() const;
        [[nodiscard]] Entity cursor() const;
        // Reports a move as a pointer drop does; false (and nothing fires)
        // when parent is one of the nodes or inside one.
        bool drop(std::span<const Entity> nodes, Entity parent, u32 index);
        // Scrolls a visible node's row into view.
        void scrollTo(Entity node);
        // A drag that is not the tree's own (an asset from another panel) shows where it lands with the drop
        // indicator: the whole row of `node` (it becomes the parent), the end of the rows for none (the roots).
        // A node with no row hides it. Writes nothing while the target and the row's place are the same; the tree's
        // own drag takes the indicator over and ends it.
        void showDropTarget(Entity node);
        void hideDropTarget();
        // The visible nodes: roots and the descendants of expanded nodes.
        [[nodiscard]] u32 rowCount() const;
        // The presenter's clock, once per frame: it times the slow second click and the drag's rest on a collapsed
        // node, and writes nothing else. A drag that ends (drop or cancel) closes the nodes it opened except those
        // the drop lands in; Escape or a modal that takes the pointer cancels it.
        void advance(float seconds);
        [[nodiscard]] bool dragging() const;

        void onSelect(std::function<void(std::span<const Entity> selection)> fn);
        void onDrop(std::function<void(std::span<const Entity> nodes, Entity parent, u32 index)> fn);
        // A drag released on a dragged node or inside one: the tree reports no drop.
        void onDropRefused(std::function<void()> fn);
        // F2 on the cursor row, or a plain click on the only selected row between 0.4 and 2 s after the click that
        // selected it (the slow second click).
        void onRename(std::function<void(Entity node)> fn);
        // A secondary press on a row (where a context menu opens): an
        // unselected node is selected first, as a click would.
        void onContextMenu(std::function<void(Entity node, Vec2 position)> fn);

    private:
        [[nodiscard]] UiTreeState* runtime() const;
    };

    // A table column: a field of the table's schema, by name.
    struct UiTableColumn
    {
        InternedString field{};
        std::string title; // empty: the field name
        float width = 100.0f;
        InternedString titleKey{}; // set: a localization key that replaces `title` and follows the locale
        bool activates =
            false; // a click on this column's cell of the selected row activates it (TableView::onActivate)
        bool sortable = true; // false: a header click does nothing and the column shows no sort indicator
        // A text field of the row schema whose value tells the cell's class: a non-empty value v gives the cell text
        // the class <rowClass>.text.<v> (a status that takes a colour), an empty one <rowClass>.text. Invalid: none.
        InternedString classField{};
    };

    // Image slots are owned by the caller. aspect is width / height; invalid values use a square.
    struct UiTableDecoration
    {
        u64 imageSlot = 0;
        std::string_view icon; // Phosphor fallback when imageSlot is zero
        float aspect = 1.0f;
    };

    struct UiTableDesc
    {
        UiRectDesc rect;
        float rowHeight = 20.0f;
        float headerHeight = 22.0f;
        float minColumnWidth = 24.0f; // resizes clamp to it
        float padding = 4.0f; // cell text inset on both sides
        InternedString rowClass{}; // style class of every generated row's root and (suffixed) parts; invalid = none
        // The rows' type: cells format its fields generically.
        const StructSchema* schema = nullptr;
        std::vector<UiTableColumn> columns; // empty: every field of the schema
        u32 rowCount = 0;
        // The model, read only for visible rows and once per row by a sort:
        // `row` points at a value of `schema` (valid until the model changes,
        // refresh), `key` identifies it for selection (an entity, or any id
        // wrapped in one).
        std::function<const void*(u32 row)> row;
        std::function<Entity(u32 row)> key;
        // A row that opens and closes content (a section header) reports its state in uiTree; unset: no row does.
        std::function<UiExpandState(u32 row)> expand;
        // Optional first-column decoration, read for bound model rows. A square of this size is reserved;
        // zero uses the row height less the vertical padding. Images are aspect fitted inside it.
        std::function<UiTableDecoration(u32 row)> decoration;
        float decorationSize = 0.0f;
    };

    enum class UiTableOpKind : u8
    {
        Sort, // by `column` (descending when set); no column: model order
        Resize, // `column` to `width`, clamped to the minimum column width
    };

    // One table state change: what header gestures emit and TableView::apply
    // takes, so the GUI, control commands and a replayed log share one
    // vocabulary. Columns are named by field.
    struct UiTableOp
    {
        UiTableOpKind kind = UiTableOpKind::Sort;
        InternedString column{};
        bool descending = false;
        float width = 0.0f;
        SPITE_SCHEMA(UiTableOp, kind, column, descending, width)
    };

    // A virtualized table over presenter-owned rows of one schema: a header
    // row over a VirtualList whose pooled rows hold a clipped cell per
    // column, so elements and per-change work scale with the visible rows.
    // Cells show their field's value as text. Clicking a header sorts by its
    // column (again: reversed); dragging the grip at a header's right edge
    // resizes the column; both emit UiTableOp through onRequest when set,
    // else apply it. Sorting orders view rows by the column's values (scalar,
    // string and entity fields; ties keep model order). Rows select as a
    // tree's do (click, Ctrl, Shift; Up, Down, Home, End, Page Up, Page Down);
    // Enter, and a click on an `activates` column's cell of the selected row, call onActivate. The
    // rows list is the table's one Tab stop (role grid); header cells are
    // columnheaders carrying their sort state, rows rows, cells gridcells.
    // State is UiTableComponent on the root, UiTableColumnComponent per header
    // cell.
    struct SPITE_UI_API TableView : Widget
    {
        using Widget::Widget;
        TableView() = default;

        // What a table has bound so far (inspection): the first shown view row, the pooled rows, the row binds and
        // the cell texts written, and the shown rows' cell texts in view order.
        struct Shown
        {
            u32 firstRow = 0;
            u32 slots = 0;
            u64 binds = 0;
            u64 writes = 0;
            std::vector<std::vector<std::string>> cells;
        };

        // The model changed: re-sorts once when sorted and rebinds the
        // visible rows. Selection keys stand; after deleting rows the
        // presenter re-syncs with setSelection.
        void refresh(u32 rowCount);
        // Re-read decorations for pooled visible rows without sorting or rebinding text.
        void refreshRowDecorations();
        [[nodiscard]] u32 rowCount() const;
        [[nodiscard]] Shown shown() const;
        struct VisibleRows
        {
            u32 firstRow = 0, slots = 0;
            u64 binds = 0, writes = 0;
        };
        // Range and counters without copying cell text or allocating container bookkeeping.
        [[nodiscard]] VisibleRows visibleRows() const;
        // The style class of every row root (UiTableDesc::rowClass); the row parts take <rowClass>.selected,
        // <rowClass>.cell and <rowClass>.text, the header <rowClass>.header with the parts .cell, .text (the title)
        // and .mark (the sort indicator); none while rowClass is invalid: their ids style them.
        void setRowClass(InternedString rowClass);
        [[nodiscard]] InternedString rowClass() const;
        // Whether the rows scroll (their list's UiScrollComponent); off, they are clipped to the table.
        void setScrollEnabled(bool enabled);
        [[nodiscard]] bool scrollEnabled() const;
        // False (and nothing changes) for an unknown column, a sort by a
        // column whose kind does not sort, or a non-finite width.
        bool apply(const UiTableOp& op);
        void onRequest(std::function<void(const UiTableOp&)> fn);
        [[nodiscard]] float columnWidth(InternedString column) const;
        void setColumnVisible(InternedString column, bool visible);
        // The row height, header height and cell text inset the table lays out with (UiTableDesc); a change re-places
        // the header and rebinds the visible rows. Heights are positive.
        void setMetrics(float rowHeight, float headerHeight, float padding);
        [[nodiscard]] float rowHeight() const;
        [[nodiscard]] float headerHeight() const;
        // A column's header cell (for a tooltip or a probe); invalid for an unknown column.
        [[nodiscard]] Widget headerCell(InternedString column) const;

        // The model row a view row shows (the sorted order).
        [[nodiscard]] u32 modelRow(u32 viewRow) const;
        // Input vocabulary (pointer and keys map onto it): fires onSelect.
        void select(Entity key, UiTreeSelect mode = UiTreeSelect::Replace);
        void navigate(UiTreeCommand command, bool extend = false); // Left and Right do nothing
        // The cursor a window of rows up or down (the shown rows less one).
        void page(bool down, bool extend = false);
        // A secondary press on a row, in layout pixels.
        void onContextMenu(std::function<void(Entity key, Vec2 position)> fn);
        // Enter on the cursor row (column undefined; a held key's repeats do nothing), or a click on an `activates`
        // cell of a row that was already selected (that column; the first click only selects, so a stray click
        // never fires); nothing activates without a cursor.
        void onActivate(std::function<void(Entity key, InternedString column)> fn);
        void onDoubleClick(std::function<void(Entity key)> fn);
        void activate();
        // Syncs from outside; fires nothing. The last key becomes cursor and anchor.
        void setSelection(std::span<const Entity> keys);
        [[nodiscard]] std::span<const Entity> selection() const;
        [[nodiscard]] Entity cursor() const;
        void scrollTo(Entity key);
        // The rows' vertical scroll offset, whether the last row is in view (also while the rows fit), and scrolling
        // to it.
        [[nodiscard]] float scrollY() const;
        [[nodiscard]] bool atEnd() const;
        void scrollToEnd();
        void setScrollY(float y); // clamped to the rows
        void onSelect(std::function<void(std::span<const Entity> selection)> fn);
        // Dragging a row out: each drag step and the release (released set)
        // report the dragged keys - the selection when the row is selected,
        // else the row - and the pointer position (layout pixels). The table
        // moves nothing; the presenter decides what a drop means.
        void onDragRows(std::function<void(std::span<const Entity> keys, Vec2 position, bool released)> fn);

    private:
        [[nodiscard]] UiTableState* runtime() const;
    };

    // Where a Dock op puts a panel against its target: into the target's tab
    // group, or beside it in a new split.
    enum class UiDockZone : u8
    {
        Center,
        Left,
        Right,
        Top,
        Bottom,
    };

    enum class UiDockOpKind : u8
    {
        Dock, // panel at `zone` of target's tab group; no target: the space's edge (Center: the empty space)
        Float, // panel into its own floating window at position/size (moves a window it is alone in)
        Move, // the floating window holding panel to position/size
        Close, // panel out of the layout; Dock or Float reopens it
        Activate, // panel's tab shows in its group
        Resize, // the split with panel on its first side, target on its second: the first side takes ratio
        Maximize, // panel's tab group (of the main tree) fills the space, the rest of the tree hidden
        Restore, // the space shows its whole tree again (panel: any panel of the maximized group)
    };

    // One dock layout change: what gestures emit and DockSpace::apply takes,
    // so the GUI, control commands and a replayed log share one vocabulary.
    // Panels are named by UiDockPanelComponent::name; positions are UI pixels
    // relative to the dock space. Ops name only panels, so a recorded op
    // replays after a save/load or a restart rebuilds the layout's nodes.
    struct UiDockOp
    {
        UiDockOpKind kind = UiDockOpKind::Dock;
        InternedString panel{};
        InternedString target{};
        UiDockZone zone = UiDockZone::Center;
        float ratio = 0.5f; // Resize: first side's share; Dock beside: the panel's share
        Vec2 position{0.0f, 0.0f};
        Vec2 size{0.0f, 0.0f};
        static constexpr u32 LAST = ~0u;
        u32 index =
            LAST; // Dock into target's group (Center): the tab's place among the others; LAST or past the end: last
        // Float: the window is detached into an OS window of its own (true) or kept in the space (false).
        // A detached window's position is in the UI's surface units, not the space's, and is never clamped to the
        // space; Move keeps a window as detached or not as it is.
        bool detached = false;
        SPITE_SCHEMA(UiDockOp, kind, panel, target, zone, ratio, position, size, index, detached)
    };

    // A floating window detached into an OS window: its root surface, a panel it shows (the name its ops
    // take) and its rect in the UI's surface units.
    struct UiDetachedWindow
    {
        Entity root;
        InternedString panel;
        Vec2 position;
        Vec2 size;
    };

    struct UiDockDesc
    {
        UiRectDesc rect;
        float tabHeight = 22.0f;
        float tabWidth = 96.0f; // without a text measurer; with one, the title plus padding
        float splitterSize = 4.0f;
        float minPaneSize = 32.0f; // a region's content is at least this big (a panel's minSize raises it)
        // Two presses of one tab within doubleClickSeconds toggle its group's maximize; seconds from `clock`
        // (null: the steady clock).
        float doubleClickSeconds = 0.4f;
        std::function<double()> clock;
        // A close button (tabHeight square) after each tab's title, requesting Close; it shows on the active tab and on
        // the tab under the pointer. Off: the tabs are their titles alone.
        bool closeButtons = false;
        // A tab let go outside the main window floats into an OS window of its own (UiDockOp::detached), for
        // a host that opens them; off: it floats inside the space like a release outside the space.
        bool detachOutside = false;
        // Hide tab and window chrome; panel content fills each group while splitters remain usable.
        bool showChrome = true;
    };

    // Dockable panels: tab groups in a tree of splits filling the space, plus
    // in-window floating windows. The layout is entity state (UiDockComponent
    // on the root, UiDockNodeComponent per node, UiDockPanelComponent per
    // panel host) and round-trips through JSON. Pressing a tab activates it,
    // dragging it re-docks (onto a group's centre or edge, the space's edge)
    // or floats it (onto the float target shown for the drag, or outside the
    // space); Escape cancels the drag; Shift+Enter on a focused tab floats it
    // or docks it back where it stood; dragging a splitter resizes; dragging a
    // floating window's tab bar or grip moves or resizes it. Gestures emit
    // UiDockOp through onRequest when set (the editor turns them into control
    // commands that call apply), else apply them. Splitter drags and tab
    // switches re-lay out only the affected subtrees. The space, its splits,
    // tab groups and panel hosts take no input, so the pointer passes through
    // bare space to what is under the dock (an editor's scene view). A theme styles the parts by id and class: the
    // tab classes "<id>.tab" and, on the shown panel's tab, "<id>.tab.active"; the elements "<id>.tabs" (a group),
    // ".tabs.bar", ".tabs.active" (the underline), ".tabs.frame" (a border-only overlay over the whole group) and
    // ".splitter".
    struct SPITE_UI_API DockSpace : Widget
    {
        using Widget::Widget;
        DockSpace() = default;

        // `content` (any scope's widget) goes into the panel's host, placed at
        // its top-left and sized to it by every layout. The panel stays closed
        // until a Dock or Float op or a layout places it.
        void addPanel(InternedString name, std::string_view title, const Widget& content);
        // The smallest content a region showing the panel may have, in UI pixels (default: UiDockDesc::minPaneSize).
        // A tab group is at least the largest minimum of its panels and wide enough for its widest tab beside the
        // overflow button; a split at least both sides and the splitter. Splitter drags, Resize ops and layouts stop
        // there; a space too small for both sides of a split keeps only a minPaneSize floor on each.
        void setPanelMinSize(InternedString name, Vec2 size);
        // Replaces the tab text of a panel and re-lays out the tab bars (tab widths follow the text); a no-op
        // for an unknown panel or the same title.
        void setPanelTitle(InternedString name, std::string_view title);
        // A tag on the panel's tab bar (a count, a unit, a row of chips): `tag` (any scope's widget) sits in a row at
        // the bar's end, between the last tab and the overflow button, right-aligned and clipped where the room is
        // short, while the panel is the shown tab of its group; it follows the panel from group to group. The panel
        // owns the tag (its text, state and visibility); the dock owns its place. Once per panel.
        void setPanelTag(InternedString name, const Widget& tag);
        // The tab bar height (UiDockDesc::tabHeight) of every group and floating window, laid out again; a no-op for
        // the same height. A host follows its density token with it.
        void setTabHeight(float height);
        // The splitter size (UiDockDesc::splitterSize: the gap between neighbouring groups) of every split, laid
        // out again; a no-op for the same size. A host follows its space token with it.
        void setSplitterSize(float size);
        // False (and nothing changes) for an unknown panel or target, a closed
        // target, malformed values, or a no-op such as docking a panel onto
        // its own lone group; re-applying an Activate or Resize that already
        // holds succeeds. Floating window sizes clamp to a tab bar plus
        // minPaneSize and to the laid-out space; Float and Move keep at
        // least minPaneSize of the window's width and one tab bar of its
        // height inside the space; Resize ratios clamp to minPaneSize per side.
        bool apply(const UiDockOp& op);
        void onRequest(std::function<void(const UiDockOp&)> fn);
        [[nodiscard]] bool isOpen(InternedString panel) const;
        // Whether the panel's tab group is the maximized one (false for a floating, closed or unknown panel).
        [[nodiscard]] bool isMaximized(InternedString panel) const;
        // The rect of the floating window holding the panel, in space pixels (zero for a docked or closed panel).
        [[nodiscard]] UiRect floatingRect(InternedString panel) const;
        // The panel's host element (undefined for an unknown panel).
        [[nodiscard]] Entity panelHost(InternedString panel) const;
        // The op that puts a closed panel back where it was closed: a Dock beside the tab group or split it left, or a
        // Float at its window's rect; none for a panel that is open, unknown or never closed, or whose place is gone
        // (its group closed too). The place is runtime state: a loaded layout forgets it.
        [[nodiscard]] std::optional<UiDockOp> reopenOp(InternedString panel) const;
        // The floating windows detached into OS windows, in stacking order; whether the panel is in one.
        [[nodiscard]] std::vector<UiDetachedWindow> detachedWindows() const;
        [[nodiscard]] bool isDetached(InternedString panel) const;
        // While a tab drag would detach its panel (the pointer is outside the main window): the rect the detached
        // window would get, in surface units. The host shows it in an OS window of its own; the space cannot.
        [[nodiscard]] std::optional<UiRect> detachPreview() const;

        // The layout as JSON: nodes by panel name, ratios, active tabs and
        // floating rects. Loading replaces the layout; panels it does not name
        // close, names not registered are skipped. False (and the layout
        // stands) when the text is not a layout or names panels but none of them is registered.
        [[nodiscard]] std::string saveLayout() const;
        bool loadLayout(std::string_view json);

    private:
        [[nodiscard]] UiDockState* runtime() const;
    };

    // One entry of a menu model. Activation reports `id`; the presenter maps
    // it to an action (the editor: a control command), so the GUI, the
    // control protocol and a replayed log share one vocabulary.
    struct UiMenuItem
    {
        InternedString id{};
        std::string label;
        bool enabled = true;
        bool separator = false; // a divider; the other fields are unused
        // A menuitemcheckbox: shown with a [x] or [ ] mark before the label, named by the label alone, reported
        // checked by uiTree; activating it reports the id and the presenter flips its own state.
        bool checkable = false;
        bool checked = false;
        std::vector<UiMenuItem> children; // non-empty: a submenu
    };

    // A menu's keyboard vocabulary, acting on the innermost open level.
    enum class UiMenuCommand : u8
    {
        Up, // highlight the previous enabled item, wrapping
        Down, // the next one
        Open, // open the highlighted item's submenu, highlighting its first item
        Back, // close the innermost submenu
        Activate, // activate the highlighted item, or open its submenu
        Close, // close the innermost submenu, or the menu
        First, // highlight the first enabled item (Home)
        Last, // the last one (End)
    };

    struct UiMenuDesc
    {
        float itemHeight = 22.0f;
        float separatorHeight = 9.0f;
        float width = 160.0f; // minimum; with a text measurer, the widest label plus padding (and arrow)
        float padding = 8.0f; // label inset on both sides
        i32 zOffset = 1000; // the root's zOffset: popups paint above roots with lower ones
        // Popups stay inside its last resolved rect (flipping before
        // clamping); invalid: unbounded.
        Widget bounds;
    };

    // A popup menu: its own root surface (zOffset desc.zOffset, so it paints
    // above docked panels and floating windows) holding one panel per open
    // nesting level, each with pooled item rows, so reopening reuses
    // elements and open, close and highlight changes re-lay out only the
    // menu. Opening focuses it; arrows, Enter and Escape navigate
    // (UiMenuCommand) and claim only the keys that did something; hovering
    // highlights and opens submenus, clicking an enabled leaf activates it. Home and End highlight the first and last
    // enabled item; typing a letter (A to Z, digits) highlights the next enabled item of the innermost level whose
    // label starts with it, wrapping, and Enter activates it. Left with no submenu open and Right on a leaf call
    // onCycle, so a menu bar moves to the neighbouring menu.
    // Focus moving outside the menu (a press elsewhere, also on nothing)
    // closes it; that press still reaches what it hit. Closing by Escape or
    // activation returns focus to where it was. State is UiMenuComponent on
    // the root, UiMenuItemComponent per row.
    struct SPITE_UI_API Menu : Widget
    {
        using Widget::Widget;
        Menu() = default;

        // Opens with its top-left at `position` (layout pixels, as pointer
        // events report), flipped above it when it does not fit below.
        void open(std::span<const UiMenuItem> items, Vec2 position);
        // Opens below the anchor's last resolved rect (a dropdown). Opening
        // from the press that just closed it on this anchor is ignored, so a
        // dropdown opened on press toggles.
        void open(std::span<const UiMenuItem> items, const Widget& anchor);
        void close();
        [[nodiscard]] bool isOpen() const;
        // Whether the command did something (the keys claim only then).
        bool apply(UiMenuCommand command);
        // Activates an enabled leaf of the open menu's model by id, as a click
        // would; false (and nothing fires) when closed or not found.
        bool activate(InternedString item);
        void onActivate(std::function<void(InternedString item)> fn);
        // Called with -1 (Left at the outermost level) or +1 (Right on an item with no submenu); the key is claimed
        // only while one is set. It may open this menu again elsewhere.
        void onCycle(std::function<void(i32 direction)> fn);

    private:
        [[nodiscard]] UiMenuState* runtime() const;
    };

    // -- Value widgets ---------------------------------------------------------------------------------
    // Composites of plain elements around one state component on the root (UiToggle, UiSlider, UiProgress,
    // UiRadioGroup): the component is the truth, the parts (styled by class: "checkbox.box", "slider.thumb", ...)
    // show it, and a raw write (control set, a binding) is applied to them by the next layout update. Input
    // (pointer, keys, uiActivate setValue) fires onChange exactly once per change of the normalized value; setters,
    // like TextInput::setText, and raw writes fire nothing. A press on any part focuses the root, the tab stop; a
    // disabled root (setInteractable) disables its parts, takes no input and is skipped by Tab. Destroying the
    // root destroys its parts.

    // Shared by Checkbox and Toggle (UiToggleComponent on the root): Space and Enter toggle, a click on any part
    // toggles. The label is a Label element after the indicator, empty by default.
    struct SPITE_UI_API Checkable : Widget
    {
        using Widget::Widget;
        Checkable() = default;

        void setChecked(bool checked);
        [[nodiscard]] bool checked() const;
        // Each change by input or uiActivate.
        void onChange(std::function<void(bool checked)> fn);
        // setText, setLocalizedText and the localized arguments of the label.
        [[nodiscard]] Label label() const;
    };

    // A box with a mark while checked.
    struct SPITE_UI_API Checkbox : Checkable
    {
        using Checkable::Checkable;
        Checkbox() = default;
    };

    // A sliding switch: the track takes the class "toggle.track.on" while checked.
    struct SPITE_UI_API Toggle : Checkable
    {
        using Checkable::Checkable;
        Toggle() = default;
    };

    struct UiSliderDesc
    {
        UiRectDesc rect; // size 0: 160 x 20
        double value = 0.0;
        double min = 0.0;
        double max = 1.0;
        double step = 0.0; // 0: continuous
    };

    // A horizontal slider (UiSliderComponent on the root). The value normalizes as the component documents. A
    // press on the track puts the thumb's centre under the pointer and a drag follows it; a press on the thumb
    // keeps its grab offset. Left and Down step down, Right and Up step up (a positive step, else 1 percent of the
    // range), Page keys ten steps, Home and End go to the ends.
    struct SPITE_UI_API Slider : Widget
    {
        using Widget::Widget;
        Slider() = default;

        // Normalizes; fires nothing.
        void setValue(double value);
        void setRange(double min, double max, double step = 0.0);
        [[nodiscard]] double value() const;
        [[nodiscard]] double min() const;
        [[nodiscard]] double max() const;
        [[nodiscard]] double step() const;
        // Each change by input or uiActivate.
        void onChange(std::function<void(double value)> fn);
    };

    struct UiProgressDesc
    {
        UiRectDesc rect; // size 0: 160 x 8
        double value = 0.0;
        double min = 0.0;
        double max = 1.0;
    };

    // A read-only bar (UiProgressComponent on the root) filled to the value's share of the range; it takes no
    // input.
    struct SPITE_UI_API ProgressBar : Widget
    {
        using Widget::Widget;
        ProgressBar() = default;

        void setValue(double value);
        void setRange(double min, double max);
        [[nodiscard]] double value() const;
        [[nodiscard]] double min() const;
        [[nodiscard]] double max() const;
    };

    // One item of a radio group (UiRadioItemComponent): a circle with a mark while it is the selection, and a
    // label.
    struct SPITE_UI_API Radio : Widget
    {
        using Widget::Widget;
        Radio() = default;

        [[nodiscard]] u32 index() const;
        [[nodiscard]] bool isSelected() const;
        [[nodiscard]] Label label() const;
    };

    struct UiRadioGroupDesc
    {
        UiRectDesc rect;
        bool horizontal = false; // items in a row, else a column
        float gap = 6.0f;
    };

    // Exclusive choices (UiRadioGroupComponent on the root; the selected item index, -1 none). The group is one tab
    // stop, on the selected item (the first enabled one while none is); Up and Left select the previous enabled
    // item, Down and Right the next, wrapping, and focus follows; Space and Enter select the focused item.
    // Selecting the selected item changes nothing.
    struct SPITE_UI_API RadioGroup : Widget
    {
        using Widget::Widget;
        RadioGroup() = default;

        Radio addItem(InternedString id, std::string_view label);
        [[nodiscard]] u32 itemCount() const;
        [[nodiscard]] Radio item(u32 index) const;
        // -1 clears; an index with no item changes nothing. Fires nothing.
        void setSelected(i32 index);
        [[nodiscard]] i32 selected() const;
        // Each change by input or uiActivate.
        void onChange(std::function<void(u32 index)> fn);
    };

    // -- Tab bar -----------------------------------------------------------------------------------------
    // The bar is a row of tabs on the radio group's machinery (UiTabBarComponent on the root, UiTabComponent per tab):
    // one tab stop, on the active tab; Left and Right activate the previous and next enabled tab (wrapping), Home and
    // End the first and last, a click activates. The active tab takes the style class "tabbar.tab.active" (else
    // "tabbar.tab") and shows its content widget, if it has one, hiding the others'. A raw write of the component is
    // applied by the next layout update and fires nothing; input and uiActivate setValue fire onChange once per change.

    // One tab (UiTabComponent): padding around its label.
    struct SPITE_UI_API Tab : Widget
    {
        using Widget::Widget;
        Tab() = default;

        [[nodiscard]] u32 index() const;
        [[nodiscard]] bool isActive() const;
        // setText, setLocalizedText and the localized arguments of the tab's text.
        [[nodiscard]] Label label() const;
        // The widget shown while this tab is the active one and hidden while another is: the bar sets its visibility on
        // every change, so parent it wherever it belongs. An invalid widget removes the content.
        void setContent(const Widget& content);
    };

    struct UiTabBarDesc
    {
        UiRectDesc rect; // content-sized by default (a row of its tabs)
        float gap = 2.0f;
    };

    struct SPITE_UI_API TabBar : Widget
    {
        using Widget::Widget;
        TabBar() = default;

        Tab addTab(InternedString id, std::string_view label);
        [[nodiscard]] u32 tabCount() const;
        [[nodiscard]] Tab tab(u32 index) const;
        // -1 clears; an index with no tab changes nothing. Fires nothing.
        void setActive(i32 index);
        [[nodiscard]] i32 active() const;
        // Each change by input or uiActivate.
        void onChange(std::function<void(u32 index)> fn);
        // Each input (click, key, uiActivate) on the tab that is already active, never setActive. The bar does nothing
        // with it: toggle semantics (a re-click hides the content, as an accordion header does) are the caller's.
        void onReselect(std::function<void(u32 index)> fn);
    };

    // -- Dropdown and combo box --------------------------------------------------------------------------
    // One choice of a dropdown, combo box or popup list: `id` is what uiActivate and tools name it by, `label` its
    // text, `key` (when valid) a localization message that replaces the label and follows the locale. A `header` is a
    // group heading: it is never selectable, highlighted or chosen, and a search shows it only above a match.
    struct UiChoiceItem
    {
        InternedString id{};
        std::string label;
        InternedString key{};
        bool enabled = true;
        bool header = false;
        std::string icon; // optional Phosphor icon name
    };

    struct UiDropdownDesc
    {
        UiRectDesc rect; // size 0: 160 x 24 (a popup list: its minimum width, 0 for 200)
        u32 maxRows = 8; // rows the list shows before it scrolls
        float rowHeight = 22.0f; // unscaled UI pixels
        i32 zOffset = 1000; // the popup's root zOffset, like a menu's: above docked panels and modals
        // A search box above the rows (a combo's face is its own): opening moves the focus into it, typing filters the
        // rows incrementally (ASCII case-insensitive substring of a row's text or its id), Up, Down and the Page keys
        // move the highlight, Enter chooses it, Escape closes.
        bool searchable = false;
        // The rows are menu items (role menuitem, the list a menu) rather than options of a list box.
        bool menu = false;
        // A popup list that leaves the keyboard focus where it is when it opens (a console input's completions): the
        // opener keeps typing and forwards keys with PopupList::keyDown, closes the list when it loses focus, and a
        // press on a row keeps the focus on the opener. Ignored by a dropdown and a combo.
        bool keepFocus = false;
    };

    // A button showing the selected item that opens a list below it (above when it does not fit), on its own root
    // surface. State is UiDropdownComponent on the root (`selected` is the truth: a raw write is applied by the next
    // layout update and fires nothing; pointer, keys and uiActivate setValue fire onChange once per change). The root
    // is a tab stop and keeps the focus while the list is open: Enter, Space, Up and Down open it; then Up, Down,
    // Page keys, Home and End move the highlight (disabled items skipped), Enter and Space choose, Escape closes;
    // the pointer highlights rows by hover, chooses on a click, scrolls with the wheel; focus moving anywhere else
    // (a press outside included), the dropdown being hidden, disabled or destroyed closes it. The list holds a bounded
    // pool of rows whatever the item count.
    struct SPITE_UI_API Dropdown : Widget
    {
        using Widget::Widget;
        Dropdown() = default;

        // Replaces the items; the selection keeps its index when an item remains there, else clears. Fires nothing.
        void setItems(std::span<const UiChoiceItem> items);
        [[nodiscard]] u32 itemCount() const;
        // -1 clears; an index with no item changes nothing. Fires nothing.
        void setSelected(i32 index);
        [[nodiscard]] i32 selected() const;
        // Each change by input or uiActivate.
        void onChange(std::function<void(u32 index)> fn);
        void open();
        void close();
        [[nodiscard]] bool isOpen() const;
        // The message under `key` shows on the face while nothing is selected (a dropdown, not a combo, whose input
        // keeps its own placeholder); an invalid key leaves the face empty.
        void setPlaceholderKey(InternedString key);
        // The text of the selected item (the combo's is its input).
        [[nodiscard]] Label label() const;
    };

    // A dropdown whose face is a text input: typing filters the list to the items containing the text (ASCII
    // case-insensitive), opens it and highlights the first match; Up and Down move the highlight, Enter chooses it
    // (with no match, the text reverts to the selected item), and losing focus or Escape reverts the text. The filter
    // is reported in UiDropdownComponent::filter.
    struct SPITE_UI_API Combo : Dropdown
    {
        using Dropdown::Dropdown;
        Combo() = default;

        // The face's text input, for styling and reading. Its onChange/onSubmit hooks belong to the combo
        // (filtering, committing, reverting): replacing one breaks the combo.
        [[nodiscard]] TextInput input() const;
    };

    // The dropdown's list without a face: one popup, its own root surface hidden while closed, opened
    // beside any widget (an Add button, a field, a tool) with the same rows, classes, placement (below the anchor,
    // above when the roomier side is above, clamped to its surface), wheel, thumb and drag scrolling, group headers,
    // search and keys as a dropdown's list. The list is the element (UiDropdownComponent on it: `open`, `highlighted`,
    // `count`, `filter`; role listbox, or menu with UiDropdownDesc::menu); the pooled rows are "<id>.item" (roles
    // option or menuitem, `selected` on the one setSelected marks, accessible name = text), the headers "<id>.item"
    // labels classed "dropdown.header". Opening takes the keyboard focus (into the search box when searchable),
    // claiming the keys and navigation intents (Up, Down, Page keys, Home, End, Enter or Accept chooses, Escape or
    // Cancel closes; Left and Right do nothing; typing selects by prefix without a search box) and remembers the focus
    // it took it from; closing by choice or Escape returns it there. Focus moving elsewhere (a press outside, also on
    // nothing, Tab) closes it without taking the focus back. Open it from the opener's onPress (a primary press):
    // opening it from the anchor whose press just closed it keeps it closed, so a press on the opener toggles. onChoose
    // fires on every choice, selected or not.
    struct SPITE_UI_API PopupList : Widget
    {
        using Widget::Widget;
        PopupList() = default;

        // Replaces the items; the selection keeps its index when an item remains there, else clears. Fires nothing.
        void setItems(std::span<const UiChoiceItem> items);
        // Search metadata for the next setItems, indexed in the same order; omitted entries are empty.
        void setSearchMetadata(std::span<const std::string> terms, std::span<const std::string> exact);
        [[nodiscard]] u32 itemCount() const;
        // The item shown as chosen and highlighted first when the list opens; -1 clears. Fires nothing.
        void setSelected(i32 index);
        [[nodiscard]] i32 selected() const;
        // Each choice (pointer, keys, uiActivate setValue): the index into the items.
        void onChoose(std::function<void(u32 index)> fn);
        // Opens beside the anchor's last resolved rect. Opening the open list from the same anchor closes it; from
        // another anchor it closes and reopens there. Ignored when the anchor is hidden, disabled, blocked by a modal
        // or the list has no items.
        void open(const Widget& anchor);
        void close();
        [[nodiscard]] bool isOpen() const;
        // A key the opener of a UiDropdownDesc::keepFocus list received: Up, Down, Page Up and Down, Home and End move
        // the highlight, Enter chooses it, Escape closes. False, and nothing done, while the list is closed.
        bool keyDown(u32 scancode);
    };

    struct UiCanvasDesc
    {
        UiRectDesc rect;
        u32 maxPrimitives = UI_CANVAS_DEFAULT_PRIMITIVES; // clamped to UI_CANVAS_MAX_PRIMITIVES
    };

    // One element drawn by its presenter's primitives (UiCanvas.hpp): the dense-data element (timelines,
    // charts, minimaps), where one entity per primitive would cost the ECS more than the drawing. The list is the
    // presenter's: update/append emit into it and bump the content generation, which repaints this element alone
    // (no layout work); a canvas nobody updates costs nothing per frame. Its primitives clip to the element and draw
    // after the element's own style (a background, a border) and under its children. UiCanvasComponent on the element
    // reports the primitive count and generation (uiTree), the uiPaint table one row of kind Canvas.
    struct SPITE_UI_API Canvas : Widget
    {
        using Widget::Widget;
        Canvas() = default;

        // Empties the list, runs `emit` to fill it, bumps the generation. False when the bound (the canvas's own, or
        // what the context budget leaves, UiContext::canvasBudget) dropped primitives or regions (UiCanvasPainter);
        // logged once, until an emission drops nothing again.
        bool update(const std::function<void(UiCanvasPainter&)>& emit);
        // Runs `emit` over the current list (a growing series): the list is lowered again as a whole.
        bool append(const std::function<void(UiCanvasPainter&)>& emit);
        void clear();
        [[nodiscard]] u32 primitives() const;
        [[nodiscard]] u64 generation() const;
        // The id of the last region of the list under a local point, UI_CANVAS_NO_REGION for none.
        [[nodiscard]] u32 regionAt(Vec2 local) const;
        // Pointer events over the element in local space (UiCanvasPointer); replaces any earlier callback, nullptr
        // removes it. The element is the hit target over its whole rect: presenters pick from `local` and `region`.
        void onPointer(std::function<void(const UiCanvasPointer&)> fn);

        // A series buffer of this canvas (UiCanvas.hpp), released with it; capacity is clamped to
        // 1..UI_SERIES_MAX_CAPACITY samples. Invalid for a stale handle.
        UiSeriesId createSeries(u32 capacity, const UiSeriesRange& range = {});
        // Appends without lowering the canvas again: the render shim uploads the new samples alone. False, appending
        // nothing, for a series this canvas does not own or a non-finite sample.
        bool appendSamples(UiSeriesId series, std::span<const float> samples);
        // What the series primitives of the series show (no relowering either).
        bool setSeriesRange(UiSeriesId series, const UiSeriesRange& range);
        bool clearSeries(UiSeriesId series);
        // The series primitives naming it draw nothing from the next lowering on.
        bool releaseSeries(UiSeriesId series);
        // Null for a series this canvas does not own.
        [[nodiscard]] const UiSeries* series(UiSeriesId series) const;

    private:
        bool run(bool replace, const std::function<void(UiCanvasPainter&)>& emit);
        [[nodiscard]] UiSeriesTable* ownSeries(UiSeriesId series) const;
    };

    struct UiChartDesc
    {
        UiRectDesc rect;
        UiChartKind kind = UiChartKind::Series;
        UiChartAxisDesc y;
        u32 xTicks = 0; // 0: by the plot's width
        UiChartHistogramDesc histogram; // kind Histogram
        double xStart = 0.0; // x of the first sample a series ever held
        double xStep = 1.0; // x per sample (> 0; anything else reads as 1)
        u32 window = 0; // the newest samples shown, following appends; 0: all held
        float textSize = 12.0f;
        bool legend = true;
        bool grid = true;
        UiChartColors colors;
    };

    // A chart: line or bar series appended sample by sample, or a histogram of added values, drawn under
    // axes with nice ticks, gridlines, localized tick labels and a legend. It is three canvases: the element ("<id>",
    // kind chart) draws axes, gridlines, labels and legend; "<id>.plot" the series (series buffers: an append uploads
    // its samples and nothing else) or the histogram bars; "<id>.overlay" the hover crosshair, markers and readout
    // and the selection. The element takes the pointer; the other two never do. Cost scales with what changed: an
    // append lowers nothing and changes no label while the window and the auto y range stay (a following window of
    // N samples that is full; a y range moves only when the data crosses a tick), a hover repaints the overlay
    // alone, and a resize (or a locale change) re-lays out the chart.
    //
    // The x axis counts samples: sample a (absolute, from the series' creation or clear) is at xStart + a * xStep.
    // A following window (desc.window or follow) labels x as offsets from the newest sample (0 at the right, no
    // relayout per append); a fixed window (setWindow) labels absolute x. Series share one x axis: append them in
    // lockstep (UiChartReadout::aligned says whether they hold the same samples), and use one style per chart.
    // Series of more than 4096 samples per pixel column decimate as the series shader does (UiCanvas.hpp: a spike
    // narrower than the stride can be missed; bars show a column's extremes). The auto y range comes from per-series
    // block extrema, so no append scans samples.
    //
    // Hover (pointer, or hover()) reads the sample nearest the pointer of every series (a histogram: the bin);
    // dragging with the primary button selects an x range, reported when released as a UiChartSelectEvent on the
    // element, to onSelect, and in UiChartComponent (a click clears it). uiActivate setValue drives both without a
    // pointer: "hover <x>", "click <x>", "select <x0> <x1>", "clear" (x in absolute x units). State is UiChartComponent
    // on the element; uiTree's `chart` adds the ticks, the plot rect and the hover values.
    struct SPITE_UI_API Chart : Widget
    {
        using Widget::Widget;
        Chart() = default;

        // The series index, or UI_CHART_NO_SERIES for a histogram chart, more than UI_CHART_MAX_SERIES series, or a
        // refused series budget (UiContext::setSeriesBudget; logged once). Series draw in the order added.
        u32 addSeries(const UiChartSeriesDesc& desc);
        // Renames a series (the legend and the hover readout show the name); false for an unknown series. The name is
        // stored as given, so a caller that localizes it sets it again when the locale changes.
        bool setSeriesName(u32 series, std::string_view name);
        [[nodiscard]] u32 seriesCount() const;
        // Appends samples; false, appending nothing, for an unknown series or a non-finite sample.
        bool append(u32 series, std::span<const float> samples);
        // Empties every series (sample numbering restarts at 0) or the histogram counts.
        void clear();
        // Show the newest `count` samples (0: all) and follow appends.
        void follow(u32 count);
        // Show `count` samples from absolute sample `first` (0: to the newest); the window clamps to what is held.
        void setWindow(u64 first, u32 count);
        // A fixed y range; setYAuto fits the data again.
        void setYRange(double min, double max);
        void setYAuto();
        // Histogram: counts the values into the bins. With an automatic range the first call with finite values sets
        // it; values outside, and non-finite ones, count in UiChartReadout::outOfRange. Costs the values added.
        void addValues(std::span<const float> values);
        // Histogram: new bins (clamped 1..UI_CHART_MAX_BINS) and range; empties the counts.
        void setHistogram(const UiChartHistogramDesc& desc);

        // The hover as a pointer sets it, at an x (absolute x units); clearHover ends it.
        void hover(double x);
        void clearHover();
        // The selection as a drag sets it (fires onSelect and the event, as a drag's release does); false when x is
        // not finite or the chart has no plot yet.
        bool select(double xMin, double xMax);
        void clearSelection();
        // A bracket over an x range (absolute x units, clamped into the plot; false when not finite or the chart has no
        // plot yet): what another view shows of the same axis. It takes no pointer, survives hover and selection, and
        // is reported by readout() and the chart component; clearBand (and clear()) remove it.
        bool setBand(double xMin, double xMax);
        void clearBand();
        // Fires when a drag selection ends or select() is called.
        void onSelect(std::function<void(const UiChartSelection&)> fn);
        // Fires when a click lands in the plot, after the click cleared a selection (not for a drag), with the sample
        // under the pointer; uiActivate setValue "click <x>" does the same without a pointer. Not Widget::onClick,
        // which would replace the chart's own click reaction.
        void onPlotClick(std::function<void(const UiChartClick&)> fn);

        [[nodiscard]] UiChartReadout readout() const;
        // The plot element and a series' buffer, read-only: samples go through append() (writes to the canvas would
        // bypass the block extrema the auto y range reads). Undefined / null for a stale handle or unknown series.
        [[nodiscard]] Entity plotEntity() const;
        [[nodiscard]] const UiSeries* series(u32 index) const;
    };

    struct UiTooltipDesc
    {
        float delay = 0.5f; // seconds of world time the pointer rests on a target
        bool follow = false; // tracks the pointer instead of anchoring to the target's rect
        UiPlacementSide side = UiPlacementSide::Below; // flips to the opposite side when it does not fit
        float gap = 6.0f; // anchor to popup
        float margin = 4.0f; // popup to the bounds' edge
        float padding = 6.0f; // inside the content, all sides
        bool label = true; // builds the text label ("<id>.text"); false for a tooltip built from other widgets
        i32 zOffset = 2000; // above menus (1000) and modals (500)
        // The area the tooltip stays inside; invalid: the surface of the element it is anchored to.
        Widget bounds;
    };

    // A tooltip: its own root surface (zOffset desc.zOffset) holding a clipping window around a content-sized
    // `content` panel (a column layout) that presenters fill with widgets — a text label by default, or any
    // subtree. Attach it to elements; hovering one for the delay shows it at the preferred side of the element
    // (or the pointer, when it follows), flipped and clamped to stay inside the bounds. It never takes hits, focus
    // or keys. State is UiOverlayComponent on the root and UiTooltipComponent on each target.
    struct SPITE_UI_API Tooltip : Widget
    {
        using Widget::Widget;
        Tooltip() = default;

        // Parent tooltip widgets here.
        [[nodiscard]] Panel content() const;
        // The default text label (invalid when desc.label was false).
        [[nodiscard]] Label label() const;
        // One tooltip serves any number of targets; onShow names which one is about to show.
        // `delay` (negative: desc.delay) is seconds of world time the pointer rests on the target.
        void attach(const Widget& target, float delay = -1.0f);
        void detach(const Widget& target);
        // Shows now, anchored to the element or a point, replacing the shown tooltip.
        void show(const Widget& anchor);
        void showAt(Vec2 point);
        void hide();
        [[nodiscard]] bool isShown() const;
        // Runs just before the tooltip shows, with its target (undefined for showAt).
        void onShow(std::function<void(Entity target)> fn);
    };

    struct UiModalDesc
    {
        bool closeOnEscape = true;
        bool closeOnBackdrop = false; // a press on the dimmed area outside the dialog closes it
        bool alert = false; // the dialog interrupts for a decision (role alertdialog), else role dialog
        float padding = 12.0f; // inside the content, all sides
        float margin = 16.0f; // dialog to the bounds' edge
        i32 zOffset = 500; // below menus (1000), so a dropdown in the dialog paints above it
        // The area the dialog is centered in and the backdrop covers; invalid: the surface of the element passed
        // to open, else the root's own rect.
        Widget bounds;
    };

    // A modal dialog: a root surface that is the backdrop, over the bounds, with the `dialog` window centered in it
    // around a content-sized `content` panel (a column layout) presenters fill. While open, nothing painted below
    // it receives hits, wheel or drags (so hover, scroll and tooltips below stop), focus is trapped in the modal
    // (Tab and Shift+Tab cycle its tab stops) and every key is claimed from gameplay. Escape closes it when
    // closeOnEscape; closing restores the previous focus. Modals stack: the latest opened is the active one.
    // State is UiOverlayComponent on the root.
    struct SPITE_UI_API Modal : Widget
    {
        using Widget::Widget;
        Modal() = default;

        [[nodiscard]] Panel dialog() const;
        // Parent dialog widgets here.
        [[nodiscard]] Panel content() const;
        // Opens over the bounds; `surface` names the element whose surface bounds it when desc.bounds is invalid.
        // No-op while open. Focus moves to the first tab stop in the content (else the backdrop).
        void open(const Widget& surface = {});
        void close();
        [[nodiscard]] bool isOpen() const;
        // Runs after the modal closed (Escape, backdrop, close()).
        void onClose(std::function<void()> fn);
    };
} // namespace spite
