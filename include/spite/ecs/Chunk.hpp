/**
 * @file Chunk.hpp
 * @brief Defines the Chunk class for ECS storage.
 */

#pragma once
#include <array>
#include <bitset>
#include <cstddef>
#include <memory>
#include <span>


#include "base/Assert.hpp"

#include "Aspect.hpp"
#include "ComponentRegistry.hpp"
#include "EcsExport.hpp"
#include "EcsTypes.hpp"

namespace spite
{
    /**
     * @brief Constant for the default chunk capacity (number of entities per chunk).
     */
    constexpr sizet DEFAULT_CHUNK_CAPACITY = 64;

    /**
     * @brief Constant for the default inline capacity for components in a chunk.
     */
    constexpr sizet DEFAULT_COMPONENTS_INLINE_CAPACITY = 8;

    // How a world frame's end rotates change tracking: after a frame that ran simulation, or after an idle
    // (paused, unstepped) frame, the first since the last simulated frame or a later one.
    enum class ModificationRotation : u8
    {
        Simulated,
        FirstIdle,
        Idle
    };

    // A chunk's per-component change bitsets: this frame's writes, last frame's, and the writes since the last
    // simulated frame (kept over idle frames).
    enum class ChangeSet : u8
    {
        Written,
        LastFrame,
        Unsimulated
    };

    class Archetype;
    class ChangeSummary;

    /**
     * @brief A storage unit that holds entities with the same component layout (aspect).
     * @details A Chunk stores a fixed number of entities (up to 16) that have the same
     *          combination of components, organizing the data in a structure-of-arrays (SoA) layout
     *          for optimal performance.
     */
    class SPITE_ECS_API Chunk
    {
        static constexpr sizet CAPACITY = DEFAULT_CHUNK_CAPACITY;

    private:
        struct State;
        struct StateDeleter
        {
            void operator()(State* state) const noexcept;
        };
        std::unique_ptr<State, StateDeleter> m_state;
        const Aspect* m_aspect = nullptr;
        sizet m_count = 0;
        std::span<std::byte* const> m_componentDataStarts;
        std::span<const std::bitset<CAPACITY>> m_writeOnlyModifications;
        std::span<const std::bitset<CAPACITY>> m_readOnlyModifications;
        std::span<const u64> m_rotatedWriteTicks;
        u32 m_indexInArchetype = 0;

        friend class Archetype;
        // binds the chunk to an archetype slot and publishes its non-empty bitsets there
        void attachChangeSummary(ChangeSummary* summary, u32 indexInArchetype);
        void noteChange(ChangeSet set, sizet componentIndexInChunk);

    public:
        Chunk(const Aspect& aspect);
        ~Chunk();
        Chunk(const Chunk&) = delete;
        Chunk& operator=(const Chunk&) = delete;
        Chunk(Chunk&& other) noexcept;
        Chunk& operator=(Chunk&& other) noexcept;
        /**
         * @brief Check if the chunk is full.
         * @return True if the chunk has reached its capacity, false otherwise
         */
        [[nodiscard]] bool full() const;
        /**
         * @brief Check if the chunk is empty.
         * @return True if no entities are in the chunk, false otherwise
         */
        [[nodiscard]] bool empty() const;
        /**
         * @brief Get the number of entities in this chunk.
         * @return Current number of entities
         */
        [[nodiscard]] sizet size() const;
        /**
         * @brief Get the capacity of this chunk.
         * @return Maximum number of entities that can be stored in the chunk
         */
        [[nodiscard]] sizet capacity() const;
        /**
         * @brief Get the aspect defining this chunk's component layout.
         * @return Reference to the aspect
         */
        [[nodiscard]] const Aspect& aspect() const;

        [[nodiscard]] sizet getStorageBlockSize() const;

        // storage held by the unused slots of a non-empty chunk (an empty chunk is not counted as waste)
        [[nodiscard]] sizet unusedSlotBytes() const
        {
            return size() > 0 && capacity() > size() ? getStorageBlockSize() * (capacity() - size()) / capacity() : 0;
        }

        template <typename T>
        sizet getComponentIndex();

        /**
         * @brief Get a const pointer to the component array for a given type.
         * @details Should be used for per-chunk iteration.
         * @tparam T Component type
         * @return Const pointer to the component array
         */
        template <typename T>
        T* getComponents() const;
        /**
         * @brief Get a mutable pointer to the component array for a given type.
         * @details Should be used for per-chunk iteration. Marks components as modified.
         * @tparam T Component type
         * @return Mutable pointer to the component array
         */
        template <typename T>
        T* getComponents();
        // will not mark components as modified even if they were written-to
        template <typename T>
        T* getComponentsWithoutModification();
        /**
         * @brief Add an entity to the chunk and return its index.
         * @details The caller is responsible for constructing/placing the component data.
         * @param[in] entity Entity to add
         * @return Index within the chunk where the entity's components will be stored
         */
        sizet addEntity(const Entity entity);
        /**
         * @brief Remove an entity at a given index within this chunk.
         * @details Uses swap-and-pop: the last entity in the chunk takes the place of the removed one.
         *          Destructors for removedEntity should be called in Archetype.
         * @param[in] entityChunkIndex Index of the entity to remove
         * @return The Entity ID that was moved into the `entityChunkIndex` slot
         *         (or Entity::undefined if the last entity was removed)
         */
        Entity removeEntityAndSwap(const sizet entityChunkIndex);
        /**
         * @brief Get the entity at the specified index.
         * @param[in] entityChunkIndex Index of the entity to retrieve
         * @return Entity at the given index
         */
        [[nodiscard]] Entity entity(const sizet entityChunkIndex) const;
        /**
         * @brief Get a span of all entities in the chunk.
         * @return Span containing all entities in the chunk
         */
        [[nodiscard]] std::span<const Entity> entities() const;
        /**
         * @brief Check if a component was modified in the last frame by its index in the aspect.
         * @param[in] componentIndexInChunk Index of the component in the chunk
         * @param[in] entityIndexInChunk Index of the entity in the chunk
         * @return True if the component was modified in the last frame, false otherwise
         */
        [[nodiscard]] bool wasModifiedLastFrameByIndex(const sizet componentIndexInChunk,
                                                       const sizet entityIndexInChunk) const;
        // live entities whose component was written last frame or earlier in the
        // current frame: what a consumer running once per frame has not seen yet
        [[nodiscard]] std::bitset<CAPACITY> modifiedSinceLastFrameByIndex(const sizet componentIndexInChunk) const
        {
            std::bitset<CAPACITY> live;
            live.set();
            live >>= CAPACITY - m_count;
            return (m_readOnlyModifications[componentIndexInChunk] | m_writeOnlyModifications[componentIndexInChunk]) &
                live;
        }
        // live entities written since the last frame end (a world frame's resets move them
        // into the "last frame" set): a consumer scanning once per frame sees each write once
        [[nodiscard]] std::bitset<CAPACITY> modifiedThisFrameByIndex(const sizet componentIndexInChunk) const
        {
            std::bitset<CAPACITY> live;
            live.set();
            live >>= CAPACITY - m_count;
            return m_writeOnlyModifications[componentIndexInChunk] & live;
        }
        // The process-wide change tick: every write stamps its column with the current value. A consumer
        // running once per frame takes the value at the start of its run (this returns it and advances the
        // tick): on its next run a column stamped no later holds only writes that run already saw.
        static u64 advanceChangeTick();
        // what a consumer that last ran at seenTick has not handled: this frame's writes, and last frame's
        // when a write of last frame followed that run (per column, so a late write re-lists the column's
        // rows; a column written in both frames before each run lists only this frame's)
        [[nodiscard]] std::bitset<CAPACITY> modifiedSinceTickByIndex(const sizet componentIndexInChunk,
                                                                     const u64 seenTick) const
        {
            return m_rotatedWriteTicks[componentIndexInChunk] > seenTick
                ? modifiedSinceLastFrameByIndex(componentIndexInChunk)
                : modifiedThisFrameByIndex(componentIndexInChunk);
        }
        [[nodiscard]] bool hasAnyModifiedLastFrameByIndices(
            std::span<const int> modifiedComponentIndicesInChunk,
            std::span<const int> enabledComponentIndicesInChunk = {}) const;
        /**
         * @brief Reset modification tracking for the next frame.
         */
        void resetModificationTracking(ModificationRotation rotation = ModificationRotation::Simulated);
        // trades the last frame's writes with the writes since the last simulated frame
        void swapUnsimulatedModifications();
        /**
         * @brief Get a pointer to mutable component data using a pre-calculated index.
         * @details Provides O(1) access to component data.
         * @param[in] componentIndexInChunk Index of the component in the chunk
         * @param[in] entityIndexInChunk Index of the entity in the chunk
         * @return Pointer to the component data
         */
        void* getComponentDataPtrByIndex(sizet componentIndexInChunk, sizet entityIndexInChunk);
        /**
         * @brief Get a pointer to const component data using a pre-calculated index.
         * @details Provides O(1) access to const component data.
         * @param[in] componentIndexInChunk Index of the component in the chunk
         * @param[in] entityIndexInChunk Index of the entity in the chunk
         * @return Const pointer to the component data
         */
        const void* getConstComponentDataPtrByIndex(sizet componentIndexInChunk, sizet entityIndexInChunk) const;

        /**
         * @brief Mark a component as modified using a pre-calculated index.
         * @details Provides O(1) access for marking components as modified.
         * @param[in] componentIndexInChunk Index of the component in the chunk
         * @param[in] entityIndexInChunk Index of the entity in the chunk
         */
        void markModifiedByIndex(sizet componentIndexInChunk, sizet entityIndexInChunk);

        // Copies the per-row state a component carries when its row moves to another chunk:
        // the enabled bit, last frame's change flag and the one since the last simulated frame. This frame's flag
        // is set by addEntity.
        void copyRowStateByIndex(sizet componentIndexInChunk, sizet entityIndexInChunk, const Chunk& from,
                                 sizet fromComponentIndexInChunk, sizet fromEntityIndexInChunk);

        /**
         * @brief Enable a component for an entity by index.
         * @param[in] componentIndexInChunk Index of the component in the chunk
         * @param[in] entityIndexInChunk Index of the entity in the chunk
         */
        void enableComponentByIndex(sizet componentIndexInChunk, sizet entityIndexInChunk);

        /**
         * @brief Disable a component for an entity by index.
         * @param[in] componentIndexInChunk Index of the component in the chunk
         * @param[in] entityIndexInChunk Index of the entity in the chunk
         */
        void disableComponentByIndex(sizet componentIndexInChunk, sizet entityIndexInChunk);


        /**
         * @brief Check if a component is enabled for an entity by index.
         * @param[in] componentIndexInChunk Index of the component in the chunk
         * @param[in] entityIndexInChunk Index of the entity in the chunk
         * @return True if the component is enabled for an entity, false otherwise
         */
        [[nodiscard]] bool isComponentEnabledByIndex(sizet componentIndexInChunk, sizet entityIndexInChunk) const;
        [[nodiscard]] int getComponentIndex(ComponentID componentId) const;
        [[nodiscard]] bool hasComponent(ComponentID componentId) const;
        [[nodiscard]] bool wasModifiedLastFrame(ComponentID componentId, sizet entityIndexInChunk) const;
        [[nodiscard]] bool isComponentEnabled(ComponentID componentId, sizet entityIndexInChunk) const;
        void markAllModifiedByIndex(sizet componentIndexInChunk);
        void markAllModified(ComponentID componentId);
        // does not trigger component modification
        template <typename T>
        T* getComponentsByIndexWithoutModification(sizet componentIndexInAspect) const;

        // does not trigger component modification
        void* getComponentArrayByIndex(sizet componentIndexInAspect);
        const void* getConstComponentArrayByIndex(sizet componentIndexInAspect) const;
        sizet getComponentSizeByIdx(sizet componentIndexInAspect) const;

        // -- typed column access (component resolved via the global registry) ------

        template <t_component T>
        [[nodiscard]] const T* readColumn() const
        {
            const int index = getComponentIndex(getComponentId<T>());
            return index == -1 ? nullptr : static_cast<const T*>(getConstComponentArrayByIndex(index));
        }

        // mutable access without the modification trigger; pair with markModified<T>
        template <t_component T>
        [[nodiscard]] T* mutableColumn()
        {
            const int index = getComponentIndex(getComponentId<T>());
            return index == -1 ? nullptr : static_cast<T*>(getComponentArrayByIndex(index));
        }

        // write access marks the whole column modified
        template <t_component T>
        [[nodiscard]] T* writeColumn()
        {
            const int index = getComponentIndex(getComponentId<T>());
            if (index == -1)
            {
                return nullptr;
            }
            markAllModifiedByIndex(index);
            return static_cast<T*>(getComponentArrayByIndex(index));
        }

        template <t_component T>
        void markModified(const sizet entityIndexInChunk)
        {
            const int index = getComponentIndex(getComponentId<T>());
            if (index != -1)
            {
                markModifiedByIndex(index, entityIndexInChunk);
            }
        }

        template <t_component T>
        void markAllModified()
        {
            const int index = getComponentIndex(getComponentId<T>());
            if (index != -1)
            {
                markAllModifiedByIndex(index);
            }
        }

        template <t_component T>
        [[nodiscard]] bool wasModifiedLastFrame(const sizet entityIndexInChunk) const
        {
            const int index = getComponentIndex(getComponentId<T>());
            return index != -1 && wasModifiedLastFrameByIndex(index, entityIndexInChunk);
        }

        template <t_component T>
        [[nodiscard]] bool isComponentEnabled(const sizet entityIndexInChunk) const
        {
            const int index = getComponentIndex(getComponentId<T>());
            return index != -1 && isComponentEnabledByIndex(index, entityIndexInChunk);
        }

        template <t_component T>
        [[nodiscard]] bool hasComponent() const
        {
            return hasComponent(getComponentId<T>());
        }
    };

    template <typename T>
    sizet Chunk::getComponentIndex()
    {
        const auto& aspectIds = m_aspect->getComponentIds();
        auto id = getComponentId<T>();
        SASSERT(m_aspect->contains(id));
        sizet componentIdx = 0;
        for (int i = 0; i < aspectIds.size(); ++i)
        {
            if (aspectIds[i] == id)
            {
                componentIdx = i;
                break;
            }
        }

        return componentIdx;
    }
    /**
     * @brief Get a const pointer to the component array for a given type.
     * @tparam T Component type
     * @param[in] this Chunk object
     * @return Const pointer to the component array
     */
    template <typename T>
    T* Chunk::getComponents() const
    {
        auto id = getComponentId<T>();
        SASSERT(m_aspect->contains(id));

        const auto& aspectIds = m_aspect->getComponentIds();
        sizet componentIdx = 0;
        for (sizet i = 0; i < aspectIds.size(); ++i)
        {
            if (aspectIds[i] == id)
            {
                componentIdx = i;
                break;
            }
        }
        return reinterpret_cast<T*>(m_componentDataStarts[componentIdx]);
    }
    /**
     * @brief Get a mutable pointer to the component array for a given type.
     * @details Marks components as modified.
     * @tparam T Component type
     * @param[in] this Chunk object
     * @return Mutable pointer to the component array
     */
    template <typename T>
    T* Chunk::getComponents()
    {
        auto id = getComponentId<T>();
        SASSERT(m_aspect->contains(id));
        const auto& aspectIds = m_aspect->getComponentIds();
        sizet componentIdx = 0;

        for (sizet i = 0; i < aspectIds.size(); ++i)
        {
            if (aspectIds[i] == id)
            {
                componentIdx = i;
                break;
            }
        }
        markAllModifiedByIndex(componentIdx);
        return reinterpret_cast<T*>(m_componentDataStarts[componentIdx]);
    }

    template <typename T>
    T* Chunk::getComponentsWithoutModification()
    {
        auto id = getComponentId<T>();
        SASSERT(m_aspect->contains(id));

        const auto& aspectIds = m_aspect->getComponentIds();
        sizet componentIdx = 0;

        for (sizet i = 0; i < aspectIds.size(); ++i)
        {
            if (aspectIds[i] == id)
            {
                componentIdx = i;
                break;
            }
        }
        return getComponentsByIndexWithoutModification<T>(componentIdx);
    }

    template <typename T>
    T* Chunk::getComponentsByIndexWithoutModification(sizet componentIndexInAspect) const
    {
        SASSERT(componentIndexInAspect < m_componentDataStarts.size());
        return reinterpret_cast<T*>(m_componentDataStarts[componentIndexInAspect]);
    }
} // namespace spite

// The engine writes the ECS vocabulary as `ecs::X`; publish the expected aliases.
namespace spite::ecs
{
    using spite::Chunk;
}
