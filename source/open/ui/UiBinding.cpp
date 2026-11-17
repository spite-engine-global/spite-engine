#include "ui/UiBinding.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <mutex>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/Handle.hpp"
#include "base/Logging.hpp"
#include "base/ServiceLocator.hpp"
#include "ecs/Chunk.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "ecs/LifecycleEvents.hpp"
#include "ecs/Query.hpp"
#include "ecs/QueryBuilder.hpp"
#include "ecs/SystemContext.hpp"
#include "ecs/SystemDescriptor.hpp"
#include "ecs/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        // What a bound field currently holds, reduced to what the targets take.
        struct FieldValue
        {
            enum class Kind : u8
            {
                Integer,
                Real32,
                Real64,
                Text,
            };
            Kind kind = Kind::Integer;
            std::int64_t integer = 0;
            double real = 0.0;
            std::string text;

            [[nodiscard]] bool equals(const FieldValue& other) const
            {
                return kind == other.kind && integer == other.integer &&
                    std::bit_cast<u64>(real) == std::bit_cast<u64>(other.real) && text == other.text;
            }
            [[nodiscard]] double number() const
            {
                switch (kind)
                {
                case Kind::Integer:
                    return static_cast<double>(integer);
                case Kind::Text:
                    return text.empty() ? 0.0 : 1.0;
                default:
                    return real;
                }
            }
            // The value as text, shortest round trip for floats.
            void write(std::string& out) const
            {
                char buffer[64];
                std::to_chars_result result{buffer, std::errc{}};
                switch (kind)
                {
                case Kind::Text:
                    out = text;
                    return;
                case Kind::Integer:
                    result = std::to_chars(buffer, buffer + sizeof buffer, integer);
                    break;
                case Kind::Real32:
                    result = std::to_chars(buffer, buffer + sizeof buffer, static_cast<float>(real));
                    break;
                case Kind::Real64:
                    result = std::to_chars(buffer, buffer + sizeof buffer, real);
                    break;
                }
                out.assign(buffer, result.ptr);
            }
        };

        bool scalarKind(const FieldKind kind)
        {
            switch (kind)
            {
            case FieldKind::Bool:
            case FieldKind::U8:
            case FieldKind::U16:
            case FieldKind::U32:
            case FieldKind::U64:
            case FieldKind::I32:
            case FieldKind::F32:
            case FieldKind::F64:
            case FieldKind::String:
            case FieldKind::InternedString:
                return true;
            default:
                return false;
            }
        }

        // `at` points at the field inside the component.
        void read(const FieldKind kind, const std::byte* at, FieldValue& out)
        {
            using Kind = FieldValue::Kind;
            switch (kind)
            {
            case FieldKind::Bool:
                out.kind = Kind::Integer;
                out.integer = *reinterpret_cast<const bool*>(at) ? 1 : 0;
                break;
            case FieldKind::U8:
                out.kind = Kind::Integer;
                out.integer = *reinterpret_cast<const u8*>(at);
                break;
            case FieldKind::U16:
                out.kind = Kind::Integer;
                out.integer = *reinterpret_cast<const u16*>(at);
                break;
            case FieldKind::U32:
                out.kind = Kind::Integer;
                out.integer = *reinterpret_cast<const u32*>(at);
                break;
            case FieldKind::U64:
                out.kind = Kind::Integer;
                out.integer = static_cast<std::int64_t>(
                    std::min<u64>(*reinterpret_cast<const u64*>(at), static_cast<u64>(INT64_MAX)));
                break;
            case FieldKind::I32:
                out.kind = Kind::Integer;
                out.integer = *reinterpret_cast<const i32*>(at);
                break;
            case FieldKind::F32:
                out.kind = Kind::Real32;
                out.real = *reinterpret_cast<const f32*>(at);
                break;
            case FieldKind::F64:
                out.kind = Kind::Real64;
                out.real = *reinterpret_cast<const double*>(at);
                break;
            case FieldKind::String:
                out.kind = Kind::Text;
                out.text = *reinterpret_cast<const std::string*>(at);
                break;
            default: // InternedString
                out.kind = Kind::Text;
                out.text = reinterpret_cast<const InternedString*>(at)->toView();
                break;
            }
        }

        enum class TargetKind : u8
        {
            Text,
            Progress,
            Slider,
            Number,
            Toggle,
            Selection, // radio group, tab bar, dropdown, combo
            Visible,
        };

        const char* bindTargetName(const UiBindTarget target)
        {
            return target == UiBindTarget::Text ? "text" : target == UiBindTarget::Value ? "value" : "visible";
        }
    } // namespace

    struct UiBindings::State
    {
        struct Binding
        {
            u32 id = 0;
            UiScope* scope = nullptr;
            Entity source = Entity::undefined();
            ComponentID component = INVALID_COMPONENT_ID;
            std::string componentName;
            std::string fieldName;
            u32 offset = 0;
            FieldKind fieldKind = FieldKind::Bool;
            Entity target = Entity::undefined();
            UiBindTarget bind = UiBindTarget::Text;
            TargetKind targetKind = TargetKind::Text;
            InternedString format{};
            std::string argument;
            FieldValue last;
            u64 updates = 0;
            bool deferred = false; // a write was held back (the target is being edited): retried once it is not
        };

        // The bindings whose source has one bound component: what a written row of that component finds.
        struct Group
        {
            std::shared_ptr<Query> query;
            Handle removed; // the component-removed subscription
            bool dead = false; // no sources left: unsubscribed by the next update, never while events dispatch
            std::unordered_map<u64, std::vector<u32>> bySource; // entity id -> binding ids
        };

        World* world = nullptr;
        UiHierarchyManager* hierarchy = nullptr;
        const UiRouter* router = nullptr;
        u32 deadGroups = 0;
        u64 checkedRevision = ~u64{0}; // the system schedule the writer check last saw
        bool blocked = false; // a system in the visit's stage writes a bound component
        bool resync = false; // the visits were suspended: compare every binding with its source once
        std::vector<u32> deferred; // bindings whose write was held back
        u32 nextId = 1;
        std::unordered_map<u32, Binding> bindings;
        std::unordered_map<ComponentID, Group> groups;
        std::unordered_map<u64, std::vector<u32>> byTarget; // element id -> binding ids

        // source entities or components destroyed: the lifecycle hooks only queue, the visit applies
        mutable std::mutex pendingMutex;
        std::vector<std::pair<ComponentID, Entity>> pending;
        std::atomic<bool> hasPending = false;

        UiBindingStats stats;
        std::vector<std::pair<u32, FieldValue>> changed; // scratch of update
        std::vector<u32> orphans;
        std::string scratch;

        void erase(const u32 id)
        {
            const auto it = bindings.find(id);
            if (it == bindings.end())
            {
                return;
            }
            const Binding& binding = it->second;
            if (const auto group = groups.find(binding.component); group != groups.end())
            {
                if (const auto source = group->second.bySource.find(binding.source.id());
                    source != group->second.bySource.end())
                {
                    std::erase(source->second, id);
                    if (source->second.empty())
                    {
                        group->second.bySource.erase(source);
                    }
                }
                if (group->second.bySource.empty() && !group->second.dead)
                {
                    // an erase can run inside a lifecycle dispatch (a hook destroying a target): unsubscribing
                    // there would move the subscriber being run, so update() drops the group
                    group->second.dead = true;
                    ++deadGroups;
                    checkedRevision = ~u64{0}; // the writers of the remaining groups are looked at again
                }
            }
            if (const auto target = byTarget.find(binding.target.id()); target != byTarget.end())
            {
                std::erase(target->second, id);
                if (target->second.empty())
                {
                    byTarget.erase(target);
                }
            }
            bindings.erase(it);
        }

        void dropDeadGroups()
        {
            for (auto it = groups.begin(); deadGroups != 0 && it != groups.end();)
            {
                if (it->second.dead)
                {
                    world->lifecycleEvents().unsubscribe(it->second.removed);
                    it = groups.erase(it);
                    --deadGroups;
                }
                else
                {
                    ++it;
                }
            }
        }

        // The first system in the visit's stage that declares a write on the component, or empty. The visit reads
        // bound components undeclared (chosen at run time), which is only race-free while none does.
        [[nodiscard]] std::string writerOf(const ComponentID component) const
        {
            for (const SystemDescriptor& system : world->getSystems())
            {
                if (system.update && system.update->stage == CoreExecutionStages::BEGIN_FRAME &&
                    std::ranges::find(system.update->writes, component) != system.update->writes.end() &&
                    std::string_view(system.name) != "UiLayoutResolve")
                {
                    return system.name;
                }
            }
            return {};
        }

        // Re-checks every group when the schedule changed since the last look.
        void checkWriters()
        {
            if (const u64 revision = world->systemRevision(); revision != checkedRevision)
            {
                checkedRevision = revision;
                const bool wasBlocked = blocked;
                blocked = false;
                for (const auto& [component, group] : groups)
                {
                    if (const std::string writer = group.dead ? std::string{} : writerOf(component); !writer.empty())
                    {
                        blocked = true;
                        SERROR_LOG("UI bindings suspended: system '{}' writes the bound component '{}' in the "
                                   "BEGIN_FRAME stage of the binding visit",
                                   writer, getComponentMetadata(component).name.toView());
                    }
                }
                // writes made while suspended are gone from the (this frame only) write bits
                resync = resync || (wasBlocked && !blocked);
            }
        }

        void flushPending()
        {
            std::vector<std::pair<ComponentID, Entity>> gone;
            {
                const std::scoped_lock lock(pendingMutex);
                gone.swap(pending);
                hasPending = false;
            }
            for (const auto& [component, entity] : gone)
            {
                const auto group = groups.find(component);
                if (group == groups.end())
                {
                    continue;
                }
                const auto source = group->second.bySource.find(entity.id());
                // removed and added again before this visit (a module reload strips and restores its components):
                // the bindings stand and the write bits carry the new value
                if (source == group->second.bySource.end() ||
                    (world->isEntityAlive(entity) && world->hasComponentById(entity, component)))
                {
                    continue;
                }
                const std::vector<u32> ids = source->second;
                for (const u32 id : ids)
                {
                    erase(id);
                }
            }
        }

        // Writes the value to the binding's target as a setter would: nothing fires. False when the target is gone.
        bool apply(Binding& binding)
        {
            if (!hierarchy->isElement(binding.target))
            {
                return false;
            }
            UiScope& scope = *binding.scope;
            Widget widget(&scope, binding.target, scope.generation());
            const FieldValue& value = binding.last;
            // static_cast of a NaN to an integer is undefined, and no widget shows a non-finite number
            if ((binding.targetKind == TargetKind::Progress || binding.targetKind == TargetKind::Slider ||
                 binding.targetKind == TargetKind::Number || binding.targetKind == TargetKind::Selection) &&
                !std::isfinite(value.number()))
            {
                return true;
            }
            // a field or combo being typed into keeps its text; the write is retried once the focus is gone
            if ((binding.targetKind == TargetKind::Number || binding.targetKind == TargetKind::Selection) &&
                (router->focused() == binding.target || hierarchy->parentOf(router->focused()) == binding.target))
            {
                if (!binding.deferred)
                {
                    binding.deferred = true;
                    deferred.push_back(binding.id);
                }
                return true;
            }
            switch (binding.targetKind)
            {
            case TargetKind::Text:
                {
                    Label label(&scope, binding.target, scope.generation());
                    if (binding.format.isValid())
                    {
                        if (value.kind == FieldValue::Kind::Text)
                        {
                            label.setLocalizedArgText(binding.argument, value.text);
                        }
                        else
                        {
                            value.write(scratch);
                            label.setLocalizedArg(binding.argument, std::string_view(scratch));
                        }
                    }
                    else
                    {
                        value.write(scratch);
                        label.setText(scratch);
                    }
                    break;
                }
            case TargetKind::Progress:
                ProgressBar(&scope, binding.target, scope.generation()).setValue(value.number());
                break;
            case TargetKind::Slider:
                Slider(&scope, binding.target, scope.generation()).setValue(value.number());
                break;
            case TargetKind::Number:
                NumberField(&scope, binding.target, scope.generation()).setNumber(value.number());
                break;
            case TargetKind::Toggle:
                Checkbox(&scope, binding.target, scope.generation()).setChecked(value.number() != 0.0);
                break;
            case TargetKind::Selection:
                {
                    const double index = std::clamp(value.number(), -1.0, 2147483647.0);
                    // every selection widget keeps its choice in a component of its own: the handle types share
                    // setSelected
                    if (world->hasComponent<UiRadioGroupComponent>(binding.target))
                    {
                        RadioGroup(&scope, binding.target, scope.generation()).setSelected(static_cast<i32>(index));
                    }
                    else if (world->hasComponent<UiTabBarComponent>(binding.target))
                    {
                        TabBar(&scope, binding.target, scope.generation()).setActive(static_cast<i32>(index));
                    }
                    else
                    {
                        Dropdown(&scope, binding.target, scope.generation()).setSelected(static_cast<i32>(index));
                    }
                    break;
                }
            case TargetKind::Visible:
                widget.setVisible(value.number() != 0.0);
                break;
            }
            return true;
        }
    };

    UiBindings::UiBindings(World& world, UiHierarchyManager& hierarchy, const UiRouter& router) :
        m_state(std::make_unique<State>())
    {
        m_state->world = &world;
        m_state->hierarchy = &hierarchy;
        m_state->router = &router;
    }

    UiBindings::~UiBindings()
    {
        for (const auto& group : m_state->groups | std::views::values)
        {
            m_state->world->lifecycleEvents().unsubscribe(group.removed);
        }
    }

    Expected<u32> UiBindings::add(UiScope& scope, const Entity target, const UiBindingDesc& desc)
    {
        State& state = *m_state;
        World& world = *state.world;
        const auto refuse = [](std::string message) { return std::unexpected(Error(std::move(message))); };
        if (!state.hierarchy->isElement(target) || !scope.owns(target))
        {
            return refuse(std::format("no UI element {} in this scope", target.id()));
        }
        if (!world.isEntityAlive(desc.source))
        {
            return refuse(std::format("no entity {}", desc.source.id()));
        }
        if (desc.argument.find_first_of("=\r\n") != std::string_view::npos)
        {
            return refuse("a binding argument is a name: no '=' or line break");
        }
        const std::string componentName(desc.component);
        const ComponentID component = getComponentIdByName(componentName.c_str());
        if (component == INVALID_COMPONENT_ID)
        {
            return refuse(std::format("'{}' is not a registered component", desc.component));
        }
        if (!world.hasComponentById(desc.source, component))
        {
            return refuse(std::format("entity {} has no {}", desc.source.id(), desc.component));
        }
        if (const auto group = state.groups.find(component); group == state.groups.end() || group->second.dead)
        {
            if (const std::string writer = state.writerOf(component); !writer.empty())
            {
                return refuse(std::format("system '{}' writes {} in the BEGIN_FRAME stage of the binding visit, which "
                                          "reads it undeclared: bind from a component no such system writes",
                                          writer, desc.component));
            }
        }
        const StructSchema* schema = getComponentMetadata(component).schema;
        const FieldDesc* field = nullptr;
        for (const FieldDesc& candidate : schema ? schema->fields : std::span<const FieldDesc>{})
        {
            field = candidate.name == desc.field ? &candidate : field;
        }
        if (!field || field->arrayCount != 0 || !scalarKind(field->kind))
        {
            return refuse(std::format("{} has no scalar field '{}' (bool, integer, float or string)", desc.component,
                                      desc.field));
        }

        State::Binding binding;
        binding.scope = &scope;
        binding.source = desc.source;
        binding.component = component;
        binding.componentName = componentName;
        binding.fieldName = std::string(desc.field);
        binding.offset = field->offset;
        binding.fieldKind = field->kind;
        binding.target = target;
        binding.bind = desc.target;
        binding.format = desc.format;
        binding.argument = desc.argument.empty() ? std::string("value") : std::string(desc.argument);
        const bool textSource = field->kind == FieldKind::String || field->kind == FieldKind::InternedString;
        switch (desc.target)
        {
        case UiBindTarget::Text:
            if (!world.hasComponent<UiLabelComponent>(target))
            {
                return refuse("a text binding needs a label or button");
            }
            binding.targetKind = TargetKind::Text;
            if (desc.format.isValid())
            {
                Label(&scope, target, scope.generation()).setLocalizedText(desc.format);
            }
            break;
        case UiBindTarget::Value:
            if (textSource)
            {
                return refuse("a value binding needs a numeric or bool field");
            }
            if (world.hasComponent<UiProgressComponent>(target))
            {
                binding.targetKind = TargetKind::Progress;
            }
            else if (world.hasComponent<UiSliderComponent>(target))
            {
                binding.targetKind = TargetKind::Slider;
            }
            else if (world.hasComponent<UiNumericComponent>(target))
            {
                binding.targetKind = TargetKind::Number;
            }
            else if (world.hasComponent<UiToggleComponent>(target))
            {
                binding.targetKind = TargetKind::Toggle;
            }
            else if (world.hasComponent<UiRadioGroupComponent>(target) ||
                     world.hasComponent<UiTabBarComponent>(target) || world.hasComponent<UiDropdownComponent>(target))
            {
                binding.targetKind = TargetKind::Selection;
            }
            else
            {
                return refuse("a value binding needs a progress bar, slider, number field, checkbox, toggle, radio "
                              "group, tab bar, dropdown or combo");
            }
            break;
        case UiBindTarget::Visible:
            binding.targetKind = TargetKind::Visible;
            break;
        }

        // one binding per target and kind
        if (const auto existing = state.byTarget.find(target.id()); existing != state.byTarget.end())
        {
            const std::vector<u32> ids = existing->second;
            for (const u32 id : ids)
            {
                if (state.bindings.at(id).bind == desc.target)
                {
                    state.erase(id);
                }
            }
        }

        binding.id = state.nextId++;
        read(binding.fieldKind,
             static_cast<const std::byte*>(world.getConstComponentDataById(desc.source, component)) + binding.offset,
             binding.last);
        const u32 id = binding.id;
        State::Group& group = state.groups[component];
        if (group.dead)
        {
            group.dead = false;
            --state.deadGroups;
        }
        if (!group.query)
        {
            QueryBuilder builder = world.createSystemContext().createQueryBuilder();
            const ComponentID ids[] = {component};
            // a binding mirrors data, which an inactive entity still holds
            builder.with_read(ids).includeInactive();
            group.query = builder.buildQuery();
            State* raw = &state;
            group.removed =
                world.lifecycleEvents().onComponentRemoved(component,
                                                           [raw, component](const Entity entity)
                                                           {
                                                               const std::scoped_lock lock(raw->pendingMutex);
                                                               raw->pending.emplace_back(component, entity);
                                                               raw->hasPending = true;
                                                           });
        }
        group.bySource[desc.source.id()].push_back(id);
        state.byTarget[target.id()].push_back(id);
        State::Binding& stored = state.bindings.emplace(id, std::move(binding)).first->second;
        state.apply(stored);
        return id;
    }

    bool UiBindings::remove(const u32 id)
    {
        const bool known = m_state->bindings.contains(id);
        m_state->erase(id);
        return known;
    }

    void UiBindings::removeOwner(const UiScope* scope)
    {
        State& state = *m_state;
        std::vector<u32> owned;
        for (const auto& [id, binding] : state.bindings)
        {
            if (binding.scope == scope)
            {
                owned.push_back(id);
            }
        }
        for (const u32 id : owned)
        {
            state.erase(id);
        }
    }

    void UiBindings::removeTarget(const Entity target)
    {
        State& state = *m_state;
        if (const auto it = state.byTarget.find(target.id()); it != state.byTarget.end())
        {
            const std::vector<u32> ids = it->second;
            for (const u32 id : ids)
            {
                state.erase(id);
            }
        }
    }

    u32 UiBindings::count() const { return static_cast<u32>(m_state->bindings.size()); }

    UiBindingStats UiBindings::stats() const { return m_state->stats; }

    void UiBindings::update()
    {
        State& state = *m_state;
        state.stats = {};
        if (state.bindings.empty())
        {
            state.dropDeadGroups();
            state.blocked = false;
            state.resync = false;
            state.checkedRevision = ~u64{0};
            state.deferred.clear();
            return;
        }
        using Clock = std::chrono::steady_clock;
        const auto started = Clock::now();
        Clock::duration applying{};
        state.checkWriters();
        state.stats.blocked = state.blocked;
        if (state.blocked)
        {
            return;
        }
        if (state.hasPending)
        {
            state.flushPending();
        }
        state.dropDeadGroups();
        if (!state.deferred.empty())
        {
            const std::vector<u32> held = std::move(state.deferred);
            state.deferred.clear();
            for (const u32 id : held)
            {
                if (const auto found = state.bindings.find(id); found != state.bindings.end())
                {
                    found->second.deferred = false;
                    if (!state.apply(found->second))
                    {
                        state.orphans.push_back(id);
                    }
                }
            }
        }
        const bool resync = std::exchange(state.resync, false);
        for (auto& [component, group] : state.groups)
        {
            const ComponentID watched[] = {component};
            state.changed.clear();
            // pass 1 reads only the write bits: the summary keeps an unwritten component at zero chunks, and the count
            // of written rows picks the cheaper pass 2
            u32 written = 0;
            group.query->forEachChangedChunkConst(
                watched, ChangeWindow::ThisFrame,
                [&](const Chunk* chunk)
                {
                    if (chunk->hasComponent(component))
                    {
                        ++state.stats.chunksVisited;
                        written += static_cast<u32>(
                            chunk->modifiedThisFrameByIndex(static_cast<sizet>(chunk->getComponentIndex(component)))
                                .count());
                    }
                });
            state.stats.rowsChanged += written;
            const auto compare = [&](const std::vector<u32>& ids, const std::byte* data)
            {
                for (const u32 id : ids)
                {
                    const State::Binding& binding = state.bindings.at(id);
                    FieldValue now;
                    read(binding.fieldKind, data + binding.offset, now);
                    if (!now.equals(binding.last))
                    {
                        state.changed.emplace_back(id, std::move(now));
                    }
                }
            };
            if (resync)
            {
                for (const auto& [source, ids] : group.bySource)
                {
                    ++state.stats.lookups;
                    const Entity entity(source);
                    if (state.world->isEntityAlive(entity) && state.world->hasComponentById(entity, component))
                    {
                        compare(
                            ids,
                            static_cast<const std::byte*>(state.world->getConstComponentDataById(entity, component)));
                    }
                }
            }
            else if (written != 0 && group.bySource.size() < written)
            {
                // fewer bound sources than written rows (a component many entities write, a few are bound): probe the
                // sources, never every written row
                for (const auto& [source, ids] : group.bySource)
                {
                    ++state.stats.lookups;
                    const Entity entity(source);
                    if (state.world->isEntityAlive(entity) &&
                        state.world->wasComponentModifiedThisFrameById(entity, component))
                    {
                        compare(
                            ids,
                            static_cast<const std::byte*>(state.world->getConstComponentDataById(entity, component)));
                    }
                }
            }
            else if (written != 0)
            {
                group.query->forEachChangedChunkConst(
                    watched, ChangeWindow::ThisFrame,
                    [&](const Chunk* chunk)
                    {
                        if (!chunk->hasComponent(component))
                        {
                            return;
                        }
                        const auto index = static_cast<sizet>(chunk->getComponentIndex(component));
                        const auto bits = chunk->modifiedThisFrameByIndex(index);
                        for (sizet row = 0; row < chunk->size(); ++row)
                        {
                            if (!bits.test(row))
                            {
                                continue;
                            }
                            ++state.stats.lookups;
                            if (const auto found = group.bySource.find(chunk->entity(row).id());
                                found != group.bySource.end())
                            {
                                compare(
                                    found->second,
                                    static_cast<const std::byte*>(chunk->getConstComponentDataPtrByIndex(index, row)));
                            }
                        }
                    });
            }
            // the targets are written after the visit: their components are never touched while a chunk is walked
            const auto visited = Clock::now();
            for (auto& [id, value] : state.changed)
            {
                State::Binding& binding = state.bindings.at(id);
                binding.last = std::move(value);
                ++binding.updates;
                ++state.stats.applied;
                if (!state.apply(binding))
                {
                    state.orphans.push_back(id); // its element was destroyed without its scope's say
                }
            }
            applying += Clock::now() - visited;
        }
        const auto milliseconds = [](const Clock::duration span)
        { return std::chrono::duration<double, std::milli>(span).count(); };
        state.stats.applyMs = milliseconds(applying);
        state.stats.visitMs = milliseconds(Clock::now() - started) - state.stats.applyMs;
        for (const u32 id : state.orphans)
        {
            state.erase(id);
        }
        state.orphans.clear();
    }

    void UiBindings::registerTable()
    {
        m_state->world->services().registerTable<UiContext>(
            "uiBindings", UiBindingRecord::spiteSchema(),
            [](const UiContext& ui, const TableRowSink& sink)
            {
                const State& state = *ui.bindings().m_state;
                std::vector<std::pair<ComponentID, Entity>> gone; // a destroyed source is gone from the listing at once
                if (state.hasPending)
                {
                    const std::scoped_lock lock(state.pendingMutex);
                    gone = state.pending;
                }
                std::vector<const State::Binding*> sorted;
                for (const auto& binding : state.bindings | std::views::values)
                {
                    if (std::ranges::find(gone, std::pair{binding.component, binding.source}) == gone.end())
                    {
                        sorted.push_back(&binding);
                    }
                }
                std::ranges::sort(sorted, {}, &State::Binding::id);
                for (const State::Binding* binding : sorted)
                {
                    UiBindingRecord record;
                    record.id = binding->id;
                    record.source = binding->source;
                    record.component = binding->componentName;
                    record.field = binding->fieldName;
                    record.target = binding->target;
                    if (const auto* identity = state.world->tryGetConstComponent<UiIdentityComponent>(binding->target))
                    {
                        record.targetId = std::string(identity->id.toView());
                    }
                    record.kind = bindTargetName(binding->bind);
                    record.format =
                        std::string(binding->format.isValid() ? binding->format.toView() : std::string_view{});
                    binding->last.write(record.value);
                    record.updates = binding->updates;
                    sink(&record);
                }
            });
    }
} // namespace spite
