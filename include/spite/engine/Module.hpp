#pragma once

#include <string_view>
#include <functional>

#include "base/InternedString.hpp"
#include "base/ModuleAbi.hpp"
#include "base/Platform.hpp"
#include "ecs/World.hpp"

// The runtime module contract: a module is a C++ DLL built against
// include/spite with the engine's toolset, CRT and configuration. It exports
// SPITE_MODULE_ENTRY returning a ModuleDescriptor; the host refuses it when the
// API stamp or toolset stamp (base/ModuleAbi.hpp) differs from its own.

#define SPITE_MODULE_ENTRY spiteModule
// Prefix of the entry point definition:
//   SPITE_MODULE_EXPORT const spite::ModuleDescriptor* SPITE_MODULE_ENTRY() { ... }
#ifdef SPITE_SHIPPING
#define SPITE_MODULE_EXPORT extern "C"
#elif defined(SPITE_PLATFORM_WINDOWS)
#define SPITE_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define SPITE_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace spite
{
    // Optional host-owned loading checkpoints. Call only between completed world mutations, with no live row
    // pointers or service locks: the host may render a frame before returning. Absent in ordinary module hosts.
    struct SceneLoadProgress
    {
        bool active = false;
        u64 renderedFrames = 0;
        std::function<void(std::string_view, u64, u64)> report;
    };

    inline bool sceneLoading(const World& world)
    {
        auto& root = world.services().rootServices();
        return root.contains<SceneLoadProgress>() && root.get<SceneLoadProgress>().active;
    }

    inline void sceneLoadCheckpoint(const World& world, std::string_view phase, u64 done = 0, u64 total = 0)
    {
        auto& root = world.services().rootServices();
        if (root.contains<SceneLoadProgress>())
        {
            const auto& progress = root.get<SceneLoadProgress>();
            if (progress.active && progress.report)
                progress.report(phase, done, total);
        }
    }

    class ServiceLocator;
    class ControlServer;
    class ConfigStore;

    // What a module registers through. owner is the module's name and the one
    // owner key every registry files its entries under, so unload drops them all:
    // registerComponent<T>(owner.id()), SystemDescriptor::moduleName and the
    // control owner = owner.toView(), ServiceLocator::registerService(service,
    // owner). Valid until unload returns.
    struct ModuleContext
    {
        InternedString owner;
        ServiceLocator& services; // the root container; worlds have their own below it
        World scene;
        ControlServer& control;
        ConfigStore* configStore = nullptr; // null when the host has no application
        // Frame-thread only. Work runs once at the next control pump, or is discarded on unload.
        // The callback must not retain component pointers or service locks between frames.
        std::function<void(std::function<void()>)> runAtFrameStart;
    };

    // apiStamp and toolset lead so a host of any build can read them.
    struct ModuleDescriptor
    {
        u64 apiStamp = SPITE_API_STAMP;
        const char* toolset = SPITE_TOOLSET_STAMP;
        const char* name = nullptr;
        // Return false or throw std::exception to refuse the load; what() appears in the refusal.
        // The host drops registrations made before either refusal.
        bool (*load)(const ModuleContext& context) = nullptr;
        // releases what the registries do not own (subscriptions, threads);
        // registry entries are dropped by the host afterwards
        void (*unload)(const ModuleContext& context) = nullptr;
        // registers what the module runs in a play world (systems, world
        // services) under context.owner: each play world at Play before its
        // systems initialize, and those already playing when the module loads.
        // A fresh Play clone has current relations but may not have a compiled
        // transform hierarchy yet; see EntityTransformManager.
        void (*attachWorld)(const ModuleContext& context, World world) = nullptr;
    };

    using ModuleEntryFn = const ModuleDescriptor* (*)();
    inline constexpr std::string_view MODULE_ENTRY_NAME = SPITE_DETAIL_MODULE_XSTR(SPITE_MODULE_ENTRY);
} // namespace spite
