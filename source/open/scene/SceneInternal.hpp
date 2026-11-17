#pragma once

// Internal scene-module helpers shared by the collect, stream and apply TUs.

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/Schema.hpp"
#include "base/memory/Memory.hpp"
#include "ecs/ComponentMetadata.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "scene/SceneDocument.hpp"
#include "serialization/FormatWalk.hpp"

namespace spite
{
    // Scene data is a registered component whose schema has at least one
    // persisted field; anything else (tags, events, derived state) is not.
    inline const ComponentMetadata* sceneComponentMetadata(const ComponentID id)
    {
        const ComponentMetadata& metadata = getComponentMetadata(id);
        return metadata.schema != nullptr && hasPersistedFields(*metadata.schema) ? &metadata : nullptr;
    }

    // Null for names this process does not register as scene data.
    inline const ComponentMetadata* sceneComponentMetadata(const char* typeName)
    {
        const ComponentID id = getComponentIdByName(typeName);
        return id == INVALID_COMPONENT_ID ? nullptr : sceneComponentMetadata(id);
    }

    // persistedLayoutHash per component, computed once per collect/parse/emit.
    class LayoutHashes
    {
    public:
        u64 of(const ComponentMetadata& metadata)
        {
            const auto [it, inserted] = m_hashes.try_emplace(metadata.id, 0);
            if (inserted)
            {
                it->second = persistedLayoutHash(*metadata.schema);
            }
            return it->second;
        }

    private:
        std::unordered_map<ComponentID, u64> m_hashes;
    };

    // Comma-separated names of the scene components whose payloads were written
    // under another persisted layout than the one registered now; empty when
    // every payload decodes. Unregistered components are not stale (skipped).
    inline std::string staleLayouts(const SceneDocument& document)
    {
        LayoutHashes hashes;
        std::vector<std::string_view> stale;
        for (const DocumentEntity& entity : document.entities)
        {
            for (const DocumentComponent& component : entity.components)
            {
                const ComponentMetadata* metadata = sceneComponentMetadata(component.typeName);
                if (metadata != nullptr && hashes.of(*metadata) != component.layoutHash &&
                    std::ranges::find(stale, std::string_view(component.typeName)) == stale.end())
                {
                    stale.emplace_back(component.typeName);
                }
            }
        }
        std::string names;
        for (const std::string_view name : stale)
        {
            names += names.empty() ? "" : ", ";
            names += name;
        }
        return names;
    }

    // Aggregates one change per (component, field, kind); the first detail wins.
    inline void recordChange(std::vector<SceneChange>& changes, const std::string_view component,
                             const std::string_view field, const SceneChangeKind kind,
                             const std::string_view detail = {})
    {
        for (SceneChange& change : changes)
        {
            if (change.kind == kind && change.component == component && change.field == field)
            {
                ++change.count;
                return;
            }
        }
        changes.push_back({std::string(component), std::string(field), kind, 1, std::string(detail)});
    }

    // Constructs one component instance through its registered metadata for
    // schema-driven payload <-> value conversions. Scene components must be
    // default-constructible.
    class ScopedInstance
    {
        struct AlignedDelete
        {
            std::align_val_t alignment;
            void operator()(void* ptr) const noexcept { ::operator delete(ptr, alignment); }
        };

    public:
        explicit ScopedInstance(const ComponentMetadata& metadata) :
            m_metadata(metadata),
            m_ptr(nullptr, AlignedDelete{std::align_val_t(std::max<sizet>(1, metadata.alignment))})
        {
            SASSERTM(metadata.constructionPolicy != nullptr,
                     "component {} has no construction policy; scene components must be "
                     "default-constructible",
                     metadata.name.toView());
            const auto alignment = static_cast<sizet>(m_ptr.get_deleter().alignment);
            const sizet rounded = ((metadata.size + alignment - 1) / alignment) * alignment;
            const sizet size = rounded == 0 ? alignment : rounded;
            m_ptr.reset(tryAllocate(size, alignment));
            if (!m_ptr)
            {
                allocationFailed(size, alignment);
            }
            metadata.constructionPolicy(m_ptr.get());
        }

        ~ScopedInstance()
        {
            if (m_metadata.destructionPolicy != nullptr)
            {
                m_metadata.destructionPolicy(m_ptr.get());
            }
        }

        ScopedInstance(const ScopedInstance&) = delete;
        ScopedInstance& operator=(const ScopedInstance&) = delete;

        [[nodiscard]] void* get() const { return m_ptr.get(); }

    private:
        const ComponentMetadata& m_metadata;
        std::unique_ptr<void, AlignedDelete> m_ptr;
    };
} // namespace spite
