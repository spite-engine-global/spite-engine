#include <algorithm>
#include <string>
#include <string_view>

#include "ecs/World.hpp"
#include "ui/UiEvents.hpp"
#include "ui/UiOverlay.hpp"
#include "ui/UiScope.hpp"
#include "ui/UiTextMeasurer.hpp"
#include "UiMenuState.hpp"

namespace spite
{
    namespace
    {
        // HID scancodes (the UiKeyTransition domain).
        namespace Key
        {
            constexpr u32 RETURN = 40;
            constexpr u32 ESCAPE = 41;
            constexpr u32 RIGHT = 79;
            constexpr u32 LEFT = 80;
            constexpr u32 DOWN = 81;
            constexpr u32 UP = 82;
            constexpr u32 KP_ENTER = 88;
            constexpr u32 HOME = 74;
            constexpr u32 END = 77;
            constexpr u32 TYPING_FIRST = 4; // A
            constexpr u32 TYPING_LAST = 39; // 0, after the letters and 1-9
        } // namespace Key

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

        bool selectable(const UiMenuItem& item) { return !item.separator && item.enabled; }

        // the first UTF-8 codepoint of `text`, 0 when it is empty or malformed
        u32 firstCodepoint(const std::string_view text)
        {
            if (text.empty())
            {
                return 0;
            }
            const auto lead = static_cast<u8>(text[0]);
            const u32 length = lead < 0x80 ? 1u
                : (lead >> 5) == 0x6       ? 2u
                : (lead >> 4) == 0xE       ? 3u
                : (lead >> 3) == 0x1E      ? 4u
                                           : 0u;
            if (length == 0 || text.size() < length)
            {
                return 0;
            }
            u32 codepoint = length == 1 ? lead : lead & (0xFFu >> (length + 1));
            for (u32 i = 1; i < length; ++i)
            {
                const auto next = static_cast<u8>(text[i]);
                if ((next & 0xC0u) != 0x80u)
                {
                    return 0;
                }
                codepoint = (codepoint << 6) | (next & 0x3Fu);
            }
            return codepoint;
        }

        // simple case folding for the scripts the locales use: ASCII, Latin-1, Greek, Cyrillic
        u32 foldCase(const u32 c)
        {
            if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x391 && c <= 0x3A9) ||
                (c >= 0x410 && c <= 0x42F))
            {
                return c + 0x20;
            }
            return c >= 0x400 && c <= 0x40F ? c + 0x50 : c;
        }
    } // namespace

    // -- UiMenuState --------------------------------------------------------------

    UiMenuComponent& UiMenuState::menu() const { return scope->world().getComponent<UiMenuComponent>(root.entity()); }

    bool UiMenuState::contains(const Entity element) const
    {
        const UiHierarchyManager& hierarchy = scope->hierarchy();
        u32 guard = 0;
        for (Entity current = element; current != Entity::undefined() && guard < 64;
             current = hierarchy.parentOf(current), ++guard)
        {
            if (current == root.entity())
            {
                return true;
            }
        }
        return false;
    }

    float UiMenuState::rowTop(const Level& level, const u32 index) const
    {
        float top = 0.0f;
        for (u32 i = 0; i < index; ++i)
        {
            top += (*level.items)[i].separator ? desc.separatorHeight : desc.itemHeight;
        }
        return top;
    }

    std::optional<UiRect> UiMenuState::bounds() const
    {
        const UiHierarchyManager& hierarchy = scope->hierarchy();
        if (!desc.bounds.isValid() || hierarchy.paintIndexOf(desc.bounds.entity()) == UiHierarchyManager::NO_INDEX)
        {
            return std::nullopt;
        }
        return hierarchy.absoluteRectOf(desc.bounds.entity());
    }

    UiRect UiMenuState::place(const Vec2 size, const UiRect& anchor, const bool beside) const
    {
        UiPlacementRequest request;
        request.size = size;
        request.anchor = anchor;
        request.bounds = bounds().value_or(UI_UNBOUNDED_BOUNDS); // a menu without bounds places against no edge
        request.side = beside ? UiPlacementSide::Right : UiPlacementSide::Below;
        // the panel keeps its full size: an oversized one overflows from the top edge, as it always did
        return UiRect{placeUiPopup(request).rect.position, size};
    }

    UiMenuState::Level& UiMenuState::level(const u32 index)
    {
        if (index < levels.size())
        {
            return levels[index];
        }
        UiScope& owner = *scope;
        Level created;
        created.panel = owner.createPanel(childId(id, ".panel"), topLeft());
        created.panel.setParent(root);
        created.panel.setVisible(false);
        created.highlight = owner.createPanel(childId(id, ".highlight"), topLeft());
        created.highlight.setParent(created.panel);
        created.highlight.setInteractable(false);
        created.highlight.setVisible(false);
        owner.setReaction(created.panel.entity(), UiEventKind::FocusLost, [this](const void*) { focusLost(); });
        levels.push_back(created);
        return levels.back();
    }

    void UiMenuState::addSlot(const u32 levelIndex)
    {
        UiScope& owner = *scope;
        Level& owning = levels[levelIndex];
        Slot slot;
        slot.row = owner.createLabel(childId(id, ".item"), "", topLeft());
        slot.row.setParent(owning.panel);
        owner.world().addComponent<UiMenuItemComponent>(slot.row.entity(), UiMenuItemComponent{});

        UiRectDesc arrowDesc;
        arrowDesc.anchor = Vec2{1.0f, 0.0f};
        arrowDesc.pivot = Vec2{1.0f, 0.0f};
        arrowDesc.size = Vec2{desc.itemHeight, desc.itemHeight};
        slot.arrow = owner.createLabel(childId(id, ".item.arrow"), ">", arrowDesc);
        UiRectDesc lineDesc;
        lineDesc.anchor = Vec2{0.0f, 0.5f};
        lineDesc.pivot = Vec2{0.0f, 0.5f};
        slot.line = owner.createPanel(childId(id, ".separator"), lineDesc);
        slot.check = owner.createLabel(childId(id, ".item.check"), "", arrowDesc);
        slot.check.setIcon("check");
        // the row takes every hit: parts never shadow it
        for (Widget part : {Widget(slot.arrow), Widget(slot.check), Widget(slot.line)})
        {
            part.setParent(slot.row);
            part.setInteractable(false);
            part.setVisible(false);
        }

        const auto index = static_cast<u32>(owning.slots.size());
        const Entity row = slot.row.entity();
        owner.setReaction(row, UiEventKind::HoverEnter,
                          [this, levelIndex, index](const void*) { hover(levelIndex, index); });
        owner.setReaction(row, UiEventKind::Click,
                          [this, levelIndex, index](const void*) { click(levelIndex, index); });
        owner.setReaction(row, UiEventKind::FocusLost, [this](const void*) { focusLost(); });
        owning.slots.push_back(slot);
    }

    void UiMenuState::openLevel(const u32 levelIndex, const std::vector<UiMenuItem>& items, const UiRect& anchor,
                                const bool beside)
    {
        level(levelIndex);
        while (levels[levelIndex].slots.size() < items.size())
        {
            addSlot(levelIndex);
        }
        Level& shown = levels[levelIndex];
        const UiTextMeasurer* measurer = scope->hierarchy().textMeasurer();
        float width = desc.width;
        float height = 0.0f;
        for (u32 i = 0; i < items.size(); ++i)
        {
            const UiMenuItem& item = items[i];
            height += item.separator ? desc.separatorHeight : desc.itemHeight;
            if (!item.separator && measurer)
            {
                const float arrow = item.children.empty() && !item.checkable ? 0.0f : desc.itemHeight;
                width = std::max(width,
                                 measurer->measureText(shown.slots[i].row.entity(), item.label).x +
                                     2.0f * desc.padding + arrow);
            }
        }

        float top = 0.0f;
        const float rowWidth = std::max(0.0f, width - 2.0f * desc.padding);
        for (u32 i = 0; i < shown.slots.size(); ++i)
        {
            Slot& slot = shown.slots[i];
            // the mark is state, not name: a checkable row is named by its label alone. First: adding the accessible
            // component moves the row's components
            if (i < items.size() &&
                (items[i].checkable || scope->world().hasComponent<UiAccessibleComponent>(slot.row.entity())))
            {
                slot.row.setAccessibleName(items[i].checkable ? std::string_view(items[i].label) : std::string_view{});
            }
            auto& component = scope->world().getComponent<UiMenuItemComponent>(slot.row.entity());
            if (i >= items.size())
            {
                if (slot.row.isVisible())
                {
                    slot.row.setVisible(false);
                    component = UiMenuItemComponent{};
                }
                continue;
            }
            const UiMenuItem& item = items[i];
            const float rowHeight = item.separator ? desc.separatorHeight : desc.itemHeight;
            slot.row.setOffset(Vec2{desc.padding, top});
            slot.row.setSize(Vec2{rowWidth, rowHeight});
            slot.row.setText(item.separator ? std::string{} : item.label);
            slot.row.setInteractable(selectable(item));
            slot.row.setVisible(true);
            slot.arrow.setVisible(!item.separator && !item.children.empty());
            slot.check.setVisible(!item.separator && item.checkable && item.checked);
            slot.line.setVisible(item.separator);
            if (item.separator)
            {
                slot.line.setSize(Vec2{rowWidth, 1.0f});
            }
            component.id = item.separator ? InternedString{} : item.id;
            component.enabled = item.enabled;
            component.separator = item.separator;
            component.submenu = !item.children.empty();
            component.checkable = item.checkable && !item.separator;
            component.checked = component.checkable && item.checked;
            top += rowHeight;
        }

        const float scale = scope->hierarchy().layoutScale();
        shown.items = &items;
        shown.width = width;
        shown.rect = place(Vec2{width, height} * scale, anchor, beside);
        shown.panel.setOffset(shown.rect.position * (1.0f / scale));
        shown.panel.setSize(Vec2{width, height});
        shown.panel.setVisible(true);
        highlight(levelIndex, NONE);
        depth = levelIndex + 1;
    }

    void UiMenuState::openSubmenu(const u32 levelIndex)
    {
        const Level& parent = levels[levelIndex];
        const UiMenuItem& item = (*parent.items)[parent.highlighted];
        const float scale = scope->hierarchy().layoutScale();
        const UiRect row{
            Vec2{parent.rect.position.x, parent.rect.position.y + rowTop(parent, parent.highlighted) * scale},
            Vec2{parent.rect.size.x, desc.itemHeight * scale}};
        openLevel(levelIndex + 1, item.children, row, true);
    }

    void UiMenuState::closeFrom(const u32 levelIndex)
    {
        for (u32 i = levelIndex; i < depth; ++i)
        {
            levels[i].panel.setVisible(false);
            levels[i].highlighted = NONE;
        }
        depth = std::min(depth, levelIndex);
    }

    void UiMenuState::highlight(const u32 levelIndex, const u32 index)
    {
        Level& shown = levels[levelIndex];
        shown.highlighted = index;
        if (index == NONE)
        {
            if (shown.highlight.isVisible())
            {
                shown.highlight.setVisible(false);
            }
            return;
        }
        shown.highlight.setOffset(Vec2{0.0f, rowTop(shown, index)});
        shown.highlight.setSize(Vec2{shown.width, desc.itemHeight});
        shown.highlight.setVisible(true);
    }

    u32 UiMenuState::nextSelectable(const u32 levelIndex, const u32 from, const bool forward) const
    {
        const std::vector<UiMenuItem>& items = *levels[levelIndex].items;
        const auto count = static_cast<u32>(items.size());
        for (u32 step = 0; step < count; ++step)
        {
            u32 index;
            if (from == NONE)
            {
                index = forward ? step : count - 1 - step;
            }
            else
            {
                index = forward ? (from + 1 + step) % count : (from + count - 1 - step) % count;
            }
            if (selectable(items[index]))
            {
                return index;
            }
        }
        return NONE;
    }

    void UiMenuState::open(const std::span<const UiMenuItem> items, const UiRect& anchor, const Entity from)
    {
        model.assign(items.begin(), items.end());
        closeFrom(0);
        UiRouter& router = scope->router();
        if (!contains(router.focused()))
        {
            returnFocus = router.focused();
        }
        else if (from != Entity::undefined())
        {
            returnFocus = from; // moved across the bar: Escape goes back to this menu's button
        }
        dismissedBy = Entity::undefined();
        openedFrom = from;
        // the menu opens in the OS window of what opened it, the hovered element's for a menu at a point
        UiHierarchyManager& hierarchy = scope->hierarchy();
        const Entity opener = from != Entity::undefined() ? from : router.hovered();
        if (const u64 window = hierarchy.isElement(opener) ? hierarchy.windowOf(opener) : 0;
            window != hierarchy.windowOf(root.entity()))
        {
            hierarchy.bindRootToWindow(root.entity(), window);
        }
        root.setVisible(true);
        openLevel(0, model, anchor, false);
        router.requestFocus(root.entity());
        sync();
    }

    void UiMenuState::close()
    {
        if (depth == 0)
        {
            return;
        }
        closeFrom(0);
        root.setVisible(false);
        UiRouter& router = scope->router();
        // closed from inside (Escape, activation): focus goes back; a focus
        // move outside already put it where it belongs
        if (contains(router.focused()))
        {
            if (scope->hierarchy().isElement(returnFocus))
            {
                router.restoreFocus(returnFocus);
            }
            else
            {
                router.clearFocus();
            }
        }
        returnFocus = Entity::undefined();
        sync();
    }

    bool UiMenuState::apply(const UiMenuCommand command)
    {
        if (depth == 0)
        {
            return false;
        }
        const u32 innermost = depth - 1;
        const Level& shown = levels[innermost];
        switch (command)
        {
        case UiMenuCommand::Up:
        case UiMenuCommand::Down:
            {
                const u32 next = nextSelectable(innermost, shown.highlighted, command == UiMenuCommand::Down);
                if (next == NONE)
                {
                    return false;
                }
                highlight(innermost, next);
                break;
            }
        case UiMenuCommand::First:
        case UiMenuCommand::Last:
            {
                const u32 next = nextSelectable(innermost, NONE, command == UiMenuCommand::First);
                if (next == NONE)
                {
                    return false;
                }
                highlight(innermost, next);
                break;
            }
        case UiMenuCommand::Open:
        case UiMenuCommand::Activate:
            {
                if (shown.highlighted == NONE)
                {
                    return false;
                }
                const UiMenuItem& item = (*shown.items)[shown.highlighted];
                if (item.children.empty())
                {
                    if (command == UiMenuCommand::Open)
                    {
                        return false;
                    }
                    fire(item.id);
                    return true;
                }
                openSubmenu(innermost);
                highlight(innermost + 1, nextSelectable(innermost + 1, NONE, true));
                break;
            }
        case UiMenuCommand::Back:
            if (depth < 2)
            {
                return false;
            }
            closeFrom(innermost);
            break;
        case UiMenuCommand::Close:
            if (depth < 2)
            {
                close();
                return true;
            }
            closeFrom(innermost);
            break;
        }
        sync();
        return true;
    }

    bool UiMenuState::typeAhead(const std::string_view text)
    {
        const u32 typed = foldCase(firstCodepoint(text));
        if (depth == 0 || typed == 0)
        {
            return false;
        }
        const u32 innermost = depth - 1;
        const Level& shown = levels[innermost];
        const std::vector<UiMenuItem>& items = *shown.items;
        const auto count = static_cast<u32>(items.size());
        // from the item after the highlighted one, so repeating a letter walks the items that share it
        for (u32 step = 1; step <= count; ++step)
        {
            const u32 index = (shown.highlighted == NONE ? step - 1 : shown.highlighted + step) % count;
            const UiMenuItem& item = items[index];
            if (selectable(item) && !item.label.empty() && foldCase(firstCodepoint(item.label)) == typed)
            {
                highlight(innermost, index);
                sync();
                return true;
            }
        }
        return false;
    }

    bool UiMenuState::cycle(const i32 direction)
    {
        if (!onCycle)
        {
            return false;
        }
        const auto fn = onCycle; // it may open this menu again, replacing it
        fn(direction);
        return true;
    }

    bool UiMenuState::activate(const InternedString item)
    {
        if (depth == 0)
        {
            return false;
        }
        // enabled leaves reachable through enabled submenus
        std::vector<const std::vector<UiMenuItem>*> stack{&model};
        while (!stack.empty())
        {
            const std::vector<UiMenuItem>* items = stack.back();
            stack.pop_back();
            for (const UiMenuItem& entry : *items)
            {
                if (!selectable(entry))
                {
                    continue;
                }
                if (!entry.children.empty())
                {
                    stack.push_back(&entry.children);
                }
                else if (entry.id == item)
                {
                    fire(item);
                    return true;
                }
            }
        }
        return false;
    }

    void UiMenuState::hover(const u32 levelIndex, const u32 slot)
    {
        if (levelIndex >= depth || slot >= levels[levelIndex].items->size())
        {
            return;
        }
        const UiMenuItem& item = (*levels[levelIndex].items)[slot];
        if (!selectable(item))
        {
            return;
        }
        const bool shown = levels[levelIndex].highlighted == slot &&
            (item.children.empty() ? depth == levelIndex + 1 : depth >= levelIndex + 2);
        if (shown)
        {
            return;
        }
        closeFrom(levelIndex + 1);
        highlight(levelIndex, slot);
        if (!item.children.empty())
        {
            openSubmenu(levelIndex);
        }
        sync();
    }

    void UiMenuState::click(const u32 levelIndex, const u32 slot)
    {
        if (levelIndex >= depth || slot >= levels[levelIndex].items->size())
        {
            return;
        }
        const UiMenuItem& item = (*levels[levelIndex].items)[slot];
        if (!selectable(item))
        {
            return;
        }
        if (!item.children.empty())
        {
            hover(levelIndex, slot);
            return;
        }
        fire(item.id);
    }

    void UiMenuState::fire(const InternedString item)
    {
        close();
        UiMenuActivateEvent event;
        event.element = root.entity();
        event.item = item;
        scope->world().fireEvent(event);
        if (onActivate)
        {
            const auto fn = onActivate; // it may replace itself
            fn(item);
        }
    }

    void UiMenuState::focusLost()
    {
        const Entity focused = scope->router().focused();
        if (depth == 0 || contains(focused))
        {
            return;
        }
        dismissedBy = focused;
        close();
    }

    void UiMenuState::sync()
    {
        UiMenuComponent& state = menu();
        state.open = depth > 0;
        state.path.clear();
        state.highlighted = InternedString{};
        for (u32 i = 0; i < depth; ++i)
        {
            const Level& shown = levels[i];
            if (shown.highlighted == NONE)
            {
                state.path.push_back(Entity::undefined());
                continue;
            }
            state.path.push_back(shown.slots[shown.highlighted].row.entity());
            state.highlighted = (*shown.items)[shown.highlighted].id;
        }
    }

    // -- UiScope ------------------------------------------------------------------

    Menu UiScope::createMenu(const InternedString id, const UiMenuDesc& desc)
    {
        auto owned = std::make_unique<UiMenuState>();
        UiMenuState& state = *owned;
        state.scope = this;
        state.id = id;
        state.desc = desc;
        UiRectDesc rootDesc = topLeft();
        rootDesc.zOffset = desc.zOffset;
        state.root = createPanel(id, rootDesc);
        const Entity root = state.root.entity();
        m_world->getComponent<UiStateComponent>(root).visible = false;
        m_world->addComponent<UiMenuComponent>(root, UiMenuComponent{});
        setReaction(root, UiEventKind::FocusLost, [&state](const void*) { state.focusLost(); });
        // keys bubble from the focused row or panel to the root; the menu
        // claims only the keys that did something
        setKeyReaction(root,
                       [&state](const UiKeyDownEvent& event)
                       {
                           switch (event.scancode)
                           {
                           case Key::UP:
                               return state.apply(UiMenuCommand::Up);
                           case Key::DOWN:
                               return state.apply(UiMenuCommand::Down);
                           case Key::RIGHT:
                               return state.apply(UiMenuCommand::Open) || state.cycle(1);
                           case Key::LEFT:
                               return state.apply(UiMenuCommand::Back) || state.cycle(-1);
                           case Key::HOME:
                               return state.apply(UiMenuCommand::First);
                           case Key::END:
                               return state.apply(UiMenuCommand::Last);
                           case Key::RETURN:
                           case Key::KP_ENTER:
                               return state.apply(UiMenuCommand::Activate);
                           case Key::ESCAPE:
                               return state.apply(UiMenuCommand::Close);
                           default:
                               {
                                   // a typing key (letters, digits) is the menu's while it is open, so the editor's
                                   // bare-key hotkeys stay out of it; the match itself is the TextInput reaction's
                                   constexpr u8 CHORDS =
                                       UiKeyModifier::CTRL | UiKeyModifier::ALT | UiKeyModifier::SUPER;
                                   return state.depth > 0 && event.scancode >= Key::TYPING_FIRST &&
                                       event.scancode <= Key::TYPING_LAST && (event.modifiers & CHORDS) == 0;
                               }
                           }
                       });
        // type-ahead follows the text the layout produced, not the scancode; a chord yields no text
        setReaction(root, UiEventKind::TextInput, [&state](const void* payload)
                    { state.typeAhead(static_cast<const UiTextInputEvent*>(payload)->text); });
        m_menus.emplace(root.id(), std::move(owned));
        return Menu(this, root, m_generation);
    }

    UiMenuState* UiScope::menuState(const Entity root) const
    {
        const auto it = m_menus.find(root.id());
        return it == m_menus.end() ? nullptr : it->second.get();
    }

    // -- Menu ---------------------------------------------------------------------

    UiMenuState* Menu::runtime() const { return isValid() ? scope()->menuState(entity()) : nullptr; }

    void Menu::open(const std::span<const UiMenuItem> items, const Vec2 position)
    {
        if (UiMenuState* state = runtime())
        {
            state->open(items, UiRect{position, Vec2{0.0f, 0.0f}}, Entity::undefined());
        }
    }

    void Menu::open(const std::span<const UiMenuItem> items, const Widget& anchor)
    {
        UiMenuState* state = runtime();
        if (!state || !anchor.isValid())
        {
            return;
        }
        const Entity element = anchor.entity();
        // the press on the anchor this menu was open from closed it: a toggle;
        // a press on another anchor sharing the menu re-opens it there
        if (state->dismissedBy == element && state->openedFrom == element && scope()->router().pressed() == element)
        {
            state->dismissedBy = Entity::undefined();
            return;
        }
        state->open(items, scope()->hierarchy().absoluteRectOf(element), element);
    }

    void Menu::close()
    {
        if (UiMenuState* state = runtime())
        {
            state->close();
        }
    }

    bool Menu::isOpen() const
    {
        const UiMenuState* state = runtime();
        return state && state->depth > 0;
    }

    bool Menu::apply(const UiMenuCommand command)
    {
        UiMenuState* state = runtime();
        return state && state->apply(command);
    }

    bool Menu::activate(const InternedString item)
    {
        UiMenuState* state = runtime();
        return state && state->activate(item);
    }

    void Menu::onActivate(std::function<void(InternedString)> fn)
    {
        if (UiMenuState* state = runtime())
        {
            state->onActivate = std::move(fn);
        }
    }

    void Menu::onCycle(std::function<void(i32)> fn)
    {
        if (UiMenuState* state = runtime())
        {
            state->onCycle = std::move(fn);
        }
    }
} // namespace spite
