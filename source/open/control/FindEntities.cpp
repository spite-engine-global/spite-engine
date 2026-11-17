#include "FindEntities.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/Schema.hpp"
#include "base/StringInterner.hpp"
#include "control/ControlServer.hpp"
#include "ecs/Aspect.hpp"
#include "ecs/Chunk.hpp"
#include "ecs/ComponentMetadata.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "ecs/World.hpp"
#include "serialization/FormatWalk.hpp"

namespace spite
{
    namespace
    {
        constexpr u64 DEFAULT_LIMIT = 100;
        constexpr u64 MAX_LIMIT = 10000;
        // the world module's name component; control sits below world, so it
        // is reached by registry name and read through its schema
        constexpr const char* NAME_COMPONENT = "EntityNameComponent";

        // * matches any run, ? any one character; greedy with one backtrack point
        bool globMatch(const std::string_view text, const std::string_view pattern)
        {
            sizet t = 0;
            sizet p = 0;
            sizet star = std::string_view::npos;
            sizet resume = 0;
            while (t < text.size())
            {
                // * first: a * in the name must not consume a pattern * literally
                if (p < pattern.size() && pattern[p] == '*')
                {
                    star = p++;
                    resume = t;
                }
                else if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t]))
                {
                    ++t;
                    ++p;
                }
                else if (star != std::string_view::npos)
                {
                    p = star + 1;
                    t = ++resume;
                }
                else
                {
                    return false;
                }
            }
            while (p < pattern.size() && pattern[p] == '*')
            {
                ++p;
            }
            return p == pattern.size();
        }

        bool containsIgnoringCase(const std::string_view text, const std::string_view part)
        {
            const auto lower = [](const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
            return std::ranges::search(text, part, {}, lower, lower).begin() != text.end();
        }

        // key: a component name or an array of them
        std::string componentList(const ControlArgs& args, const std::string_view key, std::vector<ComponentID>& out)
        {
            const std::optional<std::string> text = args.json(key);
            if (!text)
            {
                return {};
            }
            nlohmann::json list = nlohmann::json::parse(*text, nullptr, false);
            if (list.is_string())
            {
                list = nlohmann::json::array({std::move(list)});
            }
            if (!list.is_array() || !std::ranges::all_of(list, &nlohmann::json::is_string))
            {
                return "'" + std::string(key) + "' must be a component name or an array of them";
            }
            for (const nlohmann::json& name : list)
            {
                const ComponentID id = getComponentIdByName(name.get_ref<const std::string&>().c_str());
                if (id == INVALID_COMPONENT_ID)
                {
                    return "unknown component '" + name.get<std::string>() +
                        "': the `components` query lists the registered ones";
                }
                out.push_back(id);
            }
            return {};
        }

        // the byte offset of the name component's interned name; nullopt when
        // the component is not registered or its schema has no such field
        std::optional<u32> nameOffset(const ComponentID nameComponent)
        {
            if (nameComponent == INVALID_COMPONENT_ID)
            {
                return std::nullopt;
            }
            const StructSchema* schema = getComponentMetadata(nameComponent).schema;
            if (!schema)
            {
                return std::nullopt;
            }
            const auto field = std::ranges::find(schema->fields, std::string_view("name"), &FieldDesc::name);
            if (field == schema->fields.end() || field->kind != FieldKind::InternedString || field->arrayCount != 0)
            {
                return std::nullopt;
            }
            return field->offset;
        }

        InternedString readName(const void* component, const u32 offset)
        {
            return *reinterpret_cast<const InternedString*>(static_cast<const std::byte*>(component) + offset);
        }

        constexpr u64 DEFAULT_AUDIT_GROUPS = 20;
        constexpr u64 MAX_AUDIT_GROUPS = 1000;
        constexpr u64 DEFAULT_AUDIT_SAMPLES = 3;
        constexpr u64 MAX_AUDIT_SAMPLES = 100;
        // the world module's transform, reached by registry name as the name component is
        constexpr const char* TRANSFORM_COMPONENT = "TransformComponent";

        // A tree root has children and no parent; an unrooted entity is a transform entity with neither
        constexpr u32 AUDIT_UNROOTED = 1;
        constexpr u32 AUDIT_ROOT = 2;
        constexpr u32 AUDIT_UNNAMED = 4;

        struct AuditTally
        {
            u64 entities = 0;
            u64 roots = 0;
            u64 unrooted = 0;
            u64 unnamed = 0;

            void add(const u32 flags)
            {
                ++entities;
                roots += (flags & AUDIT_ROOT) != 0;
                unrooted += (flags & AUDIT_UNROOTED) != 0;
                unnamed += (flags & AUDIT_UNNAMED) != 0;
            }
        };

        struct AuditGroup
        {
            AuditTally tally;
            std::vector<u64> unrootedSample;
            std::vector<u64> unnamedSample;
        };

        class Auditor
        {
        public:
            Auditor(const World& world, const ControlServer::HierarchyReaderFn* hierarchy) :
                m_world(world), m_hierarchy(hierarchy && *hierarchy ? hierarchy : nullptr),
                m_transform(getComponentIdByName(TRANSFORM_COMPONENT)),
                m_nameComponent(getComponentIdByName(NAME_COMPONENT)), m_offset(nameOffset(m_nameComponent))
            {
            }

            u32 flags(const Entity entity) const
            {
                u32 result = 0;
                if (!m_offset || !m_world.hasComponentById(entity, m_nameComponent) ||
                    readName(m_world.getConstComponentDataById(entity, m_nameComponent), *m_offset).toView().empty())
                {
                    result |= AUDIT_UNNAMED;
                }
                if (m_hierarchy && m_transform != INVALID_COMPONENT_ID && m_world.hasComponentById(entity, m_transform))
                {
                    const ControlServer::EntityRelation place = (*m_hierarchy)(m_world, entity);
                    if (!place.parent.isValid())
                    {
                        result |= place.children > 0 ? AUDIT_ROOT : AUDIT_UNROOTED;
                    }
                }
                return result;
            }

        private:
            const World& m_world;
            const ControlServer::HierarchyReaderFn* m_hierarchy;
            ComponentID m_transform;
            ComponentID m_nameComponent;
            std::optional<u32> m_offset;
        };

        std::string auditLimit(const ControlArgs& args, const char* key, const u64 fallback, const u64 max, u64& out)
        {
            const std::optional<u64> value = args.integer(key);
            if ((args.json(key) && !value) || value.value_or(0) > max)
            {
                return std::string(key) + " must be an integer 0.." + std::to_string(max);
            }
            out = value.value_or(fallback);
            return {};
        }
    } // namespace

    std::string auditEntities(const World& world, const ControlArgs& args, IFormatWriter& out,
                              const ControlServer::HierarchyReaderFn* hierarchy)
    {
        u64 groupLimit = 0;
        u64 sampleLimit = 0;
        if (std::string error = auditLimit(args, "groups", DEFAULT_AUDIT_GROUPS, MAX_AUDIT_GROUPS, groupLimit);
            !error.empty())
        {
            return error;
        }
        if (std::string error = auditLimit(args, "samples", DEFAULT_AUDIT_SAMPLES, MAX_AUDIT_SAMPLES, sampleLimit);
            !error.empty())
        {
            return error;
        }
        const Auditor auditor(world, hierarchy);
        AuditTally total;
        std::map<std::string, AuditGroup> groups;
        for (const Chunk* chunk : world.getChunksContaining(Aspect()))
        {
            // the module's own (game) components name the archetype family; engine ones are shared by all; an
            // entity with a UI component is a UI element, not scene
            std::string key;
            bool ui = false;
            for (const ComponentID id : chunk->aspect().getComponentIds())
            {
                const ComponentMetadata& metadata = getComponentMetadata(id);
                ui = ui || metadata.category == ComponentCategory::Ui;
                if (metadata.category == ComponentCategory::Game)
                {
                    key += (key.empty() ? "" : "+") + std::string(metadata.name.toView());
                }
            }
            if (ui)
            {
                continue;
            }
            AuditGroup& group = groups[key.empty() ? "(engine components only)" : key];
            for (const Entity entity : chunk->entities())
            {
                const u32 flags = auditor.flags(entity);
                total.add(flags);
                group.tally.add(flags);
                if ((flags & AUDIT_UNROOTED) && group.unrootedSample.size() < sampleLimit)
                {
                    group.unrootedSample.push_back(entity.id());
                }
                if ((flags & AUDIT_UNNAMED) && group.unnamedSample.size() < sampleLimit)
                {
                    group.unnamedSample.push_back(entity.id());
                }
            }
        }
        std::vector<std::pair<const std::string*, AuditGroup*>> listed;
        for (auto& [key, group] : groups)
        {
            if (group.tally.unrooted + group.tally.unnamed > 0)
            {
                listed.emplace_back(&key, &group);
            }
        }
        std::ranges::stable_sort(listed,
                                 [](const auto& a, const auto& b)
                                 {
                                     return a.second->tally.unrooted + a.second->tally.unnamed >
                                         b.second->tally.unrooted + b.second->tally.unnamed;
                                 });
        out.writeU64("entities", total.entities);
        out.writeU64("roots", total.roots);
        out.writeU64("unrooted", total.unrooted);
        out.writeU64("unnamed", total.unnamed);
        out.writeU64("groupCount", listed.size());
        out.beginArray("groups", static_cast<u32>(std::min<u64>(listed.size(), groupLimit)));
        for (sizet i = 0; i < listed.size() && i < groupLimit; ++i)
        {
            const AuditGroup& group = *listed[i].second;
            out.beginObject("");
            out.writeString("group", *listed[i].first);
            out.writeU64("entities", group.tally.entities);
            out.writeU64("roots", group.tally.roots);
            out.writeU64("unrooted", group.tally.unrooted);
            out.writeU64("unnamed", group.tally.unnamed);
            for (const auto& [name, sample] :
                 {std::pair{"unrootedSample", &group.unrootedSample}, std::pair{"unnamedSample", &group.unnamedSample}})
            {
                out.beginArray(name, static_cast<u32>(sample->size()));
                for (const u64 id : *sample)
                {
                    out.writeU64("", id);
                }
                out.endArray();
            }
            out.endObject();
        }
        out.endArray();
        return {};
    }

    void auditCreated(const World& world, const std::span<const Entity> created, IFormatWriter& out,
                      const ControlServer::HierarchyReaderFn* hierarchy)
    {
        const Auditor auditor(world, hierarchy);
        AuditTally tally;
        for (const Entity entity : created)
        {
            if (world.isEntityAlive(entity))
            {
                tally.add(auditor.flags(entity));
            }
        }
        out.beginObject("audit");
        out.writeU64("entities", tally.entities);
        out.writeU64("unrooted", tally.unrooted);
        out.writeU64("unnamed", tally.unnamed);
        out.endObject();
        std::vector<std::string> warnings;
        if (tally.unrooted > 0)
        {
            warnings.push_back(std::format("{} of {} entities are unrooted (a transform, no parent, no children): "
                                           "author them under one named layout root; hierarchyAudit lists them",
                                           tally.unrooted, tally.entities));
        }
        if (tally.unnamed > 0)
        {
            warnings.push_back(std::format("{} of {} entities have no name: give each one a role name "
                                           "(EntityNameComponent); hierarchyAudit lists them",
                                           tally.unnamed, tally.entities));
        }
        if (!warnings.empty())
        {
            out.beginArray("warnings", static_cast<u32>(warnings.size()));
            for (const std::string& warning : warnings)
            {
                out.writeString("", warning);
            }
            out.endArray();
        }
    }

    // args: name (exact) or match (glob), text (the entity's name or one of its components' names, without a trailing
    // "Component", contains it; ASCII case-insensitive), with / without (component names), values (components whose
    // fields each row carries), after (an id: the page starts past it), limit (rows, 0 counts only)
    std::string findEntities(const World& world, const ControlArgs& args, IFormatWriter& out,
                             const std::function<u64(u64)>* source,
                             const std::function<void(Entity, IFormatWriter&)>* extra)
    {
        const std::optional<std::string> exact = args.string("name");
        const std::optional<std::string> pattern = args.string("match");
        if (exact && pattern)
        {
            return "find takes name or match, not both";
        }
        if ((exact && exact->empty()) || (pattern && pattern->empty()))
        {
            return "an empty name matches nothing; find unnamed entities with without=" + std::string(NAME_COMPONENT);
        }
        const std::optional<std::string> text = args.string("text");
        if (text && text->empty())
        {
            return std::string("an empty text matches everything; leave it out");
        }
        const std::optional<u64> limitArg = args.integer("limit");
        if ((args.json("limit") && !limitArg) || limitArg.value_or(0) > MAX_LIMIT)
        {
            return "limit must be an integer 0.." + std::to_string(MAX_LIMIT);
        }
        const u64 limit = limitArg.value_or(DEFAULT_LIMIT);
        const std::optional<u64> afterArg = args.integer("after");
        if (args.json("after") && !afterArg)
        {
            return std::string("after must be an entity id (the previous page's next)");
        }
        const u64 after = afterArg.value_or(0);

        std::vector<ComponentID> with;
        std::vector<ComponentID> without;
        std::vector<ComponentID> values;
        for (const auto& [key, list] : {std::pair{"with", &with}, {"without", &without}, {"values", &values}})
        {
            if (std::string error = componentList(args, key, *list); !error.empty())
            {
                return error;
            }
        }
        for (const ComponentID component : values)
        {
            if (!getComponentMetadata(component).schema)
            {
                return "component '" + std::string(getComponentName(component)) +
                    "' has no schema, so it has no values";
            }
        }

        const ComponentID nameComponent = getComponentIdByName(NAME_COMPONENT);
        const std::optional<u32> offset = nameOffset(nameComponent);
        const bool byName = exact || pattern;
        if (byName && !offset)
        {
            return std::string("entity names are unavailable: ") + NAME_COMPONENT +
                " is not registered with a name field";
        }
        // a name never interned names no entity; looking it up interns nothing
        const InternedString key = exact ? StringInterner::get()->find(*exact) : InternedString{};
        if (byName)
        {
            with.push_back(nameComponent);
        }

        // the page: the `limit` smallest ids past `after`, kept as a max-heap
        std::vector<Entity> page;
        page.reserve(limit);
        const auto byId = [](const Entity a, const Entity b) { return a.id() < b.id(); };
        u64 total = 0;
        u64 pastAfter = 0;
        if (!exact || key.isValid())
        {
            const Aspect excluded{std::span<const ComponentID>(without)};
            for (const Chunk* chunk : world.getChunksContaining(Aspect(std::span<const ComponentID>(with))))
            {
                if (chunk->aspect().intersects(excluded))
                {
                    continue;
                }
                const int nameIndex = byName || text ? chunk->getComponentIndex(nameComponent) : -1;
                // a component of the archetype named by the text matches every entity of the chunk
                bool componentHit = false;
                if (text)
                {
                    for (const ComponentID id : chunk->aspect().getComponentIds())
                    {
                        // by the name without its trailing "Component", as the editor shows it
                        constexpr std::string_view SUFFIX = "Component";
                        std::string_view name = getComponentMetadata(id).name.toView();
                        name.remove_suffix(name.size() > SUFFIX.size() && name.ends_with(SUFFIX) ? SUFFIX.size() : 0);
                        componentHit = componentHit || containsIgnoringCase(name, *text);
                    }
                }
                const std::span<const Entity> entities = chunk->entities();
                for (sizet i = 0; i < chunk->size(); ++i)
                {
                    if (byName)
                    {
                        // const chunk access: a find marks nothing modified
                        const InternedString name =
                            readName(chunk->getConstComponentDataPtrByIndex(nameIndex, i), *offset);
                        if (exact ? name != key : !name.isValid() || !globMatch(name.toView(), *pattern))
                        {
                            continue;
                        }
                    }
                    if (text && !componentHit)
                    {
                        const InternedString name = offset && nameIndex >= 0
                            ? readName(chunk->getConstComponentDataPtrByIndex(nameIndex, i), *offset)
                            : InternedString{};
                        if (!name.isValid() || !containsIgnoringCase(name.toView(), *text))
                        {
                            continue;
                        }
                    }
                    ++total;
                    const Entity entity = entities[i];
                    if (entity.id() <= after)
                    {
                        continue;
                    }
                    ++pastAfter;
                    if (page.size() < limit)
                    {
                        page.push_back(entity);
                        std::ranges::push_heap(page, byId);
                    }
                    else if (limit > 0 && entity.id() < page.front().id())
                    {
                        std::ranges::pop_heap(page, byId);
                        page.back() = entity;
                        std::ranges::push_heap(page, byId);
                    }
                }
            }
        }
        std::ranges::sort_heap(page, byId);

        out.writeU64("total", total);
        out.beginArray("entities", static_cast<u32>(page.size()));
        for (const Entity entity : page)
        {
            out.beginObject("");
            out.writeU64("id", entity.id());
            if (const u64 sourceId = source && *source ? (*source)(entity.id()) : 0)
            {
                out.writeU64("source", sourceId);
            }
            const bool named = offset && world.hasComponentById(entity, nameComponent);
            out.writeString("name",
                            named ? readName(world.getConstComponentDataById(entity, nameComponent), *offset).toView()
                                  : std::string_view{});
            if (extra && *extra)
            {
                (*extra)(entity, out);
            }
            for (const ComponentID component : values)
            {
                if (world.hasComponentById(entity, component))
                {
                    const ComponentMetadata& metadata = getComponentMetadata(component);
                    out.beginObject(metadata.name.toView());
                    writeValue(*metadata.schema, world.getConstComponentDataById(entity, component), out,
                               FieldSet::All);
                    out.endObject();
                }
            }
            out.endObject();
        }
        out.endArray();
        // the cursor for the next page: pass it as after
        if (pastAfter > page.size() && !page.empty())
        {
            out.writeU64("next", page.back().id());
        }
        return {};
    }
} // namespace spite
