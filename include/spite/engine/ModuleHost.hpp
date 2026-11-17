#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/Expected.hpp"
#include "base/ImageRange.hpp"
#include "engine/EngineExport.hpp"
#include "base/InternedString.hpp"
#include "ecs/World.hpp"

namespace spite
{
    class ServiceLocator;
    class ControlServer;
    class ConfigStore;
    struct ModuleContext;
    struct ModuleDescriptor;

    struct ModuleHostDesc
    {
        ServiceLocator* services = nullptr; // root
        ControlServer* control = nullptr;
        World scene;
        ConfigStore* configStore = nullptr;
        // where the shadow copies go; Windows locks a loaded DLL, so the build
        // keeps writing the original; copies of processes that died go at startup
        std::filesystem::path shadowDirectory;
        // Appends one line per engine-owned registry outside the worlds that
        // still holds code from an unloading module's image (UI reactions).
        std::function<void(const ImageRange& image, std::vector<std::string>& holders)> findHolders;
        // the one directory available() scans for module DLLs; empty: the executable's directory
        std::filesystem::path searchDirectory;
    };

    // A module DLL available() found.
    struct AvailableModule
    {
        std::string name; // the descriptor's name when loaded, else the file name without ".dll"
        std::filesystem::path path;
        bool loaded = false;
        u64 apiStamp = 0; // the loaded descriptor's; 0 unloaded: reading it needs the module's code to run
        std::string manifest; // the sidecar <stem>.module.json as compact JSON; empty without one
        std::string manifestError; // why the sidecar was ignored
        // the FILETIME the file had when the loaded build was staged; 0 unloaded (a rebuilt file differs)
        u64 loadedWriteFiletime = 0;
    };

    // What ModuleHost::scan found in one directory.
    struct ModuleScan
    {
        std::vector<AvailableModule> modules; // by file name; none marked loaded
        u32 skipped = 0; // DLLs left out for a name that is not valid UTF-16
        bool truncated = false; // a bound (entries, DLLs, time, cancel) ended the scan early
        std::string error; // why the directory could not be listed
    };

    struct ModuleReload
    {
        u32 restored = 0; // component instances restored into the new build
        // state that did not survive: components the new build dropped,
        // fields it could not read, an image kept mapped
        std::vector<std::string> warnings;
        std::string error; // why the load failed, when a name collision refused it
        // where the reload spent its time: instance JSON save, the old build's unload and purge,
        // the new build's load, restoring the saved instances
        double saveMs = 0, unloadMs = 0, loadMs = 0, restoreMs = 0;
    };

    // Loads and unloads runtime modules.
    class SPITE_ENGINE_API ModuleHost
    {
    public:
        struct LoadedModule
        {
            std::string name;
            std::filesystem::path source;
            std::filesystem::path shadow;
            const ModuleDescriptor* descriptor = nullptr;
            void* library = nullptr; // HMODULE
            // the source's write FILETIME when it was staged (the shadow copy keeps it); 0 when unreadable
            u64 writeFiletime = 0;
        };

        explicit ModuleHost(ModuleHostDesc desc);
        // modules must be unloaded first: only the caller knows every world
        ~ModuleHost();
        ModuleHost(const ModuleHost&) = delete;
        ModuleHost& operator=(const ModuleHost&) = delete;

        // The version, toolset stamp and name checks load() applies.
        [[nodiscard]] static Expected<void> check(const ModuleDescriptor& descriptor);
        // The file load() maps for path: ".dll" is optional, a relative path
        // resolves against the executable's directory.
        [[nodiscard]] static std::filesystem::path resolvePath(std::filesystem::path path);
        // Refused, with the reason logged and returned, when the file or its
        // entry point is missing, the process already maps the file (an
        // engine-owned DLL), the API stamp or toolset stamp differs, the
        // name is already loaded or is engine-owned (isEngineOwned), or the
        // module's load fails.
        Expected<void> load(const std::filesystem::path& path);

        // What isRemotePath asks the system; tests replace it.
        struct PathProbe
        {
            std::function<unsigned(const std::wstring& root)> driveType; // GetDriveTypeW
            std::function<std::filesystem::path(const std::filesystem::path&)> finalPath; // empty result: unresolved
        };
        [[nodiscard]] static const PathProbe& systemProbe();
        // Whether the file lives on another machine: a UNC path, a drive that is a network mapping, or either after
        // the final path resolves a subst, a junction or a symlink. Not covered: a path the OS cannot open to
        // resolve (treated as local).
        [[nodiscard]] static bool isRemotePath(const std::filesystem::path& path,
                                               const PathProbe& probe = systemProbe());
        // A name unload must never purge: an owner key control entries or
        // render pass setups are registered under (engine, host), or the name
        // of a DLL the process maps (a core module, e.g. SpiteRenderRuntime,
        // whose systems use it as moduleName). Not covered: a system moduleName
        // or service owner key the engine uses that is neither.
        [[nodiscard]] bool isEngineOwned(std::string_view name) const;
        // Calls the module's unload, then drops everything its owner key holds:
        // render pass setups (the remaining setups re-finalize), systems and
        // component instances in worlds, services (and their tables)
        // in the root and each world's container, control commands and queries,
        // component ownership and schemas; then frees the DLL, unless code from
        // it is still referenced (a component registered under another owner
        // key, a system under another moduleName, a lifecycle subscription, a
        // UI reaction): those are logged and the image stays mapped. Services
        // registered without the owner key are not detected. Pass every world that may hold its systems or
        // components. Returns those holders, empty when the image was freed;
        // an error when the module is not loaded.
        Expected<std::vector<std::string>> unload(std::string_view name, std::span<const World> worlds);
        // Swaps in the module's rebuilt source DLL with its component state
        // kept: the instances in the worlds round-trip through the new
        // build's schemas as JSON (persisted fields, by field name), so added
        // fields take their defaults and removed ones drop. Refused, the old
        // build untouched, when the new one fails its checks; when its load
        // fails, a fresh copy of the old build is loaded back with the state.
        // Call between frames: thunks and systems change under the worlds.
        Expected<ModuleReload> reload(std::string_view name, std::span<const World> worlds);
        void unloadAll(std::span<const World> worlds);

        // A play world: every loaded module's attachWorld registers into it now,
        // a module loaded later on load. Detach before destroying the world:
        // it drops the services the modules registered in it.
        void attachWorld(World world);
        void detachWorld(World world);

        [[nodiscard]] std::span<const LoadedModule> modules() const { return m_modules; }

        // Counts every load, unload and reload: a freed descriptor address can be reused by the next mapping, so a
        // cache of what a module reports keys on this, not on the addresses.
        [[nodiscard]] u64 generation() const { return m_generation; }
        // Protects component thunks while an isolated scene copy runs. Refused if any host has a runtime module;
        // module mutations refuse while leased, and unloadAll waits for every process-wide copy to finish.
        [[nodiscard]] std::shared_ptr<void> acquireSceneCopyLease();

        // The module DLLs in the search directory (direct children only, at most MAX_SCANNED files, by file name):
        // scan of searchDirectory() with the loaded ones marked, plus the loaded modules from other directories. A
        // sidecar <stem>.module.json (an object of at most MAX_MANIFEST_BYTES and MAX_MANIFEST_DEPTH nesting;
        // displayName, description, parameters, fixture) is returned as manifest. Not covered: a DLL that exports the
        // entry but is not a module (load refuses it); an export whose name lies outside the export directory's own
        // span (not found: the linker puts names inside it).
        [[nodiscard]] std::vector<AvailableModule> available() const;
        // The directory available() scans: the descriptor's searchDirectory, else the executable's.
        [[nodiscard]] std::filesystem::path searchDirectory() const;
        // The module DLLs of any directory, with no host state, so a thread other than the frame thread may run it
        // (a directory on a slow share): a file counts when its export table names SPITE_MODULE_ENTRY, read from the
        // file without mapping it as code, so no DllMain runs; DLLs the process already maps (engine-owned) are left
        // out. Only plain files are read: links, junctions, directories and cloud placeholders are skipped. At most
        // MAX_ENTRIES directory entries are visited and MAX_SCANNED DLLs examined; the scan ends truncated past
        // budget or when cancel is set.
        [[nodiscard]] static ModuleScan scan(const std::filesystem::path& directory,
                                             std::chrono::milliseconds budget = std::chrono::milliseconds(2000),
                                             const std::atomic<bool>* cancel = nullptr);
        // Sets name, loaded and apiStamp of the entries whose file a loaded module came from.
        void markLoaded(std::vector<AvailableModule>& modules) const;
        static constexpr sizet MAX_ENTRIES = 8192;
        static constexpr sizet MAX_SCANNED = 256;
        static constexpr sizet MAX_MANIFEST_BYTES = sizet{64} << 10;
        static constexpr u32 MAX_MANIFEST_DEPTH = 32; // [ and { nesting

    private:
        friend class Engine;
        struct SavedComponent;
        struct CopyLeaseState;
        [[nodiscard]] static std::shared_ptr<CopyLeaseState> copyLeaseState();
        [[nodiscard]] std::shared_ptr<void> beginModuleChange();

        // copies file to a fresh shadow named after source, maps it and checks
        // its descriptor; the module's load is not called
        [[nodiscard]] Expected<LoadedModule> stage(const std::filesystem::path& file,
                                                   const std::filesystem::path& source);
        // frees a staged module whose load never ran
        static void discard(const LoadedModule& module);
        // the owner's component instances in the worlds, as schema JSON
        [[nodiscard]] static std::vector<SavedComponent> save(InternedString owner, std::span<const World> worlds);
        // load, restore saved state, attach the play worlds; false (everything
        // it registered purged from worlds) when its load failed
        // the saved instances back onto their entities, in batches
        static void restore(std::span<const SavedComponent> saved, std::span<const World> worlds, ModuleReload& report);
        bool activate(const LoadedModule& module, std::span<const SavedComponent> saved, std::span<const World> worlds,
                      ModuleReload& report);
        // the module's unload, then the purge of its owner key
        void deactivate(const LoadedModule& module, std::span<const World> worlds) const;
        // frees the image unless something still holds its code; returns what does
        std::vector<std::string> release(const LoadedModule& module, std::span<const World> worlds);
        void purge(const LoadedModule& module, const ModuleContext& context, std::span<const World> worlds) const;
        // the caller's worlds plus the scene and attached play worlds
        [[nodiscard]] std::vector<World> withOwnWorlds(std::span<const World> worlds) const;
        [[nodiscard]] ModuleContext context(std::string_view name) const;

        ModuleHostDesc m_desc;
        std::vector<LoadedModule> m_modules;
        u64 m_generation = 0;
        std::vector<World> m_playWorlds;
        std::vector<void*> m_pinned; // images still referenced at unload; never freed
    };
} // namespace spite
