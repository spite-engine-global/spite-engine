#pragma once

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

#include "ecs/ComponentRegistry.hpp"
#include "ecs/World.hpp"
#include "world/DisabledComponent.hpp"
#include "world/EntityName.hpp"
#include "world/TransformComponents.hpp"

namespace spite
{
    // The label of an entity wherever the editor names one (hierarchy rows and query, status bar): its name, else
    // "<primary component> #<index>" ("Entity #<index>" without a primary; the index is the entity id without its
    // generation, unique among the live entities).
    inline constexpr std::string_view UNNAMED_ENTITY = "Entity"; // also the name new entities get

    // the registered name without a trailing "Component"
    [[nodiscard]] inline std::string_view componentDisplayName(const ComponentID component)
    {
        constexpr std::string_view SUFFIX = "Component";
        std::string_view name = getComponentMetadata(component).name.toView();
        if (name.size() > SUFFIX.size() && name.ends_with(SUFFIX))
        {
            name.remove_suffix(SUFFIX.size());
        }
        return name;
    }

    // An identifier as words for a person to read: "halfHeight" -> "Half Height", "GPUBudget" -> "GPU Budget",
    // "POST_UPDATE" -> "Post Update". Words start at a capital after a lower-case letter or digit, at the last capital
    // of an acronym followed by lower case, and at '_'; each word is capitalised, and an all-capital identifier longer
    // than an acronym is lowered after each word's first letter.
    [[nodiscard]] inline std::string identifierWords(const std::string_view id)
    {
        const auto upper = [](const char c) { return c >= 'A' && c <= 'Z'; };
        const auto small = [](const char c) { return c >= 'a' && c <= 'z'; };
        const auto lowerOrDigit = [&](const char c) { return small(c) || (c >= '0' && c <= '9'); };
        // all capitals: snake case ("POST_UPDATE") or a shouted word ("UPDATE"); a short one is an acronym ("GPU")
        const bool snake = (id.find('_') != std::string_view::npos || id.size() > 3) && std::ranges::none_of(id, small);
        std::string out;
        out.reserve(id.size() + 8);
        bool wordStart = true;
        for (sizet i = 0; i < id.size(); ++i)
        {
            const char c = id[i];
            if (c == '_' || c == ' ')
            {
                wordStart = true;
                continue;
            }
            const bool boundary = i > 0 && upper(c) && !snake &&
                (lowerOrDigit(id[i - 1]) || (upper(id[i - 1]) && i + 1 < id.size() && small(id[i + 1])));
            const bool first = boundary || wordStart;
            if (first && !out.empty())
            {
                out += ' ';
            }
            out += first && small(c)          ? static_cast<char>(c - 'a' + 'A')
                : !first && snake && upper(c) ? static_cast<char>(c - 'A' + 'a')
                                              : c;
            wordStart = false;
        }
        return out;
    }

    // A component field path for a person: each part without a trailing "Component", as words, joined by a single
    // right angle quotation mark ("EntityNameComponent.name" -> "Entity Name > Name").
    [[nodiscard]] inline std::string pathWords(std::string_view path)
    {
        std::string shown;
        while (!path.empty())
        {
            const sizet dot = path.find('.');
            std::string_view part = path.substr(0, dot);
            if (part.size() > 9 && part.ends_with("Component"))
            {
                part.remove_suffix(9);
            }
            if (!shown.empty())
            {
                shown += " \xE2\x80\xBA ";
            }
            shown += identifierWords(part);
            path = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);
        }
        return shown;
    }

    // a component's name for a person: its display name as words ("CapsuleColliderComponent" -> "Capsule Collider")
    [[nodiscard]] inline std::string componentTitle(const ComponentID component)
    {
        return identifierWords(componentDisplayName(component));
    }

    // The primary component, which names an unnamed entity and colours its chip: the first Game-category component of
    // the entity's archetype in component id (registration) order; else the first Engine-category one that is not the
    // Transform, the EntityName or the DisabledComponent flag; RuntimeDerived and Ui components never. Invalid: none.
    [[nodiscard]] inline ComponentID primaryComponentOf(const World& world, const Entity entity)
    {
        if (!world.isEntityAlive(entity))
        {
            return INVALID_COMPONENT_ID;
        }
        std::array<ComponentID, 64> ids;
        const u32 count = std::min<u32>(world.getEntityAspectIds(entity, ids), static_cast<u32>(ids.size()));
        ComponentID engine = INVALID_COMPONENT_ID;
        for (u32 i = 0; i < count; ++i)
        {
            const ComponentCategory category = getComponentMetadata(ids[i]).category;
            if (category == ComponentCategory::Game)
            {
                return ids[i];
            }
            if (engine == INVALID_COMPONENT_ID && category == ComponentCategory::Engine &&
                ids[i] != getComponentId<TransformComponent>() && ids[i] != getComponentId<EntityNameComponent>() &&
                ids[i] != getComponentId<DisabledComponent>())
            {
                engine = ids[i];
            }
        }
        return engine;
    }

    // the label of an unnamed entity, written over buffer
    inline std::string_view unnamedEntityLabel(std::string& buffer, const ComponentID primary, const Entity entity)
    {
        buffer.clear();
        std::format_to(std::back_inserter(buffer), "{} #{}",
                       primary == INVALID_COMPONENT_ID ? UNNAMED_ENTITY : componentDisplayName(primary),
                       entity.index());
        return buffer;
    }

    // the name, else the unnamed label (valid until buffer changes)
    [[nodiscard]] inline std::string_view entityLabel(std::string& buffer, const World& world, const Entity entity)
    {
        const auto* name =
            world.isEntityAlive(entity) ? world.tryGetConstComponent<EntityNameComponent>(entity) : nullptr;
        return name && !name->name.toView().empty()
            ? name->name.toView()
            : unnamedEntityLabel(buffer, primaryComponentOf(world, entity), entity);
    }
} // namespace spite
