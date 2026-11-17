#pragma once

#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "base/Handle.hpp"
#include "ecs/CommandBuffer.hpp"
#include "ecs/EcsTypes.hpp"
#include "ecs/SystemContext.hpp"
#include "world/DisabledComponent.hpp"
#include "world/WorldExport.hpp"

namespace spite
{
    class World;

    // The single source of truth for the parent/child relation of transform
    // entities: edges live in manager-internal storage, never in components.
    // Membership is the transform pair (TransformComponent +
    // TransformMatrixComponent) and is fed entirely by lifecycle events —
    // pair-added registers a member, transform removal unregisters it — so the
    // manager never probes the world to discover its members. A member without
    // an edge is a root. The manager compiles the relation into a DFS pre-order
    // index: a parent always sits at a lower position than its children and
    // every subtree is one contiguous range, so the frame update recomposes
    // world matrices in an order where a parent's matrix is current before any
    // child reads it. A relation or membership change re-derives only the
    // trees it touches (cost scales with their size); more queued changes than
    // members compile as one rebuild of the whole index.
    class SPITE_WORLD_API EntityTransformManager
    {
    public:
        static constexpr u32 NO_INDEX = static_cast<u32>(-1);

        explicit EntityTransformManager(World& world);
        EntityTransformManager(World& world, bool registerSchemas);
        ~EntityTransformManager();
        EntityTransformManager(const EntityTransformManager&) = delete;
        EntityTransformManager& operator=(const EntityTransformManager&) = delete;

        // Registers the manager's components and the named "TransformMatrices"
        // system (POST_UPDATE) into the world, so hosts never run the frame
        // update by hand.

        // Reparents any live entity under another, adding identity Transform + Matrix
        // to plain grouping entities (pending creations keep their queued pair). Rejects
        // self-parenting and cycles. Detaches from the previous parent. The
        // relation is stored in the manager; no components are written.
        bool attach(Entity child, Entity parent);

        // Detaches child from its parent; it becomes a root. False when the
        // relation does not exist.
        bool detach(Entity child);

        // Destroys child and its whole subtree through the world.
        void destroySubtree(Entity root);

        // Scene-load convenience: batch attach; past the member count it compiles
        // as one rebuild.
        void attachBatch(std::span<const std::pair<Entity, Entity>> childParentPairs);

        // Reserves a transform entity through the buffer: a husk handle plus
        // the queued transform pair, so hierarchy bookkeeping happens before
        // commit while registration fires through lifecycle events at commit.
        // With a parent, the relation is recorded immediately; the parent may
        // itself be a pending creation.
        Entity createTransformEntity(CommandBuffer& commandBuffer, Entity parent = Entity::undefined());

        // The named, rooted spawn for modules: a transform entity carrying an
        // EntityNameComponent (an empty name leaves it unnamed) and, with a
        // parent, attached under it; the caller adds the rest of its components.
        // The first creates the entity in the world now; the second reserves it
        // through the buffer like createTransformEntity, the parent possibly a
        // pending creation. A parent that is set but not a live
        // entity (destroyed or stale) creates nothing and
        // returns Entity::undefined(): check the result.
        Entity createNamedEntity(std::string_view name, Entity parent = Entity::undefined());
        Entity createNamedEntity(CommandBuffer& commandBuffer, std::string_view name,
                                 Entity parent = Entity::undefined());

        // Queues child and its whole subtree for destruction through the
        // buffer; the relation store prunes through lifecycle events at commit.
        void destroyTransformEntity(Entity root, CommandBuffer& commandBuffer);

        // Recompiles the index now when relations or membership changed, so the
        // reads below see edits made earlier in the frame (the frame update
        // compiles otherwise); after a rebuild here that update still
        // recomposes every matrix.
        void compile();

        // The relation store's live parent, current after every attach and
        // detach without a compile; undefined for a root.
        [[nodiscard]] Entity attachedParentOf(Entity entity) const;
        // parentOf's answer (members only) from the relation store, without a
        // compile: what the compiled index would say after a rebuild.
        [[nodiscard]] Entity currentParentOf(Entity entity) const;
        // subtreeOf's members, root first and parents before children, from
        // the relation store, without a compile; cost scales with the subtree.
        // False (root alone) when root is not a member. Replaces outSubtree's
        // contents: a caller collecting several roots appends each result itself.
        bool collectSubtree(Entity root, std::vector<Entity>& outSubtree) const;

        // Bumped by every relation or membership change: a client polling it
        // per frame (the editor hierarchy) learns the structure may differ.
        [[nodiscard]] u64 structureGeneration() const;

        // -- entity enabled state ------------------------------------
        // DisabledComponent is the entity's own saved flag; the manager keeps
        // InactiveComponent on every entity that is disabled or whose member
        // parent is inactive (an entity without a transform has no parent:
        // active = enabled). Disabling or enabling, attaching, detaching,
        // creating or loading queue the touched roots; the TransformMatrices
        // system applies the queue through the stage command buffer, and
        // applyActivity (also run by applyScene and cloneScene) applies it now.
        // An empty queue costs one check, a pass walks only below a root whose
        // tag changed, and a world with nothing disabled queues nothing.
        // setEntityEnabled and applyActivity make immediate structural
        // changes: call them on the frame thread, never from inside a system
        // (a parallel stage races with chunk iteration). A system adds or
        // removes DisabledComponent through ctx.getCommandBuffer(); the
        // TransformMatrices system then derives the tags.

        // Adds or removes DisabledComponent on a live entity and applies the
        // change at once. False when the entity is not live or already has
        // that state.
        bool setEntityEnabled(Entity entity, bool enabled);
        // The entity's own flag / its inherited state, as of the last applied
        // pass; true for an entity that is not live.
        [[nodiscard]] bool isEntityEnabled(Entity entity) const;
        [[nodiscard]] bool isEntityActive(Entity entity) const;
        // Applies the queued changes now, outside the frame update.
        void applyActivity();
        // Bumped whenever an InactiveComponent is added or removed (committed,
        // not queued): a client polling it per frame re-reads activity only then.
        [[nodiscard]] u64 activityGeneration() const;
        // Cumulative children examined by activity passes (test hook).
        [[nodiscard]] u64 activityWalkSteps() const;
        // Cumulative ancestor-chain steps taken by attach's cycle check (test hook).
        [[nodiscard]] u64 attachWalkSteps() const;
        // Cumulative ancestor-chain steps taken while applying queued hierarchy changes (test hook).
        [[nodiscard]] u64 applyWalkSteps() const;
        // Full index rebuilds so far (test hook).
        [[nodiscard]] u64 rebuildCount() const;
        // Discards the compiled index and rebuilds it from the relation store
        // and the member set now (test hook: the reference an incremental
        // index is checked against).
        void forceRebuild();
        // Every member in compiled DFS pre-order: roots by entity index, each
        // followed by its subtree (siblings by entity index), as of the last
        // compile (call compile() first for edits made this frame). Built on
        // the first call after an incremental change, so it costs the
        // member count; the frame update never calls it. Valid until
        // the next structure change.
        [[nodiscard]] std::span<const Entity> compiledOrder() const;

        // These queries read the compiled index, not the live relation store.
        // Play clone relations exist before module attachWorld, but the index may
        // not yet be compiled. Use currentParentOf or collectSubtree there, or
        // call compile() outside a system before using the compiled queries.
        [[nodiscard]] bool isInHierarchy(Entity entity) const;
        [[nodiscard]] Entity parentOf(Entity entity) const;
        [[nodiscard]] u32 depthOf(Entity entity) const;
        // The entity and its descendants in the order compiledOrder() lists
        // them — the save/copy/delete primitive. The span is valid until the
        // next structure change.
        [[nodiscard]] std::span<const Entity> subtreeOf(Entity entity) const;
        void childrenOf(Entity entity, std::vector<Entity>& outChildren) const;

    private:
        friend SPITE_WORLD_API void updateTransformMatrices(SystemContext& ctx);

        // Thin-system entry: recompiles the index when relations or membership
        // changed, then recomposes world matrices over the dirty subtrees.
        void update(SystemContext& ctx);
        struct State;
        std::unique_ptr<State> m_state;
    };

    // The module's frame entry: fetches the manager from the world's service
    // locator and runs the frame update. Registered by the manager as the
    // "TransformMatrices" system; no-op without a bound service.
    SPITE_WORLD_API void updateTransformMatrices(SystemContext& ctx);
} // namespace spite
