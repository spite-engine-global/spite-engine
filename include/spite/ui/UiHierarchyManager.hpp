#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <span>

#include "ecs/CommandBuffer.hpp"
#include "ecs/EcsTypes.hpp"
#include "ui/UiComponents.hpp"
#include "uiruntime/UiRuntimeExport.hpp"
#include "ui/UiLayout.hpp"

namespace spite
{
    class World;
    class SystemContext;
    class UiTextMeasurer;
    class Localization;

    // Contiguous paint positions [start, start + count) of the last resolve.
    struct UiPaintRange
    {
        u32 start = 0;
        u32 count = 0;
    };

    // One direct child of a custom layout container, in declared order. Rects are in scaled UI pixels.
    struct UiCustomLayoutChild
    {
        Entity entity;
        Vec2 measuredSize;
        UiRect rect;
    };

    // Called once for a container group during arrangement. Modify child rects in place.
    using UiCustomLayout = std::function<void(Entity, UiRect, float, std::span<UiCustomLayoutChild>)>;

    // The single source of truth for the UI parent/child relation: edges live
    // in manager-internal storage, never in components. Membership is the
    // UiRectComponent and is fed entirely by lifecycle events, so the manager
    // never probes the world to discover its members. A member without an edge
    // is a root — a UI surface.
    //
    // The manager compiles the relation into a DFS pre-order index (parents
    // before children, subtrees contiguous). On top of it, resolveLayout()
    // computes absolute rects, inherited clip rects and the paint order in two
    // passes: measure (children before parents: text leaves and fitted layout
    // containers size to their content) and arrange (parents before
    // children: a layout container places its children, any other
    // element's children place themselves by anchor). Paint order is the
    // DFS pre-order with siblings sorted by zOffset (declared order breaks
    // ties). Paint order is the only z mechanism; hit testing scans it in
    // reverse. Root surfaces stack the same way: by zOffset, then entity id
    // order — creation order for practical purposes.
    class SPITE_UI_RUNTIME_API UiHierarchyManager
    {
    public:
        static constexpr u32 NO_INDEX = static_cast<u32>(-1);

        explicit UiHierarchyManager(World& world);
        ~UiHierarchyManager();
        UiHierarchyManager(const UiHierarchyManager&) = delete;
        UiHierarchyManager& operator=(const UiHierarchyManager&) = delete;

        // Reparents child under parent; both must be elements (members).
        // Rejects self-parenting and cycles; detaches from the previous
        // parent. No components are written.
        bool attach(Entity child, Entity parent);

        // Detaches child; it becomes a root surface. False when no edge exists.
        bool detach(Entity child);

        // Moves child within its parent's declared child order; the index is
        // clamped to the sibling count. False when child has no parent.
        bool reorderChild(Entity child, u32 index);

        // The listed children of `parent` first, in that order, the others after them in their order: O(children)
        // for any number of moves (reorderChild is O(children) each). Entries that are not its children are ignored.
        bool setChildOrder(Entity parent, std::span<const Entity> first);

        // Queues child and its whole store-reachable subtree for destruction
        // through the buffer; the relation store prunes through lifecycle
        // events at commit.
        void destroySubtree(Entity root, CommandBuffer& commandBuffer);

        // Relation queries read the store directly and never recompile, so
        // building UI stays linear; subtreeOf alone compiles the index.
        [[nodiscard]] bool isElement(Entity entity) const;
        // The members, and a counter that moves with every structure change (an element joined or left, an edge or a
        // sibling order moved): a reader that follows the structure compares it per frame and reads nothing else.
        [[nodiscard]] u32 elementCount() const;
        [[nodiscard]] u64 structureGeneration() const;
        // Every element in the compiled DFS pre-order (roots in entity order, each followed by its subtree); compiles
        // like subtreeOf. Valid until the next structure change.
        [[nodiscard]] std::span<const Entity> compiledOrder() const;
        [[nodiscard]] World& world() const;
        [[nodiscard]] Entity parentOf(Entity entity) const;
        // O(depth) walk up the store.
        [[nodiscard]] u32 depthOf(Entity entity) const;
        // Declared-order children; valid until the next structure change.
        [[nodiscard]] std::span<const Entity> childrenOf(Entity entity) const;
        // Contiguous declared-order range; valid until the next structure change.
        [[nodiscard]] std::span<const Entity> subtreeOf(Entity entity) const;

        // Replaces direct-child placement for this container. The callback runs once per arranged group,
        // including dirty resolves, and must not mutate the hierarchy. An empty callback unregisters it.
        // Custom placement does not participate in fit-content measurement; mark the container dirty after
        // changing callback-owned inputs. The manager owns the callback; captured references must remain valid until
        // unregistered or the container loses its rect. Do not resolve recursively or change registration in it.
        void setCustomLayout(Entity container, UiCustomLayout callback);

        // Full resolve: recompiles the index when the structure changed, then
        // recomputes every element's rect, clip and paint position. Pure
        // computation; reads components. Scale, stylesheet and font changes
        // call it. The scale sticks: dirty-driven resolves reuse the last one.
        void resolveLayout(float scale = 1.0f);

        // Dirty-driven resolve (the frame contract): a full resolve after a
        // structure or measurer change or markLayoutDirty(), otherwise only
        // the subtrees of marked elements; no-op when nothing is dirty. A
        // marked layout-container child re-lays out from its container, and
        // up the container chain while a fitted container's measure moves;
        // children of a re-arranged container that did not move keep their
        // results.
        void resolveDirty();
        // Elements laid out (written) by the last resolveLayout/resolveDirty call that resolved, follow-up rounds
        // after resize notifications included.
        [[nodiscard]] u32 resolvedElementCount() const;
        // Calls of reorderChild / setChildOrder that changed a sibling order, ever (a document apply moves each host
        // once: a deterministic count where wall time would be noise).
        [[nodiscard]] u64 childOrderChanges() const;
        // Compiled-index rebuilds since construction; building UI must cost
        // one per structure change batch, not one per element.
        [[nodiscard]] u64 rebuildCount() const;

        // Rect writes through raw component access bypass the manager; widget
        // setters call this so the frame contract's dirty-driven resolve
        // picks them up. Content setters (text, image, interactable) call it
        // too, so the view repaints in the same frame. The no-argument form
        // re-lays out everything; the element form only its subtree — layout
        // flows parent to child, so a change that moves siblings (zOffset)
        // marks the parent.
        void markLayoutDirty();
        void markLayoutDirty(Entity element);
        // A change only the view's paint reads (caret, selection): repaints
        // the element without a layout resolve.
        void markPaintDirty(Entity element);
        // A change to the element's UiVisualComponent: repaints the element and its whole subtree. The layout
        // system picks up in-place writes by itself (as for rects) and the manager sees the component's adding
        // and removal; this is for hosts that resolve by hand.
        void markVisualDirty(Entity element);
        // The elements carrying a UiVisualComponent, in no order; fed by lifecycle events.
        [[nodiscard]] std::span<const Entity> visualOwners() const;
        // The element's paint position and its subtree's size in paint positions; nullopt when the last resolve
        // did not lay it out.
        [[nodiscard]] std::optional<UiPaintRange> paintRangeOf(Entity element) const;
        // Bumps on every resolve and paint mark: whatever the view repaints for.
        [[nodiscard]] u64 paintGeneration() const;
        // The paint ranges changed since the caller saw `generation` (a
        // paintGeneration() value): re-laid out subtrees and paint-marked
        // elements, in mark order, possibly overlapping. nullopt when
        // everything changed since: a full resolve, or more marks than the log
        // keeps, or older than what consumePaintDamage dropped.
        [[nodiscard]] std::optional<std::span<const UiPaintRange>> paintDamageSince(u64 generation) const;
        // The paint consumer (the view) has read the damage up to `generation`: the log drops it, so it holds
        // only unread damage and never overflows into a full repaint while the view keeps up.
        void consumePaintDamage(u64 generation);
        // Sizes UiContentSizeComponent text elements at resolve; null leaves
        // their rect sizes. Not owned; marks the layout dirty.
        void setTextMeasurer(const UiTextMeasurer* measurer);
        [[nodiscard]] const UiTextMeasurer* textMeasurer() const;
        // The Localization that resolves UiLocalizedTextComponent keys; not owned. Without a call the
        // "localization" service is looked up in the world's locator while the UI has localized text. Re-resolves every
        // localized element. The pointer is cached for the hierarchy's life: the service must outlive the last update
        // of every UI world (unregister it only at shutdown, after the last frame, as the engine does). With none bound
        // localized text shows its key until one appears.
        void setLocalization(const Localization* localization);
        [[nodiscard]] const Localization* localization() const;
        // The locale id of the bound Localization as of the last update that saw its revision change ("" before the
        // first, and while none is bound: the UI has no localized text yet). The view takes its case rules from it; a
        // change marks the layout dirty so text transforms measure again.
        [[nodiscard]] const std::string& textLocale() const;
        // Resolves one localized element now (widget setters, so the text is there before the first frame); a raw
        // component write is seen by the next update. Without a Localization it shows the key.
        void refreshLocalized(Entity element);
        [[nodiscard]] u32 localizedCount() const;
        // elements waiting for the next update to resolve (each at most once)
        [[nodiscard]] u32 localizedPendingCount() const;
        [[nodiscard]] bool isLocalized(Entity element) const;
        [[nodiscard]] bool isLayoutDirty() const;
        // Bumps on every resolve; consumers compare it to detect a new layout.
        [[nodiscard]] u64 layoutGeneration() const;
        [[nodiscard]] float layoutScale() const;
        // The text scale of the display (the view context's stylesheet sets it): content that lays out its own text,
        // such as a chart's canvas labels, sizes it by this. A stored value; setting it relays out nothing.
        [[nodiscard]] float textScale() const;
        void setTextScale(float scale);

        // Marks an element whose size others derive state from (a scroll view's thumb and rows, a table's
        // columns, a dock's panels). While a layout container places it, layoutSizeOf reports the size the
        // container gave it and a change reaches the resize sink.
        void observeSize(Entity element);
        // The element's size in unscaled UI pixels: for an observed element a container placed, the size of
        // the last resolve; otherwise its rect size.
        [[nodiscard]] Vec2 layoutSizeOf(Entity element) const;
        [[nodiscard]] u32 observedSizeCount() const;
        // Queues every observed element for the sink at the next resolve, whether or not its size changed: what
        // they derive from text (a dock's tab widths, a table's columns) is stale after a style or text scale change.
        void requeueObservedSizes();
        // Observed elements whose container-given size changed (or that lost their container) reach the
        // UiContext's sink after a resolve; observation ends when the element is destroyed.

        // Views over the last resolve (stale by contract until the next call).
        [[nodiscard]] UiLayoutView layout() const { return UiLayoutView(*this); }
        // Frontmost visible, interactable element containing the point and
        // surviving the ancestor clip chain; Entity::undefined() when none.
        // `window`: the OS window the point is in; only the roots bound to it are hit (0: the unbound
        // ones, the main window's).
        [[nodiscard]] Entity hitTest(Vec2 point, u64 window = 0) const;
        // a root surface bound to an OS window (0 unbinds). Its subtree is hit only through that window's
        // pointer and painted into that window's target (UiFrameGeometry::windowRuns), clipped to the window's rect.
        // A popup takes the binding of the element that opened it: bindRootToWindow(popup, windowOf(anchor)).
        void bindRootToWindow(Entity root, u64 window);
        // The window's client rect in this UI's layout (scaled) pixels: where its roots sit and what clips them.
        void setWindowRect(u64 window, UiRect rect);
        [[nodiscard]] std::optional<UiRect> windowRect(u64 window) const;
        // The window the element's root surface is bound to; 0: the main one.
        [[nodiscard]] u64 windowOf(Entity element) const;
        // The bound roots and their windows, in binding order.
        [[nodiscard]] std::span<const std::pair<Entity, u64>> windowRoots() const;
        // The topmost modal surface (UiOverlays): hit tests reach only elements painted at or above it; a root that
        // is not laid out yet hits nothing. Undefined: none.
        void setModalRoot(Entity root);
        [[nodiscard]] Entity modalRoot() const;
        // Hit tests skip the root's subtree while on (a tooltip never takes input).
        void setHitTransparent(Entity root, bool on);

        // Thin-system entry: resolveDirty. Registered by the
        // manager as the "UiLayoutResolve" system, so hosts never run the
        // frame update by hand.
        void update(SystemContext& ctx);
        // Runs at the start of every update, after the localization pass and before the in-place write scan, so
        // what it writes reaches this frame's layout. The UiContext's widget write scan (UiValueWidgets).
        void setUpdateHook(std::function<void(SystemContext&)> hook);

        // -- UiLayoutView accessors -----------------------------------------

        [[nodiscard]] UiRect absoluteRectOf(Entity entity) const;
        [[nodiscard]] UiRect clipRectOf(Entity entity) const;
        [[nodiscard]] Entity clipOwnerOf(Entity entity) const;
        [[nodiscard]] bool isClippedOf(Entity entity) const;
        [[nodiscard]] bool isVisibleOf(Entity entity) const;
        [[nodiscard]] u32 paintIndexOf(Entity entity) const;
        [[nodiscard]] std::span<const Entity> paintOrderOf() const;

    private:
        friend class UiContext;
        // Called after a resolve for each observed element whose container-given size changed. A call may mark
        // the layout dirty; the resolve then repeats (a few rounds) before resolveLayout/resolveDirty return.
        void setResizeSink(std::function<void(Entity)> sink);

        // subtreeOf rebuilds the compiled index on demand; the const-ness of
        // the accessors is preserved because the state is held through a
        // const unique_ptr (pointee stays mutable).
        void ensureCompiled() const;
        void resolveLayoutOnce(float scale);
        void resolveDirtyOnce();
        void settleResized();

        struct State;
        std::unique_ptr<State> m_state;
    };

    // The module's frame entry: fetches the manager from the world's service
    // locator and runs the frame update. Registered by UiContext; no-op
    // without a bound service.
    SPITE_UI_RUNTIME_API void updateUiLayout(SystemContext& ctx);
} // namespace spite
