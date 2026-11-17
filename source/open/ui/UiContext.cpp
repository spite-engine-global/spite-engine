#include "ui/UiContext.hpp"

#include <algorithm>

#include "base/ServiceLocator.hpp"
#include "ecs/Chunk.hpp"
#include "ecs/Query.hpp"
#include "ecs/SystemContext.hpp"
#include "ecs/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiEvents.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiScope.hpp"
#include "ui/UiWidgets.hpp"
#include "UiChartState.hpp"
#include "UiDropdownState.hpp"
#include "UiValueState.hpp"

namespace spite
{
    namespace
    {
        constexpr u32 ESCAPE_KEY = 41; // HID

        auto& inputFilters()
        {
            static std::unordered_map<const UiContext*, std::function<void(UiInputSnapshot&)>> filters;
            return filters;
        }

        void updateUiTweens(SystemContext& ctx)
        {
            ServiceLocator* services = ctx.tryGetServices();
            if (!services || !services->contains<UiContext>())
            {
                return;
            }
            services->write<UiContext>().get().tweens().advance(ctx);
        }

        void updateUiOverlays(SystemContext& ctx)
        {
            ServiceLocator* services = ctx.tryGetServices();
            if (!services || !services->contains<UiContext>())
            {
                return;
            }
            services->write<UiContext>().get().overlays().advance(ctx);
        }
    } // namespace

    namespace
    {
        std::string_view modalId(const World& world, const UiHierarchyManager& hierarchy)
        {
            const auto* identity = world.tryGetConstComponent<UiIdentityComponent>(hierarchy.modalRoot());
            return identity ? identity->id.toView() : std::string_view{};
        }
    } // namespace

    UiContext::UiContext(World& world) : m_world(&world)
    {
        m_manager = std::make_unique<UiHierarchyManager>(world);
        m_router = std::make_unique<UiRouter>(world, *m_manager);
        world.services().registerService(m_manager.get());
        world.services().registerService(this);
        m_manager->setResizeSink(
            [this](const Entity element)
            {
                if (UiScope* owner = ownerOf(element))
                {
                    owner->resized(element);
                }
            });
        m_tweens = std::make_unique<UiTweens>(world, *m_manager);
        m_tweens->registerTable();
        m_bindings = std::make_unique<UiBindings>(world, *m_manager, *m_router);
        m_bindings->registerTable();
        // bindings first: their setters apply at once, and the resolve that follows lays the targets out in this frame
        m_manager->setUpdateHook(
            [this](SystemContext& ctx)
            {
                syncLayoutTokens();
                applyWrittenLayoutTokens(ctx);
                m_bindings->update();
                syncValueWidgets(ctx);
                syncCharts();
            });
        m_overlays = std::make_unique<UiOverlays>(world, *m_manager, *m_router);
        // Escape on a shown tooltip hides it and is spent: focus and an open dialog stay (a held key's repeats too)
        m_router->setKeyClaim(
            [this](const UiKeyTransition& key)
            {
                if (key.scancode != ESCAPE_KEY || key.modifiers != 0)
                {
                    m_escapeSpent = false; // a repeat is spent only with the press this context saw
                    return false;
                }
                if (!key.repeat)
                {
                    const bool hid = m_overlays->dismissTooltip();
                    m_escapeSpent = hid && key.pressed;
                    return hid;
                }
                return m_escapeSpent;
            });
        m_overlays->registerTable();
        m_dragDrop = std::make_unique<UiDragDrop>(world, *m_manager, *m_router);
        m_documents = std::make_unique<UiDocuments>(*this);

        // The UI event vocabulary is this module's; EventTag backs the
        // world's event entities.
        registerComponent<UiRectComponent>();
        registerComponent<UiStateComponent>();
        registerComponent<UiLabelComponent>();
        registerComponent<UiLocalizedTextComponent>();
        registerComponent<UiValueComponent>();
        registerComponent<UiPlaceholderComponent>();
        registerComponent<UiImageComponent>();
        registerComponent<UiIdentityComponent>();
        registerComponent<UiContentSizeComponent>();
        registerComponent<UiLayoutComponent>();
        registerComponent<UiLayoutItemComponent>();
        registerComponent<UiLayoutTokensComponent>();
        registerComponent<UiScrollComponent>();
        registerComponent<UiSlotComponent>();
        registerComponent<UiVirtualListComponent>();
        registerComponent<UiTextEditComponent>();
        registerComponent<UiNumericComponent>();
        registerComponent<UiCanvasComponent>();
        registerComponent<UiChartComponent>();
        registerComponent<UiToggleComponent>();
        registerComponent<UiSliderComponent>();
        registerComponent<UiProgressComponent>();
        registerComponent<UiRadioGroupComponent>();
        registerComponent<UiRadioItemComponent>();
        registerComponent<UiTabBarComponent>();
        registerComponent<UiTabComponent>();
        registerComponent<UiDropdownComponent>();
        registerComponent<UiTreeComponent>();
        registerComponent<UiTreeRowComponent>();
        registerComponent<UiTableComponent>();
        registerComponent<UiTableColumnComponent>();
        registerComponent<UiTableRowComponent>();
        registerComponent<UiDockComponent>();
        registerComponent<UiDockNodeComponent>();
        registerComponent<UiDockPanelComponent>();
        registerComponent<UiMenuComponent>();
        registerComponent<UiMenuItemComponent>();
        registerComponent<UiOverlayComponent>();
        registerComponent<UiTooltipComponent>();
        registerComponent<UiNavigationComponent>();
        registerComponent<UiAccessibleComponent>();
        registerComponent<UiDropTargetComponent>();
        registerComponent<UiVisualComponent>();
        registerComponent<UiStyleAnimComponent>();
        registerComponent<EventTag>();
        registerComponent<UiHoverEnterEvent>();
        registerComponent<UiHoverMoveEvent>();
        registerComponent<UiHoverLeaveEvent>();
        registerComponent<UiPointerDownEvent>();
        registerComponent<UiPointerUpEvent>();
        registerComponent<UiClickEvent>();
        registerComponent<UiDragStartEvent>();
        registerComponent<UiDragMoveEvent>();
        registerComponent<UiDragEndEvent>();
        registerComponent<UiWheelEvent>();
        registerComponent<UiKeyDownEvent>();
        registerComponent<UiTextInputEvent>();
        registerComponent<UiTextCompositionEvent>();
        registerComponent<UiFocusGainedEvent>();
        registerComponent<UiFocusLostEvent>();
        registerComponent<UiTreeDropEvent>();
        registerComponent<UiChartSelectEvent>();
        registerComponent<UiDropEvent>();
        registerComponent<UiMenuActivateEvent>();
        registerComponent<UiTweenFinishedEvent>();

        world.registerSystem(ecs::SystemDescriptor{
            .name = "UiLayoutResolve",
            .fn = &updateUiLayout,
            .update = ecs::SystemPhaseDescriptor{
                .reads = {getComponentId<UiRectComponent>(), getComponentId<UiVisualComponent>(),
                          getComponentId<UiLocalizedTextComponent>(), getComponentId<UiValueComponent>(),
                          getComponentId<UiStyleAnimComponent>(), getComponentId<UiToggleComponent>(),
                          getComponentId<UiSliderComponent>(), getComponentId<UiProgressComponent>(),
                          getComponentId<UiRadioGroupComponent>(), getComponentId<UiTabBarComponent>(),
                          getComponentId<UiDropdownComponent>(), getComponentId<UiLayoutTokensComponent>()},
                // localized text resolves into the label; layout tokens write the bound layout fields
                .writes = {getComponentId<UiLabelComponent>(), getComponentId<UiPlaceholderComponent>(),
                           getComponentId<UiLayoutComponent>(), getComponentId<UiLayoutItemComponent>(),
                           getComponentId<UiContentSizeComponent>()},
                // BEGIN_FRAME, not POST_UPDATE: a paused world skips the update stages but must still
                // re-lay out edits made over control; BEGIN_FRAME runs after UPDATE and POST_UPDATE
                .stage = CoreExecutionStages::BEGIN_FRAME}});

        // UPDATE, so a paused world freezes its tweens; it writes exactly the tweenable components
        world.registerSystem(ecs::SystemDescriptor{
            .name = "UiTweens",
            .enabled = false, // UiTweens::start enables it while a tween runs: an idle UI schedules nothing
            .fn = &updateUiTweens,
            .update = ecs::SystemPhaseDescriptor{
                .writes = {getComponentId<UiRectComponent>(), getComponentId<UiVisualComponent>(),
                           getComponentId<UiContentSizeComponent>(), getComponentId<UiLayoutComponent>(),
                           getComponentId<UiLayoutItemComponent>(), getComponentId<UiStyleAnimComponent>()},
                .stage = CoreExecutionStages::UPDATE}});

        // UPDATE, so a paused world freezes a tooltip's delay; it touches no component
        world.registerSystem(
            ecs::SystemDescriptor{.name = "UiOverlays",
                                  .enabled = false, // a tooltip's delay running enables it
                                  .fn = &updateUiOverlays,
                                  .update = ecs::SystemPhaseDescriptor{.stage = CoreExecutionStages::UPDATE}});
    }

    UiResizeAxis UiContext::resizeAxis() const
    {
        const Entity element = m_router->pressed().isValid() ? m_router->pressed() : m_router->hovered();
        if (!m_manager->isElement(element))
        {
            return UiResizeAxis::None;
        }
        if (const auto* identity = m_world->tryGetConstComponent<UiIdentityComponent>(element))
        {
            const auto id = identity->id.toView();
            if (id.ends_with(".tabs.grip") || id.ends_with(".resize.nw"))
                return UiResizeAxis::DiagonalNorthWestSouthEast;
            if (id.ends_with(".resize.ne") || id.ends_with(".resize.sw"))
                return UiResizeAxis::DiagonalNorthEastSouthWest;
            if (id.ends_with(".resize.w") || id.ends_with(".resize.e"))
                return UiResizeAxis::Horizontal;
            if (id.ends_with(".resize.n") || id.ends_with(".resize.s"))
                return UiResizeAxis::Vertical;
        }
        const auto* accessible = m_world->tryGetConstComponent<UiAccessibleComponent>(element);
        if (!accessible || accessible->role != UiRole::Separator)
        {
            return UiResizeAxis::None;
        }
        const Entity parent = m_manager->parentOf(element);
        if (!m_manager->isElement(parent))
        {
            return UiResizeAxis::None;
        }
        const auto* node = m_world->tryGetConstComponent<UiDockNodeComponent>(parent);
        if (!node || node->kind != UiDockNodeKind::Split)
        {
            return UiResizeAxis::None;
        }
        return node->vertical ? UiResizeAxis::Vertical : UiResizeAxis::Horizontal;
    }

    UiContext::~UiContext()
    {
        inputFilters().erase(this);
        // document views first: the adopted ones own scopes, the presenters' ones lose their context
        m_documents.reset();
        // Force-shutdown live scopes first: presenters may outlive the
        // context, and their widget handles must degrade to invalid instead
        // of dangling. Scope destructors run shutdown again — idempotent —
        // and skip unregistration through the nulled context pointer.
        for (UiScope* scope : m_scopes)
        {
            scope->shutdown();
            scope->m_context = nullptr;
        }
        // Unregister before the manager dies so the frame entry can never
        // reach a dangling service.
        m_tweens.reset();
        m_dragDrop.reset();
        m_overlays.reset();
        m_bindings.reset();
        m_world->services().unregisterService<UiHierarchyManager>();
        m_world->services().unregisterService<UiContext>();
    }

    void UiContext::setInputFilter(std::function<void(UiInputSnapshot&)> filter)
    {
        if (filter)
            inputFilters()[this] = std::move(filter);
        else
            inputFilters().erase(this);
    }

    void UiContext::filterInput(UiInputSnapshot& snapshot) const
    {
        if (const auto it = inputFilters().find(this); it != inputFilters().end())
            it->second(snapshot);
    }

    void UiContext::processInput(const UiInputSnapshot& snapshot)
    {
        m_tweens->retireIdleSystem();
        if (m_dragDrop->active())
        {
            constexpr u32 ESCAPE = 41; // HID
            for (const UiKeyTransition& key : snapshot.keyTransitions)
            {
                if (key.pressed && key.scancode == ESCAPE)
                {
                    m_dragDrop->cancel();
                    break;
                }
            }
        }
        // With nothing focused the router queues no key, so Escape reaches a shown tooltip here; with a focus it is
        // the router's key claim (below), which spends the key
        if (m_router->focused() == Entity::undefined())
        {
            for (const UiKeyTransition& key : snapshot.keyTransitions)
            {
                if (key.pressed && !key.repeat && key.scancode == ESCAPE_KEY && key.modifiers == 0)
                {
                    (void)m_overlays->dismissTooltip();
                    break;
                }
            }
        }
        m_router->processFrame(snapshot);
        m_router->dispatch();
        m_overlays->update();
        m_manager->resolveDirty();
        m_overlays->refit();
        for (UiScope* scope : m_scopes)
        {
            if (scope->m_openDropdowns != 0)
            {
                scope->refitDropdowns();
            }
        }
        showNavigated();
    }

    bool UiContext::navigate(const UiNavIntent intent)
    {
        const bool acted = m_router->navigate(intent);
        showNavigated();
        return acted;
    }

    bool UiContext::moveFocus(const bool backward)
    {
        const bool moved = m_router->moveFocus(backward);
        showNavigated();
        return moved;
    }

    // directional navigation may have focused an element out of a scroll view's port
    void UiContext::showNavigated()
    {
        if (const Entity element = m_router->takeNavigated(); element != Entity::undefined())
        {
            (void)activate(element, Action::ScrollIntoView);
        }
    }

    void UiContext::setSubtreeLayoutTokens(Entity root, std::unordered_map<InternedString, float> values)
    {
        const bool rootChanged = root != m_layoutTokenRoot;
        m_layoutTokenRoot = root;
        m_subtreeTokensChanged |= m_subtreeLayoutTokens.assign(std::move(values)) || rootChanged;
    }

    bool UiContext::applyLayoutTokens(const Entity element)
    {
        const auto* tokens = m_world->tryGetConstComponent<UiLayoutTokensComponent>(element);
        if (!tokens)
        {
            return false;
        }
        const UiLayoutTokenTable* table = &m_layoutTokens;
        if (m_layoutTokenRoot.isValid())
        {
            for (Entity at = element; at.isValid(); at = m_manager->parentOf(at))
                if (at == m_layoutTokenRoot)
                {
                    table = &m_subtreeLayoutTokens;
                    break;
                }
        }
        bool moved = false;
        for (u8 slot = 0; slot < static_cast<u8>(UiLayoutField::Count); ++slot)
        {
            const auto field = static_cast<UiLayoutField>(slot);
            const InternedString token = (*tokens)[field];
            const std::optional<float> value = token.isValid() ? table->find(token) : std::nullopt;
            if (!value)
            {
                continue;
            }
            // documents refuse a negative length; only the basis takes one (its measured size)
            const float length = field == UiLayoutField::Basis ? *value : std::max(*value, 0.0f);
            moved |= writeUiLayoutField(*m_world, element, field, length);
        }
        if (moved)
        {
            m_manager->markLayoutDirty(element);
        }
        return moved;
    }

    void UiContext::applyWrittenLayoutTokens(SystemContext& ctx)
    {
        // a binding written in place (control set, undo, a scene load) fires no event: the write bits are the record
        const ComponentID ids[] = {getComponentId<UiLayoutTokensComponent>()};
        m_tokenScratch.clear();
        ctx.query<ecs::Read<UiLayoutTokensComponent>>()->forEachChangedChunkConst(
            ids, ChangeWindow::ThisFrame,
            [&](const Chunk* chunk)
            {
                const auto written =
                    chunk->modifiedThisFrameByIndex(static_cast<sizet>(chunk->getComponentIndex(ids[0])));
                for (sizet i = 0; i < chunk->size(); ++i)
                {
                    if (written.test(i))
                    {
                        m_tokenScratch.push_back(chunk->entity(i));
                    }
                }
            });
        for (const Entity element : m_tokenScratch)
        {
            applyLayoutTokens(element);
        }
    }

    void UiContext::syncLayoutTokens()
    {
        if (m_layoutTokens.generation() == m_appliedTokenGeneration &&
            m_colorTokens.generation() == m_appliedColorGeneration && !m_subtreeTokensChanged)
        {
            return;
        }
        m_appliedTokenGeneration = m_layoutTokens.generation();
        m_appliedColorGeneration = m_colorTokens.generation();
        m_subtreeTokensChanged = false;
        ++m_layoutTokenPasses;
        m_tokenScratch.clear();
        m_world->createSystemContext().query<ecs::Read<UiLayoutTokensComponent>>()->forEachChunkConst(
            [&](const Chunk* chunk)
            {
                for (sizet i = 0; i < chunk->size(); ++i)
                {
                    m_tokenScratch.push_back(chunk->entity(i));
                }
            });
        for (const Entity element : m_tokenScratch)
        {
            applyLayoutTokens(element);
        }
        for (UiScope* scope : m_scopes)
        {
            scope->notifyLayoutTokens();
        }
    }

    void UiContext::syncValueWidgets(SystemContext& ctx)
    {
        if (!std::ranges::any_of(m_scopes, [](const UiScope* scope) { return scope->hasValueWidgets(); }))
        {
            return;
        }
        // in-place writes (control set, a binding) fire no event: the chunk write bits are the only record
        const ComponentID watched[] = {getComponentId<UiToggleComponent>(),   getComponentId<UiSliderComponent>(),
                                       getComponentId<UiProgressComponent>(), getComponentId<UiRadioGroupComponent>(),
                                       getComponentId<UiTabBarComponent>(),   getComponentId<UiDropdownComponent>()};
        m_writtenValueWidgets.clear();
        ctx.query<ecs::Read<UiRectComponent>>()->forEachChangedChunkConst(
            watched, ChangeWindow::ThisFrame,
            [&](const Chunk* chunk)
            {
                for (const ComponentID id : watched)
                {
                    if (!chunk->hasComponent(id))
                    {
                        continue;
                    }
                    const auto written =
                        chunk->modifiedThisFrameByIndex(static_cast<sizet>(chunk->getComponentIndex(id)));
                    for (sizet i = 0; i < chunk->size(); ++i)
                    {
                        if (written.test(i))
                        {
                            m_writtenValueWidgets.push_back(chunk->entity(i));
                        }
                    }
                }
            });
        for (const Entity root : m_writtenValueWidgets)
        {
            if (UiScope* owner = ownerOf(root))
            {
                owner->syncValueWidget(root);
            }
        }
        // an open list follows its anchor; a UI with none open pays nothing
        for (UiScope* scope : m_scopes)
        {
            if (scope->m_openDropdowns != 0)
            {
                scope->refitDropdowns();
            }
        }
    }

    void UiContext::syncCharts()
    {
        for (UiScope* scope : m_scopes)
        {
            scope->syncCharts();
        }
    }

    bool UiContext::chartReadout(const Entity element, UiChartReadout& out) const
    {
        const UiScope* scope = ownerOf(element);
        const UiChartState* chart = scope ? scope->chartState(element) : nullptr;
        if (!chart)
        {
            return false;
        }
        chart->readout(out);
        return true;
    }

    UiScope* UiContext::ownerOf(const Entity element) const
    {
        const auto owner =
            std::ranges::find_if(m_scopes, [element](const UiScope* scope) { return scope->owns(element); });
        return owner == m_scopes.end() ? nullptr : *owner;
    }

    std::string UiContext::activate(const Entity element, const Action action, const std::string_view text)
    {
        if (!m_manager->isElement(element))
        {
            return "no UI element " + std::to_string(element.id());
        }
        if (!m_manager->isVisibleOf(element))
        {
            return "the element is hidden or not laid out yet";
        }
        const auto* identity = m_world->tryGetConstComponent<UiIdentityComponent>(element);
        // as hit testing: only the element's own flag gates input
        const auto* state = m_world->tryGetConstComponent<UiStateComponent>(element);
        if (action != Action::ScrollIntoView && state && !state->interactable)
        {
            return "the element is not interactable";
        }
        if (action != Action::ScrollIntoView && m_router->blockedByModal(element))
        {
            return "the element is blocked by the open modal '" + std::string(modalId(*m_world, *m_manager)) + "'";
        }
        switch (action)
        {
        case Action::Click:
            m_router->click(element);
            return {};
        case Action::Focus:
            m_router->focusAsKeys(element); // a person tabbing to it: the focused look shows
            return {};
        case Action::SetValue:
            {
                UiScope* scope = ownerOf(element);
                if (UiValueState* widget = scope ? scope->valueState(element) : nullptr)
                {
                    return widget->setFromText(element, text);
                }
                if (UiChartState* chart = scope ? scope->chartState(element) : nullptr)
                {
                    return chart->setFromText(text);
                }
                if (UiDropdownState* dropdown = scope ? scope->dropdownState(element) : nullptr)
                {
                    return dropdown->setFromText(text);
                }
                if (!scope || !identity ||
                    (identity->kind != UiWidgetKind::TextInput && identity->kind != UiWidgetKind::NumberField))
                {
                    return "the element is not a text input, number field, checkbox, toggle, slider, progress bar, "
                           "radio group, tab bar, dropdown, combo or chart";
                }
                TextInput input(scope, element, scope->generation());
                input.select(0, static_cast<u32>(input.value().size()));
                input.insert(text);
                input.apply(UiTextCommand::Commit);
                return {};
            }
        case Action::ScrollIntoView:
            {
                bool scrolled = false;
                bool disabled = false;
                for (Entity view = m_manager->parentOf(element); view != Entity::undefined();
                     view = m_manager->parentOf(view))
                {
                    const auto* scroll = m_world->tryGetConstComponent<UiScrollComponent>(view);
                    UiScope* scope = scroll ? ownerOf(view) : nullptr;
                    if (!scope)
                    {
                        continue;
                    }
                    if (!scroll->enabled)
                    {
                        disabled = true; // clipped, never scrolled: what it hides stays hidden
                        continue;
                    }
                    const UiRect target = m_manager->absoluteRectOf(element);
                    const UiRect port = m_manager->absoluteRectOf(view);
                    const auto shift = [](const float low, const float high, const float portLow, const float portHigh)
                    {
                        if (low < portLow || high - low > portHigh - portLow)
                        {
                            return low - portLow;
                        }
                        return high > portHigh ? high - portHigh : 0.0f;
                    };
                    const Vec2 delta{shift(target.position.x, target.position.x + target.size.x, port.position.x,
                                           port.position.x + port.size.x),
                                     shift(target.position.y, target.position.y + target.size.y, port.position.y,
                                           port.position.y + port.size.y)};
                    ScrollView(scope, view, scope->generation()).scrollBy(delta);
                    m_manager->resolveDirty();
                    scrolled = true;
                }
                return scrolled ? std::string{}
                    : disabled  ? std::string("the scroll views above the element have scrolling off")
                                : std::string("no scroll view above the element");
            }
        }
        return {};
    }

    std::string UiContext::clipboardText() const { return m_clipboard ? m_clipboard->text() : m_localClipboard; }

    void UiContext::setClipboardText(const std::string_view text)
    {
        if (m_clipboard)
        {
            m_clipboard->setText(text);
            return;
        }
        m_localClipboard = text;
    }

    u32 UiContext::countCallbacksIn(const ImageRange& image) const
    {
        u32 count = 0;
        for (const UiScope* scope : m_scopes)
        {
            count += scope->countCallbacksIn(image);
        }
        return count + m_overlays->countCallbacksIn(image) + m_dragDrop->countCallbacksIn(image) +
            m_documents->countCallbacksIn(image);
    }

    void UiContext::registerScope(UiScope* scope) { m_scopes.push_back(scope); }

    void UiContext::unregisterScope(UiScope* scope) { std::erase(m_scopes, scope); }
} // namespace spite
