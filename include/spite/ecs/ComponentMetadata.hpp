#pragma once
#include "base/Assert.hpp"
#include "base/InternedString.hpp"
#include "base/Platform.hpp"
#include "base/TypeName.hpp"
#include "ecs/EcsTypes.hpp"

#include <concepts>
#include <iterator>
#include <optional>
#include <string_view>
#include <typeinfo>

namespace spite
{
    struct StructSchema;

    using ComponentID = u32;
    constexpr ComponentID INVALID_COMPONENT_ID = 0;

    constexpr std::string_view COMPONENT_CATEGORY_NAMES[] = {"game", "engine", "runtimeDerived", "ui"};

    constexpr std::string_view componentCategoryName(const ComponentCategory category)
    {
        return COMPONENT_CATEGORY_NAMES[static_cast<sizet>(category)];
    }

    constexpr std::optional<ComponentCategory> parseComponentCategory(const std::string_view name)
    {
        for (sizet i = 0; i < std::size(COMPONENT_CATEGORY_NAMES); ++i)
        {
            if (COMPONENT_CATEGORY_NAMES[i] == name)
            {
                return static_cast<ComponentCategory>(i);
            }
        }
        return std::nullopt;
    }

    struct ComponentMetadata
    {
        InternedString name;
        // The component's runtime description; null when it declared
        // none. Owned by a registering module and dropped when that module's
        // ownership is unregistered.
        const StructSchema* schema = nullptr;
        // Set by the first registration of the name (ComponentCategory); never serialized.
        ComponentCategory category = ComponentCategory::Game;
        // A single function pointer to handle all destruction side-effects.
        using DestructionPolicyFn = void (*)(void* componentPtr);
        using MoveAndDestroyFn = void (*)(void* destPtr, void* srcPtr, sizet size);
        using ConstructionPolicyFn = void (*)(void* componentPtr);
        using CopyPolicyFn = void (*)(void* destPtr, const void* srcPtr);

        ComponentID id = INVALID_COMPONENT_ID;
        sizet size = 0;
        sizet alignment = 0;

        // opaque per-component payload owned by the registrant; lets engine-side
        // systems (reflection, editor, debug) attach data without touching ECS internals
        void* userData = nullptr;

        DestructionPolicyFn destructionPolicy = nullptr;
        MoveAndDestroyFn moveAndDestroy = nullptr;
        ConstructionPolicyFn constructionPolicy = nullptr;
        CopyPolicyFn copyPolicy = nullptr;

        constexpr ComponentMetadata() = default;

        constexpr ComponentMetadata(ComponentID id, sizet size, sizet alignment, DestructionPolicyFn policyFn,
                                    MoveAndDestroyFn moveAndDestroyFn, ConstructionPolicyFn constructionPolicyFn,
                                    CopyPolicyFn copyPolicyFn = nullptr, InternedString name = "") :
            name(name), id(id), size(size), alignment(alignment), destructionPolicy(policyFn),
            moveAndDestroy(moveAndDestroyFn), constructionPolicy(constructionPolicyFn), copyPolicy(copyPolicyFn)
        {
        }
    };

    // The component name is the sole identity in the registry: C, C++ and runtime
    // registrations all address the same component through it. These helpers are
    // the single bridge from a C++ component type to that name.

    namespace detail
    {
        template <class T>
        concept hasComponentName = requires {
            { T::SPITE_COMPONENT_NAME } -> std::convertible_to<const char*>;
        };

        // The registered component name of a C++ component type: the explicit
        // SPITE_COMPONENT_NAME member when the type declares one, otherwise derived
        // from the C++ type name.
        template <class T>
        const char* componentNameOf()
        {
            if constexpr (hasComponentName<T>)
            {
                return T::SPITE_COMPONENT_NAME;
            }
            else
            {
                static const std::string_view derived = typeNameSegment(typeid(T).name());
                return derived.data();
            }
        }

        // The category the type declares (SPITE_COMPONENT_CATEGORY, usually inherited from a ICategorizedComponent
        // base); Game when it declares none.
        template <class T>
        constexpr ComponentCategory componentCategoryOf()
        {
            if constexpr (requires {
                              { T::SPITE_COMPONENT_CATEGORY } -> std::convertible_to<ComponentCategory>;
                          })
            {
                return T::SPITE_COMPONENT_CATEGORY;
            }
            else
            {
                return ComponentCategory::Game;
            }
        }

        template <class T>
        const StructSchema* componentSchemaOf()
        {
            if constexpr (requires { T::spiteSchema(); })
            {
                return &T::spiteSchema();
            }
            else
            {
                return nullptr;
            }
        }
    } // namespace detail
} // namespace spite
