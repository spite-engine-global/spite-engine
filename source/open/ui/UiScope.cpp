#include "ui/UiScope.hpp"

#include <algorithm>
#include <charconv>
#include <ranges>
#include <string>
#include <vector>

#include "base/Assert.hpp"
#include "base/Logging.hpp"
#include "ecs/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiIcons.hpp"
#include "UiChartState.hpp"
#include "UiDockState.hpp"
#include "UiDropdownState.hpp"
#include "UiMenuState.hpp"
#include "UiTableState.hpp"
#include "UiTreeState.hpp"
#include "UiValueState.hpp"

namespace spite
{
    // -- Widget -----------------------------------------------------------------

    Widget::Widget(UiScope* scope, const Entity entity, const u32 generation) :
        m_scope(scope), m_entity(entity), m_generation(generation)
    {
    }

    bool Widget::isValid() const
    {
        if (!m_scope || m_scope->m_generation != m_generation || m_scope->isShutdown())
        {
            return false;
        }
        return m_scope->hierarchy().isElement(m_entity);
    }

    namespace
    {
        // Stale-handle guard: assert in dev/Test, no-op + log in release.
        const Widget* checkValid(const Widget& widget)
        {
            if (!widget.isValid())
            {
                SASSERTM(false, "Stale UI widget handle: the owning scope died or the element was destroyed.");
                SWARNING_LOG("Stale UI widget handle ignored.");
                return nullptr;
            }
            return &widget;
        }
    } // namespace

    void Widget::setParent(const Widget& parent)
    {
        if (!checkValid(*this) || !checkValid(parent))
        {
            return;
        }
        m_scope->hierarchy().attach(m_entity, parent.m_entity);
    }

    void Widget::setVisible(const bool visible)
    {
        if (!checkValid(*this))
        {
            return;
        }
        if (m_scope->world().getConstComponent<UiStateComponent>(m_entity).visible == visible)
        {
            return;
        }
        m_scope->world().getComponent<UiStateComponent>(m_entity).visible = visible;
        m_scope->markLayoutDirty(m_entity); // hit test and view filter on visibility
    }

    bool Widget::isVisible() const
    {
        if (!checkValid(*this))
        {
            return false;
        }
        return m_scope->world().getConstComponent<UiStateComponent>(m_entity).visible;
    }

    void Widget::setInteractable(const bool interactable)
    {
        if (!checkValid(*this))
        {
            return;
        }
        m_scope->world().getComponent<UiStateComponent>(m_entity).interactable = interactable;
        m_scope->markLayoutDirty(m_entity);
        if (!m_scope->m_valueParts.empty())
        {
            m_scope->propagateInteractable(m_entity, interactable);
        }
    }

    void Widget::setNeighbor(const UiNavIntent direction, const Widget& target)
    {
        if (!checkValid(*this) || direction > UiNavIntent::Right)
        {
            return;
        }
        World& world = m_scope->world();
        if (!world.hasComponent<UiNavigationComponent>(m_entity))
        {
            world.addComponent<UiNavigationComponent>(m_entity, UiNavigationComponent{});
        }
        auto& navigation = world.getComponent<UiNavigationComponent>(m_entity);
        Entity& slot = direction == UiNavIntent::Up ? navigation.up
            : direction == UiNavIntent::Down        ? navigation.down
            : direction == UiNavIntent::Left        ? navigation.left
                                                    : navigation.right;
        slot = target.isValid() ? target.entity() : Entity::undefined();
    }

    namespace
    {
        UiAccessibleComponent& accessibleOf(World& world, const Entity entity)
        {
            if (!world.hasComponent<UiAccessibleComponent>(entity))
            {
                world.addComponent<UiAccessibleComponent>(entity, UiAccessibleComponent{});
            }
            return world.getComponent<UiAccessibleComponent>(entity);
        }
    } // namespace

    void Widget::setAccessibleName(const std::string_view name)
    {
        if (checkValid(*this))
        {
            accessibleOf(m_scope->world(), m_entity).name = std::string(name);
        }
    }

    void Widget::setAccessibleNameKey(const InternedString key)
    {
        if (checkValid(*this))
        {
            accessibleOf(m_scope->world(), m_entity).nameKey = key;
        }
    }

    void Widget::setAccessibleRole(const UiRole role)
    {
        if (checkValid(*this))
        {
            accessibleOf(m_scope->world(), m_entity).role = role;
        }
    }

    void Widget::setLandmark(const bool landmark)
    {
        if (checkValid(*this))
        {
            m_scope->world().getComponent<UiStateComponent>(m_entity).landmark = landmark;
        }
    }

    void Widget::setAccessibleSelected(const bool selected)
    {
        if (checkValid(*this))
        {
            UiAccessibleComponent& accessible = accessibleOf(m_scope->world(), m_entity);
            accessible.hasSelected = true;
            accessible.selected = selected;
        }
    }

    void Widget::setAccessibleExpanded(const UiExpandState state)
    {
        if (!checkValid(*this))
        {
            return;
        }
        const auto* now = m_scope->world().tryGetConstComponent<UiAccessibleComponent>(m_entity);
        if ((now ? now->expand : UiExpandState::None) != state)
        {
            accessibleOf(m_scope->world(), m_entity).expand = state;
        }
    }

    void UiScope::setTooltipText(const Entity element, const InternedString key, const std::string_view shortcut)
    {
        UiOverlays& layer = m_context->overlays();
        if (!key.isValid())
        {
            const auto* current = m_world->tryGetConstComponent<UiTooltipComponent>(element);
            if (current && m_tip.isValid() && current->overlay == m_tip.entity())
            {
                layer.detach(element);
            }
            return;
        }
        if (!m_tip.isValid())
        {
            UiTooltipDesc desc;
            desc.label = false;
            m_tip = createTooltip("ui.tooltip", desc);
            UiRectDesc rowDesc;
            rowDesc.pivot = Vec2{0.0f, 0.0f};
            rowDesc.contentSize = UiContentSizeComponent{};
            UiLayoutComponent row;
            row.direction = UiLayoutDirection::Row;
            row.gap = 10.0f;
            row.align = UiLayoutAlign::Center;
            rowDesc.layout = row;
            Panel line = createPanel("ui.tooltip.row", rowDesc);
            line.setParent(m_tip.content());
            UiRectDesc textDesc = rowDesc;
            textDesc.layout.reset();
            textDesc.contentSize = UiContentSizeComponent{};
            m_tipText = createLabel("ui.tooltip.text", "", textDesc);
            m_tipText.setParent(line);
            m_tipText.setStyleClass("ui.tooltip.text");
            m_tipShortcut = createLabel("ui.tooltip.shortcut", "", textDesc);
            m_tipShortcut.setParent(line);
            m_tipShortcut.setStyleClass("ui.tooltip.shortcut");
            m_tipShortcut.setVisible(false);
            layer.onShow(m_tip.entity(),
                         [this](const Entity target)
                         {
                             const auto* text = m_world->tryGetConstComponent<UiTooltipComponent>(target);
                             if (!text || !m_tipText.isValid())
                             {
                                 return;
                             }
                             m_tipText.setLocalizedText(text->key);
                             m_tipShortcut.setText(text->shortcut);
                             m_tipShortcut.setVisible(!text->shortcut.empty());
                         });
        }
        layer.attach(element, m_tip.entity());
        auto* component = m_world->tryGetComponent<UiTooltipComponent>(element);
        if (!component || (component->key == key && component->shortcut == shortcut))
        {
            return;
        }
        component->key = key;
        component->shortcut = std::string(shortcut);
        if (component->shown) // a shown tooltip follows the new text at once (the refit re-places it)
        {
            m_tipText.setLocalizedText(key);
            m_tipShortcut.setText(shortcut);
            m_tipShortcut.setVisible(!shortcut.empty());
        }
    }

    void Widget::setTooltip(const InternedString key, const std::string_view shortcut)
    {
        if (checkValid(*this))
        {
            m_scope->setTooltipText(m_entity, key, shortcut);
        }
    }

    void Widget::setStyleClass(const InternedString styleClass)
    {
        if (!checkValid(*this))
        {
            return;
        }
        // a const read first: a mutable access marks the component written, and a rebind sets the same class again
        if (m_scope->world().getConstComponent<UiIdentityComponent>(m_entity).styleClass != styleClass)
        {
            m_scope->world().getComponent<UiIdentityComponent>(m_entity).styleClass = styleClass;
            // a composite's drawn parts take the class as a family, not as their own class
            if (const auto bar = m_scope->m_valueWidgets.find(m_entity.id()); bar != m_scope->m_valueWidgets.end())
            {
                bar->second->restyle();
            }
            else if (const auto choice = m_scope->m_dropdowns.find(m_entity.id()); choice != m_scope->m_dropdowns.end())
            {
                choice->second->restyle();
            }
            m_scope->markLayoutDirty(m_entity);
        }
    }

    bool Widget::isInteractable() const
    {
        if (!checkValid(*this))
        {
            return false;
        }
        return m_scope->world().getConstComponent<UiStateComponent>(m_entity).interactable;
    }

    void Widget::setAnchor(const Vec2 anchor)
    {
        if (!checkValid(*this))
        {
            return;
        }
        m_scope->world().getComponent<UiRectComponent>(m_entity).anchor = anchor;
        m_scope->markLayoutDirty(m_entity);
    }

    void Widget::setOffset(const Vec2 offset)
    {
        if (!checkValid(*this))
        {
            return;
        }
        if (m_scope->world().getConstComponent<UiRectComponent>(m_entity).offset == offset)
        {
            return;
        }
        m_scope->world().getComponent<UiRectComponent>(m_entity).offset = offset;
        m_scope->markLayoutDirty(m_entity);
    }

    Vec2 Widget::offset() const
    {
        if (!checkValid(*this))
        {
            return {};
        }
        return m_scope->world().getConstComponent<UiRectComponent>(m_entity).offset;
    }

    void Widget::setSize(const Vec2 size)
    {
        if (!checkValid(*this))
        {
            return;
        }
        if (m_scope->world().getConstComponent<UiRectComponent>(m_entity).size == size)
        {
            return;
        }
        m_scope->world().getComponent<UiRectComponent>(m_entity).size = size;
        m_scope->markLayoutDirty(m_entity);
        m_scope->resized(m_entity);
    }

    Vec2 Widget::size() const
    {
        if (!checkValid(*this))
        {
            return {};
        }
        return m_scope->hierarchy().layoutSizeOf(m_entity);
    }

    void Widget::setPivot(const Vec2 pivot)
    {
        if (!checkValid(*this))
        {
            return;
        }
        m_scope->world().getComponent<UiRectComponent>(m_entity).pivot = pivot;
        m_scope->markLayoutDirty(m_entity);
    }

    void Widget::setZOffset(const i32 zOffset)
    {
        if (!checkValid(*this))
        {
            return;
        }
        m_scope->world().getComponent<UiRectComponent>(m_entity).zOffset = zOffset;
        // sibling paint order; root surfaces stack by it in a full resolve
        const Entity parent = m_scope->hierarchy().parentOf(m_entity);
        if (parent == Entity::undefined())
        {
            m_scope->markLayoutDirty();
            return;
        }
        m_scope->markLayoutDirty(parent);
    }

    namespace
    {
        template <typename T>
        void addOrSet(const World& world, const Entity entity, const T& value)
        {
            if (T* existing = world.tryGetComponent<T>(entity))
            {
                *existing = value;
                return;
            }
            world.addComponent<T>(entity, value);
        }
    } // namespace

    void Widget::setLayout(const UiLayoutComponent& layout)
    {
        if (!checkValid(*this))
        {
            return;
        }
        addOrSet(m_scope->world(), m_entity, layout);
        m_scope->markLayoutDirty(m_entity);
        m_scope->m_context->applyLayoutTokens(m_entity);
    }

    void Widget::setLayoutItem(const UiLayoutItemComponent& item)
    {
        if (!checkValid(*this))
        {
            return;
        }
        addOrSet(m_scope->world(), m_entity, item);
        m_scope->markLayoutDirty(m_entity);
        m_scope->m_context->applyLayoutTokens(m_entity);
    }

    void Widget::bindLayoutToken(const UiLayoutField field, const InternedString token)
    {
        if (!checkValid(*this))
        {
            return;
        }
        const World& world = m_scope->world();
        auto* tokens = world.tryGetComponent<UiLayoutTokensComponent>(m_entity);
        if (!tokens && !token.isValid())
        {
            return;
        }
        if (!tokens)
        {
            world.addComponent<UiLayoutTokensComponent>(m_entity, UiLayoutTokensComponent{});
            tokens = world.tryGetComponent<UiLayoutTokensComponent>(m_entity);
        }
        (*tokens)[field] = token;
        if (tokens->empty())
        {
            world.removeComponent<UiLayoutTokensComponent>(m_entity);
            return;
        }
        m_scope->m_context->applyLayoutTokens(m_entity);
    }

    void Widget::checkAs(const std::type_info& type) const
    {
        if (!isValid())
        {
            return;
        }
        const UiWidgetKind kind = m_scope->world().getConstComponent<UiIdentityComponent>(m_entity).kind;
        const auto is = [&](const std::type_info& handle) { return type == handle; };
        bool fits = true;
        if (is(typeid(Panel)))
            fits = kind == UiWidgetKind::Panel;
        else if (is(typeid(Button)))
            fits = kind == UiWidgetKind::Button;
        else if (is(typeid(Label)))
            fits = kind == UiWidgetKind::Label;
        else if (is(typeid(TextWidget)))
            fits = kind == UiWidgetKind::Button || kind == UiWidgetKind::Label;
        else if (is(typeid(TextInput)))
            fits = kind == UiWidgetKind::TextInput || kind == UiWidgetKind::NumberField;
        else if (is(typeid(NumberField)))
            fits = kind == UiWidgetKind::NumberField;
        else if (is(typeid(Image)))
            fits = kind == UiWidgetKind::Image;
        else if (is(typeid(ScrollView)))
            fits = kind == UiWidgetKind::ScrollView;
        else if (is(typeid(Checkbox)))
            fits = kind == UiWidgetKind::Checkbox;
        else if (is(typeid(Toggle)))
            fits = kind == UiWidgetKind::Toggle;
        else if (is(typeid(Checkable)))
            fits = kind == UiWidgetKind::Checkbox || kind == UiWidgetKind::Toggle;
        else if (is(typeid(Slider)))
            fits = kind == UiWidgetKind::Slider;
        else if (is(typeid(ProgressBar)))
            fits = kind == UiWidgetKind::ProgressBar;
        SASSERTM(fits, "Widget::as<{}>() on an element of another kind", type.name());
    }

    void Widget::react(const UiEventKind kind, std::function<void(const void*)> fn, const CodeOrigin origin)
    {
        if (!checkValid(*this))
        {
            return;
        }
        m_scope->setReaction(m_entity, kind, std::move(fn), origin);
    }

    void Widget::onClick(std::function<void()> fn)
    {
        if (!fn)
        {
            react(UiEventKind::Click, nullptr);
            return;
        }
        const CodeOrigin origin = CodeOrigin::of(fn);
        react(UiEventKind::Click, [fn = std::move(fn)](const void*) { fn(); }, origin);
    }

    void Widget::onPress(std::function<void(u8, Vec2)> fn)
    {
        if (!fn)
        {
            react(UiEventKind::PointerDown, nullptr);
            return;
        }
        const CodeOrigin origin = CodeOrigin::of(fn);
        react(
            UiEventKind::PointerDown,
            [fn = std::move(fn)](const void* payload)
            {
                const auto* event = static_cast<const UiPointerDownEvent*>(payload);
                fn(event->button, event->position);
            },
            origin);
    }

    void Widget::onDrag(std::function<void(Vec2)> fn)
    {
        if (!fn)
        {
            react(UiEventKind::DragMove, nullptr);
            return;
        }
        const CodeOrigin origin = CodeOrigin::of(fn);
        react(
            UiEventKind::DragMove, [fn = std::move(fn)](const void* payload)
            { fn(static_cast<const UiDragMoveEvent*>(payload)->delta); }, origin);
    }

    void Widget::onDragEnd(std::function<void()> fn)
    {
        if (!fn)
        {
            react(UiEventKind::DragEnd, nullptr);
            return;
        }
        const CodeOrigin origin = CodeOrigin::of(fn);
        react(UiEventKind::DragEnd, [fn = std::move(fn)](const void*) { fn(); }, origin);
    }

    void Widget::onWheel(std::function<void(Vec2)> fn)
    {
        if (!fn)
        {
            react(UiEventKind::Wheel, nullptr);
            return;
        }
        const CodeOrigin origin = CodeOrigin::of(fn);
        react(
            UiEventKind::Wheel, [fn = std::move(fn)](const void* payload)
            { fn(static_cast<const UiWheelEvent*>(payload)->wheelDelta); }, origin);
    }

    void Widget::onKeyDown(std::function<bool(u32, u8, bool)> fn)
    {
        if (!checkValid(*this))
        {
            return;
        }
        if (!fn)
        {
            m_scope->setKeyReaction(m_entity, nullptr);
            return;
        }
        const CodeOrigin origin = CodeOrigin::of(fn);
        m_scope->setKeyReaction(
            m_entity, [fn = std::move(fn)](const UiKeyDownEvent& event)
            { return fn(event.scancode, event.modifiers, event.repeat); }, origin);
    }

    void Widget::onTextInput(std::function<void(std::string_view)> fn)
    {
        if (!fn)
        {
            react(UiEventKind::TextInput, nullptr);
            return;
        }
        const CodeOrigin origin = CodeOrigin::of(fn);
        react(
            UiEventKind::TextInput, [fn = std::move(fn)](const void* payload)
            { fn(static_cast<const UiTextInputEvent*>(payload)->text); }, origin);
    }

    void Widget::onFocus(std::function<void(bool)> fn)
    {
        // a text input's own focus reactions start and end its editing: the owner's hook runs after them
        if (UiScope::TextInputData* data = isValid() ? m_scope->textData(m_entity) : nullptr)
        {
            data->ownerFocus = std::move(fn);
            return;
        }
        if (!fn)
        {
            react(UiEventKind::FocusGained, nullptr);
            react(UiEventKind::FocusLost, nullptr);
            return;
        }
        const CodeOrigin origin = CodeOrigin::of(fn);
        react(UiEventKind::FocusGained, [fn](const void*) { fn(true); }, origin);
        react(UiEventKind::FocusLost, [fn](const void*) { fn(false); }, origin);
    }

    void TextWidget::setLocalizedText(const InternedString key)
    {
        if (!checkValid(*this))
        {
            return;
        }
        World& world = m_scope->world();
        if (!key.isValid())
        {
            world.removeComponent<UiLocalizedTextComponent>(m_entity);
            return;
        }
        if (const auto* existing = world.tryGetConstComponent<UiLocalizedTextComponent>(m_entity))
        {
            if (existing->key != key)
            {
                world.getComponent<UiLocalizedTextComponent>(m_entity).key = key;
                m_scope->hierarchy().refreshLocalized(m_entity);
            }
            return;
        }
        UiLocalizedTextComponent component;
        component.key = key;
        world.addComponent<UiLocalizedTextComponent>(m_entity, component);
        m_scope->hierarchy().refreshLocalized(m_entity);
    }

    bool TextWidget::setLocalizedArg(const std::string_view name, const std::string_view value)
    {
        // a leading ' marks a string in the encoding: double it so the value reads back as typed
        return value.starts_with('\'') ? setLocalizedArgText(name, value) : storeLocalizedArg(name, value);
    }

    bool TextWidget::setLocalizedArgText(const std::string_view name, const std::string_view value)
    {
        std::string encoded;
        encoded.reserve(value.size() + 1);
        encoded.append("'").append(value);
        return storeLocalizedArg(name, encoded);
    }

    bool TextWidget::storeLocalizedArg(const std::string_view name, const std::string_view value)
    {
        if (!checkValid(*this) || name.empty() || name.find_first_of("=\n") != std::string_view::npos ||
            value.find('\n') != std::string_view::npos)
        {
            return false;
        }
        const auto* component = m_scope->world().tryGetConstComponent<UiLocalizedTextComponent>(m_entity);
        if (!component)
        {
            return false;
        }
        const std::string_view args = component->args;
        std::string edited;
        edited.reserve(args.size() + name.size() + value.size() + 2);
        bool found = false;
        for (sizet start = 0; start < args.size();)
        {
            sizet end = args.find('\n', start);
            end = end == std::string_view::npos ? args.size() : end;
            const std::string_view line = args.substr(start, end - start);
            start = end + 1;
            if (!edited.empty())
            {
                edited += '\n';
            }
            if (line.size() > name.size() && line[name.size()] == '=' && line.starts_with(name))
            {
                found = true;
                edited.append(name).append("=").append(value);
            }
            else
            {
                edited += line;
            }
        }
        if (!found)
        {
            edited.append(edited.empty() ? "" : "\n").append(name).append("=").append(value);
        }
        if (edited == args)
        {
            return true;
        }
        m_scope->world().getComponent<UiLocalizedTextComponent>(m_entity).args = std::move(edited);
        m_scope->hierarchy().refreshLocalized(m_entity);
        return true;
    }

    bool TextWidget::setLocalizedArg(const std::string_view name, const std::int64_t value)
    {
        return setLocalizedArg(name, std::string_view(std::to_string(value)));
    }

    bool TextWidget::setLocalizedArg(const std::string_view name, const double value)
    {
        char buffer[64];
        const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
        return setLocalizedArg(name, std::string_view(buffer, static_cast<sizet>(result.ptr - buffer)));
    }

    void TextWidget::setRichText(const bool rich)
    {
        if (!checkValid(*this) || isRichText() == rich)
        {
            return;
        }
        m_scope->world().getComponent<UiLabelComponent>(m_entity).rich = rich;
        if (m_scope->hierarchy().isLocalized(m_entity))
        {
            m_scope->hierarchy().refreshLocalized(m_entity);
        }
        m_scope->markLayoutDirty(m_entity);
    }

    bool TextWidget::isRichText() const
    {
        return checkValid(*this) && m_scope->world().getConstComponent<UiLabelComponent>(m_entity).rich;
    }

    bool TextWidget::setIcon(const std::string_view name)
    {
        if (!checkValid(*this))
        {
            return false;
        }
        UiLabelComponent& label = m_scope->world().getComponent<UiLabelComponent>(m_entity);
        const bool known = name.empty() || uiIconCodepoint(name) != 0;
        if (label.icon != name)
        {
            label.icon = name;
            m_scope->markLayoutDirty(m_entity);
            if (!known)
            {
                SWARNING_LOG("setIcon: no icon named '{}' (closest: '{}'); it draws nothing", name,
                             uiIconClosestName(name));
            }
        }
        return known;
    }

    std::string_view TextWidget::icon() const
    {
        return checkValid(*this) ? std::string_view(m_scope->world().getConstComponent<UiLabelComponent>(m_entity).icon)
                                 : std::string_view{};
    }

    bool TextWidget::isLocalized() const { return checkValid(*this) && m_scope->hierarchy().isLocalized(m_entity); }

    void Button::setText(const std::string_view text)
    {
        if (!checkValid(*this))
        {
            return;
        }
        if (m_scope->hierarchy().isLocalized(m_entity))
        {
            m_scope->world().removeComponent<UiLocalizedTextComponent>(m_entity);
        }
        m_scope->world().getComponent<UiLabelComponent>(m_entity).text = text;
        m_scope->markLayoutDirty(m_entity);
    }

    std::string_view Button::text() const
    {
        if (!checkValid(*this))
        {
            return {};
        }
        return m_scope->world().getConstComponent<UiLabelComponent>(m_entity).text;
    }

    void Label::setText(const std::string_view text)
    {
        if (!checkValid(*this))
        {
            return;
        }
        const bool localized = m_scope->hierarchy().isLocalized(m_entity);
        if (!localized && m_scope->world().getConstComponent<UiLabelComponent>(m_entity).text == text)
        {
            return;
        }
        if (localized)
        {
            m_scope->world().removeComponent<UiLocalizedTextComponent>(m_entity);
        }
        m_scope->world().getComponent<UiLabelComponent>(m_entity).text = text;
        m_scope->markLayoutDirty(m_entity);
    }

    std::string_view Label::text() const
    {
        if (!checkValid(*this))
        {
            return {};
        }
        return m_scope->world().getConstComponent<UiLabelComponent>(m_entity).text;
    }

    void Image::setImageSlot(const u64 slot)
    {
        if (!checkValid(*this))
        {
            return;
        }
        m_scope->world().getComponent<UiImageComponent>(m_entity).imageSlot = slot;
        m_scope->markLayoutDirty(m_entity);
    }

    u64 Image::imageSlot() const
    {
        if (!checkValid(*this))
        {
            return 0;
        }
        return m_scope->world().getConstComponent<UiImageComponent>(m_entity).imageSlot;
    }

    // -- UiScope ----------------------------------------------------------------

    UiScope::UiScope(UiContext& context) :
        m_context(&context), m_world(&context.hierarchy().world()),
        m_commandBuffer(context.hierarchy().world().createCommandBuffer())
    {
        m_context->registerScope(this);
    }

    UiScope::~UiScope()
    {
        shutdown();
        // the context nulls this pointer when it force-flushes on its own
        // destruction, so a scope outliving its context skips unregistration
        if (m_context)
        {
            m_context->unregisterScope(this);
        }
    }

    UiHierarchyManager& UiScope::hierarchy() const { return m_context->hierarchy(); }

    UiRouter& UiScope::router() const { return m_context->router(); }

    const UiLayoutTokenTable& UiScope::layoutTokens() const { return m_context->layoutTokens(); }

    const UiColorTokenTable& UiScope::colorTokens() const { return m_context->colorTokens(); }

    u32 UiScope::observeLayoutTokens(std::function<void()> fn)
    {
        m_tokenObservers.emplace_back(m_nextTokenObserver, std::move(fn));
        return m_nextTokenObserver++;
    }

    void UiScope::unobserveLayoutTokens(const u32 id)
    {
        std::erase_if(m_tokenObservers, [id](const auto& observer) { return observer.first == id; });
    }

    void UiScope::notifyLayoutTokens()
    {
        // the ids present now, each looked up before its call: an observer may remove itself or another, and a
        // removed one is not called
        std::vector<u32> ids;
        ids.reserve(m_tokenObservers.size());
        for (const auto& observer : m_tokenObservers)
        {
            ids.push_back(observer.first);
        }
        for (const u32 id : ids)
        {
            const auto it = std::ranges::find(m_tokenObservers, id, &std::pair<u32, std::function<void()>>::first);
            if (it != m_tokenObservers.end())
            {
                const std::function<void()> fn = it->second;
                fn();
            }
        }
    }

    bool UiScope::owns(const Entity entity) const { return m_elements.contains(entity); }

    Entity UiScope::createElement(const InternedString id, const UiWidgetKind kind, const UiRectDesc& desc)
    {
        SASSERTM(id.isValid(), "UI elements need an id for diagnostics and test targeting.");
        // created in its final archetype: one component add after another moves the entity through one archetype
        // per component, which dominated a fresh editor's start
        ComponentID ids[6] = {getComponentId<UiRectComponent>(), getComponentId<UiStateComponent>(),
                              getComponentId<UiIdentityComponent>()};
        sizet count = 3;
        if (desc.contentSize)
        {
            ids[count++] = getComponentId<UiContentSizeComponent>();
        }
        if (desc.layout)
        {
            ids[count++] = getComponentId<UiLayoutComponent>();
        }
        if (desc.layoutItem)
        {
            ids[count++] = getComponentId<UiLayoutItemComponent>();
        }
        const Entity entity = m_world->createEntity(std::span<const ComponentID>(ids, count));
        UiRectComponent& rect = m_world->getComponent<UiRectComponent>(entity);
        rect.anchor = desc.anchor;
        rect.offset = desc.offset;
        rect.size = desc.size;
        rect.pivot = desc.pivot;
        rect.zOffset = desc.zOffset;
        UiIdentityComponent& identity = m_world->getComponent<UiIdentityComponent>(entity);
        identity.id = id;
        identity.kind = kind;
        if (desc.contentSize)
        {
            m_world->getComponent<UiContentSizeComponent>(entity) = *desc.contentSize;
        }
        if (desc.layout)
        {
            m_world->getComponent<UiLayoutComponent>(entity) = *desc.layout;
        }
        if (desc.layoutItem)
        {
            m_world->getComponent<UiLayoutItemComponent>(entity) = *desc.layoutItem;
        }
        m_elements.insert(entity);
        return entity;
    }

    void UiScope::addLabel(UiScope& scope, const Entity entity, const std::string_view text)
    {
        UiLabelComponent label;
        label.text = text;
        scope.m_world->addComponent<UiLabelComponent>(entity, label);
    }

    Panel UiScope::createPanel(const InternedString id, const UiRectDesc& desc)
    {
        return Panel(this, createElement(id, UiWidgetKind::Panel, desc), m_generation);
    }

    Button UiScope::createButton(const InternedString id, const std::string_view text, const UiRectDesc& desc)
    {
        const Entity entity = createElement(id, UiWidgetKind::Button, desc);
        m_world->getComponent<UiStateComponent>(entity).tabStop = true;
        addLabel(*this, entity, text);
        return Button(this, entity, m_generation);
    }

    Label UiScope::createLabel(const InternedString id, const std::string_view text, const UiRectDesc& desc)
    {
        const Entity entity = createElement(id, UiWidgetKind::Label, desc);
        addLabel(*this, entity, text);
        return Label(this, entity, m_generation);
    }

    Image UiScope::createImage(const InternedString id, const UiRectDesc& desc)
    {
        const Entity entity = createElement(id, UiWidgetKind::Image, desc);
        m_world->addComponent<UiImageComponent>(entity, UiImageComponent{});
        return Image(this, entity, m_generation);
    }

    void UiScope::destroy(const Widget& widget)
    {
        SASSERTM(widget.scope() == this, "destroy: widget belongs to another scope.");
        if (widget.scope() != this || !owns(widget.entity()))
        {
            return;
        }
        // a scroll view cannot outlive a part: destroying one destroys the view
        if (const auto part = m_scrollParts.find(widget.entity().id()); part != m_scrollParts.end())
        {
            destroy(Widget(this, part->second, m_generation));
            return;
        }
        // likewise a value widget
        if (const auto part = m_valueParts.find(widget.entity().id()); part != m_valueParts.end())
        {
            destroy(Widget(this, Entity(part->second), m_generation));
            return;
        }
        m_commandBuffer.destroyEntity(widget.entity());
        m_context->router().clearReactions(widget.entity());
        m_context->bindings().removeTarget(widget.entity());
        const bool overlayRoot = m_context->overlays().forget(widget.entity());
        m_context->dragDrop().forget(widget.entity());
        m_textInputs.erase(widget.entity().id());
        m_trees.erase(widget.entity().id());
        if (const auto canvas = m_canvases.find(widget.entity().id()); canvas != m_canvases.end())
        {
            m_context->m_canvasPrimitives -= canvas->second.content.charged();
            m_context->m_canvasIndex.erase(widget.entity().id());
            m_context->m_series.releaseOwner(widget.entity().id());
            m_canvases.erase(canvas);
        }
        m_elements.erase(widget.entity());
        markLayoutDirty();
        // a scroll view's, dock space's, menu's or table's parts (pooled rows,
        // nodes, panel hosts, levels, header and list) are this scope's elements in its subtree
        const u64 id = widget.entity().id();
        bool isContainer = false;
        if (const auto view = m_scrollViews.find(id); view != m_scrollViews.end())
        {
            std::erase_if(m_scrollParts, [widget](const auto& part) { return part.second == widget.entity(); });
            m_scrollViews.erase(view);
            isContainer = true;
        }
        if (m_valueWidgets.erase(id) != 0)
        {
            std::erase_if(m_valueParts, [id](const auto& part) { return part.second == id; });
            isContainer = true;
        }
        if (const auto dropdown = m_dropdowns.find(id); dropdown != m_dropdowns.end())
        {
            // the list is a root surface of its own: it goes with its dropdown
            std::vector<Entity> popup{dropdown->second->popup.entity()};
            m_openDropdowns -= dropdown->second->listOpen ? 1u : 0u;
            m_dropdowns.erase(dropdown);
            std::erase_if(m_valueParts, [id](const auto& part) { return part.second == id; });
            isContainer = true;
            for (sizet at = 0; at < popup.size(); ++at)
            {
                for (const Entity child : hierarchy().childrenOf(popup[at]))
                {
                    popup.push_back(child);
                }
            }
            for (const Entity part : popup)
            {
                if (owns(part))
                {
                    destroy(Widget(this, part, m_generation));
                }
            }
        }
        Entity dockList = Entity::undefined(); // a dock's overflow list is a root surface, not its child
        if (const auto dock = m_docks.find(id); dock != m_docks.end())
        {
            dockList = dock->second->overflowList.entity();
        }
        if (isContainer || overlayRoot || m_docks.erase(id) != 0 || m_menus.erase(id) != 0 || m_tables.erase(id) != 0 ||
            m_charts.erase(id) != 0)
        {
            std::vector<Entity> parts;
            for (std::vector<Entity> stack{widget.entity()}; !stack.empty();)
            {
                const Entity entity = stack.back();
                stack.pop_back();
                for (const Entity child : hierarchy().childrenOf(entity))
                {
                    parts.push_back(child);
                    stack.push_back(child);
                }
            }
            for (const Entity part : parts)
            {
                if (owns(part))
                {
                    destroy(Widget(this, part, m_generation));
                }
            }
        }
        if (dockList != Entity::undefined() && owns(dockList))
        {
            destroy(Widget(this, dockList, m_generation));
        }
    }

    void UiScope::commit() { m_commandBuffer.commit(); }

    Expected<u32> UiScope::bind(const Widget& target, const UiBindingDesc& desc)
    {
        if (!target.isValid() || target.scope() != this)
        {
            return std::unexpected(Error("bind: the target is not a live widget of this scope"));
        }
        return m_context->bindings().add(*this, target.entity(), desc);
    }

    bool UiScope::unbind(const u32 binding) { return m_context->bindings().remove(binding); }

    void UiScope::setReaction(const Entity entity, const UiEventKind kind, UiRouter::ReactionFn fn,
                              const CodeOrigin origin)
    {
        m_context->router().subscribe(entity, kind, std::move(fn), origin);
    }

    void UiScope::setKeyReaction(const Entity entity, UiRouter::KeyReactionFn fn, const CodeOrigin origin)
    {
        m_context->router().subscribeKeys(entity, std::move(fn), origin);
    }

    void UiScope::resized(const Entity element)
    {
        if (m_scrollViews.contains(element.id()))
        {
            syncScroll(element, m_world->getConstComponent<UiScrollComponent>(element).offset, true);
        }
        if (UiDockState* dock = dockState(element))
        {
            dock->layoutAll();
        }
        if (UiTableState* table = tableState(element))
        {
            table->layout();
        }
        if (UiChartState* chart = chartState(element))
        {
            chart->resized();
        }
    }

    void UiScope::markLayoutDirty() const { hierarchy().markLayoutDirty(); }

    void UiScope::markLayoutDirty(const Entity element) const { hierarchy().markLayoutDirty(element); }

    void UiScope::shutdown()
    {
        if (!m_active)
        {
            return;
        }
        m_context->bindings().removeOwner(this);
        m_tip = {};
        m_tipText = {};
        m_tipShortcut = {};
        for (const Entity entity : m_elements)
        {
            m_context->router().clearReactions(entity);
            m_context->overlays().forget(entity);
            m_context->dragDrop().forget(entity);
            if (m_world->isEntityAlive(entity)) // one destroyed with a raw world call is gone already
            {
                m_commandBuffer.destroyEntity(entity);
            }
        }
        m_elements.clear();
        m_tokenObservers.clear();
        m_textInputs.clear();
        m_scrollViews.clear();
        m_scrollParts.clear();
        m_trees.clear();
        m_docks.clear();
        m_menus.clear();
        m_tables.clear();
        m_valueWidgets.clear();
        m_dropdowns.clear();
        m_openDropdowns = 0;
        m_valueParts.clear();
        m_charts.clear();
        for (const auto& [canvas, data] : m_canvases)
        {
            m_context->m_canvasPrimitives -= data.content.charged();
            m_context->m_canvasIndex.erase(canvas);
            m_context->m_series.releaseOwner(canvas);
        }
        m_canvases.clear();
        m_commandBuffer.commit();
        ++m_generation; // all outstanding handles read invalid from here on
        m_active = false;
    }

    void UiScope::clear()
    {
        shutdown();
        m_active = true;
    }

    u32 UiScope::countCallbacksIn(const ImageRange& image) const
    {
        u32 count = 0;
        const auto add = [&](const auto&... callbacks) { count += (u32{image.contains(callbacks)} + ...); };
        for (const auto& input : m_textInputs | std::views::values)
        {
            add(input.onChange, input.onCommit, input.onSubmit, input.onCancel, input.cancelTaken, input.onKey,
                input.onNumber, input.onInvalid, input.onFocus, input.ownerFocus);
        }
        for (const auto& view : m_scrollViews | std::views::values)
        {
            add(view.onScrolled, view.createRow, view.bindRow);
        }
        for (const auto& tree : m_trees | std::views::values)
        {
            add(tree->onSelect, tree->onDrop, tree->onContextMenu, tree->desc.children, tree->desc.parent,
                tree->desc.label);
        }
        for (const auto& dock : m_docks | std::views::values)
        {
            add(dock->onRequest);
        }
        for (const auto& menu : m_menus | std::views::values)
        {
            add(menu->onActivate);
        }
        for (const auto& table : m_tables | std::views::values)
        {
            add(table->onSelect, table->onRequest, table->onDragRows, table->desc.row, table->desc.key);
        }
        for (const auto& observer : m_tokenObservers)
        {
            add(observer.second);
        }
        for (const auto& value : m_valueWidgets | std::views::values)
        {
            add(value->onToggle, value->onValue, value->onSelect, value->onReselect);
        }
        for (const auto& dropdown : m_dropdowns | std::views::values)
        {
            add(dropdown->onSelect);
        }
        for (const auto& canvas : m_canvases | std::views::values)
        {
            add(canvas.onPointer);
        }
        for (const auto& chart : m_charts | std::views::values)
        {
            add(chart->onSelect);
        }
        return count;
    }
} // namespace spite
