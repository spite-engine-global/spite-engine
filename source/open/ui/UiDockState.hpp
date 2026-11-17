#pragma once

#include <functional>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ui/UiComponents.hpp"
#include "ui/UiLayout.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class UiScope;

    // A dock space's runtime half, owned by its scope: the registered panels,
    // each node's parts and the tab drag in flight. The layout itself lives in
    // the dock components.
    class UiDockState
    {
    public:
        // Where a panel stood when it was floated or closed, as the op that puts it back (runtime only, not saved; a
        // loaded layout clears it): a tab beside `target` (Center, at `index`), or a split beside the group `target`
        // is in; no target: the space's edge (Center: the empty space). `floating`: it was alone in a floating window,
        // which comes back at position and size. Unplaced while the panel was closed when it floated.
        struct Home
        {
            bool placed = false;
            bool floating = false;
            Vec2 position{0.0f, 0.0f};
            Vec2 size{0.0f, 0.0f};
            InternedString target{};
            UiDockZone zone = UiDockZone::Center;
            float share = 0.5f;
            u32 index = UiDockOp::LAST;
        };

        struct PanelData
        {
            Panel host;
            Button tab;
            Button close; // beside the tab on its bar: requests Close
            Widget content;
            std::string title;
            Panel tagHost; // the tag's row on the group's tab bar, shown while the panel is (invalid: no tag)
            Vec2 minSize{0.0f, 0.0f}; // content, raised to UiDockDesc::minPaneSize
            Home home;
            bool active = false; // the tab is its group's shown one (setTabActive)
            bool hovered = false; // the pointer is over the tab: an inactive tab shows its close button then
        };

        // Tabs: bar, active-tab highlight, floating grip, the frame over the whole group (a theme's border), the
        // button that lists the tabs the bar has no room for (overflowing: some tab is folded into it). Split: the
        // splitter.
        struct NodeParts
        {
            Panel bar;
            Panel highlight;
            Panel grip;
            std::array<Panel, 7> edges;
            Panel frame;
            Button more;
            bool overflowing = false;
        };

        // Where a tab drag would land; preview in dock-space pixels.
        struct Target
        {
            bool valid = false;
            UiDockOp op;
            UiRect preview;
            Entity surface = Entity::undefined();
        };

        [[nodiscard]] UiDockComponent& dock() const;
        [[nodiscard]] const UiDockComponent& dockRead() const; // a read marks nothing modified (the idle check)
        [[nodiscard]] UiDockNodeComponent& node(Entity entity) const;
        [[nodiscard]] bool isNode(Entity entity) const;
        [[nodiscard]] PanelData* panel(InternedString name);
        [[nodiscard]] const PanelData* panel(InternedString name) const;
        [[nodiscard]] InternedString nameOf(Entity host) const;
        // The tab group showing the panel; undefined while it is closed.
        [[nodiscard]] Entity groupOf(InternedString name) const;
        // The main or floating node holding `entity`.
        [[nodiscard]] Entity windowOf(Entity entity) const;
        [[nodiscard]] bool isFloating(Entity window) const;
        [[nodiscard]] bool isDetached(Entity window) const;
        // A floating window's place: detached, a root surface of its own at `position` in surface units;
        // else a child of the space at `position` in the space, clamped to it.
        void placeWindow(Entity window, Vec2 position, Vec2 size, bool detach);
        // Part of the last resolve: its rect can be read.
        [[nodiscard]] bool isResolved(Entity element) const;
        // The node owns its window's bottom-right corner (where a floating grip sits).
        [[nodiscard]] bool holdsEdge(Entity node, int x, int y) const;
        // The split with `first`'s group on its first side and `second`'s on its
        // second (their lowest common split), else undefined.
        [[nodiscard]] Entity splitBetween(InternedString first, InternedString second) const;
        // The first tab of the subtree's first-most group.
        [[nodiscard]] InternedString leadingPanel(Entity node) const;
        // The smallest size the subtree may have: a group's tab bar plus its content minimum, wide enough for its
        // widest tab beside the overflow button; a split's two sides and the splitter.
        [[nodiscard]] Vec2 minSizeOf(Entity node) const;
        // The ratio kept between both sides' minimums for a split `size` big.
        [[nodiscard]] float clampRatio(Entity split, float ratio, Vec2 size) const;
        // `ancestor` is `entity` or holds it.
        [[nodiscard]] bool holds(Entity ancestor, Entity entity) const;
        // A second press of the tab within the double-click time.
        [[nodiscard]] bool pressedTwice(InternedString name);
        // Requests Restore when the panel's group is maximized, else Maximize; a floating window does neither.
        void requestMaximizeToggle(InternedString name);
        // Requests Float for a docked panel (a window as big as its group, at the group's position plus the tab
        // height), else Dock back to its Home; the space's bottom edge (the empty space's centre) when that place is
        // gone.
        void requestFloatToggle(InternedString name);
        [[nodiscard]] Home homeOf(InternedString name) const;
        // The op that puts a closed panel back where it was closed (its Home), when that place still exists: its
        // tab group or the group beside which it split, its window, the space's edge.
        [[nodiscard]] std::optional<UiDockOp> reopenOpFor(InternedString name) const;
        // Float op for the panel with its window's top-left at `at`, kept inside the space.
        [[nodiscard]] UiDockOp floatOpFor(InternedString name, Vec2 at) const;
        // The "float here" target a tab drag shows, in dock-space pixels.
        [[nodiscard]] UiRect floatTargetRect(Vec2 spaceSize) const;
        [[nodiscard]] Vec2 clampWindowSize(Vec2 size) const;
        // A window the space can reach: no larger than the space (a floor of one tab bar plus minPaneSize wins), at
        // least a strip of its title bar inside it. A space not laid out yet clamps nothing but the minimum.
        void clampWindow(Vec2& position, Vec2& size) const;
        // The floating window holding the panel, in dock-space pixels; zero for a panel docked or closed.
        [[nodiscard]] UiRect floatingRectOf(InternedString name) const;

        void addPanel(InternedString name, std::string_view title, const Widget& content);
        void setPanelTitle(InternedString name, std::string_view title);
        void setPanelTag(InternedString name, const Widget& tag);
        void setPanelMinSize(InternedString name, Vec2 size);
        // Ownership: every entity the dock's scope made for the space (nodes and their parts, panel hosts and tabs,
        // the preview, the overflow list) dies with the space (UiScope::destroy takes the list, a root surface of
        // its own), and a node dies when the layout drops it: apply and load commit the scope before they return,
        // so a host that commits only once never holds a dead node. Panel contents belong to whoever made them.
        bool apply(const UiDockOp& op);
        bool applyOp(const UiDockOp& op);
        void request(const UiDockOp& op);

        // Writes the rects of the node's subtree from its offset and size.
        void layout(Entity node, Vec2 offset, Vec2 size);
        // Places a tab group's tabs, the highlight and the overflow button in a bar `width` wide.
        void layoutTabs(Entity group, float width);
        // The tab's selected state and its style class: "<id>.tab.active" while shown, else "<id>.tab"; its close
        // button's "<id>.tab.close.active <id>.tab.active", else "<id>.tab.close <id>.tab" (dressed as its tab).
        void setTabActive(PanelData& data, bool active);
        void styleClose(PanelData& data);
        // Lists the group's folded tabs under its overflow button; a choice activates the panel.
        void openOverflow(Entity group);
        void relayout(Entity node);
        void layoutAll();
        Vec2 clampedSpace{}; // the space size the floating windows were last clamped to

        [[nodiscard]] std::string save() const;
        bool load(std::string_view json);

        Entity createTabs();
        Entity createSplit(bool vertical, float ratio);
        void insertTab(Entity group, InternedString name, u32 index);
        // Takes the panel out of its group (no-op while closed); an emptied
        // group leaves the layout.
        void removeTab(InternedString name);
        void park(PanelData& data);
        void removeNode(Entity node);
        void replaceNode(Entity old, Entity replacement);
        void destroyNode(Entity node);
        Entity splitBeside(Entity existing, Entity added, UiDockZone zone, float share);
        void collectNodes(Entity node, std::vector<Entity>& out) const;

        // The tab's slot on the bar: the tab and its close button (a square of the tab height).
        [[nodiscard]] float tabWidthOf(const PanelData& data) const;
        [[nodiscard]] UiRect localRect(Entity element) const; // last resolve, dock-space pixels
        [[nodiscard]] Target targetAt(Vec2 pointer, u64 pointerWindow) const;
        void dragTab(Vec2 pointer, u64 pointerWindow);
        // A release, or the router ending the press because a modal opened over it (`element` is blocked: the drag is
        // cancelled, nothing drops).
        void dropTab(Vec2 pointer, Entity element, u64 pointerWindow);
        // The drag ends without a drop (Escape, a lost capture): preview and float target hide.
        void endDrag();
        void dragSplitter(Entity split, Vec2 pointer);
        // An arrow along the split moves its splitter one step (a Resize request); whether the key was the splitter's.
        bool nudgeSplitter(Entity split, u32 scancode, u8 modifiers);
        // Names the split's splitter after the first panels of its two sides.
        void describeSplitter(Entity split, Entity first, Entity second);
        void dragWindow(Entity group, Vec2 delta, int x, int y);

        UiScope* scope = nullptr;
        InternedString id{};
        UiDockDesc desc;
        Panel root;
        Panel preview;
        Button floatTarget; // shown while a tab drags: dropping on it floats the panel
        PopupList overflowList; // one list, opened under whichever group's overflow button asked
        std::vector<InternedString> overflowPanels; // its items, by index
        std::unordered_map<InternedString, PanelData> panels;
        std::vector<InternedString> order; // registration order
        std::unordered_map<u64, NodeParts> parts; // by node entity id
        InternedString dragging{};
        InternedString lastPressed{}; // the tab of the last press and when (seconds), for a second press
        double lastPressedAt = 0.0;
        std::function<void(const UiDockOp&)> onRequest;
        std::vector<Entity> detached; // the floating windows detached into OS windows
        std::optional<UiRect> detachPreview; // while a tab drag would detach: its window's rect, surface units
    };
} // namespace spite
