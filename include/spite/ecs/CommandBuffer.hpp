#pragma once

#include <cstddef>
#include <mutex>
#include <vector>

#include "Aspect.hpp"
#include "ComponentMetadata.hpp"
#include "ComponentRegistry.hpp"
#include "EcsExport.hpp"
#include "EcsTypes.hpp"

namespace spite
{
    class ArchetypeManager;
    class EntityManager;

    // Deferred entity/component commands, flushed by commit(). Command buffers
    // live in exactly three homes: the per-stage buffers owned by the system
    // manager's schedule (committed at each stage boundary), the event manager's
    // internal buffer for event entities (committed at frame start), and
    // standalone buffers for host-serialized mutation outside the system loop,
    // created through World::createCommandBuffer() and committed by the host.
    class SPITE_ECS_API CommandBuffer
    {
    private:
        ArchetypeManager* m_archetypeManager;
        EntityManager* m_entityManager;

        enum class CommandType : u8
        {
            eCreateEntity,
            eDestroyEntity,
            eAddComponent,
            eRemoveComponent
        };

        struct CommandHeader
        {
            CommandType type;
            u16 size;
        };

        struct CreateEntityCmd
        {
            CommandHeader header;
            Entity entity;
        };

        struct DestroyEntityCmd
        {
            CommandHeader header;
            Entity entity;
        };

        struct AddComponentCmd
        {
            CommandHeader header;
            Entity entity;
            ComponentID componentId;
        };

        struct RemoveComponentCmd
        {
            CommandHeader header;
            Entity entity;
            ComponentID componentId;
        };

        mutable std::mutex m_bufferMutex;
        std::vector<std::byte> m_commandBuffer;
        sizet m_pendingCount = 0;

        struct DecodedCommand
        {
            Entity entity;
            CommandType type;
            ComponentID componentId;
            void* componentData;
            u32 sequence = 0; // position in the buffer; intra-entity command
                              // order is semantic (remove+add nets to replace)
        };

        // decodes the raw command stream into per-entity-processable commands; caller holds m_bufferMutex
        void decodeCommands(std::vector<DecodedCommand>& outCommands) const;

        void* writeCommand(CommandType type, u16 size);

    public:
        CommandBuffer(EntityManager* entityManager, ArchetypeManager* archetypeManager);
        ~CommandBuffer() = default;

        CommandBuffer(const CommandBuffer&) = delete;
        CommandBuffer& operator=(const CommandBuffer&) = delete;

        CommandBuffer(CommandBuffer&& other) noexcept;
        CommandBuffer& operator=(CommandBuffer&& other) noexcept;

        // returns a husk entity - an Entity with valid id and generation, but with no Archetype registration
        Entity createEntity();

        // recorded, applied when the buffer plays back; no transform reparenting or other reference editing.
        void destroyEntity(Entity entity);

        template <t_component T>
        void addComponent(Entity entity, T&& component);

        template <t_component T>
        void addComponent(Entity entity);

        void addComponent(Entity entity, ComponentID componentId);

        void addComponent(Entity entity, ComponentID componentId, void* componentData);

        template <t_component T>
        void removeComponent(Entity entity);

        void removeComponent(Entity entity, ComponentID componentId);

        // applies the queued commands into the world the buffer was created for
        void commit();

        // queued, uncommitted command count
        [[nodiscard]] sizet pendingCommandCount() const;
    };

    template <t_component T>
    void CommandBuffer::addComponent(Entity entity, T&& component)
    {
        const ComponentID componentId = getComponentId<T>();

        constexpr u16 dataAlignment = alignof(T);

        constexpr u16 headerSize = sizeof(AddComponentCmd);
        u16 dataOffset = (headerSize + dataAlignment - 1) & ~(dataAlignment - 1);

        u16 commandSize = dataOffset + sizeof(T);

        auto cmd = static_cast<AddComponentCmd*>(writeCommand(CommandType::eAddComponent, commandSize));
        cmd->entity = entity;
        cmd->componentId = componentId;

        void* dataPtr = reinterpret_cast<std::byte*>(cmd) + dataOffset;

        new (dataPtr) T(std::forward<T>(component));
    }

    template <t_component T>
    void CommandBuffer::addComponent(Entity entity)
    {
        const ComponentID componentId = getComponentId<T>();

        constexpr u16 dataAlignment = alignof(T);
        constexpr u16 headerSize = sizeof(AddComponentCmd);
        u16 dataOffset = (headerSize + dataAlignment - 1) & ~(dataAlignment - 1);
        u16 commandSize = dataOffset + sizeof(T);

        auto cmd = static_cast<AddComponentCmd*>(writeCommand(CommandType::eAddComponent, commandSize));
        cmd->entity = entity;
        cmd->componentId = componentId;

        void* dataPtr = reinterpret_cast<std::byte*>(cmd) + dataOffset;

        new (dataPtr) T();
    }

    template <t_component T>
    void CommandBuffer::removeComponent(Entity entity)
    {
        const ComponentID componentId = getComponentId<T>();
        constexpr u16 commandSize = sizeof(RemoveComponentCmd);

        auto cmd = static_cast<RemoveComponentCmd*>(writeCommand(CommandType::eRemoveComponent, commandSize));
        cmd->entity = entity;
        cmd->componentId = componentId;
    }
} // namespace spite
