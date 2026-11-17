#pragma once

#include <functional>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/ImageRange.hpp"
#include "ecs/EcsTypes.hpp"
#include "ui/UiBinding.hpp"
#include "ui/UiCanvas.hpp"
#include "ui/UiChart.hpp"
#include "ui/UiClipboard.hpp"
#include "ui/UiDragDrop.hpp"
#include "ui/UiExport.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiInputSnapshot.hpp"
#include "ui/UiLayout.hpp"
#include "ui/UiLayoutTokens.hpp"
#include "ui/UiOverlay.hpp"
#include "ui/UiTween.hpp"

namespace spite
{
    class World;
    class UiRouter;
    class UiScope;
    class UiDocuments;
    class UiDocumentView;
    class SystemContext;

    enum class UiResizeAxis : u8
    {
        None,
        Horizontal,
        Vertical,
        DiagonalNorthWestSouthEast,
        DiagonalNorthEastSouthWest,
    };

    // The ui module root, bound to a world it never owns. Registers the
    // module's components and the "UiLayoutResolve" system, publishes itself
    // and the hierarchy manager through the world's service locator (how a
    // game module finds the UI of the world it attaches to), and unregisters
    // both on destruction. The bound world must carry a host ServiceLocator
    // (World::create(threadCount, &services)).
    //
    // Frame contract: processInput(snapshot) runs router -> dispatch ->
    // dirty-driven layout resolve. The router's dispatch fires typed ECS
    // events and invokes presenter reactions from one pass.
    //
    // Lifetime: destroy the context before the world. Live scopes are
    // force-shut down by the context's destructor, so presenter scopes that
    // outlive it hold only invalid handles — no dangling entity access.
    // Where the focused text input is entering text, in the UI's own pixels: the rect of the text being
    // composed, or a 1 px caret rect without a composition; `cursor` is the insertion point's x offset from the
    // rect's left edge. The input method places its candidate window beside it.
    struct UiTextInputArea
    {
        Vec2 position{0.0f, 0.0f};
        Vec2 size{0.0f, 0.0f};
        float cursor = 0.0f;
        bool operator==(const UiTextInputArea&) const = default;
    };

    class SPITE_UI_API UiContext
    {
    public:
        explicit UiContext(World& world);
        ~UiContext();
        UiContext(const UiContext&) = delete;
        UiContext& operator=(const UiContext&) = delete;

        [[nodiscard]] UiHierarchyManager& hierarchy() { return *m_manager; }
        [[nodiscard]] const UiHierarchyManager& hierarchy() const { return *m_manager; }
        [[nodiscard]] UiRouter& router() { return *m_router; }
        [[nodiscard]] const UiRouter& router() const { return *m_router; }
        [[nodiscard]] UiResizeAxis resizeAxis() const;
        // The active tweens of this UI, advanced by the "UiTweens" system.
        [[nodiscard]] UiTweens& tweens() { return *m_tweens; }
        [[nodiscard]] const UiTweens& tweens() const { return *m_tweens; }
        // Tooltips and modal dialogs, advanced by the "UiOverlays" system.
        [[nodiscard]] UiOverlays& overlays() { return *m_overlays; }
        [[nodiscard]] const UiOverlays& overlays() const { return *m_overlays; }
        // Data bindings from component fields to UI fields, visited in the layout system's update.
        [[nodiscard]] UiBindings& bindings() { return *m_bindings; }
        [[nodiscard]] const UiBindings& bindings() const { return *m_bindings; }
        // Drag and drop between elements and panels; idle costs nothing.
        [[nodiscard]] UiDragDrop& dragDrop() { return *m_dragDrop; }
        [[nodiscard]] const UiDragDrop& dragDrop() const { return *m_dragDrop; }
        [[nodiscard]] UiLayoutView layout() const { return m_manager->layout(); }
        [[nodiscard]] Entity hitTest(Vec2 point) const { return m_manager->hitTest(point); }
        void resolveLayout(float scale = 1.0f) { m_manager->resolveLayout(scale); }

        // The frame entry: router state machines, event dispatch, then the
        // dirty-driven layout resolve.
        void processInput(const UiInputSnapshot& snapshot);
        // A host may consume input before this layer and the layers below are routed.
        void setInputFilter(std::function<void(UiInputSnapshot&)> filter);
        void filterInput(UiInputSnapshot& snapshot) const;

        // One navigation intent now (UiRouter::navigate), then the element it focused scrolls into view: whether
        // anything acted. The frame's own intents arrive in the snapshot and scroll the same way.
        bool navigate(UiNavIntent intent);

        // The next or previous tab stop now (UiRouter::moveFocus), scrolled into view before it returns.
        bool moveFocus(bool backward);

        // Semantic actions on one element, the widget path a person's pointer and keys take (uiActivate).
        // Click queues PointerDown/PointerUp/Click at the centre for the next dispatch; Focus moves the router's
        // focus; SetValue replaces a text input's or number field's text through select-all, insert and commit
        // (change, commit and number reactions fire, a number field clamps and rounds); on a checkbox or toggle it
        // takes true/false, on a slider or progress bar a number, on a radio group an item index or id (a radio item:
        // true), each firing the widget's onChange once when the value changed; ScrollIntoView scrolls
        // every scrolling view above the element by the least offset that shows it (one with scrolling off clips: it
        // is skipped, and an element under no scrolling view is refused). Click, Focus and SetValue refuse
        // an element that is not laid out, visible (it and its ancestors) and interactable (its own flag, as hit
        // testing reads it).
        enum class Action : u8
        {
            Click,
            Focus,
            SetValue,
            ScrollIntoView,
        };
        // Empty on success, else why the action did not apply.
        [[nodiscard]] std::string activate(Entity element, Action action, std::string_view text = {});

        // The focused text input's area, published by the view that paints it (UiViewContext) and read by the host to
        // tell the platform; none while no text input is focused.
        void setTextInputArea(const std::optional<UiTextInputArea>& area) { m_textInputArea = area; }
        [[nodiscard]] const std::optional<UiTextInputArea>& textInputArea() const { return m_textInputArea; }

        // Not owned; null falls back to the context's own clipboard text.
        void setClipboard(UiClipboard* clipboard) { m_clipboard = clipboard; }
        [[nodiscard]] std::string clipboardText() const;
        void setClipboardText(std::string_view text);

        // The canvas content of an element, null for an element that is no canvas (the view lowers it).
        [[nodiscard]] const UiCanvasContent* canvasContent(Entity element) const
        {
            const auto it = m_canvasIndex.find(element.id());
            return it == m_canvasIndex.end() ? nullptr : it->second;
        }
        [[nodiscard]] u32 canvasCount() const { return static_cast<u32>(m_canvasIndex.size()); }
        // Primitives held by all canvases together, and the most they may hold: an emission that would
        // pass the budget drops the excess, counted in that canvas's `dropped` like its own bound. A series
        // primitive counts as the instances it lowers to (its width in pixels).
        [[nodiscard]] u64 canvasPrimitives() const { return m_canvasPrimitives; }
        [[nodiscard]] u32 canvasBudget() const { return m_canvasBudget; }
        void setCanvasBudget(const u32 primitives) { m_canvasBudget = primitives; }
        // The canvases' series buffers, written through Canvas.
        [[nodiscard]] const UiSeriesTable& series() const { return m_series; }
        // The samples all series may hold together (UI_SERIES_CONTEXT_BUDGET); a create past it returns an invalid id.
        void setSeriesBudget(const u64 samples) { m_series.setBudget(samples); }

        // What a chart element shows (axes, ticks, hover, selection); false for any other element.
        [[nodiscard]] bool chartReadout(Entity element, UiChartReadout& out) const;

        // Stored widget callbacks of every live scope with code in image (UiScope::countCallbacksIn), and of the
        // document views' contracts.
        [[nodiscard]] u32 countCallbacksIn(const ImageRange& image) const;

        // The views built from UI documents (UiDocument.hpp).
        [[nodiscard]] UiDocuments& documents() { return *m_documents; }
        [[nodiscard]] const UiDocuments& documents() const { return *m_documents; }

        // The layout token table the elements' UiLayoutTokensComponent bindings read; the view publishes
        // the active theme's numeric tokens into it, replacing what code set (see UiLayoutTokenTable).
        [[nodiscard]] UiLayoutTokenTable& layoutTokens() { return m_layoutTokens; }
        [[nodiscard]] const UiLayoutTokenTable& layoutTokens() const { return m_layoutTokens; }
        // Overrides numeric layout tokens for the root and its descendants; an invalid root clears it.
        void setSubtreeLayoutTokens(Entity root, std::unordered_map<InternedString, float> values);
        // The active theme's colours by name, for canvas painters; syncLayoutTokens also runs the scopes' token
        // observers when it moved.
        [[nodiscard]] UiColorTokenTable& colorTokens() { return m_colorTokens; }
        [[nodiscard]] const UiColorTokenTable& colorTokens() const { return m_colorTokens; }
        // Writes the element's bound layout fields from the table now: a token the table lacks, or a field whose
        // component the element lacks, keeps the field's value. Marks the layout dirty when a value moved; whether
        // one did.
        bool applyLayoutTokens(Entity element);
        // Applies every bound element when the table's generation moved since the last pass (the layout system
        // calls it each frame; the view after publishing). Passes run so far (diagnostics: a quiet UI adds none).
        void syncLayoutTokens();
        [[nodiscard]] u64 layoutTokenPasses() const { return m_layoutTokenPasses; }

    private:
        friend class UiScope;
        friend class UiDocumentView;
        friend struct Canvas;

        [[nodiscard]] UiScope* ownerOf(Entity element) const;
        void showNavigated();
        // The value widgets whose state component was written in place this frame apply it to their parts.
        void syncValueWidgets(SystemContext& ctx);
        // The elements whose UiLayoutTokensComponent was written in place this frame take their tokens.
        void applyWrittenLayoutTokens(SystemContext& ctx);
        // Charts re-label their axes when the locale changed.
        void syncCharts();
        void registerScope(UiScope* scope);
        void unregisterScope(UiScope* scope);

        World* m_world = nullptr;
        std::unique_ptr<UiHierarchyManager> m_manager;
        std::unique_ptr<UiRouter> m_router;
        std::unique_ptr<UiTweens> m_tweens;
        std::unique_ptr<UiOverlays> m_overlays;
        bool m_escapeSpent = false; // the last Escape press hid a tooltip: its repeats are spent too
        std::unique_ptr<UiBindings> m_bindings;
        std::unique_ptr<UiDragDrop> m_dragDrop;
        std::unique_ptr<UiDocuments> m_documents;
        std::vector<UiScope*> m_scopes;
        std::vector<Entity> m_writtenValueWidgets; // scratch of syncValueWidgets
        std::unordered_map<u64, const UiCanvasContent*> m_canvasIndex; // the scopes' canvases, by element id
        u64 m_canvasPrimitives = 0; // running sum over m_canvasIndex, adjusted where a canvas changes or goes
        u32 m_canvasBudget = UI_CANVAS_CONTEXT_BUDGET;
        UiSeriesTable m_series;
        UiClipboard* m_clipboard = nullptr;
        std::string m_localClipboard;
        std::optional<UiTextInputArea> m_textInputArea;
        UiLayoutTokenTable m_layoutTokens;
        UiLayoutTokenTable m_subtreeLayoutTokens;
        Entity m_layoutTokenRoot = Entity::undefined();
        bool m_subtreeTokensChanged = false;
        UiColorTokenTable m_colorTokens;
        u64 m_appliedTokenGeneration = 0;
        u64 m_appliedColorGeneration = 0;
        u64 m_layoutTokenPasses = 0;
        std::vector<Entity> m_tokenScratch;
    };
} // namespace spite
