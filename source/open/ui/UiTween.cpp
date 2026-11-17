#include "ui/UiTween.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/Schema.hpp"
#include "base/ServiceLocator.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "ecs/SystemContext.hpp"
#include "ecs/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiEvents.hpp"
#include "ui/UiHierarchyManager.hpp"

namespace spite
{
    namespace
    {
        constexpr std::array<std::string_view, static_cast<sizet>(UiEase::Count)> EASE_NAMES = {
            "linear", "inQuad", "outQuad", "inOutQuad", "inCubic", "outCubic", "inOutCubic", "outBack"};

        u32 laneCount(const FieldKind kind)
        {
            switch (kind)
            {
            case FieldKind::F32:
                return 1;
            case FieldKind::Vec2:
                return 2;
            case FieldKind::Vec3:
                return 3;
            case FieldKind::Vec4:
                return 4;
            default:
                return 0;
            }
        }

        float lane(const Vec4& value, const u32 index)
        {
            return index == 0 ? value.x : index == 1 ? value.y : index == 2 ? value.z : value.w;
        }

        void setLane(Vec4& value, const u32 index, const float newValue)
        {
            (index == 0 ? value.x : index == 1 ? value.y : index == 2 ? value.z : value.w) = newValue;
        }

        // The components tweens may write: the "UiTweens" system declares write access to exactly these.
        std::array<ComponentID, 6> tweenables()
        {
            return {getComponentId<UiRectComponent>(),        getComponentId<UiVisualComponent>(),
                    getComponentId<UiContentSizeComponent>(), getComponentId<UiLayoutComponent>(),
                    getComponentId<UiLayoutItemComponent>(),  getComponentId<UiStyleAnimComponent>()};
        }
    } // namespace

    std::string_view uiEaseName(const UiEase ease)
    {
        return ease < UiEase::Count ? EASE_NAMES[static_cast<sizet>(ease)] : std::string_view{};
    }

    std::optional<UiEase> uiEaseFromName(const std::string_view name)
    {
        const auto it = std::ranges::find(EASE_NAMES, name);
        return it == EASE_NAMES.end() ? std::nullopt : std::optional(static_cast<UiEase>(it - EASE_NAMES.begin()));
    }

    float evaluateUiEase(const UiEase ease, float t)
    {
        t = std::clamp(t, 0.0f, 1.0f);
        switch (ease)
        {
        case UiEase::Linear:
        case UiEase::Count:
            return t;
        case UiEase::InQuad:
            return t * t;
        case UiEase::OutQuad:
            return 1.0f - (1.0f - t) * (1.0f - t);
        case UiEase::InOutQuad:
            return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
        case UiEase::InCubic:
            return t * t * t;
        case UiEase::OutCubic:
            return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        case UiEase::InOutCubic:
            return t < 0.5f ? 4.0f * t * t * t : 1.0f - 4.0f * (1.0f - t) * (1.0f - t) * (1.0f - t);
        case UiEase::OutBack:
            {
                constexpr float OVERSHOOT = 1.70158f;
                const float u = t - 1.0f;
                return 1.0f + (OVERSHOOT + 1.0f) * u * u * u + OVERSHOOT * u * u;
            }
        }
        return t;
    }

    struct UiTweens::State
    {
        struct Tween
        {
            UiTweenId id = NO_UI_TWEEN;
            Entity element = Entity::undefined();
            ComponentID component = INVALID_COMPONENT_ID;
            std::string_view componentName; // registry and schema strings outlive the table
            std::string_view fieldName;
            u32 index = NO_UI_TWEEN_INDEX; // of the element of an array field
            u32 offset = 0; // of the field in the component
            u32 lanes = 0;
            bool started = false; // `from` is set: given, or read when the delay ended
            bool done = false; // leaves the table at the end of the update
            bool yoyo = false;
            UiEase ease = UiEase::OutCubic;
            u32 repeat = 1;
            UiTweenId after = NO_UI_TWEEN;
            float duration = 0.0f;
            float delay = 0.0f;
            double clock = 0.0; // seconds since the tween stopped waiting
            Vec4 from{0.0f, 0.0f, 0.0f, 0.0f};
            Vec4 to{0.0f, 0.0f, 0.0f, 0.0f};
        };

        enum class Step : u8
        {
            Running,
            Finished,
            Gone, // its element or component vanished
        };

        World* world = nullptr;
        UiHierarchyManager* hierarchy = nullptr;
        std::vector<Tween> tweens; // ascending by id

        // (element, component, field offset) to the ids of the tweens on it, so a start finds the older tweens of
        // its field without scanning the table
        struct FieldKey
        {
            u64 element = 0;
            ComponentID component = INVALID_COMPONENT_ID;
            u32 offset = 0;
            bool operator==(const FieldKey&) const = default;
        };
        struct FieldKeyHash
        {
            sizet operator()(const FieldKey& key) const noexcept
            {
                u64 h = key.element * 0x9E3779B97F4A7C15ull;
                h = (h ^ (h >> 29) ^ (u64{key.component} << 32 | key.offset)) * 0xBF58476D1CE4E5B9ull;
                return static_cast<sizet>(h ^ (h >> 32));
            }
        };
        std::unordered_multimap<FieldKey, UiTweenId, FieldKeyHash> byField;

        static FieldKey keyOf(const Tween& tween) { return {tween.element.id(), tween.component, tween.offset}; }

        void unindex(const Tween& tween)
        {
            const auto range = byField.equal_range(keyOf(tween));
            for (auto it = range.first; it != range.second; ++it)
            {
                if (it->second == tween.id)
                {
                    byField.erase(it);
                    return;
                }
            }
        }

        u32 replaced = 0; // tweens marked done by a start that took their field, still in the table until settle()

        // Drops the tweens marked done (finished, gone or replaced) in one pass, keeping the order by id.
        void compact()
        {
            std::erase_if(tweens,
                          [this](const Tween& tween)
                          {
                              if (tween.done)
                              {
                                  unindex(tween);
                              }
                              return tween.done;
                          });
            replaced = 0;
        }

        // A start only marks the tween it replaces (erasing from the middle of the table made a theme switch over
        // running tweens cost the table size per start); every reader but alive-by-id settles first.
        void settle()
        {
            if (replaced != 0)
            {
                compact();
            }
        }
        UiTweenId nextId = 1;
        bool reduced = false;
        bool systemOn = false; // the "UiTweens" system is scheduled

        void scheduleSystem(const bool on)
        {
            systemOn = on;
            world->setSystemEnabled(SystemName{nullptr, "UiTweens"}, on);
        }

        [[nodiscard]] bool alive(const UiTweenId id) const
        {
            const auto it = std::ranges::lower_bound(tweens, id, {}, &Tween::id);
            return it != tweens.end() && it->id == id;
        }

        // False when the field holds NaN or infinity: such a tween has no start to interpolate from.
        static bool read(const void* component, const Tween& tween, Vec4& out)
        {
            const auto* lanes = reinterpret_cast<const float*>(static_cast<const char*>(component) + tween.offset);
            bool finite = true;
            for (u32 i = 0; i < tween.lanes; ++i)
            {
                finite = finite && std::isfinite(lanes[i]);
                setLane(out, i, lanes[i]);
            }
            return finite;
        }

        static void write(void* component, const Tween& tween, const Vec4& value)
        {
            auto* lanes = reinterpret_cast<float*>(static_cast<char*>(component) + tween.offset);
            for (u32 i = 0; i < tween.lanes; ++i)
            {
                lanes[i] = lane(value, i);
            }
        }

        // The value after the last play: `to`, or `from` when a yoyo's last play runs backwards or the tween
        // repeats forever (only reduced motion ends that one).
        static Vec4 endValue(const Tween& tween)
        {
            const bool backwards = tween.repeat == 0 || (tween.yoyo && tween.repeat % 2 == 0);
            return backwards ? tween.from : tween.to;
        }

        Step step(SystemContext& ctx, Tween& tween, const float dt) const
        {
            if (!ctx.isEntityValid(tween.element) || !ctx.hasComponent(tween.element, tween.component))
            {
                return Step::Gone;
            }
            void* component = ctx.getComponentData(tween.element, tween.component);
            Vec4 value;
            Step result = Step::Running;
            if (reduced)
            {
                if (!tween.started && !read(component, tween, tween.from))
                {
                    return Step::Gone;
                }
                tween.started = true;
                value = endValue(tween);
                result = Step::Finished;
            }
            else
            {
                tween.clock += dt;
                if (tween.clock < tween.delay)
                {
                    return Step::Running;
                }
                if (!tween.started && !read(component, tween, tween.from))
                {
                    return Step::Gone;
                }
                tween.started = true;
                const double plays = tween.duration > 0.0f ? (tween.clock - tween.delay) / tween.duration : 0.0;
                const double playIndex = std::floor(plays);
                if (tween.duration <= 0.0f || (tween.repeat != 0 && playIndex >= tween.repeat))
                {
                    value = endValue(tween);
                    result = Step::Finished;
                }
                else
                {
                    float t = static_cast<float>(plays - playIndex);
                    if (tween.yoyo && static_cast<u64>(playIndex) % 2 == 1)
                    {
                        t = 1.0f - t;
                    }
                    const float eased = evaluateUiEase(tween.ease, t);
                    value = tween.from + (tween.to - tween.from) * eased;
                }
            }
            write(component, tween,
                  value); // the chunk write bits mark layout or paint dirty (UiHierarchyManager::update)
            return result;
        }
    };

    UiTweens::UiTweens(World& world, UiHierarchyManager& hierarchy) : m_state(std::make_unique<State>())
    {
        m_state->world = &world;
        m_state->hierarchy = &hierarchy;
    }

    UiTweens::~UiTweens() = default;

    Expected<UiTweenField> UiTweens::field(const std::string_view componentName, const std::string_view fieldName)
    {
        const auto refuse = [](std::string message) { return std::unexpected(Error(std::move(message))); };
        const std::string name(componentName);
        const ComponentID component = getComponentIdByName(name.c_str());
        const auto allowed = tweenables();
        if (component == INVALID_COMPONENT_ID || !std::ranges::contains(allowed, component))
        {
            return refuse(std::format("'{}' is not a tweenable UI component (UiRect, UiVisual, UiContentSize, "
                                      "UiLayout, UiLayoutItem, UiStyleAnim)",
                                      componentName));
        }
        // an element of an array field is named `field[i]`
        std::string_view baseName = fieldName;
        u32 index = NO_UI_TWEEN_INDEX;
        if (const sizet open = fieldName.find('['); open != std::string_view::npos && fieldName.ends_with(']'))
        {
            const std::string_view digits = fieldName.substr(open + 1, fieldName.size() - open - 2);
            const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
            if (digits.empty() || error != std::errc{} || end != digits.data() + digits.size())
            {
                return refuse(std::format("'{}' is not field[index]", fieldName));
            }
            baseName = fieldName.substr(0, open);
        }
        const StructSchema* schema = getComponentMetadata(component).schema;
        const FieldDesc* found = nullptr;
        for (const FieldDesc& candidate : schema ? schema->fields : std::span<const FieldDesc>{})
        {
            if (candidate.name == baseName)
            {
                found = &candidate;
            }
        }
        const bool indexed = index != NO_UI_TWEEN_INDEX;
        if (!found || (found->arrayCount != 0) != indexed || (indexed && index >= found->arrayCount) ||
            laneCount(found->kind) == 0 || (found->flags & fieldFlags::READONLY))
        {
            return refuse(std::format("{} has no float or vector field '{}'{}", componentName, fieldName,
                                      found && found->arrayCount != 0 && !indexed ? " (an array: name an element, "
                                                                                    "field[index])"
                                                                                  : ""));
        }
        return UiTweenField{component,
                            getComponentName(component),
                            found->name,
                            found->offset + (indexed ? index * found->elementSize : 0u),
                            laneCount(found->kind),
                            index};
    }

    Expected<UiTweenId> UiTweens::start(const UiTweenDesc& desc)
    {
        if (!m_state->hierarchy->isElement(desc.element))
        {
            return std::unexpected(Error(std::format("no UI element {}", desc.element.id())));
        }
        const Expected<UiTweenField> resolved = field(desc.component, desc.field);
        if (!resolved)
        {
            return std::unexpected(resolved.error());
        }
        return start(*resolved, desc);
    }

    Expected<UiTweenId> UiTweens::start(const UiTweenField& field, const UiTweenDesc& desc)
    {
        State& state = *m_state;
        const auto refuse = [](std::string message) { return std::unexpected(Error(std::move(message))); };
        if (!state.hierarchy->isElement(desc.element))
        {
            return refuse(std::format("no UI element {}", desc.element.id()));
        }
        if (desc.lanes != 0 && desc.lanes != field.lanes)
        {
            return refuse(std::format("{}.{} takes {} values, not {}", field.componentName, field.fieldName,
                                      field.lanes, desc.lanes));
        }
        const auto finite = [](const Vec4& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
        };
        if (!finite(desc.to) || (desc.from && !finite(*desc.from)) || !std::isfinite(desc.duration) ||
            !std::isfinite(desc.delay) || desc.duration < 0.0f || desc.delay < 0.0f)
        {
            return refuse("tween values must be finite and times must not be negative");
        }
        if (desc.repeat == 0 && desc.duration <= 0.0f)
        {
            return refuse("a tween that repeats forever needs a duration above zero");
        }
        if (desc.after >= state.nextId)
        {
            return refuse(std::format("no tween {} to wait for", desc.after));
        }
        if (!state.world->hasComponentById(desc.element, field.component))
        {
            if (field.component != getComponentId<UiVisualComponent>())
            {
                return refuse(std::format("element {} has no {}", desc.element.id(), field.componentName));
            }
            state.world->addComponent<UiVisualComponent>(desc.element);
        }

        State::Tween tween;
        tween.id = state.nextId++;
        tween.element = desc.element;
        tween.component = field.component;
        tween.componentName = field.componentName;
        tween.fieldName = field.fieldName;
        tween.index = field.index;
        tween.offset = field.offset;
        tween.lanes = field.lanes;
        tween.started = desc.from.has_value();
        tween.yoyo = desc.yoyo;
        tween.ease = desc.ease;
        tween.repeat = desc.repeat;
        tween.after = desc.after;
        tween.duration = desc.duration;
        tween.delay = desc.delay;
        tween.from = desc.from.value_or(Vec4{0.0f, 0.0f, 0.0f, 0.0f});
        tween.to = desc.to;
        // one tween per field: the older stops where it is and leaves the table at once, so the table is bounded by
        // distinct fields and a burst of starts costs O(1) each; the tweens this one waits on (its `after` chain)
        // stay, or the sequence would start at once. The field index finds the older ones.
        std::vector<UiTweenId> chain; // empty (no allocation) unless the tween waits on another
        for (UiTweenId link = desc.after; link != NO_UI_TWEEN;)
        {
            const auto it = std::ranges::lower_bound(state.tweens, link, {}, &State::Tween::id);
            if (it == state.tweens.end() || it->id != link || it->done)
            {
                break;
            }
            chain.push_back(link);
            link = it->after;
        }
        const auto older = state.byField.equal_range(State::keyOf(tween));
        for (auto it = older.first; it != older.second;)
        {
            if (std::ranges::contains(chain, it->second))
            {
                ++it;
                continue;
            }
            const auto other = std::ranges::lower_bound(state.tweens, it->second, {}, &State::Tween::id);
            if (other != state.tweens.end() && other->id == it->second && !other->done)
            {
                other->done = true;
                ++state.replaced;
            }
            it = state.byField.erase(it);
        }
        state.tweens.push_back(tween);
        state.byField.emplace(State::keyOf(tween), tween.id);
        if (state.replaced > 64 && state.replaced * 2 > state.tweens.size())
        {
            state.compact(); // amortized: the table stays within twice its live tweens
        }
        if (!state.systemOn)
        {
            state.scheduleSystem(true);
        }
        return tween.id;
    }

    bool UiTweens::cancel(const UiTweenId id)
    {
        State& state = *m_state;
        state.settle();
        const auto it = std::ranges::lower_bound(state.tweens, id, {}, &State::Tween::id);
        if (it == state.tweens.end() || it->id != id)
        {
            return false;
        }
        state.unindex(*it);
        state.tweens.erase(it);
        return true;
    }

    u32 UiTweens::cancelElement(const Entity element)
    {
        State& state = *m_state;
        state.settle();
        return static_cast<u32>(std::erase_if(state.tweens,
                                              [&](const State::Tween& tween)
                                              {
                                                  const bool match = tween.element == element;
                                                  if (match)
                                                  {
                                                      state.unindex(tween);
                                                  }
                                                  return match;
                                              }));
    }

    void UiTweens::setReducedMotion(const bool reduced) { m_state->reduced = reduced; }

    bool UiTweens::reducedMotion() const { return m_state->reduced; }

    u32 UiTweens::activeCount() const { return static_cast<u32>(m_state->tweens.size()) - m_state->replaced; }

    void UiTweens::advance(SystemContext& ctx)
    {
        State& state = *m_state;
        state.settle();
        if (state.tweens.empty())
        {
            return;
        }
        bool anyDone = false;
        for (State::Tween& tween : state.tweens)
        {
            // a finished predecessor leaves the table after the loop: its follower starts next frame
            if (!state.reduced && tween.after != NO_UI_TWEEN)
            {
                if (state.alive(tween.after))
                {
                    continue;
                }
                tween.after = NO_UI_TWEEN;
            }
            const State::Step result = state.step(ctx, tween, ctx.deltaTime);
            if (result == State::Step::Running)
            {
                continue;
            }
            tween.done = anyDone = true;
            if (result == State::Step::Finished)
            {
                UiTweenFinishedEvent event;
                event.element = tween.element;
                event.tween = tween.id;
                ctx.fireEvent(event);
            }
        }
        if (anyDone)
        {
            state.compact();
        }
    }

    void UiTweens::retireIdleSystem()
    {
        m_state->settle();
        if (m_state->systemOn && m_state->tweens.empty())
        {
            m_state->scheduleSystem(false);
        }
    }

    void UiTweens::registerTable()
    {
        m_state->world->services().registerTable<UiContext>(
            "uiTweens", UiTweenRecord::spiteSchema(),
            [](const UiContext& ui, const TableRowSink& sink)
            {
                State& state = *ui.tweens().m_state;
                state.settle();
                for (const State::Tween& tween : state.tweens)
                {
                    const bool waiting = tween.after != NO_UI_TWEEN && state.alive(tween.after);
                    const UiTweenRecord record{tween.id,
                                               tween.element,
                                               std::string(tween.componentName),
                                               tween.index == NO_UI_TWEEN_INDEX
                                                   ? std::string(tween.fieldName)
                                                   : std::format("{}[{}]", tween.fieldName, tween.index),
                                               tween.from,
                                               tween.to,
                                               tween.duration,
                                               tween.delay,
                                               std::string(uiEaseName(tween.ease)),
                                               tween.repeat,
                                               tween.yoyo,
                                               tween.after,
                                               waiting                         ? "waiting"
                                                   : tween.clock < tween.delay ? "delay"
                                                                               : "running",
                                               static_cast<float>(std::max(0.0, tween.clock - tween.delay))};
                    sink(&record);
                }
            });
    }
} // namespace spite
