#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "base/Assert.hpp"
#include "ecs/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiScope.hpp"
#include "ui/UiTextMeasurer.hpp"
#include "UiDockState.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;

        constexpr float TAB_PADDING = 10.0f; // each side of a measured title
        constexpr float HIGHLIGHT = 2.0f; // active-tab strip height
        constexpr float GRIP = 6.0f;
        constexpr float WINDOW_DRAG_THRESHOLD = 8.0f;
        constexpr std::array<std::pair<int, int>, 8> RESIZE_EDGES{
            {{1, 1}, {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}}};
        constexpr std::array<std::string_view, 8> EDGE_IDS{".tabs.grip", ".resize.nw", ".resize.n",  ".resize.ne",
                                                           ".resize.w",  ".resize.e",  ".resize.sw", ".resize.s"};
        constexpr float OVERFLOW_WIDTH = 32.0f; // the button listing the tabs a bar has no room for
        constexpr float EDGE_ZONE = 0.25f; // of a group: docks beside it; between, into it
        constexpr float SPACE_EDGE = 24.0f; // pixels along the space's border: docks at its edge
        constexpr float SPACE_SHARE = 0.25f; // a panel docked at the space's edge
        constexpr i32 PREVIEW_Z = 1000;
        const Vec2 FLOAT_SIZE{320.0f, 240.0f};

        InternedString childId(const InternedString id, const std::string_view suffix)
        {
            return InternedString(std::string(id.toView()) + std::string(suffix));
        }

        UiRectDesc topLeft()
        {
            UiRectDesc desc;
            desc.pivot = Vec2{0.0f, 0.0f};
            return desc;
        }

        bool isFinite(const Vec2 value) { return std::isfinite(value.x) && std::isfinite(value.y); }

        void setVisible(World& world, const Entity element, const bool visible)
        {
            if (world.getConstComponent<UiStateComponent>(element).visible != visible)
            {
                world.getComponent<UiStateComponent>(element).visible = visible;
            }
        }

        // Parsed layout: names stay names until the build resolves them.
        struct LayoutNode
        {
            bool split = false;
            bool vertical = false;
            float ratio = 0.5f;
            std::unique_ptr<LayoutNode> first;
            std::unique_ptr<LayoutNode> second;
            std::vector<InternedString> tabs;
            u32 active = 0;
        };

        struct LayoutWindow
        {
            Vec2 position{0.0f, 0.0f};
            Vec2 size{0.0f, 0.0f};
            std::unique_ptr<LayoutNode> node;
            bool detached = false;
        };

        bool readFloat(const Json& json, float& out)
        {
            if (!json.is_number())
            {
                return false;
            }
            const double value = json.get<double>();
            if (!(std::abs(value) <= std::numeric_limits<float>::max())) // also rejects NaN
            {
                return false;
            }
            out = static_cast<float>(value);
            return true;
        }

        bool readVec2(const Json& json, Vec2& out)
        {
            return json.is_array() && json.size() == 2 && readFloat(json[0], out.x) && readFloat(json[1], out.y);
        }

        std::unique_ptr<LayoutNode> parseNode(const Json& json, const u32 depth)
        {
            if (!json.is_object() || depth > 64)
            {
                return nullptr;
            }
            auto node = std::make_unique<LayoutNode>();
            if (const auto tabs = json.find("tabs"); tabs != json.end())
            {
                if (!tabs->is_array())
                {
                    return nullptr;
                }
                for (const Json& name : *tabs)
                {
                    if (!name.is_string())
                    {
                        return nullptr;
                    }
                    node->tabs.emplace_back(name.get<std::string>());
                }
                const auto active = json.find("active");
                if (active != json.end())
                {
                    if (!active->is_number_unsigned())
                    {
                        return nullptr;
                    }
                    node->active = active->get<u32>();
                }
                return node;
            }
            const auto split = json.find("split");
            const auto ratio = json.find("ratio");
            const auto first = json.find("first");
            const auto second = json.find("second");
            if (split == json.end() || !split->is_string() || ratio == json.end() || !readFloat(*ratio, node->ratio) ||
                first == json.end() || second == json.end())
            {
                return nullptr;
            }
            const std::string axis = split->get<std::string>();
            if (axis != "horizontal" && axis != "vertical")
            {
                return nullptr;
            }
            node->split = true;
            node->vertical = axis == "vertical";
            node->ratio = std::clamp(node->ratio, 0.0f, 1.0f);
            node->first = parseNode(*first, depth + 1);
            node->second = parseNode(*second, depth + 1);
            return node->first && node->second ? std::move(node) : nullptr;
        }
    } // namespace

    // -- UiDockState: queries -------------------------------------------------------

    UiDockComponent& UiDockState::dock() const { return scope->world().getComponent<UiDockComponent>(root.entity()); }

    const UiDockComponent& UiDockState::dockRead() const
    {
        return scope->world().getConstComponent<UiDockComponent>(root.entity());
    }

    UiDockNodeComponent& UiDockState::node(const Entity entity) const
    {
        return scope->world().getComponent<UiDockNodeComponent>(entity);
    }

    bool UiDockState::isNode(const Entity entity) const
    {
        return entity != Entity::undefined() && parts.contains(entity.id()); // this space's live nodes only
    }

    bool UiDockState::isResolved(const Entity element) const
    {
        return scope->hierarchy().paintIndexOf(element) != UiHierarchyManager::NO_INDEX;
    }

    bool UiDockState::holdsEdge(Entity entity, const int x, const int y) const
    {
        const UiHierarchyManager& hierarchy = scope->hierarchy();
        for (Entity parent = hierarchy.parentOf(entity); parent != root.entity() && parent != Entity::undefined();
             parent = hierarchy.parentOf(entity))
        {
            if (!isNode(parent))
            {
                return false;
            }
            const auto& split = node(parent);
            const int direction = split.vertical ? y : x;
            if (direction != 0 && (direction < 0 ? split.first : split.second) != entity)
                return false;
            entity = parent;
        }
        return isFloating(entity) && !isDetached(entity);
    }

    Entity UiDockState::splitBetween(const InternedString first, const InternedString second) const
    {
        const Entity firstGroup = groupOf(first);
        const Entity secondGroup = groupOf(second);
        if (firstGroup == Entity::undefined() || secondGroup == Entity::undefined() || firstGroup == secondGroup)
        {
            return Entity::undefined();
        }
        const UiHierarchyManager& hierarchy = scope->hierarchy();
        // The lowest split holding both: first's side must be its first child, second's its second.
        std::vector<std::pair<Entity, Entity>> firstPath; // (split, child on first's path)
        for (Entity child = firstGroup, parent = hierarchy.parentOf(child); isNode(parent);
             child = parent, parent = hierarchy.parentOf(parent))
        {
            firstPath.emplace_back(parent, child);
        }
        for (Entity child = secondGroup, parent = hierarchy.parentOf(child); isNode(parent);
             child = parent, parent = hierarchy.parentOf(parent))
        {
            const auto it = std::ranges::find(firstPath, parent, &std::pair<Entity, Entity>::first);
            if (it != firstPath.end())
            {
                const UiDockNodeComponent& split = node(parent);
                return split.first == it->second && split.second == child ? parent : Entity::undefined();
            }
        }
        return Entity::undefined();
    }

    InternedString UiDockState::leadingPanel(Entity entity) const
    {
        while (node(entity).kind == UiDockNodeKind::Split)
        {
            entity = node(entity).first;
        }
        return nameOf(node(entity).tabs.front());
    }

    Vec2 UiDockState::minSizeOf(const Entity entity) const
    {
        const UiDockNodeComponent& component = node(entity);
        if (component.kind == UiDockNodeKind::Split)
        {
            const Vec2 first = minSizeOf(component.first);
            const Vec2 second = minSizeOf(component.second);
            return component.vertical ? Vec2{std::max(first.x, second.x), first.y + desc.splitterSize + second.y}
                                      : Vec2{first.x + desc.splitterSize + second.x, std::max(first.y, second.y)};
        }
        Vec2 content{desc.minPaneSize, desc.minPaneSize};
        float widestTab = 0.0f;
        for (const Entity host : component.tabs)
        {
            const PanelData& data = panels.at(nameOf(host));
            content = Vec2{std::max(content.x, data.minSize.x), std::max(content.y, data.minSize.y)};
            // a title, not its close button: the button is a convenience a narrow group may clip, never what sizes the
            // splits around it
            if (desc.showChrome)
            {
                widestTab = std::max(widestTab, tabWidthOf(data) - (desc.closeButtons ? desc.tabHeight : 0.0f));
            }
        }
        // the active tab must stay on the bar beside the button that lists the others
        const float bar = desc.showChrome && component.tabs.size() > 1 ? widestTab + OVERFLOW_WIDTH : widestTab;
        return Vec2{std::max(content.x, bar), (desc.showChrome ? desc.tabHeight : 0.0f) + content.y};
    }

    float UiDockState::clampRatio(const Entity split, const float ratio, const Vec2 size) const
    {
        const UiDockNodeComponent& component = node(split);
        const float available = (component.vertical ? size.y : size.x) - desc.splitterSize;
        if (available <= 0.0f)
        {
            return std::clamp(ratio, 0.0f, 1.0f);
        }
        const Vec2 first = minSizeOf(component.first);
        const Vec2 second = minSizeOf(component.second);
        const float firstMin = component.vertical ? first.y : first.x;
        const float secondMin = component.vertical ? second.y : second.x;
        float low = firstMin / available;
        float high = 1.0f - secondMin / available;
        if (low > high)
        {
            // no room for both minimums: the plain floor of a small pane, so a cramped window lays out as before
            low = std::min(0.5f, desc.minPaneSize / available);
            high = 1.0f - low;
        }
        return std::clamp(ratio, low, high);
    }

    bool UiDockState::holds(const Entity ancestor, Entity entity) const
    {
        const UiHierarchyManager& hierarchy = scope->hierarchy();
        for (; entity != Entity::undefined() && entity != root.entity(); entity = hierarchy.parentOf(entity))
        {
            if (entity == ancestor)
            {
                return true;
            }
        }
        return false;
    }

    bool UiDockState::pressedTwice(const InternedString name)
    {
        const double now = desc.clock
            ? desc.clock()
            : std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool twice = lastPressed == name && now - lastPressedAt <= desc.doubleClickSeconds;
        lastPressed = twice ? InternedString{} : name;
        lastPressedAt = now;
        return twice;
    }

    void UiDockState::requestMaximizeToggle(const InternedString name)
    {
        const Entity group = groupOf(name);
        if (group == Entity::undefined() || isFloating(windowOf(group))) // a floating window is its own window
        {
            return;
        }
        request(UiDockOp{.kind = dock().maximized == group ? UiDockOpKind::Restore : UiDockOpKind::Maximize,
                         .panel = name});
    }

    UiDockState::Home UiDockState::homeOf(const InternedString name) const
    {
        const Entity group = groupOf(name);
        const UiDockNodeComponent& component = node(group);
        Home home;
        home.placed = true;
        if (component.tabs.size() > 1)
        {
            const Entity self = panel(name)->host.entity();
            const auto at = std::ranges::find(component.tabs, self);
            home.index = static_cast<u32>(at - component.tabs.begin());
            home.target =
                nameOf(*std::ranges::find_if(component.tabs, [self](const Entity host) { return host != self; }));
            return home;
        }
        const Entity parent = scope->hierarchy().parentOf(group);
        if (isNode(parent)) // else alone in its window: the space's centre once it is empty
        {
            const UiDockNodeComponent& split = node(parent);
            const bool first = split.first == group;
            if (parent != dock().main) // the main split's sibling is the rest of the space: its edge, no target
            {
                home.target = leadingPanel(first ? split.second : split.first);
            }
            home.zone = split.vertical ? (first ? UiDockZone::Top : UiDockZone::Bottom)
                                       : (first ? UiDockZone::Left : UiDockZone::Right);
            home.share = first ? split.ratio : 1.0f - split.ratio;
        }
        return home;
    }

    std::optional<UiDockOp> UiDockState::reopenOpFor(const InternedString name) const
    {
        const PanelData* data = panel(name);
        if (!data || !data->home.placed || groupOf(name) != Entity::undefined())
        {
            return std::nullopt;
        }
        const Home& home = data->home;
        UiDockOp op{.kind = UiDockOpKind::Float, .panel = name};
        if (home.floating)
        {
            op.position = home.position;
            op.size = home.size;
            return op;
        }
        op.kind = UiDockOpKind::Dock;
        op.target = home.target;
        op.zone = home.zone;
        op.ratio = home.share;
        op.index = home.index;
        const bool targetKept = home.target.isValid() && groupOf(home.target) != Entity::undefined();
        const bool edgeKept =
            !home.target.isValid() && (home.zone != UiDockZone::Center || dock().main == Entity::undefined());
        return targetKept || edgeKept ? std::optional(op) : std::nullopt;
    }

    UiDockOp UiDockState::floatOpFor(const InternedString name, const Vec2 at) const
    {
        const Vec2 spaceSize = scope->hierarchy().layoutSizeOf(root.entity());
        const Vec2 hostSize = scope->world().getConstComponent<UiRectComponent>(panels.at(name).host.entity()).size;
        const bool shown = groupOf(name) != Entity::undefined() && hostSize.x > 0.0f && hostSize.y > 0.0f;
        UiDockOp op{.kind = UiDockOpKind::Float, .panel = name};
        op.size = shown ? hostSize + Vec2{0.0f, desc.tabHeight} : FLOAT_SIZE;
        op.position = Vec2{std::clamp(at.x, 0.0f, std::max(0.0f, spaceSize.x - op.size.x)),
                           std::clamp(at.y, 0.0f, std::max(0.0f, spaceSize.y - op.size.y))};
        return op;
    }

    UiRect UiDockState::floatTargetRect(const Vec2 spaceSize) const
    {
        // as wide as its label (UI scale, text scale and locale included) plus a tab's padding, one tab bar tall at
        // least; centred, half a tab bar above the space's bottom
        const Entity element = floatTarget.entity();
        const UiTextMeasurer* measurer = scope->hierarchy().textMeasurer();
        const Vec2 text = measurer
            ? measurer->measureText(element, scope->world().getConstComponent<UiLabelComponent>(element).text)
            : Vec2{desc.tabWidth, desc.tabHeight};
        const Vec2 size{std::min(std::ceil(text.x) + 4.0f * TAB_PADDING, spaceSize.x),
                        std::min(std::max(desc.tabHeight, std::ceil(text.y) + TAB_PADDING), spaceSize.y)};
        return UiRect{Vec2{(spaceSize.x - size.x) * 0.5f, std::max(0.0f, spaceSize.y - size.y - desc.tabHeight * 0.5f)},
                      size};
    }

    void UiDockState::requestFloatToggle(const InternedString name)
    {
        const Entity group = groupOf(name);
        if (group == Entity::undefined())
        {
            return;
        }
        if (!isFloating(windowOf(group)))
        {
            request(floatOpFor(
                name, (isResolved(group) ? localRect(group).position : Vec2{}) + Vec2{desc.tabHeight, desc.tabHeight}));
            return;
        }
        const Home& home = panel(name)->home;
        const Entity homeGroup = groupOf(home.target);
        const bool targetKept =
            homeGroup != Entity::undefined() && home.target != name && !isFloating(windowOf(homeGroup));
        const bool edgeKept = home.placed && !home.target.isValid() &&
            (home.zone != UiDockZone::Center || dock().main == Entity::undefined());
        UiDockOp op{.kind = UiDockOpKind::Dock, .panel = name};
        if (targetKept || edgeKept)
        {
            op.target = home.target;
            op.zone = home.zone;
            op.ratio = home.share;
            op.index = home.index;
        }
        else
        {
            op.zone = dock().main == Entity::undefined() ? UiDockZone::Center : UiDockZone::Bottom;
            op.ratio = SPACE_SHARE;
        }
        request(op);
    }

    Vec2 UiDockState::clampWindowSize(const Vec2 size) const
    {
        const float minimum = desc.tabHeight + desc.minPaneSize;
        return Vec2{std::max(minimum, size.x), std::max(minimum, size.y)};
    }

    void UiDockState::clampWindow(Vec2& position, Vec2& size) const
    {
        const Vec2 space = scope->hierarchy().layoutSizeOf(root.entity());
        size = clampWindowSize(size);
        if (!(space.x > 0.0f) || !(space.y > 0.0f))
        {
            return;
        }
        size = clampWindowSize(Vec2{std::min(size.x, space.x), std::min(size.y, space.y)});
        const float left = desc.minPaneSize - size.x;
        position.x = std::clamp(position.x, left, std::max(left, space.x - desc.minPaneSize));
        position.y = std::clamp(position.y, 0.0f, std::max(0.0f, space.y - desc.tabHeight));
    }

    UiRect UiDockState::floatingRectOf(const InternedString name) const
    {
        const Entity group = groupOf(name);
        const Entity window = group == Entity::undefined() ? Entity::undefined() : windowOf(group);
        if (window == Entity::undefined() || !isFloating(window))
        {
            return UiRect{};
        }
        const UiRectComponent& rect = scope->world().getConstComponent<UiRectComponent>(window);
        return UiRect{rect.offset, rect.size};
    }

    UiDockState::PanelData* UiDockState::panel(const InternedString name)
    {
        const auto it = panels.find(name);
        return it == panels.end() ? nullptr : &it->second;
    }

    const UiDockState::PanelData* UiDockState::panel(const InternedString name) const
    {
        const auto it = panels.find(name);
        return it == panels.end() ? nullptr : &it->second;
    }

    InternedString UiDockState::nameOf(const Entity host) const
    {
        return scope->world().getConstComponent<UiDockPanelComponent>(host).name;
    }

    Entity UiDockState::groupOf(const InternedString name) const
    {
        const PanelData* data = panel(name);
        if (!data)
        {
            return Entity::undefined();
        }
        const Entity parent = scope->hierarchy().parentOf(data->host.entity());
        return isNode(parent) ? parent : Entity::undefined();
    }

    Entity UiDockState::windowOf(Entity entity) const
    {
        const UiHierarchyManager& hierarchy = scope->hierarchy();
        for (Entity parent = hierarchy.parentOf(entity); parent != root.entity() && parent != Entity::undefined();
             parent = hierarchy.parentOf(entity))
        {
            entity = parent;
        }
        return entity;
    }

    bool UiDockState::isDetached(const Entity window) const { return std::ranges::contains(detached, window); }

    void UiDockState::placeWindow(const Entity window, const Vec2 position, const Vec2 size, const bool detach)
    {
        if (detach != isDetached(window))
        {
            if (detach)
            {
                scope->hierarchy().detach(window);
                detached.push_back(window);
            }
            else
            {
                scope->hierarchy().attach(window, root.entity());
                std::erase(detached, window);
            }
        }
        auto& rect = scope->world().getComponent<UiRectComponent>(window);
        rect.offset = position;
        rect.size = clampWindowSize(size);
        if (!detach)
        {
            clampWindow(rect.offset, rect.size);
        }
    }

    bool UiDockState::isFloating(const Entity window) const
    {
        const std::vector<Entity>& floating = dock().floating;
        return std::ranges::find(floating, window) != floating.end();
    }

    float UiDockState::tabWidthOf(const PanelData& data) const
    {
        const UiTextMeasurer* measurer = scope->hierarchy().textMeasurer();
        const float tab = measurer
            ? std::ceil(measurer->measureText(data.tab.entity(), data.title).x) + 2.0f * TAB_PADDING
            : desc.tabWidth;
        return tab + (desc.closeButtons ? desc.tabHeight : 0.0f);
    }

    UiRect UiDockState::localRect(const Entity element) const
    {
        const UiHierarchyManager& hierarchy = scope->hierarchy();
        const float scale = hierarchy.layoutScale();
        const UiRect rect = hierarchy.absoluteRectOf(element);
        const Vec2 origin = hierarchy.absoluteRectOf(root.entity()).position;
        return UiRect{(rect.position - origin) * (1.0f / scale), rect.size * (1.0f / scale)};
    }

    // -- UiDockState: structure -----------------------------------------------------

    void UiDockState::setPanelTag(const InternedString name, const Widget& tag)
    {
        PanelData* data = panel(name);
        if (!data || !tag.isValid())
        {
            return;
        }
        if (!data->tagHost.isValid())
        {
            UiRectDesc hostDesc = topLeft();
            hostDesc.layout = UiLayoutComponent{
                .direction = UiLayoutDirection::Row, .justify = UiLayoutJustify::End, .align = UiLayoutAlign::Center};
            data->tagHost = scope->createPanel(childId(id, ".tag." + std::string(name.toView())), hostDesc);
            data->tagHost.bindLayoutToken(UiLayoutField::Gap, InternedString("space.xs"));
            data->tagHost.bindLayoutToken(UiLayoutField::PaddingLeft, InternedString("space.xs"));
            data->tagHost.bindLayoutToken(UiLayoutField::PaddingRight, InternedString("space.sm"));
            auto& state = scope->world().getComponent<UiStateComponent>(data->tagHost.entity());
            state.clipsChildren = true;
            state.interactable = false; // the bar under it still drags a floating window
            const Entity group = groupOf(name);
            scope->hierarchy().attach(data->tagHost.entity(),
                                      group == Entity::undefined() ? root.entity() : parts.at(group.id()).bar.entity());
            state.visible = false;
        }
        Widget placed = tag;
        placed.setParent(data->tagHost);
        if (const Entity group = groupOf(name); group != Entity::undefined())
        {
            relayout(group);
        }
    }

    void UiDockState::setPanelTitle(const InternedString name, const std::string_view title)
    {
        const auto it = panels.find(name);
        if (it == panels.end() || it->second.title == title)
        {
            return;
        }
        it->second.title = std::string(title);
        it->second.tab.setText(title);
        layoutAll();
    }

    void UiDockState::setPanelMinSize(const InternedString name, const Vec2 size)
    {
        PanelData* data = panel(name);
        if (!data || !isFinite(size) || data->minSize == size)
        {
            return;
        }
        data->minSize = size;
        layoutAll();
    }

    void UiDockState::addPanel(const InternedString name, const std::string_view title, const Widget& content)
    {
        SASSERTM(name.isValid() && !panels.contains(name), "DockSpace::addPanel: needs a new, valid panel name.");
        if (!name.isValid() || panels.contains(name))
        {
            return;
        }
        UiScope& owner = *scope;
        const std::string suffix(name.toView());
        PanelData data;
        data.title = std::string(title);
        data.host = owner.createPanel(childId(id, ".panel." + suffix), topLeft());
        auto& hostState = owner.world().getComponent<UiStateComponent>(data.host.entity());
        hostState.clipsChildren = true;
        hostState.interactable = false;
        hostState.focusScope = true; // Tab stays inside a panel
        hostState.landmark = true; // and F6 reaches it while it is shown and holds a stop
        UiDockPanelComponent component;
        component.name = name;
        owner.world().addComponent<UiDockPanelComponent>(data.host.entity(), component);
        UiRectDesc tabDesc = topLeft();
        tabDesc.size = Vec2{desc.tabWidth, desc.tabHeight};
        data.tab = owner.createButton(childId(id, ".tab." + suffix), title, tabDesc);
        data.tab.setStyleClass(childId(id, ".tab")); // one class for every tab: a theme sets their face
        if (!desc.showChrome)
        {
            auto& tabState = owner.world().getComponent<UiStateComponent>(data.tab.entity());
            tabState.interactable = false;
            tabState.tabStop = false;
        }
        data.tab.setAccessibleRole(UiRole::Tab);
        data.tab.setAccessibleSelected(false);
        data.close = owner.createButton(childId(id, ".tab.close." + suffix), "", tabDesc);
        data.close.setIcon("x");
        data.close.setAccessibleNameKey(InternedString("engine.ui.dock.close"));
        owner.world().getComponent<UiStateComponent>(data.close.entity()).tabStop = false; // keys: the tab's own
        data.close.onClick([this, name] { request(UiDockOp{.kind = UiDockOpKind::Close, .panel = name}); });
        // an inactive tab's close button shows while the pointer is over the tab (or over the button itself)
        for (const auto [kind, over] :
             {std::pair{UiEventKind::HoverEnter, true}, std::pair{UiEventKind::HoverLeave, false}})
        {
            owner.setReaction(data.tab.entity(), kind,
                              [this, name, over](const void*)
                              {
                                  if (const auto it = panels.find(name);
                                      it != panels.end() && it->second.hovered != over)
                                  {
                                      it->second.hovered = over;
                                      styleClose(it->second);
                                  }
                              });
        }
        setTabActive(data, false);
        data.content = content;
        if (content.isValid())
        {
            Widget placed = content;
            placed.setParent(data.host);
            placed.setAnchor(Vec2{0.0f, 0.0f});
            placed.setPivot(Vec2{0.0f, 0.0f});
            placed.setOffset(Vec2{0.0f, 0.0f});
        }

        const Entity tab = data.tab.entity();
        owner.world().getComponent<UiStateComponent>(tab).dragThreshold = WINDOW_DRAG_THRESHOLD;
        owner.setReaction(tab, UiEventKind::PointerDown,
                          [this, name](const void* payload)
                          {
                              if (static_cast<const UiPointerDownEvent*>(payload)->button == 0)
                              {
                                  request(UiDockOp{.kind = UiDockOpKind::Activate, .panel = name});
                                  if (pressedTwice(name))
                                  {
                                      requestMaximizeToggle(name);
                                  }
                              }
                          });
        // Enter on a focused tab also enters its panel: focus to the panel's
        // first tab stop (one inside a nested landmark counts), or (while the
        // activation has yet to lay it out) the panel itself, which the next
        // landmark intent leaves for a stop. Space only activates; Shift+Space
        // toggles the group's maximize and Shift+Enter floats the panel or docks it back. Navigation's keys, so
        // off whenever navigation is; Escape cancels this tab's drag whatever navigation does.
        owner.setKeyReaction(tab,
                             [this, name](const UiKeyDownEvent& event)
                             {
                                 constexpr u32 ESCAPE = 41;
                                 constexpr u32 RETURN = 40;
                                 constexpr u32 SPACE = 44;
                                 constexpr u32 KP_ENTER = 88;
                                 constexpr u8 CHORDS = UiKeyModifier::CTRL | UiKeyModifier::ALT | UiKeyModifier::SUPER;
                                 UiRouter& router = scope->router();
                                 if (event.scancode == ESCAPE && dragging == name)
                                 {
                                     endDrag();
                                     return true;
                                 }
                                 if ((event.modifiers & CHORDS) != 0 || !router.navigationEnabled())
                                 {
                                     return false;
                                 }
                                 if (event.scancode == SPACE && event.modifiers == UiKeyModifier::SHIFT)
                                 {
                                     if (!event.repeat)
                                     {
                                         requestMaximizeToggle(name);
                                     }
                                     return true;
                                 }
                                 if (event.scancode != RETURN && event.scancode != KP_ENTER)
                                 {
                                     return false;
                                 }
                                 if (event.modifiers == UiKeyModifier::SHIFT)
                                 {
                                     if (!event.repeat)
                                     {
                                         requestFloatToggle(name);
                                     }
                                     return true;
                                 }
                                 if (!event.repeat)
                                 {
                                     request(UiDockOp{.kind = UiDockOpKind::Activate, .panel = name});
                                     router.focusInto(panels.at(name).host.entity());
                                 }
                                 return true;
                             });
        owner.setReaction(tab, UiEventKind::DragStart,
                          [this, name](const void* payload)
                          {
                              dragging = name;
                              dragTab(static_cast<const UiDragStartEvent*>(payload)->position,
                                      static_cast<const UiDragStartEvent*>(payload)->pointerWindow);
                          });
        owner.setReaction(tab, UiEventKind::DragMove,
                          [this](const void* payload)
                          {
                              dragTab(static_cast<const UiDragMoveEvent*>(payload)->position,
                                      static_cast<const UiDragMoveEvent*>(payload)->pointerWindow);
                          });
        owner.setReaction(tab, UiEventKind::DragEnd,
                          [this](const void* payload)
                          {
                              const auto* event = static_cast<const UiDragEndEvent*>(payload);
                              dropTab(event->position, event->element, event->pointerWindow);
                          });

        order.push_back(name);
        park(panels.emplace(name, std::move(data)).first->second);
    }

    void UiDockState::park(PanelData& data)
    {
        UiHierarchyManager& hierarchy = scope->hierarchy();
        World& world = scope->world();
        hierarchy.attach(data.host.entity(), root.entity());
        hierarchy.attach(data.tab.entity(), root.entity());
        hierarchy.attach(data.close.entity(), root.entity());
        world.getComponent<UiStateComponent>(data.host.entity()).visible = false;
        world.getComponent<UiStateComponent>(data.tab.entity()).visible = false;
        world.getComponent<UiStateComponent>(data.close.entity()).visible = false;
        if (data.tagHost.isValid())
        {
            hierarchy.attach(data.tagHost.entity(), root.entity());
            world.getComponent<UiStateComponent>(data.tagHost.entity()).visible = false;
        }
    }

    Entity UiDockState::createTabs()
    {
        UiScope& owner = *scope;
        const Panel group = owner.createPanel(childId(id, ".tabs"), topLeft());
        owner.world().addComponent<UiDockNodeComponent>(group.entity(), UiDockNodeComponent{});
        owner.world().getComponent<UiStateComponent>(group.entity()).clipsChildren = true;
        owner.world().getComponent<UiStateComponent>(group.entity()).interactable = false;
        NodeParts nodeParts;
        nodeParts.bar = owner.createPanel(childId(id, ".tabs.bar"), topLeft());
        owner.world().getComponent<UiStateComponent>(nodeParts.bar.entity()).dragThreshold = WINDOW_DRAG_THRESHOLD;
        UiRectDesc highlightDesc = topLeft();
        highlightDesc.zOffset = 1; // over the tabs
        nodeParts.highlight = owner.createPanel(childId(id, ".tabs.active"), highlightDesc);
        UiRectDesc gripDesc = topLeft();
        gripDesc.zOffset = 1; // over the panel
        nodeParts.grip = owner.createPanel(childId(id, ".tabs.grip"), gripDesc);
        UiHierarchyManager& hierarchy = owner.hierarchy();
        hierarchy.attach(nodeParts.bar.entity(), group.entity());
        hierarchy.attach(nodeParts.highlight.entity(), nodeParts.bar.entity());
        hierarchy.attach(nodeParts.grip.entity(), group.entity());
        for (sizet i = 0; i < nodeParts.edges.size(); ++i)
        {
            nodeParts.edges[i] = owner.createPanel(childId(id, EDGE_IDS[i + 1]), gripDesc);
            hierarchy.attach(nodeParts.edges[i].entity(), group.entity());
        }
        for (sizet i = 0; i < RESIZE_EDGES.size(); ++i)
        {
            const Panel& edge = i == 0 ? nodeParts.grip : nodeParts.edges[i - 1];
            owner.world().addComponent<UiVisualComponent>(edge.entity(), UiVisualComponent{.opacity = 0.0f});
            const auto [x, y] = RESIZE_EDGES[i];
            const Entity entity = group.entity();
            const auto resize = [this, entity, x, y](Vec2 delta) { dragWindow(entity, delta, x, y); };
            owner.setReaction(edge.entity(), UiEventKind::DragStart, [resize](const void* payload)
                              { resize(static_cast<const UiDragStartEvent*>(payload)->delta); });
            owner.setReaction(edge.entity(), UiEventKind::DragMove, [resize](const void* payload)
                              { resize(static_cast<const UiDragMoveEvent*>(payload)->delta); });
            if (!desc.showChrome)
                owner.world().getComponent<UiStateComponent>(edge.entity()).interactable = false;
        }
        owner.world().getComponent<UiStateComponent>(nodeParts.highlight.entity()).interactable = false;
        owner.world().getComponent<UiStateComponent>(nodeParts.grip.entity()).visible = false;
        UiRectDesc frameDesc = topLeft();
        frameDesc.zOffset = 1; // over the bar and the panels, taking no input
        nodeParts.frame = owner.createPanel(childId(id, ".tabs.frame"), frameDesc);
        hierarchy.attach(nodeParts.frame.entity(), group.entity());
        owner.world().getComponent<UiStateComponent>(nodeParts.frame.entity()).interactable = false;
        nodeParts.more = owner.createButton(childId(id, ".tabs.more"), "", topLeft());
        nodeParts.more.setStyleClass(childId(id, ".tab")); // dressed as a tab
        nodeParts.more.setIcon("caret-double-right");
        nodeParts.more.setAccessibleNameKey(InternedString("engine.ui.dock.more"));
        hierarchy.attach(nodeParts.more.entity(), nodeParts.bar.entity());
        owner.world().getComponent<UiStateComponent>(nodeParts.more.entity()).visible = false;
        if (!desc.showChrome)
        {
            owner.world().getComponent<UiStateComponent>(nodeParts.bar.entity()).interactable = false;
            owner.world().getComponent<UiStateComponent>(nodeParts.grip.entity()).interactable = false;
            auto& moreState = owner.world().getComponent<UiStateComponent>(nodeParts.more.entity());
            moreState.interactable = false;
            moreState.tabStop = false;
        }

        // DragStart carries the first movement, so both kinds move.
        const Entity entity = group.entity();
        const auto move = [this, entity](const Vec2 delta) { dragWindow(entity, delta, 0, 0); };
        owner.setReaction(nodeParts.bar.entity(), UiEventKind::DragStart,
                          [move](const void* payload) { move(static_cast<const UiDragStartEvent*>(payload)->delta); });
        owner.setReaction(nodeParts.bar.entity(), UiEventKind::DragMove,
                          [move](const void* payload) { move(static_cast<const UiDragMoveEvent*>(payload)->delta); });
        nodeParts.more.onPress(
            [this, entity](const u8 button, Vec2)
            {
                if (button == 0)
                {
                    openOverflow(entity);
                }
            });
        nodeParts.more.onKeyDown(
            [this, entity](const u32 scancode, const u8 modifiers, const bool repeat)
            {
                constexpr u32 RETURN = 40;
                constexpr u32 SPACE = 44;
                constexpr u32 KP_ENTER = 88;
                constexpr u8 CHORDS = UiKeyModifier::CTRL | UiKeyModifier::ALT | UiKeyModifier::SUPER;
                if ((scancode != RETURN && scancode != KP_ENTER && scancode != SPACE) || (modifiers & CHORDS) != 0 ||
                    !scope->router().navigationEnabled() || repeat)
                {
                    return false;
                }
                openOverflow(entity);
                return true;
            });
        parts.emplace(entity.id(), nodeParts);
        return entity;
    }

    Entity UiDockState::createSplit(const bool vertical, const float ratio)
    {
        UiScope& owner = *scope;
        const Panel split = owner.createPanel(childId(id, ".split"), topLeft());
        UiDockNodeComponent component;
        component.kind = UiDockNodeKind::Split;
        component.vertical = vertical;
        component.ratio = std::clamp(ratio, 0.0f, 1.0f);
        owner.world().addComponent<UiDockNodeComponent>(split.entity(), component);
        owner.world().getComponent<UiStateComponent>(split.entity()).interactable = false;
        NodeParts nodeParts;
        UiRectDesc barDesc = topLeft();
        barDesc.zOffset = 1; // paints after its two sides: Tab reaches the tabs of both before the splitter
        nodeParts.bar = owner.createPanel(childId(id, ".splitter"), barDesc);
        owner.hierarchy().attach(nodeParts.bar.entity(), split.entity());
        const Entity entity = split.entity();
        // a keyboard stop: the arrows along the split move it (describeSplitter names it once it is laid out)
        nodeParts.bar.setStyleClass(childId(id, ".splitter"));
        nodeParts.bar.setAccessibleRole(UiRole::Separator);
        owner.world().getComponent<UiStateComponent>(nodeParts.bar.entity()).tabStop = true;
        owner.setKeyReaction(nodeParts.bar.entity(), [this, entity](const UiKeyDownEvent& event)
                             { return nudgeSplitter(entity, event.scancode, event.modifiers); });
        owner.setReaction(nodeParts.bar.entity(), UiEventKind::DragStart, [this, entity](const void* payload)
                          { dragSplitter(entity, static_cast<const UiDragStartEvent*>(payload)->position); });
        owner.setReaction(nodeParts.bar.entity(), UiEventKind::DragMove, [this, entity](const void* payload)
                          { dragSplitter(entity, static_cast<const UiDragMoveEvent*>(payload)->position); });
        parts.emplace(entity.id(), nodeParts);
        return entity;
    }

    void UiDockState::insertTab(const Entity group, const InternedString name, const u32 index)
    {
        PanelData& data = *panel(name);
        std::vector<Entity>& tabs = node(group).tabs;
        tabs.insert(tabs.begin() + std::min<sizet>(index, tabs.size()), data.host.entity());
        UiHierarchyManager& hierarchy = scope->hierarchy();
        hierarchy.attach(data.host.entity(), group);
        hierarchy.attach(data.tab.entity(), parts.at(group.id()).bar.entity());
        hierarchy.attach(data.close.entity(), parts.at(group.id()).bar.entity()); // over its tab's slot
        scope->world().getComponent<UiStateComponent>(data.tab.entity()).visible = desc.showChrome;
        scope->world().getComponent<UiStateComponent>(data.close.entity()).visible =
            desc.showChrome && desc.closeButtons;
        for (const Entity host : tabs)
        {
            // the tags come after every tab, so Tab reaches the tabs before a tag's controls, as they sit on the bar
            if (const Panel& tagHost = panels.at(nameOf(host)).tagHost; tagHost.isValid())
            {
                hierarchy.attach(tagHost.entity(), parts.at(group.id()).bar.entity());
            }
        }
    }

    void UiDockState::removeTab(const InternedString name)
    {
        if (dragging == name)
        {
            endDrag(); // the panel is leaving mid-drag: nothing to drop
        }
        const Entity group = groupOf(name);
        if (group == Entity::undefined())
        {
            return;
        }
        PanelData& data = *panel(name);
        UiDockNodeComponent& component = node(group);
        const auto it = std::ranges::find(component.tabs, data.host.entity());
        const auto index = static_cast<u32>(it - component.tabs.begin());
        component.tabs.erase(it);
        if (component.active > index || component.active >= component.tabs.size())
        {
            component.active = component.active > 0 ? component.active - 1 : 0;
        }
        const bool emptied = component.tabs.empty();
        park(data);
        if (emptied)
        {
            removeNode(group);
        }
    }

    void UiDockState::removeNode(const Entity entity)
    {
        if (dock().maximized == entity)
        {
            dock().maximized = Entity::undefined();
        }
        const Entity parent = scope->hierarchy().parentOf(entity);
        if (parent == root.entity() || isDetached(entity)) // a detached window is a root surface
        {
            UiDockComponent& space = dock();
            if (space.main == entity)
            {
                space.main = Entity::undefined();
            }
            std::erase(space.floating, entity);
            std::erase(detached, entity);
        }
        else if (isNode(parent))
        {
            const UiDockNodeComponent& split = node(parent);
            const Entity sibling = split.first == entity ? split.second : split.first;
            replaceNode(parent, sibling);
            destroyNode(parent);
        }
        destroyNode(entity);
    }

    void UiDockState::replaceNode(const Entity old, const Entity replacement)
    {
        UiHierarchyManager& hierarchy = scope->hierarchy();
        World& world = scope->world();
        const Entity parent = hierarchy.parentOf(old);
        if (parent == root.entity() || isDetached(old))
        {
            UiDockComponent& space = dock();
            if (space.main == old)
            {
                space.main = replacement;
            }
            else
            {
                std::ranges::replace(space.floating, old, replacement);
                std::ranges::replace(detached, old, replacement);
                const UiRectComponent rect = world.getConstComponent<UiRectComponent>(old);
                auto& placed = world.getComponent<UiRectComponent>(replacement);
                placed.offset = rect.offset;
                placed.size = rect.size;
            }
        }
        else if (isNode(parent))
        {
            UiDockNodeComponent& split = node(parent);
            (split.first == old ? split.first : split.second) = replacement;
        }
        if (isDetached(replacement))
        {
            hierarchy.detach(replacement); // the window's new root surface
        }
        else
        {
            hierarchy.attach(replacement, parent);
        }
    }

    void UiDockState::destroyNode(const Entity entity)
    {
        World& world = scope->world();
        world.getComponent<UiStateComponent>(entity).visible = false;
        scope->hierarchy().detach(entity); // dies at the scope's commit; hidden until then
        if (const auto it = parts.find(entity.id()); it != parts.end())
        {
            for (const Panel& part : {it->second.bar, it->second.highlight, it->second.grip, it->second.frame})
            {
                if (part.entity() != Entity::undefined())
                {
                    scope->destroy(part);
                }
            }
            for (const Panel& edge : it->second.edges)
                if (edge.isValid())
                    scope->destroy(edge);
            if (it->second.more.isValid())
            {
                scope->destroy(it->second.more);
            }
            parts.erase(it);
        }
        scope->destroy(Widget(scope, entity, scope->generation()));
    }

    Entity UiDockState::splitBeside(const Entity existing, const Entity added, const UiDockZone zone, const float share)
    {
        const bool vertical = zone == UiDockZone::Top || zone == UiDockZone::Bottom;
        const bool addedFirst = zone == UiDockZone::Left || zone == UiDockZone::Top;
        const float clamped = std::clamp(share, 0.0f, 1.0f);
        const Entity split = createSplit(vertical, addedFirst ? clamped : 1.0f - clamped);
        replaceNode(existing, split);
        UiDockNodeComponent& component = node(split);
        component.first = addedFirst ? added : existing;
        component.second = addedFirst ? existing : added;
        scope->hierarchy().attach(component.first, split);
        scope->hierarchy().attach(component.second, split);
        return split;
    }

    void UiDockState::collectNodes(const Entity entity, std::vector<Entity>& out) const
    {
        if (entity == Entity::undefined())
        {
            return;
        }
        out.push_back(entity);
        const UiDockNodeComponent& component = node(entity);
        if (component.kind == UiDockNodeKind::Split)
        {
            const Entity first = component.first;
            const Entity second = component.second;
            collectNodes(first, out);
            collectNodes(second, out);
        }
    }

    // -- UiDockState: layout ------------------------------------------------------

    void UiDockState::layout(const Entity entity, const Vec2 offset, const Vec2 size)
    {
        World& world = scope->world();
        const auto place = [&world](const Entity element, const Vec2 at, const Vec2 extent)
        {
            auto& rect = world.getComponent<UiRectComponent>(element);
            rect.anchor = Vec2{0.0f, 0.0f};
            rect.pivot = Vec2{0.0f, 0.0f};
            rect.offset = at;
            rect.size = extent;
        };
        place(entity, offset, size);
        // by value: resizing a panel's content may create elements (list pools)
        const UiDockNodeComponent component = node(entity);
        const NodeParts& nodeParts = parts.at(entity.id());
        if (component.kind == UiDockNodeKind::Split)
        {
            const Entity firstNode = component.first;
            const Entity secondNode = component.second;
            // a maximized group fills the split holding it; the other side and the splitter are hidden
            const Entity maximized = dock().maximized;
            const bool inFirst = maximized != Entity::undefined() && holds(firstNode, maximized);
            const bool inSecond = !inFirst && maximized != Entity::undefined() && holds(secondNode, maximized);
            setVisible(world, nodeParts.bar.entity(), !inFirst && !inSecond);
            setVisible(world, firstNode, !inSecond);
            setVisible(world, secondNode, !inFirst);
            if (inFirst || inSecond)
            {
                layout(inFirst ? firstNode : secondNode, Vec2{}, size);
                return;
            }
            const float along = component.vertical ? size.y : size.x;
            const float available = std::max(0.0f, along - desc.splitterSize);
            const float first = std::round(available * clampRatio(entity, component.ratio, size));
            const float second = available - first;
            const Vec2 axis = component.vertical ? Vec2{0.0f, 1.0f} : Vec2{1.0f, 0.0f};
            const Vec2 across = component.vertical ? Vec2{size.x, 0.0f} : Vec2{0.0f, size.y};
            place(nodeParts.bar.entity(), axis * first, across + axis * desc.splitterSize);
            describeSplitter(entity, firstNode, secondNode);
            layout(firstNode, Vec2{}, across + axis * first);
            layout(secondNode, axis * (first + desc.splitterSize), across + axis * second);
            return;
        }

        const float chromeHeight = desc.showChrome ? desc.tabHeight : 0.0f;
        setVisible(world, nodeParts.bar.entity(), desc.showChrome);
        setVisible(world, nodeParts.frame.entity(), desc.showChrome);
        place(nodeParts.bar.entity(), Vec2{}, Vec2{size.x, chromeHeight});
        for (sizet i = 0; i < RESIZE_EDGES.size(); ++i)
        {
            const auto [x, y] = RESIZE_EDGES[i];
            const Panel& edge = i == 0 ? nodeParts.grip : nodeParts.edges[i - 1];
            world.getComponent<UiStateComponent>(edge.entity()).visible = desc.showChrome && holdsEdge(entity, x, y);
            const float left = x == 0 && holdsEdge(entity, -1, y) ? GRIP : 0.0f;
            const float right = x == 0 && holdsEdge(entity, 1, y) ? GRIP : 0.0f;
            const float top = y == 0 && holdsEdge(entity, x, -1) ? GRIP : 0.0f;
            const float bottom = y == 0 && holdsEdge(entity, x, 1) ? GRIP : 0.0f;
            const Vec2 edgeSize{x == 0 ? std::max(0.0f, size.x - left - right) : GRIP,
                                y == 0 ? std::max(0.0f, size.y - top - bottom) : GRIP};
            const Vec2 position{x < 0       ? 0.0f
                                    : x > 0 ? size.x - GRIP
                                            : left,
                                y < 0       ? 0.0f
                                    : y > 0 ? size.y - GRIP
                                            : top};
            place(edge.entity(), position, edgeSize);
        }
        place(nodeParts.frame.entity(), Vec2{}, size);
        const Vec2 hostSize{size.x, std::max(0.0f, size.y - chromeHeight)};
        layoutTabs(entity, size.x);
        for (u32 index = 0; index < component.tabs.size(); ++index)
        {
            PanelData& data = panels.at(nameOf(component.tabs[index]));
            place(data.host.entity(), Vec2{0.0f, chromeHeight}, hostSize);
            world.getComponent<UiStateComponent>(data.host.entity()).visible = index == component.active;
            if (data.content.isValid() && data.content.size() != hostSize)
            {
                data.content.setSize(hostSize);
            }
        }
    }

    void UiDockState::setTabActive(PanelData& data, const bool active)
    {
        data.tab.setAccessibleSelected(active);
        data.tab.setStyleClass(childId(id, active ? ".tab.active" : ".tab"));
        data.active = active;
        styleClose(data);
    }

    void UiDockState::styleClose(PanelData& data)
    {
        if (!desc.closeButtons)
        {
            return;
        }
        const std::string own(childId(id,
                                      data.active        ? ".tab.close.active"
                                          : data.hovered ? ".tab.close.hover"
                                                         : ".tab.close")
                                  .toView());
        data.close.setStyleClass(
            InternedString(own + " " + std::string(childId(id, data.active ? ".tab.active" : ".tab").toView())));
    }

    // The tabs that fit beside the overflow button stay in order and the active one always stays; the rest fold into
    // the button, so no tab is clipped half-way and the shown panel has its tab on the strip.
    void UiDockState::layoutTabs(const Entity group, const float width)
    {
        World& world = scope->world();
        const UiDockNodeComponent& component = node(group);
        NodeParts& nodeParts = parts.at(group.id());
        if (!desc.showChrome)
        {
            nodeParts.overflowing = false;
            setVisible(world, nodeParts.more.entity(), false);
            for (const Entity host : component.tabs)
            {
                PanelData& data = panels.at(nameOf(host));
                setVisible(world, data.tab.entity(), false);
                setVisible(world, data.close.entity(), false);
                if (data.tagHost.isValid())
                {
                    setVisible(world, data.tagHost.entity(), false);
                }
            }
            return;
        }
        std::vector<float> widths;
        float total = 0.0f;
        for (const Entity host : component.tabs)
        {
            widths.push_back(tabWidthOf(panels.at(nameOf(host))));
            total += widths.back();
        }
        std::vector<bool> shown(widths.size(), true);
        if (total > width)
        {
            shown.assign(widths.size(), false);
            float room = width - OVERFLOW_WIDTH - widths[component.active];
            shown[component.active] = true;
            for (u32 index = 0; index < widths.size(); ++index)
            {
                if (index == component.active)
                {
                    continue; // its width is taken
                }
                if (widths[index] > room)
                {
                    break;
                }
                room -= widths[index];
                shown[index] = true;
            }
        }
        nodeParts.overflowing = std::ranges::contains(shown, false);
        const auto place = [&world](const Entity element, const Vec2 at, const Vec2 extent)
        {
            auto& rect = world.getComponent<UiRectComponent>(element);
            rect.anchor = Vec2{0.0f, 0.0f};
            rect.pivot = Vec2{0.0f, 0.0f};
            rect.offset = at;
            rect.size = extent;
        };
        const auto show = [&world](const Entity element, const bool visible) { setVisible(world, element, visible); };
        float x = 0.0f;
        for (u32 index = 0; index < widths.size(); ++index)
        {
            PanelData& data = panels.at(nameOf(component.tabs[index]));
            show(data.tab.entity(), shown[index]);
            show(data.close.entity(), shown[index] && desc.closeButtons);
            if (const auto& accessible = world.getConstComponent<UiAccessibleComponent>(data.tab.entity());
                accessible.selected != (index == component.active))
            {
                setTabActive(data, index == component.active);
            }
            if (!shown[index])
            {
                continue;
            }
            const float tab = widths[index] - (desc.closeButtons ? desc.tabHeight : 0.0f);
            place(data.tab.entity(), Vec2{x, 0.0f}, Vec2{tab, desc.tabHeight});
            if (desc.closeButtons)
            {
                place(data.close.entity(), Vec2{x + tab, 0.0f}, Vec2{desc.tabHeight, desc.tabHeight});
            }
            if (index == component.active)
            {
                place(nodeParts.highlight.entity(), Vec2{x, desc.tabHeight - HIGHLIGHT},
                      Vec2{widths[index], HIGHLIGHT});
            }
            x += widths[index];
        }
        show(nodeParts.more.entity(), nodeParts.overflowing);
        place(nodeParts.more.entity(), Vec2{width - OVERFLOW_WIDTH, 0.0f}, Vec2{OVERFLOW_WIDTH, desc.tabHeight});
        const float tagEnd = nodeParts.overflowing ? width - OVERFLOW_WIDTH : width;
        for (u32 index = 0; index < widths.size(); ++index)
        {
            if (const PanelData& data = panels.at(nameOf(component.tabs[index])); data.tagHost.isValid())
            {
                const bool shows = index == component.active;
                show(data.tagHost.entity(), shows);
                if (shows)
                {
                    place(data.tagHost.entity(), Vec2{x, 0.0f}, Vec2{std::max(0.0f, tagEnd - x), desc.tabHeight});
                }
            }
        }
    }

    void UiDockState::openOverflow(const Entity group)
    {
        std::vector<UiChoiceItem> items;
        overflowPanels.clear();
        for (const Entity host : node(group).tabs)
        {
            const PanelData& data = panels.at(nameOf(host));
            if (!scope->world().getConstComponent<UiStateComponent>(data.tab.entity()).visible)
            {
                overflowPanels.push_back(nameOf(host));
                items.push_back({.label = data.title});
            }
        }
        overflowList.setItems(items);
        overflowList.open(parts.at(group.id()).more);
    }

    void UiDockState::relayout(const Entity entity)
    {
        const UiRectComponent rect = scope->world().getConstComponent<UiRectComponent>(entity);
        layout(entity, rect.offset, rect.size);
        scope->markLayoutDirty(entity);
    }

    void UiDockState::layoutAll()
    {
        World& world = scope->world();
        const UiDockComponent space = dock();
        const Vec2 spaceSize = scope->hierarchy().layoutSizeOf(root.entity());
        if (spaceSize != clampedSpace && spaceSize.x > 0.0f && spaceSize.y > 0.0f)
        {
            clampedSpace = spaceSize; // a space that changed size takes the floating windows back inside
            for (const Entity window : space.floating)
            {
                if (!isDetached(window))
                {
                    auto& rect = world.getComponent<UiRectComponent>(window);
                    clampWindow(rect.offset, rect.size);
                }
            }
        }
        if (space.main != Entity::undefined())
        {
            world.getComponent<UiRectComponent>(space.main).zOffset = 0;
            layout(space.main, Vec2{}, spaceSize);
        }
        for (u32 index = 0; index < space.floating.size(); ++index)
        {
            const Entity window = space.floating[index];
            auto& rect = world.getComponent<UiRectComponent>(window);
            rect.zOffset = static_cast<i32>(index) + 1;
            const Vec2 offset = rect.offset;
            const Vec2 size = rect.size;
            layout(window, offset, size);
        }
        scope->markLayoutDirty(root.entity());
    }

    // -- UiDockState: ops ---------------------------------------------------------

    bool UiDockState::apply(const UiDockOp& op)
    {
        const bool applied = applyOp(op);
        scope->commit(); // the nodes the op destroyed die now, not at a commit some host may never make
        return applied;
    }

    bool UiDockState::applyOp(const UiDockOp& op)
    {
        if (op.kind == UiDockOpKind::Resize)
        {
            const Entity split = splitBetween(op.panel, op.target);
            if (split == Entity::undefined() || !std::isfinite(op.ratio))
            {
                return false;
            }
            const float ratio =
                clampRatio(split, op.ratio, scope->world().getConstComponent<UiRectComponent>(split).size);
            if (node(split).ratio != ratio)
            {
                node(split).ratio = ratio;
                relayout(split);
            }
            return true;
        }

        PanelData* data = panel(op.panel);
        if (!data)
        {
            return false;
        }
        const Entity group = groupOf(op.panel);
        World& world = scope->world();
        switch (op.kind)
        {
        case UiDockOpKind::Activate:
            {
                if (group == Entity::undefined())
                {
                    return false;
                }
                if (dock().maximized != Entity::undefined() && group != dock().maximized &&
                    !isFloating(windowOf(group)))
                {
                    dock().maximized = Entity::undefined(); // the panel would stay hidden behind the maximized group
                    layoutAll();
                }
                UiDockNodeComponent& component = node(group);
                const auto index =
                    static_cast<u32>(std::ranges::find(component.tabs, data->host.entity()) - component.tabs.begin());
                if (index == component.active)
                {
                    return true;
                }
                const Entity previous = component.tabs[component.active];
                component.active = index;
                // only the two hosts and the highlight change: the rest stays laid out
                world.getComponent<UiStateComponent>(previous).visible = false;
                world.getComponent<UiStateComponent>(data->host.entity()).visible = true;
                setTabActive(panels.at(nameOf(previous)), false);
                setTabActive(*data, true);
                scope->markLayoutDirty(previous);
                scope->markLayoutDirty(data->host.entity());
                if (parts.at(group.id()).overflowing)
                {
                    // the strip folds around the new active tab
                    layoutTabs(group, world.getConstComponent<UiRectComponent>(group).size.x);
                    scope->markLayoutDirty(parts.at(group.id()).bar.entity());
                    return true;
                }
                if (data->tagHost.isValid() || panels.at(nameOf(previous)).tagHost.isValid())
                {
                    layoutTabs(group, world.getConstComponent<UiRectComponent>(group).size.x); // the tag changes hands
                    scope->markLayoutDirty(parts.at(group.id()).bar.entity());
                    return true;
                }
                const UiRectComponent tab = world.getConstComponent<UiRectComponent>(data->tab.entity());
                auto& highlight = world.getComponent<UiRectComponent>(parts.at(group.id()).highlight.entity());
                highlight.offset.x = tab.offset.x;
                highlight.size.x = tab.size.x;
                scope->markLayoutDirty(parts.at(group.id()).highlight.entity());
                return true;
            }
        case UiDockOpKind::Close:
            if (group == Entity::undefined())
            {
                return false;
            }
            if (isFloating(group) && node(group).tabs.size() == 1)
            {
                const auto& rect = world.getConstComponent<UiRectComponent>(group);
                data->home = Home{.placed = true, .floating = true, .position = rect.offset, .size = rect.size};
            }
            else
            {
                data->home = homeOf(op.panel);
            }
            removeTab(op.panel);
            layoutAll();
            return true;
        case UiDockOpKind::Move:
            {
                const Entity window = group == Entity::undefined() ? Entity::undefined() : windowOf(group);
                if (window == Entity::undefined() || !isFloating(window) || !isFinite(op.position) ||
                    !isFinite(op.size))
                {
                    return false;
                }
                placeWindow(window, op.position, op.size, isDetached(window));
                relayout(window);
                return true;
            }
        case UiDockOpKind::Float:
            {
                if (!isFinite(op.position) || !isFinite(op.size))
                {
                    return false;
                }
                const Vec2 position = op.position;
                const Vec2 size = op.size.x > 0.0f && op.size.y > 0.0f ? op.size : FLOAT_SIZE;
                if (group != Entity::undefined() && isFloating(group) && node(group).tabs.size() == 1)
                {
                    placeWindow(group, position, size, op.detached);
                    layoutAll();
                    return true;
                }
                if (group == Entity::undefined())
                {
                    data->home = Home{}; // reopened from closed: no place to go back to
                }
                else if (!isFloating(windowOf(group)))
                {
                    data->home = homeOf(op.panel);
                }
                removeTab(op.panel);
                const Entity window = createTabs();
                insertTab(window, op.panel, 0);
                scope->hierarchy().attach(window, root.entity());
                placeWindow(window, position, size, op.detached);
                dock().floating.push_back(window);
                layoutAll();
                return true;
            }
        case UiDockOpKind::Dock:
            {
                if (!std::isfinite(op.ratio))
                {
                    return false;
                }
                if (op.target.isValid())
                {
                    const Entity targetGroup = groupOf(op.target);
                    if (op.target == op.panel || targetGroup == Entity::undefined())
                    {
                        return false;
                    }
                    if (op.zone != UiDockZone::Center || targetGroup != dock().maximized)
                    {
                        dock().maximized = Entity::undefined(); // a new split would show the panel beside the group
                    }
                    removeTab(op.panel); // the target's group stays: the target is in it
                    if (op.zone == UiDockZone::Center)
                    {
                        const u32 at = std::min(op.index, static_cast<u32>(node(targetGroup).tabs.size()));
                        insertTab(targetGroup, op.panel, at);
                        node(targetGroup).active = at;
                    }
                    else
                    {
                        const Entity added = createTabs();
                        insertTab(added, op.panel, 0);
                        splitBeside(targetGroup, added, op.zone, op.ratio);
                    }
                    layoutAll();
                    return true;
                }
                // Center needs an empty space; an edge of a space the panel fills alone is where it is
                if ((op.zone == UiDockZone::Center && dock().main != Entity::undefined()) ||
                    (group != Entity::undefined() && group == dock().main && node(group).tabs.size() == 1))
                {
                    return false;
                }
                dock().maximized = Entity::undefined();
                removeTab(op.panel);
                const Entity added = createTabs();
                insertTab(added, op.panel, 0);
                const Entity main = dock().main;
                if (main == Entity::undefined())
                {
                    dock().main = added;
                    scope->hierarchy().attach(added, root.entity());
                }
                else
                {
                    splitBeside(main, added, op.zone, op.ratio);
                }
                layoutAll();
                return true;
            }
        case UiDockOpKind::Maximize:
            {
                if (group == Entity::undefined() || isFloating(windowOf(group)))
                {
                    return false;
                }
                if (dock().maximized != group)
                {
                    dock().maximized = group;
                    layoutAll();
                }
                return true;
            }
        case UiDockOpKind::Restore:
            {
                if (dock().maximized == Entity::undefined())
                {
                    return true;
                }
                if (group != dock().maximized)
                {
                    return false;
                }
                dock().maximized = Entity::undefined();
                layoutAll();
                return true;
            }
        case UiDockOpKind::Resize:
            break;
        }
        return false;
    }

    void UiDockState::request(const UiDockOp& op)
    {
        if (onRequest)
        {
            onRequest(op);
            return;
        }
        apply(op);
    }

    // -- UiDockState: gestures ----------------------------------------------------

    UiDockState::Target UiDockState::targetAt(const Vec2 pointer, const u64 pointerWindow) const
    {
        const UiHierarchyManager& hierarchy = scope->hierarchy();
        const float scale = hierarchy.layoutScale();
        const Vec2 origin = hierarchy.absoluteRectOf(root.entity()).position;
        const Vec2 local = (pointer - origin) * (1.0f / scale);
        const Vec2 spaceSize = scope->hierarchy().layoutSizeOf(root.entity());
        const PanelData& dragged = panels.at(dragging);

        Target target;
        target.valid = true;
        target.op.panel = dragging;

        const auto floatHere = [&]
        {
            // grabbed by the tab, kept inside the space
            target.op = floatOpFor(dragging, local - Vec2{tabWidthOf(dragged) * 0.5f, desc.tabHeight * 0.5f});
            target.preview = UiRect{target.op.position, target.op.size};
            return target;
        };
        Entity surface = root.entity();
        for (Entity parent = hierarchy.parentOf(surface); parent != Entity::undefined();
             parent = hierarchy.parentOf(surface))
        {
            surface = parent;
        }
        const UiDockComponent& space = dock();
        Entity detachedTarget = Entity::undefined();
        for (auto it = space.floating.rbegin(); it != space.floating.rend(); ++it)
        {
            if (isDetached(*it) && pointerWindow != 0 && hierarchy.windowOf(*it) == pointerWindow && isResolved(*it) &&
                hierarchy.isVisibleOf(*it) && hierarchy.absoluteRectOf(*it).contains(pointer))
            {
                detachedTarget = *it;
                break;
            }
        }
        if (detachedTarget == Entity::undefined() && desc.detachOutside &&
            (pointerWindow != 0 || !hierarchy.absoluteRectOf(surface).contains(pointer)))
        {
            target.op = floatOpFor(dragging, Vec2{});
            target.op.detached = true;
            target.op.position = pointer * (1.0f / scale) - Vec2{tabWidthOf(dragged) * 0.5f, desc.tabHeight * 0.5f};
            target.preview = UiRect{};
            return target;
        }
        if (detachedTarget == Entity::undefined() &&
            (!UiRect{Vec2{}, spaceSize}.contains(local) || floatTargetRect(spaceSize).contains(local)))
        {
            return floatHere();
        }

        std::vector<Entity> windows;
        const bool edge = local.x < SPACE_EDGE || local.y < SPACE_EDGE || local.x >= spaceSize.x - SPACE_EDGE ||
            local.y >= spaceSize.y - SPACE_EDGE;
        if (detachedTarget != Entity::undefined())
        {
            windows.push_back(detachedTarget);
        }
        else
        {
            for (auto it = space.floating.rbegin(); it != space.floating.rend(); ++it)
            {
                if (!isDetached(*it))
                {
                    windows.push_back(*it);
                }
            }
            if (space.main == Entity::undefined() || !edge)
            {
                windows.push_back(space.main);
            }
        }
        for (const Entity window : windows)
        {
            std::vector<Entity> nodes;
            collectNodes(window, nodes);
            for (const Entity entity : nodes)
            {
                const UiDockNodeComponent& component = node(entity);
                // ops applied since the last resolve add nodes not yet laid out: not targets until then
                if (component.kind != UiDockNodeKind::Tabs || !isResolved(entity) ||
                    !scope->hierarchy().isVisibleOf(entity))
                {
                    continue;
                }
                const UiRect rect = localRect(entity);
                if (!rect.contains(local))
                {
                    continue;
                }
                const Vec2 at{(local.x - rect.position.x) / rect.size.x, (local.y - rect.position.y) / rect.size.y};
                const float nearest = std::min({at.x, 1.0f - at.x, at.y, 1.0f - at.y});
                UiDockZone zone = UiDockZone::Center;
                if (local.y - rect.position.y >= desc.tabHeight && nearest < EDGE_ZONE)
                {
                    zone = nearest == at.x       ? UiDockZone::Left
                        : nearest == 1.0f - at.x ? UiDockZone::Right
                        : nearest == at.y        ? UiDockZone::Top
                                                 : UiDockZone::Bottom;
                }
                const auto other = std::ranges::find_if(component.tabs, [&](const Entity host)
                                                        { return host != dragged.host.entity(); });
                if (other == component.tabs.end())
                {
                    return Target{}; // its own lone group: nothing to do
                }
                target.surface = detachedTarget;
                target.op.kind = UiDockOpKind::Dock;
                target.op.target = nameOf(*other);
                target.op.zone = zone;
                target.op.ratio = 0.5f;
                const Vec2 half = rect.size * 0.5f;
                switch (zone)
                {
                case UiDockZone::Center:
                    target.preview = rect;
                    break;
                case UiDockZone::Left:
                    target.preview = UiRect{rect.position, Vec2{half.x, rect.size.y}};
                    break;
                case UiDockZone::Right:
                    target.preview = UiRect{rect.position + Vec2{half.x, 0.0f}, Vec2{half.x, rect.size.y}};
                    break;
                case UiDockZone::Top:
                    target.preview = UiRect{rect.position, Vec2{rect.size.x, half.y}};
                    break;
                case UiDockZone::Bottom:
                    target.preview = UiRect{rect.position + Vec2{0.0f, half.y}, Vec2{rect.size.x, half.y}};
                    break;
                }
                return target;
            }
        }

        if (detachedTarget != Entity::undefined())
        {
            return Target{};
        }
        target.op.kind = UiDockOpKind::Dock;
        if (space.main == Entity::undefined())
        {
            target.op.zone = UiDockZone::Center;
            target.preview = UiRect{Vec2{}, spaceSize};
            return target;
        }
        if (!edge)
        {
            return Target{}; // a splitter
        }
        const float left = local.x;
        const float right = spaceSize.x - local.x;
        const float top = local.y;
        const float bottom = spaceSize.y - local.y;
        const float nearest = std::min({left, right, top, bottom});
        target.op.ratio = SPACE_SHARE;
        const Vec2 strip = spaceSize * SPACE_SHARE;
        if (nearest == left)
        {
            target.op.zone = UiDockZone::Left;
            target.preview = UiRect{Vec2{}, Vec2{strip.x, spaceSize.y}};
        }
        else if (nearest == right)
        {
            target.op.zone = UiDockZone::Right;
            target.preview = UiRect{Vec2{spaceSize.x - strip.x, 0.0f}, Vec2{strip.x, spaceSize.y}};
        }
        else if (nearest == top)
        {
            target.op.zone = UiDockZone::Top;
            target.preview = UiRect{Vec2{}, Vec2{spaceSize.x, strip.y}};
        }
        else
        {
            target.op.zone = UiDockZone::Bottom;
            target.preview = UiRect{Vec2{0.0f, spaceSize.y - strip.y}, Vec2{spaceSize.x, strip.y}};
        }
        return target;
    }

    void UiDockState::dragTab(const Vec2 pointer, const u64 pointerWindow)
    {
        if (!dragging.isValid())
        {
            return;
        }
        const Target target = targetAt(pointer, pointerWindow);
        World& world = scope->world();
        detachPreview = target.valid && target.op.detached ? std::optional(UiRect{target.op.position, target.op.size})
                                                           : std::nullopt;
        const Entity previewParent = target.surface != Entity::undefined() ? target.surface : root.entity();
        if (scope->hierarchy().parentOf(preview.entity()) != previewParent)
        {
            scope->hierarchy().attach(preview.entity(), previewParent);
        }
        setVisible(world, preview.entity(), target.valid && !target.op.detached);
        if (target.valid)
        {
            // a window, a half of a group or the space's edge strip, and the whole group, tinted apart
            const bool beside = target.op.kind == UiDockOpKind::Dock && target.op.zone != UiDockZone::Center;
            const char* suffix = target.op.kind == UiDockOpKind::Float ? ".preview.float"
                : beside                                               ? ".preview.edge"
                                                                       : ".preview";
            const InternedString styleClass = childId(id, suffix);
            if (world.getConstComponent<UiIdentityComponent>(preview.entity()).styleClass != styleClass)
            {
                preview.setStyleClass(styleClass);
            }
        }
        auto& rect = world.getComponent<UiRectComponent>(preview.entity());
        rect.offset = target.preview.position;
        if (target.surface != Entity::undefined())
        {
            rect.offset = rect.offset - localRect(target.surface).position;
        }
        rect.size = target.preview.size;
        scope->markLayoutDirty(preview.entity());
        const UiRect floatRect = floatTargetRect(scope->hierarchy().layoutSizeOf(root.entity()));
        auto& chip = world.getComponent<UiRectComponent>(floatTarget.entity());
        if (chip.offset != floatRect.position || chip.size != floatRect.size)
        {
            chip.offset = floatRect.position;
            chip.size = floatRect.size;
            scope->markLayoutDirty(floatTarget.entity());
        }
        setVisible(world, floatTarget.entity(), true);
    }

    void UiDockState::dropTab(const Vec2 pointer, const Entity element, const u64 pointerWindow)
    {
        if (!dragging.isValid())
        {
            return;
        }
        const Target target = targetAt(pointer, pointerWindow);
        const bool cancelled = scope->router().blockedByModal(element);
        endDrag();
        if (target.valid && !cancelled)
        {
            request(target.op);
        }
    }

    void UiDockState::endDrag()
    {
        dragging = InternedString{};
        detachPreview.reset();
        World& world = scope->world();
        setVisible(world, preview.entity(), false);
        if (scope->hierarchy().parentOf(preview.entity()) != root.entity())
        {
            scope->hierarchy().attach(preview.entity(), root.entity());
        }
        setVisible(world, floatTarget.entity(), false);
        scope->markLayoutDirty(preview.entity());
        scope->markLayoutDirty(floatTarget.entity());
    }

    void UiDockState::dragSplitter(const Entity split, const Vec2 pointer)
    {
        // an op since the last resolve may have removed the split or not laid it out yet
        if (!isNode(split) || !isResolved(split))
        {
            return;
        }
        const UiDockNodeComponent& component = node(split);
        const UiRect rect = localRect(split);
        const float scale = scope->hierarchy().layoutScale();
        const Vec2 origin = scope->hierarchy().absoluteRectOf(root.entity()).position;
        const Vec2 local = (pointer - origin) * (1.0f / scale) - rect.position;
        const Vec2 size = scope->world().getConstComponent<UiRectComponent>(split).size;
        const float available = (component.vertical ? size.y : size.x) - desc.splitterSize;
        if (available <= 0.0f)
        {
            return;
        }
        const float along = (component.vertical ? local.y : local.x) - desc.splitterSize * 0.5f;
        UiDockOp op;
        op.kind = UiDockOpKind::Resize;
        op.panel = leadingPanel(component.first);
        op.target = leadingPanel(component.second);
        op.ratio = clampRatio(split, along / available, size);
        request(op);
    }

    bool UiDockState::nudgeSplitter(const Entity split, const u32 scancode, const u8 modifiers)
    {
        constexpr u32 RIGHT = 79;
        constexpr u32 LEFT = 80;
        constexpr u32 DOWN = 81;
        constexpr u32 UP = 82;
        constexpr float STEP = 16.0f; // layout pixels per key press
        if (modifiers != 0 || !isNode(split) || !isResolved(split) || !scope->router().navigationEnabled())
        {
            return false;
        }
        const UiDockNodeComponent& component = node(split);
        const bool grow = scancode == (component.vertical ? DOWN : RIGHT);
        if (!grow && scancode != (component.vertical ? UP : LEFT))
        {
            return false; // the other axis moves focus to a neighbour
        }
        const Vec2 size = scope->world().getConstComponent<UiRectComponent>(split).size;
        const float available = (component.vertical ? size.y : size.x) - desc.splitterSize;
        if (available > 0.0f)
        {
            UiDockOp op;
            op.kind = UiDockOpKind::Resize;
            op.panel = leadingPanel(component.first);
            op.target = leadingPanel(component.second);
            op.ratio = clampRatio(split, component.ratio + (grow ? STEP : -STEP) / available, size);
            request(op);
        }
        return true; // at its limit the key still belongs to the splitter
    }

    void UiDockState::describeSplitter(const Entity split, const Entity first, const Entity second)
    {
        const auto leading = [this](Entity entity)
        {
            while (node(entity).kind == UiDockNodeKind::Split)
            {
                entity = node(entity).first;
            }
            const auto& tabs = node(entity).tabs;
            return tabs.empty() ? InternedString{} : nameOf(tabs.front());
        };
        const InternedString before = leading(first);
        const InternedString after = leading(second);
        if (!before.isValid() || !after.isValid())
        {
            return;
        }
        // named for the two groups' first panels (the names `dock kind=resize` takes), which a layout change moves
        Widget bar = parts.at(split.id()).bar;
        const std::string name = panels.at(before).title + " | " + panels.at(after).title;
        const std::string splitterId =
            std::string(id.toView()) + ".splitter." + std::string(before.toView()) + "." + std::string(after.toView());
        auto& identity = scope->world().getComponent<UiIdentityComponent>(bar.entity());
        if (identity.id.toView() != splitterId)
        {
            identity.id = InternedString(splitterId);
        }
        if (const auto* accessible = scope->world().tryGetConstComponent<UiAccessibleComponent>(bar.entity());
            !accessible || accessible->name != name)
        {
            bar.setAccessibleName(name);
        }
    }

    void UiDockState::dragWindow(const Entity group, const Vec2 delta, const int x, const int y)
    {
        if (!isNode(group))
        {
            return; // removed by an op during the drag
        }
        const Entity window = windowOf(group);
        if (!isFloating(window))
        {
            return;
        }
        const UiDockNodeComponent& component = node(group);
        const UiRectComponent rect = scope->world().getConstComponent<UiRectComponent>(window);
        const Vec2 moved = delta * (1.0f / scope->hierarchy().layoutScale());
        // The op is relative to the window as it stands, and apply clamps it (clampWindow): a drag that overshoots
        // the space stops at the limit, and the pointer coming back moves the window from there (a sticky edge).
        UiDockOp op;
        op.kind = UiDockOpKind::Move;
        op.panel = nameOf(component.tabs[component.active]);
        if (x == 0 && y == 0)
        {
            op.position = rect.offset + moved;
            op.size = rect.size;
        }
        else
        {
            op.size = clampWindowSize(rect.size + Vec2{moved.x * x, moved.y * y});
            op.position =
                rect.offset + Vec2{x < 0 ? rect.size.x - op.size.x : 0.0f, y < 0 ? rect.size.y - op.size.y : 0.0f};
        }
        request(op);
    }

    // -- UiDockState: persistence -------------------------------------------------

    std::string UiDockState::save() const
    {
        const auto write = [this](const auto& self, const Entity entity) -> Json
        {
            const UiDockNodeComponent& component = node(entity);
            if (component.kind == UiDockNodeKind::Split)
            {
                return Json{{"split", component.vertical ? "vertical" : "horizontal"},
                            {"ratio", component.ratio},
                            {"first", self(self, component.first)},
                            {"second", self(self, component.second)}};
            }
            Json tabs = Json::array();
            for (const Entity host : component.tabs)
            {
                tabs.push_back(std::string(nameOf(host).toView()));
            }
            return Json{{"tabs", std::move(tabs)}, {"active", component.active}};
        };
        const UiDockComponent& space = dock();
        Json json = Json::object();
        json["main"] = space.main == Entity::undefined() ? Json(nullptr) : write(write, space.main);
        Json floating = Json::array();
        for (const Entity window : space.floating)
        {
            const UiRectComponent& rect = scope->world().getConstComponent<UiRectComponent>(window);
            Json entry{{"position", {rect.offset.x, rect.offset.y}},
                       {"size", {rect.size.x, rect.size.y}},
                       {"node", write(write, window)}};
            if (isDetached(window))
            {
                entry["detached"] = true;
            }
            floating.push_back(std::move(entry));
        }
        json["floating"] = std::move(floating);
        if (space.maximized != Entity::undefined())
        {
            const UiDockNodeComponent& group = node(space.maximized);
            json["maximized"] = std::string(nameOf(group.tabs[group.active]).toView());
        }
        return json.dump(2);
    }

    bool UiDockState::load(const std::string_view text)
    {
        const Json json = Json::parse(text, nullptr, false);
        if (json.is_discarded() || !json.is_object())
        {
            return false;
        }
        if (!json.contains("main") && !json.contains("floating"))
        {
            return false; // saveLayout writes both: a file with neither is not a layout of this format
        }
        std::unique_ptr<LayoutNode> main;
        if (const auto it = json.find("main"); it != json.end() && !it->is_null())
        {
            main = parseNode(*it, 0);
            if (!main)
            {
                return false;
            }
        }
        std::vector<LayoutWindow> windows;
        if (const auto it = json.find("floating"); it != json.end())
        {
            if (!it->is_array())
            {
                return false;
            }
            for (const Json& entry : *it)
            {
                LayoutWindow window;
                if (!entry.is_object() || !entry.contains("position") || !entry.contains("size") ||
                    !entry.contains("node") || !readVec2(entry["position"], window.position) ||
                    !readVec2(entry["size"], window.size))
                {
                    return false;
                }
                window.node = parseNode(entry["node"], 0);
                window.detached = entry.value("detached", false);
                if (!window.node)
                {
                    return false;
                }
                windows.push_back(std::move(window));
            }
        }

        InternedString maximized{};
        if (const auto it = json.find("maximized"); it != json.end())
        {
            if (!it->is_string())
            {
                return false;
            }
            maximized = InternedString(it->get<std::string>());
        }

        // A layout naming only panels this space lacks (another editor's, a stale file) would empty the space.
        u32 named = 0;
        u32 known = 0;
        const auto count = [&](const auto& self, const LayoutNode& layoutNode) -> void
        {
            if (layoutNode.split)
            {
                self(self, *layoutNode.first);
                self(self, *layoutNode.second);
                return;
            }
            named += static_cast<u32>(layoutNode.tabs.size());
            known += static_cast<u32>(std::ranges::count_if(layoutNode.tabs, [this](const InternedString name)
                                                            { return panel(name) != nullptr; }));
        };
        if (main)
        {
            count(count, *main);
        }
        for (const LayoutWindow& window : windows)
        {
            count(count, *window.node);
        }
        if (named != 0 && known == 0)
        {
            return false;
        }

        // Clear: every panel parks, every node dies.
        std::vector<Entity> nodes;
        collectNodes(dock().main, nodes);
        for (const Entity window : std::vector<Entity>(dock().floating))
        {
            collectNodes(window, nodes);
        }
        for (const InternedString name : order)
        {
            if (groupOf(name) != Entity::undefined())
            {
                park(panels.at(name));
            }
        }
        dock().main = Entity::undefined();
        dock().floating.clear();
        dock().maximized = Entity::undefined();
        clampedSpace = Vec2{}; // its windows are clamped to the space as it stands
        for (auto& entry : panels)
        {
            entry.second.home = Home{}; // a loaded layout has its own places
        }
        for (const Entity entity : nodes)
        {
            destroyNode(entity);
        }

        // Unknown and repeated names drop; emptied groups and their splits collapse.
        const auto build = [this](const auto& self, const LayoutNode& layoutNode) -> Entity
        {
            if (layoutNode.split)
            {
                const Entity first = self(self, *layoutNode.first);
                const Entity second = self(self, *layoutNode.second);
                if (first == Entity::undefined() || second == Entity::undefined())
                {
                    return first == Entity::undefined() ? second : first;
                }
                const Entity split = createSplit(layoutNode.vertical, layoutNode.ratio);
                node(split).first = first;
                node(split).second = second;
                scope->hierarchy().attach(first, split);
                scope->hierarchy().attach(second, split);
                return split;
            }
            Entity group = Entity::undefined();
            for (const InternedString name : layoutNode.tabs)
            {
                if (!panel(name) || groupOf(name) != Entity::undefined())
                {
                    continue;
                }
                group = group == Entity::undefined() ? createTabs() : group;
                insertTab(group, name, static_cast<u32>(node(group).tabs.size()));
            }
            if (group != Entity::undefined())
            {
                UiDockNodeComponent& component = node(group);
                component.active = std::min(layoutNode.active, static_cast<u32>(component.tabs.size()) - 1);
            }
            return group;
        };
        const Entity built = main ? build(build, *main) : Entity::undefined();
        if (built != Entity::undefined())
        {
            scope->hierarchy().attach(built, root.entity());
            dock().main = built;
        }
        for (const LayoutWindow& window : windows)
        {
            const Entity entity = build(build, *window.node);
            if (entity == Entity::undefined())
            {
                continue;
            }
            scope->hierarchy().attach(entity, root.entity());
            auto& rect = scope->world().getComponent<UiRectComponent>(entity);
            rect.offset = window.position;
            rect.size = clampWindowSize(window.size);
            if (window.detached)
            {
                scope->hierarchy().detach(entity);
                detached.push_back(entity);
            }
            dock().floating.push_back(entity);
        }
        if (const Entity group = groupOf(maximized); group != Entity::undefined() && !isFloating(windowOf(group)))
        {
            dock().maximized = group;
        }
        scope->commit(); // see apply
        layoutAll();
        return true;
    }

    // -- UiScope ------------------------------------------------------------------

    DockSpace UiScope::createDock(const InternedString id, const UiDockDesc& desc)
    {
        auto owned = std::make_unique<UiDockState>();
        UiDockState& state = *owned;
        state.scope = this;
        state.id = id;
        state.desc = desc;
        state.root = createPanel(id, desc.rect);
        m_world->addComponent<UiDockComponent>(state.root.entity(), UiDockComponent{});
        // containers take no input: presses on bare space reach what is under the dock
        m_world->getComponent<UiStateComponent>(state.root.entity()).clipsChildren = true;
        m_world->getComponent<UiStateComponent>(state.root.entity()).interactable = false;
        UiRectDesc previewDesc = topLeft();
        previewDesc.zOffset = PREVIEW_Z;
        state.preview = createPanel(childId(id, ".preview"), previewDesc);
        hierarchy().attach(state.preview.entity(), state.root.entity());
        m_world->getComponent<UiStateComponent>(state.preview.entity()).interactable = false;
        m_world->getComponent<UiStateComponent>(state.preview.entity()).visible = false;
        state.floatTarget = createButton(childId(id, ".floattarget"), "", previewDesc);
        state.floatTarget.setStyleClass(childId(id, ".floattarget"));
        state.floatTarget.setLocalizedText(InternedString("engine.ui.dock.float"));
        hierarchy().attach(state.floatTarget.entity(), state.root.entity());
        m_world->getComponent<UiStateComponent>(state.floatTarget.entity()).visible = false;
        state.overflowList =
            createPopupList(childId(id, ".tabs.more.list"), UiDropdownDesc{.maxRows = 12, .menu = true});
        state.overflowList.setAccessibleNameKey(InternedString("engine.ui.dock.more"));
        state.overflowList.onChoose(
            [&state](const u32 index)
            {
                if (index < state.overflowPanels.size())
                {
                    state.request(UiDockOp{.kind = UiDockOpKind::Activate, .panel = state.overflowPanels[index]});
                }
            });
        m_docks.emplace(state.root.entity().id(), std::move(owned));
        hierarchy().observeSize(state.root.entity());
        return DockSpace(this, state.root.entity(), m_generation);
    }

    UiDockState* UiScope::dockState(const Entity root) const
    {
        const auto it = m_docks.find(root.id());
        return it == m_docks.end() ? nullptr : it->second.get();
    }

    // -- DockSpace ----------------------------------------------------------------

    UiDockState* DockSpace::runtime() const { return isValid() ? scope()->dockState(entity()) : nullptr; }

    void DockSpace::addPanel(const InternedString name, const std::string_view title, const Widget& content)
    {
        if (UiDockState* state = runtime())
        {
            state->addPanel(name, title, content);
        }
    }

    void DockSpace::setPanelMinSize(const InternedString name, const Vec2 size)
    {
        if (UiDockState* state = runtime())
        {
            state->setPanelMinSize(name, size);
        }
    }

    void DockSpace::setPanelTag(const InternedString name, const Widget& tag)
    {
        if (UiDockState* state = runtime())
        {
            state->setPanelTag(name, tag);
        }
    }

    void DockSpace::setPanelTitle(const InternedString name, const std::string_view title)
    {
        if (UiDockState* state = runtime())
        {
            state->setPanelTitle(name, title);
        }
    }

    void DockSpace::setTabHeight(const float height)
    {
        if (UiDockState* state = runtime(); state && state->desc.tabHeight != height)
        {
            state->desc.tabHeight = height;
            state->layoutAll();
        }
    }

    void DockSpace::setSplitterSize(const float size)
    {
        if (UiDockState* state = runtime(); state && state->desc.splitterSize != size)
        {
            state->desc.splitterSize = size;
            state->layoutAll();
        }
    }

    bool DockSpace::apply(const UiDockOp& op)
    {
        UiDockState* state = runtime();
        return state && state->apply(op);
    }

    void DockSpace::onRequest(std::function<void(const UiDockOp&)> fn)
    {
        if (UiDockState* state = runtime())
        {
            state->onRequest = std::move(fn);
        }
    }

    bool DockSpace::isOpen(const InternedString panel) const
    {
        const UiDockState* state = runtime();
        return state && state->groupOf(panel) != Entity::undefined();
    }

    bool DockSpace::isMaximized(const InternedString panel) const
    {
        const UiDockState* state = runtime();
        if (!state)
        {
            return false;
        }
        const Entity group = state->groupOf(panel);
        return group != Entity::undefined() && state->dockRead().maximized == group;
    }

    UiRect DockSpace::floatingRect(const InternedString panel) const
    {
        const UiDockState* state = runtime();
        return state ? state->floatingRectOf(panel) : UiRect{};
    }

    Entity DockSpace::panelHost(const InternedString panel) const
    {
        const UiDockState* state = runtime();
        const UiDockState::PanelData* data = state ? state->panel(panel) : nullptr;
        return data ? data->host.entity() : Entity::undefined();
    }

    std::vector<UiDetachedWindow> DockSpace::detachedWindows() const
    {
        std::vector<UiDetachedWindow> out;
        if (const UiDockState* state = runtime())
        {
            for (const Entity window : state->dockRead().floating)
            {
                if (state->isDetached(window))
                {
                    const UiRectComponent& rect = state->scope->world().getConstComponent<UiRectComponent>(window);
                    out.push_back({window, state->leadingPanel(window), rect.offset, rect.size});
                }
            }
        }
        return out;
    }

    std::optional<UiRect> DockSpace::detachPreview() const
    {
        const UiDockState* state = runtime();
        return state ? state->detachPreview : std::nullopt;
    }

    bool DockSpace::isDetached(const InternedString panel) const
    {
        const UiDockState* state = runtime();
        const Entity group = state ? state->groupOf(panel) : Entity::undefined();
        return group != Entity::undefined() && state->isDetached(state->windowOf(group));
    }

    std::optional<UiDockOp> DockSpace::reopenOp(const InternedString panel) const
    {
        const UiDockState* state = runtime();
        return state ? state->reopenOpFor(panel) : std::nullopt;
    }

    std::string DockSpace::saveLayout() const
    {
        const UiDockState* state = runtime();
        return state ? state->save() : std::string{};
    }

    bool DockSpace::loadLayout(const std::string_view json)
    {
        UiDockState* state = runtime();
        return state && state->load(json);
    }
} // namespace spite
