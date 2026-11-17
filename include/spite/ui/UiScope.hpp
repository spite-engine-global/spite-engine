#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ecs/CommandBuffer.hpp"
#include "base/Expected.hpp"
#include "ui/UiBinding.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiExport.hpp"
#include "ui/UiOverlay.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class UiTreeState; // the ui module's tree runtime (source/ui/UiTreeState.hpp)
    class UiDockState; // the ui module's dock runtime (source/ui/UiDockState.hpp)
    class UiMenuState; // the ui module's menu runtime (source/ui/UiMenuState.hpp)
    class UiTableState; // the ui module's table runtime (source/ui/UiTableState.hpp)
    class UiChartState; // the ui module's chart runtime (source/ui/UiChartState.hpp)
    class UiDropdownState; // the ui module's dropdown and combo box runtime (UiDropdownState.hpp)
    class UiValueState; // the ui module's checkbox, toggle, slider, progress and radio runtime (UiValueState.hpp)

    // The UI scope: the single RAII owner of a presenter's elements,
    // subscriptions, and bindings. Destruction follows ownership (the
    // scope), never parentage — a widget created here may be parented under
    // another scope's widget without dying with it. The destructor enqueues
    // destruction of every owned element through a CommandBuffer and purges
    // its reaction slots and bindings in one pass; widget handles captured
    // by the presenter merely read as invalid afterwards.
    //
    // Mutations queue through the scope's CommandBuffer; commit() applies
    // them (the destructor commits implicitly). The context tracks live
    // scopes and force-shuts them down if it dies first.
    class SPITE_UI_API UiScope
    {
    public:
        explicit UiScope(UiContext& context);
        ~UiScope();
        UiScope(const UiScope&) = delete;
        UiScope& operator=(const UiScope&) = delete;

        [[nodiscard]] u32 generation() const { return m_generation; }
        [[nodiscard]] bool owns(Entity entity) const;

        // Queues destruction of the element and purges its reactions and
        // bindings immediately, so later dispatch in the same frame never
        // reaches it. Asserts when the widget belongs to another scope.
        void destroy(const Widget& widget);

        // Applies queued structural mutations (frame-boundary helper).
        void commit();

        // Destroys every element the scope owns (committed) and invalidates its handles; the scope stays usable.
        void clear();

        Panel createPanel(InternedString id, const UiRectDesc& desc = {});
        Button createButton(InternedString id, std::string_view text, const UiRectDesc& desc = {});
        Label createLabel(InternedString id, std::string_view text, const UiRectDesc& desc = {});
        TextInput createTextInput(InternedString id, const UiRectDesc& desc = {});
        NumberField createNumberField(InternedString id, const UiNumberDesc& desc = {});
        Image createImage(InternedString id, const UiRectDesc& desc = {});
        // Also creates "<id>.content" and "<id>.thumb"; destroying the view
        // destroys them and this scope's elements under it (pooled rows).
        ScrollView createScrollView(InternedString id, const UiRectDesc& desc, Vec2 contentSize = {});
        VirtualList createVirtualList(InternedString id, UiVirtualListDesc desc);
        // A virtual list ("<id>") whose pooled rows are "<id>.row" with
        // "<id>.row.selected", "<id>.row.toggle" and "<id>.row.text" parts,
        // plus the "<id>.drop" indicator in its content.
        TreeView createTree(InternedString id, UiTreeDesc desc);
        // A dock space ("<id>") whose nodes are "<id>.tabs" (with "<id>.tabs.bar",
        // "<id>.tabs.active" and "<id>.tabs.grip") and "<id>.split" (with
        // "<id>.splitter"); panels add "<id>.tab.<name>" and "<id>.panel.<name>",
        // and a tab drag shows "<id>.preview".
        DockSpace createDock(InternedString id, const UiDockDesc& desc);
        // A closed popup menu: the root surface "<id>"; each nesting level is
        // an "<id>.panel" with an "<id>.highlight" and "<id>.item" rows (with
        // "<id>.item.arrow" and "<id>.separator" parts), created as levels and
        // rows are first needed.
        Menu createMenu(InternedString id, const UiMenuDesc& desc = {});
        // A hidden tooltip: the root surface "<id>" with the content panel "<id>.content" (and the label
        // "<id>.text" unless desc.label is off) inside a clipping window; see Tooltip.
        Tooltip createTooltip(InternedString id, const UiTooltipDesc& desc = {});
        // A closed modal dialog: the backdrop root "<id>" with the window "<id>.dialog" around the content panel
        // "<id>.content"; see Modal.
        Modal createModal(InternedString id, const UiModalDesc& desc = {});
        // A hidden drag preview: the root surface "<id>" (zOffset 2500, above tooltips; never takes hits) with the
        // label "<id>.text", installed as the UI's UiDragDrop change observer: while a drag is in flight it shows the
        // payload's label beside the pointer, styled "ui.dragPreview.accept" or ".reject" over a target that takes
        // or refuses the payload. One per UI; a second replaces the first.
        Panel createDragPreview(InternedString id);
        // A table: the root "<id>" holding "<id>.header" (with "<id>.header.cell",
        // "<id>.header.cell.text" and "<id>.header.grip" per column) and the
        // virtual list "<id>.list", whose pooled rows are "<id>.row" with
        // "<id>.row.selected" and per column "<id>.cell" and "<id>.cell.text". The header is reached by keys and pad
        // from the first row (Up) and sorts: Left and Right move over the sortable cells, Enter or Space sorts, Down
        // returns to the rows; the cell holding the focus takes the style class "table.header.cursor".
        TableView createTable(InternedString id, UiTableDesc desc);
        // Value widgets (see UiWidgets.hpp). The root "<id>" holds "<id>.box" (with "<id>.mark") or "<id>.track"
        // (with "<id>.thumb"), and "<id>.label"; a slider "<id>.bar" with "<id>.fill", "<id>.thumb" and
        // "<id>.rest"; a progress bar "<id>.fill" and "<id>.rest"; a radio group its items "<item id>" with
        // "<item id>.box", "<item id>.mark" and "<item id>.label".
        Checkbox createCheckbox(InternedString id, std::string_view label, const UiRectDesc& desc = {},
                                bool checked = false);
        Toggle createToggle(InternedString id, std::string_view label, const UiRectDesc& desc = {},
                            bool checked = false);
        Slider createSlider(InternedString id, const UiSliderDesc& desc = {});
        ProgressBar createProgressBar(InternedString id, const UiProgressDesc& desc = {});
        RadioGroup createRadioGroup(InternedString id, const UiRadioGroupDesc& desc = {});
        // A tab bar "<id>": each tab "<tab id>" holds its label "<tab id>.label".
        TabBar createTabBar(InternedString id, const UiTabBarDesc& desc = {});
        // A dropdown: the root "<id>" holds "<id>.label" and "<id>.arrow"; the list is the separate root surface
        // "<id>.popup" with the window "<id>.list", the "<id>.highlight", the "<id>.thumb" and pooled "<id>.item" rows.
        // A combo's "<id>.input" replaces the label.
        Dropdown createDropdown(InternedString id, const UiDropdownDesc& desc = {});
        Combo createCombo(InternedString id, const UiDropdownDesc& desc = {});
        // A popup list: the element "<id>" is the list (its own root surface, hidden while closed) with the
        // "<id>.search", "<id>.highlight", "<id>.thumb", "<id>.empty" and pooled "<id>.item" parts of a dropdown's.
        PopupList createPopupList(InternedString id, const UiDropdownDesc& desc = {});
        // Binds the target to an entity component field of this world (UiBindings): it follows the field with no
        // per-frame code, and goes when the scope, the target or the source does. The binding's id, or why not.
        [[nodiscard]] Expected<u32> bind(const Widget& target, const UiBindingDesc& desc);
        bool unbind(u32 binding);
        // A canvas element "<id>" (UiCanvas.hpp).
        Canvas createCanvas(InternedString id, const UiCanvasDesc& desc = {});
        // A chart "<id>" with the canvases "<id>.plot" and "<id>.overlay" (see Chart).
        Chart createChart(InternedString id, const UiChartDesc& desc = {});

        // -- Widget internals ---------------------------------------------------

        [[nodiscard]] World& world() const { return *m_world; }
        [[nodiscard]] UiHierarchyManager& hierarchy() const;
        [[nodiscard]] UiRouter& router() const;
        [[nodiscard]] UiOverlays& overlays() const;
        [[nodiscard]] UiDragDrop& dragDrop() const;
        [[nodiscard]] bool isShutdown() const { return !m_active; }
        // The context's layout token table (UiContext::layoutTokens), for a presenter that places its own
        // children and reads the sizes and spaces a density sets.
        [[nodiscard]] const UiLayoutTokenTable& layoutTokens() const;
        // The context's color token table: the active theme's colours for a canvas painter to read when it paints.
        [[nodiscard]] const UiColorTokenTable& colorTokens() const;
        // Runs `fn` when the layout or the color table's content changed (a theme or density), after the bound fields
        // took the tokens and before the layout resolves; returns the id unobserveLayoutTokens takes. The scope drops
        // them with itself. An observer may remove observers, but must not create or destroy a scope.
        u32 observeLayoutTokens(std::function<void()> fn);
        void unobserveLayoutTokens(u32 id);
        // The callbacks this scope's widgets store (text input, scroll view, list row, tree, dock, menu and table
        // handlers) whose code lies in image: what unloading the image would leave dangling. Not the router's
        // reactions.
        [[nodiscard]] u32 countCallbacksIn(const ImageRange& image) const;
        void setReaction(Entity entity, UiEventKind kind, UiRouter::ReactionFn fn, CodeOrigin origin = {});
        void setKeyReaction(Entity entity, UiRouter::KeyReactionFn fn, CodeOrigin origin = {});
        void markLayoutDirty() const;
        void markLayoutDirty(Entity element) const;
        [[nodiscard]] Entity scrollContent(Entity viewport) const;
        // Clamps and applies the viewport's scroll offset. A scroll that does
        // not move is free; `full` (sizes or rows changed) re-pools a list and
        // rebinds every visible slot.
        void syncScroll(Entity viewport, Vec2 offset, bool full);
        // Called after every scroll view offset or size sync (one per view).
        void onScrolled(Entity viewport, std::function<void(Vec2 offset)> fn);
        // The element's size changed (set, or given by a layout container): resyncs a scroll view, a table
        // or a dock space.
        void resized(Entity element);
        // The state of the value widget the element is the root or a part of; null for none.
        [[nodiscard]] UiValueState* valueState(Entity element) const;
        // The state of the dropdown or combo the element is the root or a part of (not its popup's); null for none.
        [[nodiscard]] UiDropdownState* dropdownState(Entity element) const;
        // A canvas element's runtime (the presenter's list and pointer callback); null for any other element.
        struct CanvasData
        {
            UiCanvasContent content;
            std::function<void(const UiCanvasPointer&)> onPointer;
            Vec2 lastLocal{}; // the last pointer position over the canvas, local
            bool warned = false; // dropped primitives were logged; cleared by an emission that drops nothing
        };
        [[nodiscard]] CanvasData* canvasData(Entity element);
        // The chart whose root is the element; null for any other element.
        [[nodiscard]] UiChartState* chartState(Entity element) const;
        void syncCharts();
        // Local-space pointer event for the canvas, from a router payload position (layout pixels).
        void canvasPointer(Entity element, UiCanvasPointerKind kind, Vec2 position, Vec2 delta, u8 button, u8 modifiers,
                           Vec2 wheel);

    private:
        friend class Widget;
        friend struct TreeView;
        friend struct DockSpace;
        friend struct Menu;
        friend struct Tooltip;
        friend struct Modal;
        friend struct TableView;
        friend struct TextInput;
        friend struct NumberField;
        friend struct Checkable;
        friend struct Slider;
        friend struct ProgressBar;
        friend struct Radio;
        friend struct RadioGroup;
        friend struct Tab;
        friend struct TabBar;
        friend struct Dropdown;
        friend struct Combo;
        friend struct PopupList;
        friend struct Canvas;
        friend struct Chart;
        friend class UiChartState;
        friend class UiValueState;
        friend class UiDropdownState;
        friend class UiContext;

        // How an edit coalesces into the undo history: consecutive typing
        // (or deleting) is one step.
        enum class TextEditKind : u8
        {
            None,
            Typing,
            Deleting,
            Other,
        };

        struct TextSnapshot
        {
            std::string text;
            u32 caret = 0;
            u32 anchor = 0;
        };

        struct TextInputData
        {
            std::function<void(bool)> onFocus; // a combo box's hook: the input gained or lost focus
            // Widget::onFocus on the input: after its own focus handling, which a reaction of the owner's would replace
            std::function<void(bool)> ownerFocus;
            std::function<void()> onCancel; // a search box's hook: Escape, before the edit cancels and the focus clears
            std::function<bool()> cancelTaken; // a console's hook: Escape asks first; true leaves the edit and focus
            std::string* bound = nullptr;
            std::function<void(std::string_view)> onChange;
            std::function<void(std::string_view)> onCommit;
            std::function<void(std::string_view)> onSubmit;
            std::function<void(u32, u8)> onKey;
            std::function<void(double)> onNumber;
            std::function<void(std::string_view)> onInvalid;
            std::string committed; // the text the edit started from, or the last commit
            std::vector<TextSnapshot> undo;
            std::vector<TextSnapshot> redo;
            TextEditKind lastEdit = TextEditKind::None;
            bool numeric = false;
            bool selectOnFocus = false; // focus selects the whole text (a number field, a combo box)
            bool focusPress = false; // the focusing press of such a field keeps its select-all
        };

        struct ScrollData
        {
            Entity content = Entity::undefined();
            Entity thumb = Entity::undefined();
            Entity hthumb = Entity::undefined(); // created once content is wider than the view
            std::function<void(Vec2)> onScrolled;
            // Virtualized lists only: pool slot -> row element and bound row.
            std::vector<Entity> rows;
            std::vector<u32> boundRows;
            u32 poolSize = 0;
            Vec2 dragOrigin{0.0f, 0.0f}; // pointer and offset when the active thumb drag began
            Vec2 dragOffset{0.0f, 0.0f};
            std::function<Widget(u32)> createRow;
            std::function<void(u32, u32)> bindRow;
        };

        Entity createElement(InternedString id, UiWidgetKind kind, const UiRectDesc& desc);
        Canvas createCanvasOfKind(InternedString id, UiWidgetKind kind, const UiCanvasDesc& desc);
        Entity createScrollElements(InternedString id, const UiRectDesc& desc, Vec2 contentSize);
        void syncRows(Entity viewport, ScrollData& data, const UiVirtualListComponent& list, Vec2 viewSize,
                      float rowWidth, float offsetY, bool full);
        void bindThumbDrag(Entity viewport, Entity thumb, bool horizontal);
        void createHorizontalThumb(Entity viewport, ScrollData& data);
        static void addLabel(UiScope& scope, Entity entity, std::string_view text);

        // Text input internals (UiTextInput.cpp).
        Entity createTextElement(InternedString id, UiWidgetKind kind, const UiRectDesc& desc, bool numeric);
        [[nodiscard]] TextInputData* textData(Entity entity);
        void textCompose(Entity entity, std::string_view text, u32 cursor, u32 selected);
        void textCommand(Entity entity, UiTextCommand command, bool extend);
        void textInsert(Entity entity, std::string_view text);
        void textSelect(Entity entity, u32 anchor, u32 caret);
        void textEdit(Entity entity, std::string text, u32 caret, TextEditKind kind);
        void textRestore(Entity entity, std::vector<TextSnapshot>& from, std::vector<TextSnapshot>& to);
        void textChanged(Entity entity);
        void textCommit(Entity entity);
        void textReset(Entity entity);
        void textFocus(Entity entity, bool gained);
        [[nodiscard]] u32 caretAt(Entity entity, Vec2 position) const;
        void numberDrag(Entity entity, Vec2 delta);
        [[nodiscard]] UiTreeState* treeState(Entity viewport) const;
        [[nodiscard]] UiDockState* dockState(Entity root) const;
        [[nodiscard]] UiMenuState* menuState(Entity root) const;
        [[nodiscard]] UiTableState* tableState(Entity root) const;
        Entity createCheckable(InternedString id, std::string_view label, const UiRectDesc& desc, bool checked,
                               UiWidgetKind kind);
        Entity createChoice(InternedString id, const UiDropdownDesc& desc, UiWidgetKind kind, bool standalone = false);
        void propagateInteractable(Entity element, bool interactable);
        void applyInteractable(Entity element, bool interactable);
        [[nodiscard]] bool hasValueWidgets() const { return !m_valueWidgets.empty() || !m_dropdowns.empty(); }
        // Applies a raw write of a value widget's state component to its parts (UiContext's write scan).
        void syncValueWidget(Entity root);
        // Re-places (or closes) the open dropdown lists whose anchor moved, was hidden or disabled.
        void refitDropdowns();
        void setNumber(Entity entity, double value, bool notify);
        // Tears down owned elements and tables; idempotent (the context's
        // force-flush runs it before the presenter's own scope destructor).
        void shutdown();

        // The shared tooltip of Widget::setTooltip, built at the first use: text, and the shortcut beside it.
        void setTooltipText(Entity element, InternedString key, std::string_view shortcut);
        Tooltip m_tip;
        Label m_tipText;
        Label m_tipShortcut;

        void notifyLayoutTokens();

        UiContext* m_context = nullptr; // nulled by the context's force-flush
        std::vector<std::pair<u32, std::function<void()>>> m_tokenObservers;
        u32 m_nextTokenObserver = 1;
        World* m_world = nullptr;
        u32 m_generation = 0;
        bool m_active = true;
        CommandBuffer m_commandBuffer;
        std::unordered_set<Entity> m_elements;
        std::unordered_map<u64, TextInputData> m_textInputs;
        std::unordered_map<u64, ScrollData> m_scrollViews; // by viewport entity id
        std::unordered_map<u64, Entity> m_scrollParts; // content, thumbs and pooled rows -> their viewport
        std::unordered_map<u64, std::unique_ptr<UiTreeState>> m_trees; // by viewport entity id
        std::unordered_map<u64, std::unique_ptr<UiDockState>> m_docks; // by root entity id
        std::unordered_map<u64, std::unique_ptr<UiMenuState>> m_menus; // by root entity id
        std::unordered_map<u64, std::unique_ptr<UiTableState>> m_tables; // by root entity id
        std::unordered_map<u64, std::unique_ptr<UiValueState>> m_valueWidgets; // by root entity id
        u32 m_openDropdowns = 0; // lists open now (UiDropdownState keeps it)
        std::unordered_map<u64, std::unique_ptr<UiDropdownState>> m_dropdowns; // by root entity id
        std::unordered_map<u64, u64> m_valueParts; // part (radio items included) -> its root's entity id
        std::unordered_map<u64, CanvasData> m_canvases; // by element entity id
        std::unordered_map<u64, std::unique_ptr<UiChartState>> m_charts; // by root entity id
    };
} // namespace spite
