#pragma once

#include <functional>
#include <span>
#include <vector>

#include "base/Expected.hpp"
#include "base/InternedString.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    class World;

    // How a game module spawns baked models from C++ (static or rigged; a rigged copy binds its skeleton like the
    // instantiateModel command's). The asset runtime registers it in the root service locator (ModuleContext::services)
    // and unregisters it at shutdown. A model with one submesh and no rig can also be drawn by a single entity with a
    // MeshComponent {model, MESH, 0} and the model's material, which is cheaper than a copy of its hierarchy.
    struct ModelSpawner
    {
        // whether the manifest holds a baked model of that name
        std::function<bool(InternedString model)> hasModel;
        // Builds one copy of the model per parent into world (an invalid parent leaves that copy a root), reading and
        // planning the model once and adopting every copy in one staging world; returns the copies' roots in parent
        // order. world is the scene or an attached Play world. Engine Play binds assets before a module's attachWorld,
        // so startup may spawn there. Check service availability: a no-GPU engine does not initialize this service.
        // Frame thread, outside system execution. An unknown or
        // damaged model is an error and creates nothing.
        std::function<Expected<std::vector<Entity>>(const World& world, InternedString model,
                                                    std::span<const Entity> parents)>
            spawn;
    };
} // namespace spite
