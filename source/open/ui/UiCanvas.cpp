#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>

#include "base/Assert.hpp"
#include "base/Logging.hpp"
#include "ecs/World.hpp"
#include "ui/UiCanvas.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiScope.hpp"

// The canvas element: the presenter's primitive list, its content generation, and local-space pointer
// events. The view lowers the list (uiview/UiPainter.cpp).

namespace spite
{
    namespace
    {
        bool finite(const Vec2 v) { return std::isfinite(v.x) && std::isfinite(v.y); }

        // Unique across tables, so a reader keyed by revision or generation never mistakes one UI's series for
        // another's (the render shim's copies follow the sources' positions).
        std::atomic<u64> g_seriesRevision{0};
        std::atomic<u32> g_seriesGeneration{0};
    } // namespace

    // -- UiCanvasPainter ------------------------------------------------------------------------------------------

    void UiCanvasPainter::clear()
    {
        m_content->primitives.clear();
        m_content->extra = 0;
        m_content->texts.clear();
        m_content->regions.clear();
    }

    void UiCanvasPainter::reserve(const u32 count)
    {
        m_content->primitives.reserve(m_content->primitives.size() +
                                      std::min({count, m_content->maxPrimitives, m_content->allowed}));
    }

    bool UiCanvasPainter::add(const UiCanvasPrimitive& primitive, const u32 extra)
    {
        if (m_content->charged() + 1 + extra > std::min(m_content->maxPrimitives, m_content->allowed) ||
            !finite(primitive.a) || !finite(primitive.b) || !finite(primitive.c) || !std::isfinite(primitive.width))
        {
            ++m_dropped;
            return false;
        }
        m_content->primitives.push_back(primitive);
        m_content->extra += extra;
        return true;
    }

    bool UiCanvasPainter::rect(const Vec2 position, const Vec2 size, const u32 color, const float cornerRadius)
    {
        UiCanvasPrimitive primitive;
        primitive.kind = UiCanvasKind::Rect;
        primitive.color = color;
        primitive.a = position;
        primitive.b = size;
        primitive.width = cornerRadius;
        return add(primitive);
    }

    bool UiCanvasPainter::line(const Vec2 from, const Vec2 to, const float width, const u32 color)
    {
        UiCanvasPrimitive primitive;
        primitive.kind = UiCanvasKind::Line;
        primitive.color = color;
        primitive.a = from;
        primitive.b = to;
        primitive.width = width;
        return add(primitive);
    }

    bool UiCanvasPainter::polyline(const Vec2* points, const u32 count, const float width, const u32 color)
    {
        bool all = true;
        for (u32 i = 1; i < count; ++i)
        {
            all = line(points[i - 1], points[i], width, color) && all;
        }
        return all;
    }

    bool UiCanvasPainter::triangle(const Vec2 a, const Vec2 b, const Vec2 c, const u32 color)
    {
        UiCanvasPrimitive primitive;
        primitive.kind = UiCanvasKind::Triangle;
        primitive.color = color;
        primitive.a = a;
        primitive.b = b;
        primitive.c = c;
        return add(primitive);
    }

    bool UiCanvasPainter::text(const Vec2 position, const Vec2 size, const std::string_view text, const u32 color,
                               const UiCanvasAlign align, const float textSize)
    {
        UiCanvasPrimitive primitive;
        primitive.kind = UiCanvasKind::Text;
        primitive.color = color;
        primitive.align = align;
        primitive.a = position;
        primitive.b = size;
        primitive.width = textSize;
        primitive.text = static_cast<u32>(m_content->texts.size());
        if (!add(primitive))
        {
            return false;
        }
        m_content->texts.emplace_back(text);
        return true;
    }

    bool UiCanvasPainter::region(const u32 id, const Vec2 position, const Vec2 size)
    {
        if (m_content->regions.size() >= UI_CANVAS_MAX_REGIONS || !finite(position) || !finite(size))
        {
            ++m_regionsDropped;
            return false;
        }
        m_content->regions.push_back({id, position, size});
        return true;
    }

    bool UiCanvasPainter::series(const UiSeriesId id, const Vec2 position, const Vec2 size, const u32 color,
                                 const float width, const UiSeriesStyle style)
    {
        UiCanvasPrimitive primitive;
        primitive.kind = UiCanvasKind::Series;
        primitive.style = style;
        primitive.color = color;
        primitive.a = position;
        primitive.b = size;
        primitive.width = width;
        primitive.text = id.slot;
        primitive.c.x = std::bit_cast<float>(id.generation); // a stale id (the slot reused) draws nothing
        const u32 columns = finite(size) && size.x > 0.0f
            ? std::clamp(static_cast<u32>(std::min(std::ceil(size.x - 1e-3f), float(UI_SERIES_MAX_COLUMNS))), 1u,
                         UI_SERIES_MAX_COLUMNS)
            : 1u;
        return add(primitive, columns - 1);
    }

    // -- UiSeriesTable --------------------------------------------------------------------------------------------

    UiSeries* UiSeriesTable::live(const UiSeriesId id)
    {
        if (id.slot >= m_slots.size())
        {
            return nullptr;
        }
        UiSeries& series = m_slots[id.slot];
        return series.owner != 0 && series.generation == id.generation ? &series : nullptr;
    }

    const UiSeries* UiSeriesTable::find(const UiSeriesId id) const
    {
        return const_cast<UiSeriesTable*>(this)->live(id);
    }

    const UiSeries* UiSeriesTable::at(const u32 slot) const
    {
        return slot < m_slots.size() && m_slots[slot].owner != 0 ? &m_slots[slot] : nullptr;
    }

    UiSeriesId UiSeriesTable::create(const u64 owner, const u32 capacity)
    {
        SASSERT(owner != 0);
        const u32 samples = std::clamp(capacity, 1u, UI_SERIES_MAX_CAPACITY);
        if (m_samples + samples > m_budget)
        {
            ++m_refused;
            m_noticePending = std::exchange(m_noticeArmed, false);
            return {};
        }
        auto slot = static_cast<u32>(std::ranges::find(m_slots, u64{0}, &UiSeries::owner) - m_slots.begin());
        if (slot == m_slots.size())
        {
            m_slots.emplace_back();
        }
        UiSeries& series = m_slots[slot];
        const u32 generation = ++g_seriesGeneration;
        series = UiSeries{};
        series.samples.resize(samples);
        m_samples += samples;
        series.owner = owner;
        series.generation = generation;
        series.version = 1;
        m_revision = ++g_seriesRevision;
        return {slot, generation};
    }

    bool UiSeriesTable::release(const UiSeriesId id)
    {
        UiSeries* series = live(id);
        if (!series)
        {
            return false;
        }
        const u32 generation = series->generation;
        m_samples -= series->capacity();
        m_noticeArmed = true;
        *series = UiSeries{}; // frees the samples
        series->generation = generation;
        m_revision = ++g_seriesRevision;
        return true;
    }

    void UiSeriesTable::releaseOwner(const u64 owner)
    {
        for (u32 slot = 0; slot < m_slots.size(); ++slot)
        {
            if (m_slots[slot].owner == owner)
            {
                release({slot, m_slots[slot].generation});
            }
        }
    }

    bool UiSeriesTable::append(const UiSeriesId id, const std::span<const float> samples)
    {
        UiSeries* series = live(id);
        if (!series || !std::ranges::all_of(samples, [](const float v) { return std::isfinite(v); }))
        {
            return false;
        }
        if (samples.empty())
        {
            return true;
        }
        const u32 capacity = series->capacity();
        // only the newest `capacity` of a long append can stay
        const std::span<const float> kept = samples.size() > capacity ? samples.last(capacity) : samples;
        for (const float value : kept)
        {
            if (series->count < capacity)
            {
                const u32 index = series->head + series->count;
                series->samples[index >= capacity ? index - capacity : index] = value;
                ++series->count;
            }
            else
            {
                series->samples[series->head] = value; // the oldest goes
                series->head = series->head + 1 == capacity ? 0 : series->head + 1;
            }
        }
        series->appended += samples.size();
        ++series->version;
        m_revision = ++g_seriesRevision;
        return true;
    }

    bool UiSeriesTable::setRange(const UiSeriesId id, const UiSeriesRange& range)
    {
        UiSeries* series = live(id);
        if (!series || !std::isfinite(range.min) || !std::isfinite(range.max))
        {
            return false;
        }
        if (series->range != range)
        {
            series->range = range;
            ++series->version;
            m_revision = ++g_seriesRevision;
        }
        return true;
    }

    bool UiSeriesTable::clear(const UiSeriesId id)
    {
        UiSeries* series = live(id);
        if (!series)
        {
            return false;
        }
        series->head = 0;
        series->count = 0;
        series->appended = 0;
        ++series->reset;
        ++series->version;
        m_revision = ++g_seriesRevision;
        return true;
    }

    // -- Canvas
    // ---------------------------------------------------------------------------------------------------------

    namespace
    {
        // Stale-handle guard as the other widgets': assert in dev/Test, no-op + log in release.
        UiScope::CanvasData* dataOf(const Widget& widget)
        {
            if (!widget.isValid())
            {
                SASSERTM(false, "Stale UI widget handle: the owning scope died or the element was destroyed.");
                return nullptr;
            }
            return widget.scope()->canvasData(widget.entity());
        }
    } // namespace

    bool Canvas::update(const std::function<void(UiCanvasPainter&)>& emit) { return run(true, emit); }

    bool Canvas::append(const std::function<void(UiCanvasPainter&)>& emit) { return run(false, emit); }

    bool Canvas::run(const bool replace, const std::function<void(UiCanvasPainter&)>& emit)
    {
        UiScope::CanvasData* data = dataOf(*this);
        if (!data)
        {
            return false;
        }
        UiCanvasContent& content = data->content;
        UiContext& context = *m_scope->m_context;
        const u64 before = content.charged();
        UiCanvasPainter painter(content);
        if (replace)
        {
            painter.clear();
        }
        // the context budget less what the other canvases hold
        const u64 others = context.canvasPrimitives() - before;
        content.allowed = others >= context.canvasBudget() ? 0u : context.canvasBudget() - static_cast<u32>(others);
        emit(painter);
        content.allowed = ~0u;
        context.m_canvasPrimitives += content.charged();
        context.m_canvasPrimitives -= before;
        UiCanvasComponent& component = m_scope->world().getComponent<UiCanvasComponent>(m_entity);
        component.generation = ++content.generation;
        component.primitives = static_cast<u32>(content.primitives.size());
        // an append adds to what earlier appends dropped
        component.dropped = (replace ? 0 : component.dropped) + painter.dropped();
        component.droppedRegions = (replace ? 0 : component.droppedRegions) + painter.regionsDropped();
        const bool dropping = painter.dropped() != 0 || painter.regionsDropped() != 0;
        if (dropping && !data->warned)
        {
            SWARNING_LOG(
                "canvas '{}': {} primitives and {} regions dropped (limits: canvas {}, regions {}, UI total {}, "
                "or non-finite coordinates)",
                m_scope->world().getConstComponent<UiIdentityComponent>(m_entity).id.toView(), painter.dropped(),
                painter.regionsDropped(), content.maxPrimitives, UI_CANVAS_MAX_REGIONS, context.canvasBudget());
        }
        data->warned = dropping;
        m_scope->hierarchy().markPaintDirty(m_entity);
        return !dropping;
    }

    void Canvas::clear()
    {
        update([](UiCanvasPainter&) {});
    }

    u32 Canvas::primitives() const
    {
        const UiScope::CanvasData* data = dataOf(*this);
        return data ? static_cast<u32>(data->content.primitives.size()) : 0;
    }

    u64 Canvas::generation() const
    {
        const UiScope::CanvasData* data = dataOf(*this);
        return data ? data->content.generation : 0;
    }

    u32 Canvas::regionAt(const Vec2 local) const
    {
        const UiScope::CanvasData* data = dataOf(*this);
        if (!data)
        {
            return UI_CANVAS_NO_REGION;
        }
        const std::vector<UiCanvasRegion>& regions = data->content.regions;
        for (auto it = regions.rbegin(); it != regions.rend(); ++it)
        {
            if (local.x >= it->position.x && local.y >= it->position.y && local.x < it->position.x + it->size.x &&
                local.y < it->position.y + it->size.y)
            {
                return it->id;
            }
        }
        return UI_CANVAS_NO_REGION;
    }

    UiSeriesTable* Canvas::ownSeries(const UiSeriesId series) const
    {
        if (!dataOf(*this))
        {
            return nullptr;
        }
        UiSeriesTable& table = m_scope->m_context->m_series;
        const UiSeries* found = table.find(series);
        return found && found->owner == m_entity.id() ? &table : nullptr;
    }

    UiSeriesId Canvas::createSeries(const u32 capacity, const UiSeriesRange& range)
    {
        if (!dataOf(*this))
        {
            return {};
        }
        UiSeriesTable& table = m_scope->m_context->m_series;
        const UiSeriesId id = table.create(m_entity.id(), capacity);
        if (!id.isValid())
        {
            if (table.takeRefusalNotice())
            {
                SWARNING_LOG("canvas '{}': series refused, the UI's series budget of {} samples is spent (held {})",
                             m_scope->world().getConstComponent<UiIdentityComponent>(m_entity).id.toView(),
                             table.budget(), table.samples());
            }
            return id;
        }
        table.setRange(id, range);
        return id;
    }

    bool Canvas::appendSamples(const UiSeriesId series, const std::span<const float> samples)
    {
        UiSeriesTable* table = ownSeries(series);
        return table && table->append(series, samples);
    }

    bool Canvas::setSeriesRange(const UiSeriesId series, const UiSeriesRange& range)
    {
        UiSeriesTable* table = ownSeries(series);
        return table && table->setRange(series, range);
    }

    bool Canvas::clearSeries(const UiSeriesId series)
    {
        UiSeriesTable* table = ownSeries(series);
        return table && table->clear(series);
    }

    bool Canvas::releaseSeries(const UiSeriesId series)
    {
        UiSeriesTable* table = ownSeries(series);
        if (!table)
        {
            return false;
        }
        table->release(series);
        m_scope->hierarchy().markPaintDirty(m_entity); // its primitives stop drawing
        return true;
    }

    const UiSeries* Canvas::series(const UiSeriesId series) const
    {
        const UiSeriesTable* table = ownSeries(series);
        return table ? table->find(series) : nullptr;
    }

    void Canvas::onPointer(std::function<void(const UiCanvasPointer&)> fn)
    {
        UiScope::CanvasData* data = dataOf(*this);
        if (!data)
        {
            return;
        }
        const bool had = static_cast<bool>(data->onPointer);
        data->onPointer = std::move(fn);
        if (static_cast<bool>(data->onPointer) == had)
        {
            return;
        }
        // The reactions stay while a callback is set and go with it: an unobserved canvas holds no router slots.
        UiScope* scope = m_scope;
        const Entity entity = m_entity;
        const bool set = static_cast<bool>(data->onPointer);
        const auto react = [&](const UiEventKind kind, auto&& body)
        {
            if (set)
            {
                scope->setReaction(entity, kind, std::forward<decltype(body)>(body));
            }
            else
            {
                scope->setReaction(entity, kind, nullptr);
            }
        };
        using K = UiCanvasPointerKind;
        react(UiEventKind::HoverEnter,
              [scope, entity](const void* p)
              {
                  scope->canvasPointer(entity, K::Enter, static_cast<const UiHoverEnterEvent*>(p)->position, {}, 0, 0,
                                       {});
              });
        react(UiEventKind::HoverMove,
              [scope, entity](const void* p)
              {
                  scope->canvasPointer(entity, K::Move, static_cast<const UiHoverMoveEvent*>(p)->position, {}, 0, 0,
                                       {});
              });
        react(UiEventKind::HoverLeave,
              [scope, entity](const void* p)
              {
                  scope->canvasPointer(entity, K::Leave, static_cast<const UiHoverLeaveEvent*>(p)->position, {}, 0, 0,
                                       {});
              });
        react(UiEventKind::PointerDown,
              [scope, entity](const void* p)
              {
                  const auto* e = static_cast<const UiPointerDownEvent*>(p);
                  scope->canvasPointer(entity, K::Down, e->position, {}, e->button, e->modifiers, {});
              });
        react(UiEventKind::PointerUp,
              [scope, entity](const void* p)
              {
                  const auto* e = static_cast<const UiPointerUpEvent*>(p);
                  scope->canvasPointer(entity, K::Up, e->position, {}, e->button, 0, {});
              });
        react(UiEventKind::Click,
              [scope, entity](const void* p)
              {
                  const auto* e = static_cast<const UiClickEvent*>(p);
                  scope->canvasPointer(entity, K::Click, e->position, {}, e->button, e->modifiers, {});
              });
        react(UiEventKind::DragStart,
              [scope, entity](const void* p)
              {
                  const auto* e = static_cast<const UiDragStartEvent*>(p);
                  scope->canvasPointer(entity, K::DragStart, e->position, e->delta, 0, 0, {});
              });
        react(UiEventKind::DragMove,
              [scope, entity](const void* p)
              {
                  const auto* e = static_cast<const UiDragMoveEvent*>(p);
                  scope->canvasPointer(entity, K::DragMove, e->position, e->delta, 0, 0, {});
              });
        react(UiEventKind::DragEnd,
              [scope, entity](const void* p)
              {
                  const auto* e = static_cast<const UiDragEndEvent*>(p);
                  scope->canvasPointer(entity, K::DragEnd, e->position, e->delta, 0, 0, {});
              });
        react(UiEventKind::Wheel,
              [scope, entity](const void* p)
              {
                  const auto* e = static_cast<const UiWheelEvent*>(p);
                  scope->canvasPointer(entity, K::Wheel, {}, {}, 0, e->modifiers, e->wheelDelta);
              });
    }

    // -- UiScope ------------------------------------------------------------------------------------------------------

    UiScope::CanvasData* UiScope::canvasData(const Entity element)
    {
        const auto it = m_canvases.find(element.id());
        return it == m_canvases.end() ? nullptr : &it->second;
    }

    Canvas UiScope::createCanvas(const InternedString id, const UiCanvasDesc& desc)
    {
        return createCanvasOfKind(id, UiWidgetKind::Canvas, desc);
    }

    Canvas UiScope::createCanvasOfKind(const InternedString id, const UiWidgetKind kind, const UiCanvasDesc& desc)
    {
        const Entity entity = createElement(id, kind, desc.rect);
        UiCanvasComponent component;
        component.maxPrimitives = std::min(desc.maxPrimitives, UI_CANVAS_MAX_PRIMITIVES);
        m_world->addComponent<UiCanvasComponent>(entity, component);
        CanvasData& data = m_canvases[entity.id()];
        data.content.maxPrimitives = component.maxPrimitives;
        data.content.series = &m_context->m_series;
        m_context->m_canvasIndex[entity.id()] = &data.content;
        return Canvas(this, entity, m_generation);
    }

    void UiScope::canvasPointer(const Entity element, const UiCanvasPointerKind kind, const Vec2 position,
                                const Vec2 delta, const u8 button, const u8 modifiers, const Vec2 wheel)
    {
        CanvasData* data = canvasData(element);
        if (!data || !data->onPointer)
        {
            return;
        }
        const float scale = std::max(hierarchy().layoutScale(), 1e-6f);
        UiCanvasPointer event;
        event.kind = kind;
        // a wheel event carries no position: it is the last one the pointer had over this canvas
        if (kind != UiCanvasPointerKind::Wheel)
        {
            data->lastLocal = (position - hierarchy().absoluteRectOf(element).position) * (1.0f / scale);
        }
        event.local = data->lastLocal;
        event.delta = delta * (1.0f / scale);
        event.wheel = wheel;
        event.button = button;
        event.modifiers = modifiers;
        const std::vector<UiCanvasRegion>& regions = data->content.regions;
        for (auto it = regions.rbegin(); it != regions.rend(); ++it)
        {
            if (event.local.x >= it->position.x && event.local.y >= it->position.y &&
                event.local.x < it->position.x + it->size.x && event.local.y < it->position.y + it->size.y)
            {
                event.region = it->id;
                break;
            }
        }
        const auto callback = data->onPointer; // the callback may destroy the canvas or replace itself
        callback(event);
    }
} // namespace spite
