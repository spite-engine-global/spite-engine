#pragma once

// Component registry: the process-global, world-independent vocabulary of
// component types. One registry per process, owned by SpiteEcs and shared by
// every world and every linked module; component ids are process-global values.
//
// The component NAME is the sole identity: typed and runtime registrations, from
// the host or a runtime module, address the same component through it. The
// by-description functions below are the exported surface; the typed templates
// are header-only convenience over them and derive the name from the C++ type
// (see detail::componentNameOf).

#include <cstring>
#include <span>
#include <typeinfo>
#include <vector>

#include "base/Assert.hpp"
#include "base/ImageRange.hpp"

#include "EcsExport.hpp"
#include "ComponentMetadata.hpp"
#include "EcsTypes.hpp"
#include "base/StringInterner.hpp"

namespace spite
{
    // Registers a component by runtime description. Returns the id of the existing
    // component when the name is already registered (metadata must be compatible).
    // Returns INVALID_COMPONENT_ID on an invalid description. schema, when given,
    // must outlive the owner's registration: unregisterComponentOwnership drops it.
    // category: the first registration of a name decides (a dormant component takes the re-registering
    // module's); later registrations of a live component keep it.
    SPITE_ECS_API ComponentID registerComponentDesc(const char* name, sizet sizeBytes, sizet alignmentBytes,
                                                    ComponentMetadata::ConstructionPolicyFn construct,
                                                    ComponentMetadata::DestructionPolicyFn destroy,
                                                    ComponentMetadata::MoveAndDestroyFn moveAndDestroy,
                                                    ComponentMetadata::CopyPolicyFn copy, uint64_t ownerModuleId = 0,
                                                    void* userData = nullptr, const StructSchema* schema = nullptr,
                                                    ComponentCategory category = ComponentCategory::Game);

    // INVALID_COMPONENT_ID when the name is not registered.
    SPITE_ECS_API ComponentID getComponentIdByName(const char* name);
    SPITE_ECS_API const char* getComponentName(ComponentID id);
    SPITE_ECS_API bool isComponentRegistered(const char* name);
    // every id ever issued, ids retired by a layout change included: the
    // buffer size getComponentIds needs
    SPITE_ECS_API sizet getRegisteredComponentCount();
    // Changes when component schema ownership or registrations change.
    SPITE_ECS_API u64 getComponentRegistryRevision();
    // writes up to bufferCapacity ids, returns the full registered count
    SPITE_ECS_API uint32_t getComponentIds(ComponentID* outBuffer, uint32_t bufferCapacity);
    // full metadata incl. lifecycle function pointers; asserts on an unregistered id
    SPITE_ECS_API const ComponentMetadata& getComponentMetadata(ComponentID id);
    // Components no other owner key registered (every copy of the module counts
    // as this owner): what a module unload strips from its engine's worlds before
    // unregisterComponentOwnership.
    SPITE_ECS_API std::vector<ComponentID> getComponentsOwnedOnlyBy(uint64_t ownerModuleId);
    // Drops a module's ownership records and schemas. A component another owner
    // registered moves to that owner's lifecycle thunks; one nobody else
    // registered goes dormant: its id stays (archetypes key on it), its thunks are
    // null until it is registered again.
    SPITE_ECS_API void unregisterComponentOwnership(uint64_t ownerModuleId);
    // Drops only the records of one loaded copy of a module: those of the owner
    // whose thunks or schema lie in its image. Engines that each map their own copy
    // of one module register under one owner key; the other copies' records stay.
    SPITE_ECS_API void unregisterComponentOwnership(uint64_t ownerModuleId, const ImageRange& image);

    // Components with any ownership record, of any owner key, whose thunks or schema lie in
    // image, whether or not the metadata currently uses it: what an unmap of that image
    // would leave dangling.
    SPITE_ECS_API std::vector<ComponentID> getComponentsWithRecordsIn(const ImageRange& image);

    namespace detail
    {
        // lifecycle thunks matching the typed C++ semantics of T
        template <t_component T>
        inline ComponentMetadata makeComponentDesc(const char* name)
        {
            ComponentMetadata desc;
            desc.name = toHashedString(name);
            desc.size = sizeof(T);
            desc.alignment = alignof(T);
            desc.destructionPolicy = [](void* ptr)
            {
                if constexpr (!std::is_trivially_destructible_v<T>)
                {
                    static_cast<T*>(ptr)->~T();
                }
            };
            desc.moveAndDestroy = [](void* dest, void* src, sizet)
            {
                if constexpr (std::is_trivially_move_constructible_v<T> && std::is_trivially_destructible_v<T>)
                {
                    std::memcpy(dest, src, sizeof(T));
                }
                else
                {
                    new (dest) T(std::move(*static_cast<T*>(src)));
                    static_cast<T*>(src)->~T();
                }
            };
            desc.constructionPolicy = [](void* ptr)
            {
                if constexpr (std::is_default_constructible_v<T>)
                {
                    new (ptr) T();
                }
            };
            // components without a copy constructor (owning GPU handles, etc.)
            // register with a null copyPolicy: consumers assert on it, so an
            // attempted copy is a loud error instead of aliased GPU state
            if constexpr (std::is_copy_constructible_v<T>)
            {
                desc.copyPolicy = [](void* dest, const void* src)
                {
                    if constexpr (std::is_trivially_copy_constructible_v<T>)
                    {
                        std::memcpy(dest, src, sizeof(T));
                    }
                    else
                    {
                        new (dest) T(*static_cast<const T*>(src));
                    }
                };
            }
            return desc;
        }
    } // namespace detail

    // Registers the component type under the name derived from the C++ type
    // (idempotent), attaching its SPITE_SCHEMA when it declares one. Same-name
    // registrations with compatible metadata converge on the existing component.
    template <t_component T>
    ComponentID registerComponent(uint64_t ownerModuleId = 0)
    {
        const char* name = detail::componentNameOf<T>();
        const ComponentMetadata desc = detail::makeComponentDesc<T>(name);
        return registerComponentDesc(name, desc.size, desc.alignment, desc.constructionPolicy, desc.destructionPolicy,
                                     desc.moveAndDestroy, desc.copyPolicy, ownerModuleId, nullptr,
                                     detail::componentSchemaOf<T>(), detail::componentCategoryOf<T>());
    }

    // memoized per type: one interned-name lookup on first use, a branch afterwards
    template <t_component T>
    ComponentID getComponentId()
    {
        static const ComponentID id = getComponentIdByName(detail::componentNameOf<T>());
        SASSERTM(id != INVALID_COMPONENT_ID, "Component type used before registerComponent<T>: {}", typeid(T).name());
        return id;
    }
} // namespace spite
