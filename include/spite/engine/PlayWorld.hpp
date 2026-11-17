#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "base/ServiceLocator.hpp"
#include "ecs/World.hpp"
#include "engine/EngineExport.hpp"
#include "physics/PhysicsSettings.hpp"

namespace spite
{
    class AnimationWorld;
    class AudioWorld;
    class AssetRuntime;
    class EntityTransformManager;
    class ModuleHost;
    class PhysicsWorld;
    class UiContext;
    class UiViewContext;

    // The play world: a world cloned from the edit scene (cloneScene: its scene data, world to world),
    // with its own world services under the root, its own transforms and its own
    // physics (the edit scene has none: it never simulates) and animation (skins bind in the clone), so nothing done to
    // it reaches the edit scene. Its game UI stack (a UiContext and its view)
    // is its own too: game modules build game UI through the world's UiContext
    // service; edit-scene UI is code-built runtime state and is not cloned.
    // Destroying it is Stop. With a module host, the loaded modules register
    // their play systems into it (ModuleDescriptor::attachWorld) before its
    // systems initialize.
    class SPITE_ENGINE_API PlayWorld
    {
    public:
        // clones every non-UI entity of edit; its physics starts from `physics` (the host's physics.world section).
        // Optional assets must already be initialized. They bind before module callbacks and system initialization,
        // and unbind after module-world services are released, including failed startup.
        static std::unique_ptr<PlayWorld> create(const World& edit, const EntityTransformManager& editTransforms,
                                                 ServiceLocator& root, const PhysicsSettings& physics,
                                                 ModuleHost* modules = nullptr, AssetRuntime* assets = nullptr,
                                                 bool audioEnabled = false);
        ~PlayWorld();
        PlayWorld(const PlayWorld&) = delete;
        PlayWorld& operator=(const PlayWorld&) = delete;

        [[nodiscard]] World& world() { return m_world; }
        [[nodiscard]] EntityTransformManager& transforms() { return *m_transforms; }
        [[nodiscard]] PhysicsWorld& physics() { return *m_physics; }
        [[nodiscard]] AnimationWorld& animation() { return *m_animation; }
        [[nodiscard]] UiContext& ui() { return *m_ui; }
        [[nodiscard]] UiViewContext& uiView() { return *m_uiView; }
        // the play-world clone of an edit-scene entity; undefined when Play did not clone it or the play destroyed it
        [[nodiscard]] Entity cloneOf(Entity edit) const;
        // the edit-scene id a live play entity was cloned from; 0 for one the play created or destroyed
        [[nodiscard]] u64 sourceOf(const u64 play) const
        {
            const u32 index = Entity(play).index();
            return index < m_sources.size() && m_sources[index].play == play && m_world.isEntityAlive(Entity(play))
                ? m_sources[index].edit
                : 0;
        }
        // edit entity id -> play entity, for every entity Play cloned, destroyed ones included (check isEntityAlive)
        [[nodiscard]] const std::unordered_map<u64, Entity>& clones() const { return m_clones; }

    private:
        PlayWorld(ServiceLocator& root, const PhysicsSettings& physics, bool audioEnabled);

        ServiceLocator m_services;
        World m_world;
        std::unique_ptr<EntityTransformManager> m_transforms;
        std::unique_ptr<PhysicsWorld> m_physics;
        std::unique_ptr<AnimationWorld> m_animation;
        std::unique_ptr<AudioWorld> m_audio;
        std::unique_ptr<UiContext> m_ui;
        std::unique_ptr<UiViewContext> m_uiView;
        ModuleHost* m_modules = nullptr; // attached to, until destruction
        AssetRuntime* m_assets = nullptr; // initialized runtime, bound before module startup
        std::unordered_map<u64, Entity> m_clones; // edit entity id -> play entity
        struct Source
        {
            u64 play = 0;
            u64 edit = 0;
        };
        std::vector<Source> m_sources; // by play entity index
    };
} // namespace spite
