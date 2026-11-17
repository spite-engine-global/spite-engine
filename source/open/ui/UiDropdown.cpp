#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <ranges>
#include <string>
#include <utility>

#include "base/Assert.hpp"
#include "ecs/World.hpp"
#include "localization/Localization.hpp"
#include "ui/UiOverlay.hpp"
#include "ui/UiScope.hpp"
#include "UiDropdownState.hpp"
#include "UiPartClass.hpp"

// Dropdown and combo box: a root holding the selected text (or a text input) and an arrow, and a list on a
// root surface of its own; see UiDropdownState.hpp.

namespace spite
{
    namespace
    {
        // HID scancodes (the UiKeyTransition domain).
        namespace Key
        {
            constexpr u32 RETURN = 40;
            constexpr u32 ESCAPE = 41;
            constexpr u32 TAB = 43;
            constexpr u32 SPACE = 44;
            constexpr u32 HOME = 74;
            constexpr u32 PAGE_UP = 75;
            constexpr u32 END = 77;
            constexpr u32 PAGE_DOWN = 78;
            constexpr u32 RIGHT = 79;
            constexpr u32 LEFT = 80;
            constexpr u32 DOWN = 81;
            constexpr u32 UP = 82;
            constexpr u32 KP_ENTER = 88;
            constexpr u32 TYPING_FIRST = 4; // A
            constexpr u32 TYPING_LAST = 39; // 0, after the letters and 1-9
        } // namespace Key

        constexpr Vec2 DEFAULT_SIZE{160.0f, 24.0f};
        constexpr float ROOT_PADDING_LEFT = 6.0f;
        constexpr float ROOT_PADDING_RIGHT = 4.0f;
        constexpr float ARROW_WIDTH = 24.0f;
        constexpr float ROW_INSET = 6.0f;
        constexpr float MARK_WIDTH = 20.0f;
        constexpr float ROW_TEXT_INSET = ROW_INSET + MARK_WIDTH;
        constexpr float THUMB_WIDTH = 4.0f;
        constexpr float THUMB_MIN = 12.0f;
        constexpr float POPUP_MARGIN = 4.0f;
        constexpr float POPUP_MIN_WIDTH = 200.0f; // a popup list whose descriptor names none
        constexpr float SEARCH_PAD = 3.0f; // around the search box
        constexpr u32 WHEEL_ROWS = 3;
        constexpr u32 MAX_PARENT_DEPTH = 64;

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

        bool interactableNow(const World& world, const Entity element)
        {
            const auto* state = world.tryGetConstComponent<UiStateComponent>(element);
            return !state || state->interactable;
        }

        // The callback is copied first: it may destroy the widget it belongs to.
        template <typename Fn, typename... Args>
        void fire(const Fn& callback, Args&&... args)
        {
            if (callback)
            {
                const Fn local = callback;
                local(std::forward<Args>(args)...);
            }
        }

        char lower(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

        bool equalsNoCase(const std::string_view a, const std::string_view b)
        {
            return a.size() == b.size() &&
                std::ranges::equal(a, b, [](char x, char y) { return lower(x) == lower(y); });
        }

        bool containsNoCase(const std::string_view text, const std::string_view needle)
        {
            return needle.empty() ||
                !std::ranges::search(text, needle, [](char x, char y) { return lower(x) == lower(y); }).empty();
        }

        bool startsWithNoCase(const std::string_view text, const std::string_view prefix)
        {
            return text.size() >= prefix.size() && equalsNoCase(text.substr(0, prefix.size()), prefix);
        }

        // Writes the rect when it differs and re-lays the element out.
        void setRect(UiScope& scope, const Entity element, const Vec2 offset, const Vec2 size)
        {
            const auto& rect = scope.world().getConstComponent<UiRectComponent>(element);
            if (rect.offset == offset && rect.size == size)
            {
                return;
            }
            auto& written = scope.world().getComponent<UiRectComponent>(element);
            written.offset = offset;
            written.size = size;
            scope.markLayoutDirty(element);
        }
    } // namespace

    // -- UiDropdownState: building ------------------------------------------------------------------------------------

    void UiDropdownState::build()
    {
        World& world = scope->world();
        const bool combo = kind == UiWidgetKind::Combo;
        const InternedString id = world.getConstComponent<UiIdentityComponent>(root).id;
        const auto part = [this, &world](const Entity element, const char* styleClass)
        {
            world.getComponent<UiIdentityComponent>(element).styleClass = InternedString(styleClass);
            scope->hierarchy().attach(element, root);
            scope->m_valueParts[element.id()] = root.id();
        };
        UiLayoutItemComponent grow;
        grow.grow = 1.0f;
        grow.shrink = 0.0f;
        grow.basis = 0.0f;
        grow.alignSelf = UiLayoutAlign::Stretch;
        UiRectDesc growDesc;
        growDesc.layoutItem = grow;
        if (combo)
        {
            input = scope->createTextElement(childId(id, ".input"), UiWidgetKind::TextInput, growDesc, false);
            part(input, "combo.input");
            UiScope::TextInputData& data = scope->m_textInputs[input.id()];
            data.selectOnFocus = true;
            data.onChange = [this](const std::string_view text) { filterChanged(text); };
            data.onSubmit = [this](const std::string_view text) { submit(text); };
            data.onKey = [this](const u32 scancode, const u8 modifiers) { comboKey(scancode, modifiers); };
            data.onFocus = [this](const bool gained)
            {
                if (!gained)
                {
                    closeIfOutside();
                }
            };
        }
        else
        {
            label = scope->createLabel(childId(id, ".label"), "", growDesc).entity();
            part(label, "dropdown.label");
            Label(scope, label, scope->generation()).setInteractable(false);
            Label(scope, label, scope->generation()).setAccessibleRole(UiRole::Presentation);
        }
        UiRectDesc arrowDesc;
        arrowDesc.size = Vec2{ARROW_WIDTH, 0.0f};
        UiLayoutItemComponent fixed;
        fixed.shrink = 0.0f;
        arrowDesc.layoutItem = fixed;
        arrow = scope->createLabel(childId(id, ".arrow"), "", arrowDesc).entity();
        part(arrow, "dropdown.arrow");
        Label caret(scope, arrow, scope->generation());
        caret.setIcon("caret-down");
        caret.setInteractable(combo);
        caret.setAccessibleRole(UiRole::Presentation);

        for (const Entity element : {root, combo ? arrow : Entity::undefined()})
        {
            if (element == Entity::undefined())
            {
                continue;
            }
            scope->setReaction(element, UiEventKind::PointerDown,
                               [this](const void* payload)
                               {
                                   if (static_cast<const UiPointerDownEvent*>(payload)->button == 0)
                                   {
                                       scope->router().requestFocus(focusTarget());
                                   }
                               });
            scope->setReaction(element, UiEventKind::Click,
                               [this](const void* payload)
                               {
                                   if (static_cast<const UiClickEvent*>(payload)->button == 0)
                                   {
                                       toggle();
                                   }
                               });
        }
        if (!combo)
        {
            scope->setReaction(root, UiEventKind::FocusLost, [this](const void*) { closeIfOutside(); });
            scope->setKeyReaction(root, [this](const UiKeyDownEvent& event) { return key(event); });
        }
    }

    void UiDropdownState::restyle()
    {
        World& world = scope->world();
        const InternedString family = world.getConstComponent<UiIdentityComponent>(root).styleClass;
        const auto apply = [&](const Entity part, const std::string_view own)
        {
            if (part == Entity::undefined())
            {
                return;
            }
            auto& identity = world.getComponent<UiIdentityComponent>(part);
            const InternedString wanted = partClass(family, own);
            if (identity.styleClass != wanted)
            {
                identity.styleClass = wanted;
                scope->markLayoutDirty(part);
            }
        };
        apply(label, "dropdown.label");
        apply(input, "combo.input");
        apply(arrow, "dropdown.arrow");
    }

    void UiDropdownState::buildStandalone()
    {
        popup = Panel(scope, root, scope->generation());
        popup.setStyleClass(InternedString("dropdown.popup"));
        popup.setAccessibleRole(desc.menu ? UiRole::Menu : UiRole::ListBox);
        scope->world().getComponent<UiStateComponent>(root).visible = false;
    }

    float UiDropdownState::topInset() const { return desc.searchable ? desc.rowHeight + 2.0f * SEARCH_PAD : 0.0f; }

    Entity UiDropdownState::focusTarget() const
    {
        if (kind == UiWidgetKind::Combo)
        {
            return input;
        }
        if (desc.keepFocus && standalone && returnFocus != Entity::undefined())
        {
            return returnFocus; // the opener holds the focus
        }
        return desc.searchable && (standalone || listOpen) ? searchInput : (standalone ? popup.entity() : root);
    }

    void UiDropdownState::ensurePopup()
    {
        if (m_built)
        {
            return;
        }
        m_built = true;
        World& world = scope->world();
        const InternedString id = world.getConstComponent<UiIdentityComponent>(root).id;
        if (!standalone)
        {
            UiRectDesc rootDesc = topLeft();
            rootDesc.zOffset = desc.zOffset;
            popup = scope->createPanel(childId(id, ".popup"), rootDesc);
            popup.setStyleClass(InternedString("dropdown.popup"));
            popup.setAccessibleRole(desc.menu ? UiRole::Menu : UiRole::ListBox);
            world.getComponent<UiStateComponent>(popup.entity()).visible = false;
        }
        highlightBar = scope->createPanel(childId(id, ".highlight"), topLeft());
        highlightBar.setParent(popup);
        highlightBar.setStyleClass(InternedString("dropdown.highlight"));
        world.getComponent<UiStateComponent>(highlightBar.entity()).visible = false;
        markBar = scope->createLabel(childId(id, ".mark"), "", topLeft());
        markBar.setIcon("check");
        markBar.setAccessibleRole(UiRole::Presentation);
        markBar.setParent(popup);
        markBar.setStyleClass(InternedString("dropdown.mark"));
        markBar.setInteractable(false);
        world.getComponent<UiStateComponent>(markBar.entity()).visible = false;
        thumb = scope->createPanel(childId(id, ".thumb"), topLeft());
        thumb.setParent(popup);
        thumb.setStyleClass(InternedString("dropdown.thumb"));
        world.getComponent<UiStateComponent>(thumb.entity()).visible = false;
        if (desc.searchable)
        {
            searchInput = scope->createTextElement(childId(id, ".search"), UiWidgetKind::TextInput, topLeft(), false);
            world.getComponent<UiIdentityComponent>(searchInput).styleClass = InternedString("dropdown.search");
            TextInput box(scope, searchInput, scope->generation());
            box.setParent(popup);
            box.setAccessibleNameKey(InternedString("engine.ui.popup.search"));
            UiScope::TextInputData& data = scope->m_textInputs[searchInput.id()];
            data.onChange = [this](const std::string_view text) { filterChanged(text); };
            data.onSubmit = [this](const std::string_view text) { submit(text); };
            data.onKey = [this](const u32 scancode, const u8 modifiers) { comboKey(scancode, modifiers); };
            data.onCancel = [this] { close(); };
            data.onFocus = [this](const bool gained)
            {
                if (!gained)
                {
                    closeIfOutside();
                }
            };
        }

        for (const Entity element : {popup.entity(), highlightBar.entity(), thumb.entity()})
        {
            scope->setReaction(element, UiEventKind::PointerDown,
                               [this](const void* payload)
                               {
                                   if (static_cast<const UiPointerDownEvent*>(payload)->button == 0)
                                   {
                                       scope->router().requestFocus(focusTarget());
                                   }
                               });
        }
        // the highlighted row's inset edges: the bar is under the rows, so it only takes what they leave
        const auto hoverGutter = [this](const Vec2 pointer)
        {
            const i32 position = positionAt(pointer);
            if (position >= 0 && navigable(view[static_cast<u32>(position)]))
                setHighlight(position, false);
        };
        scope->setReaction(popup.entity(), UiEventKind::HoverEnter, [hoverGutter](const void* payload)
                           { hoverGutter(static_cast<const UiHoverEnterEvent*>(payload)->position); });
        scope->setReaction(popup.entity(), UiEventKind::HoverMove, [hoverGutter](const void* payload)
                           { hoverGutter(static_cast<const UiHoverMoveEvent*>(payload)->position); });
        scope->setReaction(popup.entity(), UiEventKind::Click,
                           [this](const void* payload)
                           {
                               const auto& click = *static_cast<const UiClickEvent*>(payload);
                               const i32 position = positionAt(click.position);
                               if (click.button == 0 && position >= 0 && navigable(view[static_cast<u32>(position)]))
                                   choose(view[static_cast<u32>(position)]);
                           });
        scope->setReaction(highlightBar.entity(), UiEventKind::Click,
                           [this](const void* payload)
                           {
                               if (static_cast<const UiClickEvent*>(payload)->button == 0 && highlighted >= 0 &&
                                   static_cast<u32>(highlighted) < view.size())
                               {
                                   choose(view[static_cast<u32>(highlighted)]);
                               }
                           });
        scope->setReaction(
            popup.entity(), UiEventKind::Wheel,
            [this](const void* payload)
            {
                const float wheel = static_cast<const UiWheelEvent*>(payload)->wheelDelta.y;
                const auto notches = static_cast<i32>(std::lround(-wheel * static_cast<float>(WHEEL_ROWS)));
                scrollBy(static_cast<float>(notches != 0 ? notches : (wheel > 0.0f ? -1 : (wheel < 0.0f ? 1 : 0))));
            });
        // the thumb moves the window by its share of the travel; a drag on the rows or the panel pulls the content
        scope->setReaction(popup.entity(), UiEventKind::DragStart,
                           [this](const void* payload)
                           {
                               m_dragRows = 0.0f;
                               dragContent(static_cast<const UiDragStartEvent*>(payload)->delta.y);
                           });
        scope->setReaction(popup.entity(), UiEventKind::DragMove, [this](const void* payload)
                           { dragContent(static_cast<const UiDragMoveEvent*>(payload)->delta.y); });
        scope->setReaction(thumb.entity(), UiEventKind::DragStart,
                           [this](const void* payload)
                           {
                               m_dragRows = 0.0f;
                               scrollThumb(static_cast<const UiDragStartEvent*>(payload)->delta.y);
                           });
        scope->setReaction(thumb.entity(), UiEventKind::DragMove, [this](const void* payload)
                           { scrollThumb(static_cast<const UiDragMoveEvent*>(payload)->delta.y); });
        if (standalone)
        {
            scope->setKeyReaction(popup.entity(), [this](const UiKeyDownEvent& event) { return key(event); });
            scope->setReaction(popup.entity(), UiEventKind::FocusLost, [this](const void*) { closeIfOutside(); });
            if (!desc.searchable)
            {
                scope->setReaction(popup.entity(), UiEventKind::TextInput, [this](const void* payload)
                                   { typeAhead(static_cast<const UiTextInputEvent*>(payload)->text); });
            }
        }
    }

    void UiDropdownState::ensureRows(const u32 count)
    {
        World& world = scope->world();
        const InternedString id = world.getConstComponent<UiIdentityComponent>(root).id;
        while (rows.size() < count)
        {
            const auto index = static_cast<u32>(rows.size());
            Label row = scope->createLabel(childId(id, ".item"), "", topLeft());
            row.setParent(popup);
            row.setStyleClass(InternedString("dropdown.item"));
            Label icon = scope->createLabel(childId(id, ".item.icon"), "", topLeft());
            icon.setParent(popup);
            icon.setStyleClass(InternedString("dropdown.item"));
            icon.setAccessibleRole(UiRole::Presentation);
            icon.setVisible(false);
            rowIcons.push_back(icon);
            scope->setReaction(icon.entity(), UiEventKind::HoverEnter,
                               [this, index](const void*)
                               {
                                   const u32 position = first + index;
                                   if (index < shownRows && position < view.size() && navigable(view[position]) &&
                                       highlighted != static_cast<i32>(position))
                                       setHighlight(static_cast<i32>(position), false);
                               });
            scope->setReaction(icon.entity(), UiEventKind::PointerDown,
                               [this](const void* payload)
                               {
                                   if (static_cast<const UiPointerDownEvent*>(payload)->button == 0)
                                       scope->router().requestFocus(focusTarget());
                               });
            scope->setReaction(icon.entity(), UiEventKind::Click,
                               [this, index](const void* payload)
                               {
                                   const u32 position = first + index;
                                   if (static_cast<const UiClickEvent*>(payload)->button == 0 && index < shownRows &&
                                       position < view.size())
                                       choose(view[position]);
                               });
            scope->setReaction(icon.entity(), UiEventKind::DragStart,
                               [this](const void* payload)
                               {
                                   m_dragRows = 0.0f;
                                   dragContent(static_cast<const UiDragStartEvent*>(payload)->delta.y);
                               });
            scope->setReaction(icon.entity(), UiEventKind::DragMove, [this](const void* payload)
                               { dragContent(static_cast<const UiDragMoveEvent*>(payload)->delta.y); });
            row.setAccessibleRole(desc.menu ? UiRole::MenuItem : UiRole::Option);
            world.getComponent<UiStateComponent>(row.entity()).visible = false;
            scope->setReaction(row.entity(), UiEventKind::HoverEnter,
                               [this, index](const void*)
                               {
                                   const u32 position = first + index;
                                   if (index < shownRows && position < view.size() && navigable(view[position]) &&
                                       highlighted != static_cast<i32>(position))
                                   {
                                       setHighlight(static_cast<i32>(position), false);
                                   }
                               });
            scope->setReaction(row.entity(), UiEventKind::PointerDown,
                               [this](const void* payload)
                               {
                                   if (static_cast<const UiPointerDownEvent*>(payload)->button == 0)
                                   {
                                       scope->router().requestFocus(focusTarget());
                                   }
                               });
            scope->setReaction(row.entity(), UiEventKind::Click,
                               [this, index](const void* payload)
                               {
                                   const u32 position = first + index;
                                   if (static_cast<const UiClickEvent*>(payload)->button == 0 && index < shownRows &&
                                       position < view.size())
                                   {
                                       choose(view[position]);
                                   }
                               });
            scope->setReaction(row.entity(), UiEventKind::DragStart,
                               [this](const void* payload)
                               {
                                   m_dragRows = 0.0f;
                                   dragContent(static_cast<const UiDragStartEvent*>(payload)->delta.y);
                               });
            scope->setReaction(row.entity(), UiEventKind::DragMove, [this](const void* payload)
                               { dragContent(static_cast<const UiDragMoveEvent*>(payload)->delta.y); });
            rows.push_back(row);
            m_stamps.emplace_back();
            m_width = -1.0f; // the next layout sizes every row
        }
    }

    // -- UiDropdownState: model
    // -----------------------------------------------------------------------------------------

    std::string_view UiDropdownState::textOf(const UiChoiceItem& item, std::string& scratch) const
    {
        const Localization* localization = item.key.isValid() ? scope->hierarchy().localization() : nullptr;
        return localization ? localization->resolve(item.key, scratch) : std::string_view(item.label);
    }

    void UiDropdownState::rebuildView()
    {
        view.clear();
        const bool all = !filtering || filter.empty();
        std::string scratch;
        u32 heading = NONE; // a filtered list shows a group heading only above a match
        for (u32 i = 0; i < items.size(); ++i)
        {
            if (items[i].header && !all)
            {
                heading = i;
            }
            // the shown text or the id: a row that reads "Capsule Collider" is found by its name typed as one word
            else if (all || containsNoCase(textOf(items[i], scratch), filter) ||
                     (items[i].id.isValid() && containsNoCase(items[i].id.toView(), filter)) ||
                     (i < searchTerms.size() && containsNoCase(searchTerms[i], filter)))
            {
                if (heading != NONE)
                {
                    view.push_back(std::exchange(heading, NONE));
                }
                view.push_back(i);
            }
        }
        if (!all)
        {
            const auto exact = std::ranges::find_if(view,
                                                    [&](const u32 index)
                                                    {
                                                        return index < exactSearch.size() &&
                                                            !exactSearch[index].empty() &&
                                                            exactSearch[index].size() == filter.size() &&
                                                            containsNoCase(exactSearch[index], filter);
                                                    });
            if (exact != view.end())
            {
                const u32 index = *exact;
                view.erase(exact);
                view.insert(view.begin(), index);
            }
        }
    }

    void UiDropdownState::applySelected()
    {
        if (standalone)
        {
            return;
        }
        World& world = scope->world();
        const UiChoiceItem* item =
            shownSelected >= 0 && static_cast<sizet>(shownSelected) < items.size() ? &items[shownSelected] : nullptr;
        if (kind == UiWidgetKind::Combo)
        {
            std::string scratch;
            const std::string_view text = item ? textOf(*item, scratch) : std::string_view{};
            if (world.getConstComponent<UiValueComponent>(input).text != text)
            {
                TextInput(scope, input, scope->generation()).setText(text);
            }
            return;
        }
        Label shown(scope, label, scope->generation());
        if (!item && placeholder.isValid())
        {
            const auto* localized = world.tryGetConstComponent<UiLocalizedTextComponent>(label);
            if (!localized || localized->key != placeholder)
            {
                shown.setLocalizedText(placeholder);
            }
        }
        else if (item && item->key.isValid())
        {
            const auto* localized = world.tryGetConstComponent<UiLocalizedTextComponent>(label);
            if (!localized || localized->key != item->key)
            {
                shown.setLocalizedText(item->key);
            }
        }
        else
        {
            const std::string_view text = item ? std::string_view(item->label) : std::string_view{};
            if (world.tryGetConstComponent<UiLocalizedTextComponent>(label) ||
                world.getConstComponent<UiLabelComponent>(label).text != text)
            {
                shown.setText(text);
            }
        }
    }

    void UiDropdownState::publish()
    {
        const auto& current = scope->world().getConstComponent<UiDropdownComponent>(root);
        if (!standalone && current.open != listOpen)
            Label(scope, arrow, scope->generation()).setIcon(listOpen ? "caret-up" : "caret-down");
        const i32 position = highlighted >= 0 && static_cast<u32>(highlighted) < view.size()
            ? static_cast<i32>(view[static_cast<u32>(highlighted)])
            : -1;
        const auto count = static_cast<u32>(items.size());
        if (current.open == listOpen && current.highlighted == position && current.count == count &&
            current.filter == (filtering ? filter : std::string{}))
        {
            return;
        }
        auto& written = scope->world().getComponent<UiDropdownComponent>(root);
        written.open = listOpen;
        written.highlighted = position;
        written.count = count;
        written.filter = filtering ? filter : std::string{};
    }

    void UiDropdownState::setItems(const std::span<const UiChoiceItem> choices)
    {
        items.assign(choices.begin(), choices.end());
        ++revision;
        World& world = scope->world();
        const i32 selected = world.getConstComponent<UiDropdownComponent>(root).selected;
        shownSelected = selected >= -1 && selected < static_cast<i32>(items.size()) ? selected : -1;
        if (world.getConstComponent<UiDropdownComponent>(root).selected != shownSelected)
        {
            world.getComponent<UiDropdownComponent>(root).selected = shownSelected;
        }
        applySelected();
        rebuildView();
        if (listOpen)
        {
            if (view.empty() && !desc.searchable)
            {
                close(false);
            }
            else
            {
                if (highlighted < 0 || static_cast<u32>(highlighted) >= view.size())
                {
                    highlighted = nextEnabled(-1, 1);
                    m_scrolledTo = -2;
                }
                place();
            }
        }
        publish();
    }

    void UiDropdownState::assign(const i32 index)
    {
        if (index < -1 || index >= static_cast<i32>(items.size()))
        {
            return;
        }
        if (scope->world().getConstComponent<UiDropdownComponent>(root).selected != index)
        {
            scope->world().getComponent<UiDropdownComponent>(root).selected = index;
        }
        shownSelected = index;
        applySelected();
        if (listOpen)
        {
            bindRows();
        }
    }

    void UiDropdownState::choose(const u32 item)
    {
        if (item >= items.size() || !navigable(item))
        {
            return;
        }
        const bool changed =
            scope->world().getConstComponent<UiDropdownComponent>(root).selected != static_cast<i32>(item);
        filtering = false;
        filter.clear();
        rebuildView();
        if (!standalone) // a popup list's choice is an action: the selection stays the app's
        {
            assign(static_cast<i32>(item));
        }
        close(false);
        if (changed || standalone) // last: the callback may destroy the widget
        {
            fire(onSelect, item);
        }
    }

    void UiDropdownState::sync()
    {
        const i32 selected = scope->world().getConstComponent<UiDropdownComponent>(root).selected;
        if (selected < -1 || selected >= static_cast<i32>(items.size()))
        {
            scope->world().getComponent<UiDropdownComponent>(root).selected = shownSelected;
        }
        else if (selected != shownSelected)
        {
            shownSelected = selected;
            applySelected();
        }
        publish();
    }

    std::string UiDropdownState::setFromText(const std::string_view text)
    {
        u32 item = NONE;
        std::int64_t number = 0;
        const char* end = text.data() + text.size();
        if (const auto [stop, error] = std::from_chars(text.data(), end, number);
            error == std::errc{} && stop == end && number >= -1 && number < static_cast<std::int64_t>(items.size()))
        {
            if (number < 0)
            {
                filtering = false;
                filter.clear();
                rebuildView();
                assign(-1);
                close(false);
                return {};
            }
            item = static_cast<u32>(number);
        }
        else
        {
            // an out-of-range number is still an id or a label ("1080")
            std::string scratch;
            for (u32 i = 0; i < items.size() && item == NONE; ++i)
            {
                item = items[i].id.isValid() && items[i].id.toView() == text ? i : NONE;
            }
            for (u32 i = 0; i < items.size() && item == NONE; ++i)
            {
                item = textOf(items[i], scratch) == text ? i : NONE;
            }
            if (item == NONE)
            {
                return "value is an item index (-1 clears), id or label";
            }
        }
        if (!navigable(item))
        {
            return items[item].header ? "the item is a group heading" : "the item is disabled";
        }
        choose(item);
        return {};
    }

    // -- UiDropdownState: the list
    // --------------------------------------------------------------------------------------

    bool UiDropdownState::insidePopup(const Entity element) const
    {
        if (!popup.isValid())
        {
            return false;
        }
        const UiHierarchyManager& hierarchy = scope->hierarchy();
        u32 guard = 0;
        for (Entity current = element; current != Entity::undefined() && guard < MAX_PARENT_DEPTH;
             current = hierarchy.parentOf(current), ++guard)
        {
            if (current == popup.entity())
            {
                return true;
            }
        }
        return false;
    }

    bool UiDropdownState::insideRoot(const Entity element) const
    {
        const UiHierarchyManager& hierarchy = scope->hierarchy();
        u32 guard = 0;
        for (Entity current = element; current != Entity::undefined() && guard < MAX_PARENT_DEPTH;
             current = hierarchy.parentOf(current), ++guard)
        {
            if (current == root)
            {
                return true;
            }
        }
        return false;
    }

    i32 UiDropdownState::nextEnabled(const i32 from, const i32 direction) const
    {
        const auto count = static_cast<i32>(view.size());
        for (i32 step = 1; step <= count; ++step)
        {
            const i32 start = from < 0 ? (direction > 0 ? -1 : 0) : from;
            const i32 position = ((start + direction * step) % count + count) % count;
            if (navigable(view[static_cast<u32>(position)]))
            {
                return position;
            }
        }
        return -1;
    }

    void UiDropdownState::open()
    {
        World& world = scope->world();
        UiHierarchyManager& hierarchy = scope->hierarchy();
        const Entity anchor = anchorEntity();
        if (listOpen || anchor == Entity::undefined() || !world.isEntityValid(anchor) ||
            !interactableNow(world, anchor) || !hierarchy.isVisibleOf(anchor) || scope->router().blockedByModal(anchor))
        {
            return;
        }
        if (desc.searchable)
        {
            clearSearch();
        }
        if (view.empty())
        {
            return;
        }
        ensurePopup();
        const Entity focused = scope->router().focused();
        returnFocus = standalone ? (insidePopup(focused) ? Entity::undefined() : focused) : root;
        listOpen = true;
        ++scope->m_openDropdowns;
        dismissedBy = Entity::undefined();
        m_typed.clear();
        i32 position = -1;
        if (!filtering && shownSelected >= 0 && std::cmp_less(shownSelected, items.size()))
        {
            const auto at = std::ranges::lower_bound(view, static_cast<u32>(shownSelected));
            position =
                at != view.end() && *at == static_cast<u32>(shownSelected) ? static_cast<i32>(at - view.begin()) : -1;
        }
        highlighted = position >= 0 && navigable(view[static_cast<u32>(position)]) ? position : nextEnabled(-1, 1);
        first = 0;
        m_scrolledTo = -2;
        m_shownHighlight = -2;
        world.getComponent<UiStateComponent>(popup.entity()).visible = true;
        scope->markLayoutDirty(popup.entity());
        place();
        publish();
        // a list with a face leaves the focus on it; one with a search box or none of its own takes it
        if (!(desc.keepFocus && standalone) &&
            (standalone || desc.searchable || (!insideRoot(focused) && !insidePopup(focused))))
        {
            scope->router().requestFocus(focusTarget());
        }
    }

    void UiDropdownState::openAt(const Entity anchorElement)
    {
        if (listOpen)
        {
            const bool same = openedFrom == anchorElement;
            close();
            if (same)
            {
                return;
            }
        }
        // the press on the anchor this list was open from closed it (its focus moved there): a toggle
        else if (dismissedBy == anchorElement && openedFrom == anchorElement &&
                 scope->router().pressed() == anchorElement)
        {
            dismissedBy = Entity::undefined();
            return;
        }
        openedFrom = anchorElement;
        open();
    }

    void UiDropdownState::restoreFocus()
    {
        const Entity focused = scope->router().focused();
        if (focused == Entity::undefined() || !insidePopup(focused))
        {
            return;
        }
        const bool back = returnFocus != Entity::undefined() && scope->world().isEntityValid(returnFocus) &&
            scope->hierarchy().isVisibleOf(returnFocus) && !insidePopup(returnFocus);
        if (back)
        {
            scope->router().restoreFocus(returnFocus);
        }
    }

    void UiDropdownState::clearSearch()
    {
        filtering = false;
        filter.clear();
        rebuildView();
        if (searchInput != Entity::undefined() &&
            !scope->world().getConstComponent<UiValueComponent>(searchInput).text.empty())
        {
            TextInput(scope, searchInput, scope->generation()).setText("");
        }
    }

    void UiDropdownState::close(const bool revert)
    {
        const bool was = listOpen;
        if (was)
        {
            listOpen = false;
            --scope->m_openDropdowns;
            restoreFocus();
            scope->world().getComponent<UiStateComponent>(popup.entity()).visible = false;
            scope->markLayoutDirty(popup.entity());
            highlighted = -1;
            m_typed.clear();
        }
        if (kind == UiWidgetKind::Combo && revert && (filtering || was))
        {
            filtering = false;
            filter.clear();
            rebuildView();
            applySelected();
        }
        else if (desc.searchable && (was || filtering))
        {
            clearSearch();
        }
        publish();
    }

    void UiDropdownState::toggle()
    {
        if (listOpen)
        {
            close();
        }
        else
        {
            open();
        }
    }

    void UiDropdownState::closeIfOutside()
    {
        if (!listOpen && !filtering)
        {
            return;
        }
        const Entity focused = scope->router().focused();
        if (focused != Entity::undefined() && (insideRoot(focused) || insidePopup(focused)))
        {
            return;
        }
        const Entity pressed = scope->router().pressed();
        dismissedBy = pressed; // only a press toggles: focus walking onto the opener must not swallow its next click
        close();
    }

    void UiDropdownState::refit()
    {
        if (!listOpen)
        {
            return;
        }
        UiHierarchyManager& hierarchy = scope->hierarchy();
        const Entity anchor = anchorEntity();
        if (anchor == Entity::undefined() || !scope->world().isEntityValid(anchor) ||
            !interactableNow(scope->world(), anchor) || !hierarchy.isVisibleOf(anchor) ||
            scope->router().blockedByModal(anchor))
        {
            close();
            return;
        }
        Entity top = anchor;
        for (u32 guard = 0; guard < MAX_PARENT_DEPTH; ++guard)
        {
            const Entity parent = hierarchy.parentOf(top);
            if (parent == Entity::undefined())
            {
                break;
            }
            top = parent;
        }
        const UiRect rect = hierarchy.absoluteRectOf(anchor);
        const UiRect bounds = top == anchor ? m_bounds : hierarchy.absoluteRectOf(top);
        if (rect.position != m_anchor.position || rect.size != m_anchor.size || bounds.position != m_bounds.position ||
            bounds.size != m_bounds.size || hierarchy.layoutScale() != m_scale)
        {
            place();
        }
    }

    // Sizes and places the popup beside the anchor, then binds the rows it shows.
    void UiDropdownState::place()
    {
        UiHierarchyManager& hierarchy = scope->hierarchy();
        const Entity anchor = anchorEntity();
        m_scale = hierarchy.layoutScale() > 0.0f ? hierarchy.layoutScale() : 1.0f;
        m_anchor = hierarchy.absoluteRectOf(anchor);
        Entity top = anchor;
        for (u32 guard = 0; guard < MAX_PARENT_DEPTH; ++guard)
        {
            const Entity parent = hierarchy.parentOf(top);
            if (parent == Entity::undefined())
            {
                break;
            }
            top = parent;
        }
        // an anchor that is a root surface itself has no surface to stay inside
        m_bounds = top == anchor ? UI_UNBOUNDED_BOUNDS : hierarchy.absoluteRectOf(top);
        // the list opens in its anchor's OS window
        if (const u64 window = hierarchy.windowOf(top); window != hierarchy.windowOf(popup.entity()))
        {
            scope->hierarchy().bindRootToWindow(popup.entity(), window);
        }

        const float rowPx = desc.rowHeight * m_scale;
        const float margin = POPUP_MARGIN * m_scale;
        const float inset = topInset();
        const u32 wanted = std::min<u32>(static_cast<u32>(view.size()), std::clamp(desc.maxRows, 1u, MAX_ROWS));
        // the rows the roomier side holds: a popup never slides over its anchor
        const float below = m_bounds.position.y + m_bounds.size.y - margin - (m_anchor.position.y + m_anchor.size.y);
        const float above = m_anchor.position.y - margin - m_bounds.position.y;
        const auto room = static_cast<u32>(std::max(0.0f, std::max(below, above) - inset * m_scale) / rowPx + 1.0e-4f);
        const u32 fit = std::clamp(room, 1u, std::max(wanted, 1u));
        UiPlacementRequest request;
        request.anchor = m_anchor;
        request.bounds = m_bounds;
        request.side = UiPlacementSide::Below;
        request.margin = margin;
        const float minimum =
            standalone ? (desc.rect.size.x > 0.0f ? desc.rect.size.x : POPUP_MIN_WIDTH) * m_scale : 0.0f;
        request.size =
            Vec2{std::max(m_anchor.size.x, minimum), (inset + static_cast<float>(fit) * desc.rowHeight) * m_scale};
        const UiPlacement placement = placeUiPopup(request);
        const bool resized = fit != shownRows;
        shownRows = fit;
        const float inverse = 1.0f / m_scale;
        const float width = placement.rect.size.x * inverse;
        setRect(*scope, popup.entity(), placement.rect.position * inverse,
                Vec2{width, inset + static_cast<float>(fit) * desc.rowHeight});

        ensureRows(fit);
        if (width != m_width)
        {
            m_width = width;
            m_shownHighlight = -2;
            m_shownMark = -2;
            for (u32 i = 0; i < rows.size(); ++i)
            {
                m_stamps[i] = {};
                setRect(*scope, rows[i].entity(), Vec2{ROW_TEXT_INSET, inset + static_cast<float>(i) * desc.rowHeight},
                        Vec2{std::max(0.0f, width - ROW_TEXT_INSET - ROW_INSET), desc.rowHeight});
            }
            if (searchInput != Entity::undefined())
            {
                setRect(*scope, searchInput, Vec2{ROW_INSET, SEARCH_PAD},
                        Vec2{std::max(0.0f, width - 2.0f * ROW_INSET), desc.rowHeight});
            }
        }
        if (highlighted != m_scrolledTo || resized) // a re-place keeps a wheel- or drag-scrolled window
        {
            scrollTo(highlighted);
        }
        else
        {
            first = std::min(first, view.size() > shownRows ? static_cast<u32>(view.size()) - shownRows : 0u);
        }
        bindRows();
        showHighlight();
        showThumb();
        showEmpty();
    }

    void UiDropdownState::scrollTo(const i32 position)
    {
        m_scrolledTo = position;
        const u32 last = view.size() > shownRows ? static_cast<u32>(view.size()) - shownRows : 0;
        if (position >= 0)
        {
            const auto at = static_cast<u32>(position);
            // the group heading above the row comes into view with it
            const u32 top = at > 0 && items[view[at - 1]].header ? at - 1 : at;
            if (top < first)
            {
                first = top;
            }
            else if (at >= first + shownRows)
            {
                first = at - shownRows + 1;
            }
        }
        first = std::min(first, last);
    }

    void UiDropdownState::scrollBy(const float delta)
    {
        m_dragRows += delta;
        const auto whole = static_cast<i32>(m_dragRows);
        if (whole == 0)
        {
            return;
        }
        m_dragRows -= static_cast<float>(whole);
        const auto last = static_cast<i32>(view.size() > shownRows ? view.size() - shownRows : 0);
        const i32 moved = std::clamp(static_cast<i32>(first) + whole, 0, last);
        if (moved != static_cast<i32>(first))
        {
            first = static_cast<u32>(moved);
            bindRows();
            showHighlight();
            showThumb();
        }
    }

    void UiDropdownState::dragContent(const float pixels) { scrollBy(-pixels / (desc.rowHeight * m_scale)); }

    // A thumb drag of `pixels` moves the window by the same share of the rows as of the thumb's travel.
    void UiDropdownState::scrollThumb(const float pixels)
    {
        const float height = static_cast<float>(shownRows) * desc.rowHeight;
        const auto total = static_cast<float>(view.size());
        const float length = std::max(THUMB_MIN, height * static_cast<float>(shownRows) / total);
        const float travel = (height - length) * m_scale;
        if (travel > 0.0f && total > static_cast<float>(shownRows))
        {
            scrollBy(pixels * (total - static_cast<float>(shownRows)) / travel);
        }
    }

    void UiDropdownState::bindRows()
    {
        World& world = scope->world();
        for (u32 r = 0; r < rows.size(); ++r)
        {
            const u32 position = first + r;
            Label& row = rows[r];
            if (r >= shownRows || position >= view.size())
            {
                if (m_stamps[r].item != NONE)
                {
                    world.getComponent<UiStateComponent>(row.entity()).visible = false;
                    rowIcons[r].setVisible(false);
                    scope->markLayoutDirty(row.entity());
                    m_stamps[r] = {};
                }
                continue;
            }
            const u32 item = view[position];
            const UiChoiceItem& choice = items[item];
            const bool selected = !choice.header && static_cast<i32>(item) == shownSelected;
            const RowStamp stamp{item, revision, selected};
            if (m_stamps[r] == stamp)
            {
                continue;
            }
            if (choice.key.isValid())
            {
                row.setLocalizedText(choice.key);
            }
            else
            {
                row.setText(choice.label);
            }
            const bool hasIcon = !choice.header && !choice.icon.empty();
            Label& icon = rowIcons[r];
            icon.setVisible(hasIcon);
            icon.setInteractable(choice.enabled && !choice.header);
            if (hasIcon)
                icon.setIcon(choice.icon);
            const float iconWidth = hasIcon ? desc.rowHeight : 0.0f;
            setRect(*scope, icon.entity(), Vec2{ROW_TEXT_INSET, topInset() + static_cast<float>(r) * desc.rowHeight},
                    Vec2{iconWidth, desc.rowHeight});
            setRect(*scope, row.entity(),
                    Vec2{ROW_TEXT_INSET + iconWidth, topInset() + static_cast<float>(r) * desc.rowHeight},
                    Vec2{std::max(0.0f, m_width - ROW_TEXT_INSET - ROW_INSET - iconWidth), desc.rowHeight});
            row.setInteractable(choice.enabled && !choice.header);
            row.setStyleClass(InternedString(choice.header ? "dropdown.header"
                                                           : (selected ? "dropdown.item.selected" : "dropdown.item")));
            if (choice.header)
            {
                row.setAccessibleRole(UiRole::Label);
                if (auto* accessible = world.tryGetComponent<UiAccessibleComponent>(row.entity()))
                {
                    accessible->hasSelected = false;
                    accessible->selected = false;
                }
            }
            else
            {
                row.setAccessibleRole(desc.menu ? UiRole::MenuItem : UiRole::Option);
                row.setAccessibleSelected(selected);
            }
            if (!world.getConstComponent<UiStateComponent>(row.entity()).visible)
            {
                world.getComponent<UiStateComponent>(row.entity()).visible = true;
                scope->markLayoutDirty(row.entity());
            }
            m_stamps[r] = stamp;
        }
        showMark();
    }

    i32 UiDropdownState::positionAt(const Vec2 pointer) const
    {
        const UiRect rect = scope->hierarchy().absoluteRectOf(popup.entity());
        const float y = (pointer.y - rect.position.y) / m_scale - topInset();
        if (y < 0.0f || y >= shownRows * desc.rowHeight)
            return -1;
        const u32 position = first + static_cast<u32>(y / desc.rowHeight);
        return position < view.size() ? static_cast<i32>(position) : -1;
    }

    void UiDropdownState::showMark()
    {
        i32 wanted = -1;
        if (listOpen && shownSelected >= 0)
        {
            const auto at = std::ranges::lower_bound(view, static_cast<u32>(shownSelected));
            const auto position = static_cast<u32>(at - view.begin());
            if (at != view.end() && *at == static_cast<u32>(shownSelected) && position >= first &&
                position < first + shownRows)
            {
                wanted = static_cast<i32>(position - first);
            }
        }
        if (wanted == m_shownMark)
        {
            return;
        }
        m_shownMark = wanted;
        World& world = scope->world();
        const bool visible = wanted >= 0;
        if (world.getConstComponent<UiStateComponent>(markBar.entity()).visible != visible)
        {
            world.getComponent<UiStateComponent>(markBar.entity()).visible = visible;
            scope->markLayoutDirty(markBar.entity());
        }
        if (visible)
        {
            setRect(*scope, markBar.entity(), Vec2{ROW_INSET, topInset() + static_cast<float>(wanted) * desc.rowHeight},
                    Vec2{MARK_WIDTH, desc.rowHeight});
        }
    }

    void UiDropdownState::showHighlight()
    {
        showMark();
        const bool visible = listOpen && highlighted >= 0 && static_cast<u32>(highlighted) >= first &&
            static_cast<u32>(highlighted) < first + shownRows;
        const i32 wanted = visible ? static_cast<i32>(static_cast<u32>(highlighted) - first) : -1;
        if (wanted == m_shownHighlight)
        {
            return;
        }
        m_shownHighlight = wanted;
        World& world = scope->world();
        if (world.getConstComponent<UiStateComponent>(highlightBar.entity()).visible != visible)
        {
            world.getComponent<UiStateComponent>(highlightBar.entity()).visible = visible;
            scope->markLayoutDirty(highlightBar.entity());
        }
        if (visible)
        {
            setRect(*scope, highlightBar.entity(), Vec2{0.0f, topInset() + static_cast<float>(wanted) * desc.rowHeight},
                    Vec2{m_width, desc.rowHeight});
        }
    }

    void UiDropdownState::showThumb()
    {
        World& world = scope->world();
        const bool visible = listOpen && view.size() > shownRows;
        if (world.getConstComponent<UiStateComponent>(thumb.entity()).visible != visible)
        {
            world.getComponent<UiStateComponent>(thumb.entity()).visible = visible;
            scope->markLayoutDirty(thumb.entity());
        }
        if (!visible)
        {
            return;
        }
        const float height = static_cast<float>(shownRows) * desc.rowHeight;
        const auto total = static_cast<float>(view.size());
        const float length = std::max(THUMB_MIN, height * static_cast<float>(shownRows) / total);
        const float travel = height - length;
        const float at = travel * static_cast<float>(first) / (total - static_cast<float>(shownRows));
        setRect(*scope, thumb.entity(), Vec2{m_width - THUMB_WIDTH - 1.0f, topInset() + at}, Vec2{THUMB_WIDTH, length});
    }

    // A search that matches nothing says so in the row where the first match would be.
    void UiDropdownState::showEmpty()
    {
        const bool visible = listOpen && desc.searchable && view.empty();
        if (!visible && emptyRow.entity() == Entity::undefined())
        {
            return;
        }
        World& world = scope->world();
        if (emptyRow.entity() == Entity::undefined())
        {
            const InternedString id = world.getConstComponent<UiIdentityComponent>(root).id;
            emptyRow = scope->createLabel(childId(id, ".empty"), "", topLeft());
            emptyRow.setParent(popup);
            emptyRow.setStyleClass(InternedString("dropdown.empty"));
            emptyRow.setLocalizedText(InternedString("engine.ui.popup.empty"));
            emptyRow.setInteractable(false);
        }
        if (world.getConstComponent<UiStateComponent>(emptyRow.entity()).visible != visible)
        {
            world.getComponent<UiStateComponent>(emptyRow.entity()).visible = visible;
            scope->markLayoutDirty(emptyRow.entity());
        }
        if (visible)
        {
            setRect(*scope, emptyRow.entity(), Vec2{ROW_INSET, topInset()},
                    Vec2{std::max(0.0f, m_width - 2.0f * ROW_INSET), desc.rowHeight});
        }
    }

    void UiDropdownState::setHighlight(const i32 position, const bool scroll)
    {
        highlighted = position;
        m_scrolledTo = position; // a hovered row the wheel later scrolled out of view stays put
        if (scroll)
        {
            const u32 before = first;
            scrollTo(position);
            if (first != before)
            {
                bindRows();
                showThumb();
            }
        }
        showHighlight();
        publish();
    }

    // -- UiDropdownState: keyboard and text ---------------------------------------------------------------------------

    bool UiDropdownState::moveHighlight(const u32 scancode)
    {
        if (view.empty())
        {
            return false;
        }
        const auto last = static_cast<i32>(view.size()) - 1;
        const auto page = static_cast<i32>(std::max(shownRows, 1u));
        // the enabled item nearest `target`, looking along `direction` first
        const auto near = [this, last](const i32 target, const i32 direction)
        {
            for (i32 i = target; i >= 0 && i <= last; i += direction)
            {
                if (navigable(view[static_cast<u32>(i)]))
                {
                    return i;
                }
            }
            for (i32 i = target; i >= 0 && i <= last; i -= direction)
            {
                if (navigable(view[static_cast<u32>(i)]))
                {
                    return i;
                }
            }
            return -1;
        };
        i32 next = -1;
        switch (scancode)
        {
        case Key::DOWN:
            next = nextEnabled(highlighted, 1);
            break;
        case Key::UP:
            next = nextEnabled(highlighted, -1);
            break;
        case Key::PAGE_DOWN:
            next = near(std::min(std::max(highlighted, 0) + page, last), 1);
            break;
        case Key::PAGE_UP:
            next = near(std::max(highlighted - page, 0), -1);
            break;
        case Key::HOME:
            next = near(0, 1);
            break;
        case Key::END:
            next = near(last, -1);
            break;
        default:
            return false;
        }
        m_typed.clear();
        if (next >= 0 && next != highlighted)
        {
            setHighlight(next, true);
        }
        return true;
    }

    // Typing in a list without a search box selects by prefix: the characters typed since the last navigation key, else
    // (nothing starts with them) the last one alone, from the row after the highlight.
    void UiDropdownState::typeAhead(const std::string_view text)
    {
        if (!listOpen || view.empty())
        {
            return;
        }
        std::string scratch;
        const auto count = static_cast<i32>(view.size());
        const auto find = [&](const std::string_view prefix, const i32 from)
        {
            for (i32 step = 0; step < count; ++step)
            {
                const i32 position = (from + step) % count;
                const u32 item = view[static_cast<u32>(position)];
                if (navigable(item) && startsWithNoCase(textOf(items[item], scratch), prefix))
                {
                    return position;
                }
            }
            return -1;
        };
        for (const char c : text)
        {
            if (static_cast<unsigned char>(c) >= 0x20)
            {
                m_typed.push_back(c);
            }
        }
        if (m_typed.empty())
        {
            return;
        }
        i32 hit = find(m_typed, std::max(highlighted, 0));
        if (hit < 0 && m_typed.size() > text.size())
        {
            m_typed = text;
            hit = find(m_typed, highlighted + 1);
        }
        if (hit >= 0 && hit != highlighted)
        {
            setHighlight(hit, true);
        }
    }

    bool UiDropdownState::key(const UiKeyDownEvent& event)
    {
        constexpr u8 CHORDS = UiKeyModifier::CTRL | UiKeyModifier::ALT | UiKeyModifier::SUPER;
        if ((event.modifiers & CHORDS) != 0 || !interactableNow(scope->world(), root))
        {
            return false;
        }
        const u32 code = event.scancode;
        const bool activate = code == Key::SPACE || code == Key::RETURN || code == Key::KP_ENTER;
        if (!listOpen)
        {
            if (!activate && code != Key::DOWN && code != Key::UP)
            {
                return false;
            }
            if (!event.repeat) // a held key opens once
            {
                open();
            }
            return true;
        }
        if (code == Key::ESCAPE)
        {
            if (!event.repeat)
            {
                close();
            }
            return true;
        }
        if (activate)
        {
            if (!event.repeat)
            {
                if (highlighted >= 0 && static_cast<u32>(highlighted) < view.size())
                {
                    choose(view[static_cast<u32>(highlighted)]);
                }
                else
                {
                    close();
                }
            }
            return true;
        }
        // the open list claims the sideways keys and intents too: they must not walk the focus out of it, and a typing
        // key (the type-ahead's), which must not reach the editor's bare-key hotkeys
        return moveHighlight(code) || code == Key::LEFT || code == Key::RIGHT ||
            (code >= Key::TYPING_FIRST && code <= Key::TYPING_LAST);
    }

    void UiDropdownState::comboKey(const u32 scancode, const u8 modifiers)
    {
        if (scancode == Key::TAB) // an input with an onKey leaves Tab to the callback: focus moves on
        {
            scope->router().moveFocus((modifiers & UiKeyModifier::SHIFT) != 0);
            return;
        }
        if (!interactableNow(scope->world(), root))
        {
            return;
        }
        if (!listOpen)
        {
            if (scancode == Key::DOWN || scancode == Key::UP)
            {
                open();
            }
            return;
        }
        moveHighlight(scancode);
    }

    void UiDropdownState::filterChanged(const std::string_view text)
    {
        // a search box edits a list that is open (Escape reverts its text as the list closes); only a combo's typing
        // opens
        if (!interactableNow(scope->world(), root) || (!listOpen && kind != UiWidgetKind::Combo))
        {
            return;
        }
        filter = text;
        filtering = true;
        rebuildView();
        first = 0;
        if (view.empty() && !desc.searchable)
        {
            close(false);
            return;
        }
        if (!listOpen)
        {
            open();
            return;
        }
        highlighted = nextEnabled(-1, 1);
        m_scrolledTo = -2;
        m_shownHighlight = -2;
        place();
        publish();
    }

    void UiDropdownState::submit(const std::string_view text)
    {
        if (!interactableNow(scope->world(), root))
        {
            return;
        }
        if (listOpen && highlighted >= 0 && static_cast<u32>(highlighted) < view.size())
        {
            choose(view[static_cast<u32>(highlighted)]);
            return;
        }
        if (desc.searchable) // nothing matches: the list stays for the next try
        {
            return;
        }
        std::string scratch;
        for (u32 i = 0; i < items.size(); ++i)
        {
            if (navigable(i) && equalsNoCase(textOf(items[i], scratch), text))
            {
                choose(i);
                return;
            }
        }
        close();
    }

    // -- UiScope
    // --------------------------------------------------------------------------------------------------------

    UiDropdownState* UiScope::dropdownState(const Entity element) const
    {
        if (m_dropdowns.empty())
        {
            return nullptr;
        }
        u64 root = element.id();
        if (const auto part = m_valueParts.find(root); part != m_valueParts.end())
        {
            root = part->second;
        }
        const auto it = m_dropdowns.find(root);
        return it == m_dropdowns.end() ? nullptr : it->second.get();
    }

    void UiScope::refitDropdowns()
    {
        for (const auto& state : m_dropdowns | std::views::values)
        {
            state->refit();
        }
        hierarchy().resolveDirty(); // a re-placed list is laid out in this frame
    }

    // The root of a dropdown or combo: a row holding the face and the arrow, its state registered with the scope.
    Entity UiScope::createChoice(const InternedString id, const UiDropdownDesc& desc, const UiWidgetKind kind,
                                 const bool standalone)
    {
        if (standalone) // a popup list is its list: a root surface, hidden until it opens
        {
            UiRectDesc list;
            list.pivot = Vec2{0.0f, 0.0f};
            list.zOffset = desc.zOffset;
            const Entity entity = createElement(id, UiWidgetKind::Panel, list);
            m_world->addComponent<UiDropdownComponent>(entity, UiDropdownComponent{});
            auto& state = m_dropdowns[entity.id()] = std::make_unique<UiDropdownState>(*this, kind, entity, desc, true);
            state->buildStandalone();
            return entity;
        }
        UiRectDesc rect = desc.rect;
        if (rect.size == Vec2{})
        {
            rect.size = DEFAULT_SIZE;
        }
        UiLayoutComponent layout;
        layout.direction = UiLayoutDirection::Row;
        layout.align = UiLayoutAlign::Stretch;
        layout.padding = Vec4{ROOT_PADDING_LEFT, 0.0f, ROOT_PADDING_RIGHT, 0.0f};
        rect.layout = layout;
        const Entity entity = createElement(id, kind, rect);
        m_world->getComponent<UiStateComponent>(entity).tabStop = kind == UiWidgetKind::Dropdown;
        m_world->addComponent<UiDropdownComponent>(entity, UiDropdownComponent{});
        auto& state = m_dropdowns[entity.id()] = std::make_unique<UiDropdownState>(*this, kind, entity, desc, false);
        state->build();
        return entity;
    }

    Dropdown UiScope::createDropdown(const InternedString id, const UiDropdownDesc& desc)
    {
        return Dropdown(this, createChoice(id, desc, UiWidgetKind::Dropdown), m_generation);
    }

    Combo UiScope::createCombo(const InternedString id, const UiDropdownDesc& desc)
    {
        return Combo(this, createChoice(id, desc, UiWidgetKind::Combo), m_generation);
    }

    PopupList UiScope::createPopupList(const InternedString id, const UiDropdownDesc& desc)
    {
        return PopupList(this, createChoice(id, desc, UiWidgetKind::Dropdown, true), m_generation);
    }

    // -- Handles
    // ---------------------------------------------------------------------------------------------------------

    namespace
    {
        UiDropdownState* stateOf(const Widget& widget)
        {
            if (!widget.isValid())
            {
                SASSERTM(false, "Stale UI widget handle: the owning scope died or the element was destroyed.");
                return nullptr;
            }
            return widget.scope()->dropdownState(widget.entity());
        }
    } // namespace

    void Dropdown::setItems(const std::span<const UiChoiceItem> items)
    {
        if (UiDropdownState* state = stateOf(*this))
        {
            state->setItems(items);
        }
    }

    u32 Dropdown::itemCount() const
    {
        const UiDropdownState* state = stateOf(*this);
        return state ? static_cast<u32>(state->items.size()) : 0;
    }

    void Dropdown::setSelected(const i32 index)
    {
        if (UiDropdownState* state = stateOf(*this))
        {
            state->assign(index);
        }
    }

    i32 Dropdown::selected() const
    {
        return stateOf(*this) ? m_scope->world().getConstComponent<UiDropdownComponent>(m_entity).selected : -1;
    }

    void Dropdown::onChange(std::function<void(u32)> fn)
    {
        if (UiDropdownState* state = stateOf(*this))
        {
            state->onSelect = std::move(fn);
        }
    }

    void Dropdown::open()
    {
        if (UiDropdownState* state = stateOf(*this))
        {
            state->open();
        }
    }

    void Dropdown::close()
    {
        if (UiDropdownState* state = stateOf(*this))
        {
            state->close();
        }
    }

    bool Dropdown::isOpen() const
    {
        const UiDropdownState* state = stateOf(*this);
        return state && state->listOpen;
    }

    void Dropdown::setPlaceholderKey(const InternedString key)
    {
        if (UiDropdownState* state = stateOf(*this))
        {
            state->setPlaceholder(key);
        }
    }

    Label Dropdown::label() const
    {
        const UiDropdownState* state = stateOf(*this);
        return state && state->label != Entity::undefined() ? Label(m_scope, state->label, m_generation) : Label{};
    }

    TextInput Combo::input() const
    {
        const UiDropdownState* state = stateOf(*this);
        return state ? TextInput(m_scope, state->input, m_generation) : TextInput{};
    }

    void PopupList::setItems(const std::span<const UiChoiceItem> items)
    {
        if (UiDropdownState* state = stateOf(*this))
        {
            state->setItems(items);
        }
    }

    void PopupList::setSearchMetadata(const std::span<const std::string> terms,
                                      const std::span<const std::string> exact)
    {
        if (UiDropdownState* state = stateOf(*this))
        {
            state->searchTerms.assign(terms.begin(), terms.end());
            state->exactSearch.assign(exact.begin(), exact.end());
        }
    }

    u32 PopupList::itemCount() const
    {
        const UiDropdownState* state = stateOf(*this);
        return state ? static_cast<u32>(state->items.size()) : 0;
    }

    void PopupList::setSelected(const i32 index)
    {
        if (UiDropdownState* state = stateOf(*this))
        {
            state->assign(index);
        }
    }

    i32 PopupList::selected() const
    {
        return stateOf(*this) ? m_scope->world().getConstComponent<UiDropdownComponent>(m_entity).selected : -1;
    }

    void PopupList::onChoose(std::function<void(u32)> fn)
    {
        if (UiDropdownState* state = stateOf(*this))
        {
            state->onSelect = std::move(fn);
        }
    }

    void PopupList::open(const Widget& anchor)
    {
        if (UiDropdownState* state = stateOf(*this); state && anchor.isValid())
        {
            state->openAt(anchor.entity());
        }
    }

    bool PopupList::keyDown(const u32 scancode)
    {
        UiDropdownState* state = stateOf(*this);
        return state && state->forwardKey(scancode);
    }

    void PopupList::close()
    {
        if (UiDropdownState* state = stateOf(*this))
        {
            state->close();
        }
    }

    bool PopupList::isOpen() const
    {
        const UiDropdownState* state = stateOf(*this);
        return state && state->listOpen;
    }
} // namespace spite
