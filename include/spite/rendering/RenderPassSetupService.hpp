#pragma once

#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

#include "base/InternedString.hpp"
#include "rendering/RenderDeviceCapabilities.hpp"
#include "rendering/RenderingExport.hpp"

namespace spite
{
    class ServiceLocator;
    class RenderGraph;

    enum class RenderPassImplementation : u8
    {
        Default = 0,
        MeshShader,
        Fallback
    };

    struct RenderPassSetupRuntimeConfig
    {
        std::unordered_map<InternedString, bool, InternedString::hash> enabledByPassName;
        std::unordered_map<InternedString, RenderPassImplementation, InternedString::hash>
            preferredImplementationByPassName;

        void setPassEnabled(InternedString passName, bool enabled) { enabledByPassName[passName] = enabled; }

        bool isPassEnabled(InternedString passName) const
        {
            auto it = enabledByPassName.find(passName);
            if (it == enabledByPassName.end())
            {
                return true;
            }
            return it->second;
        }

        void setPreferredImplementation(InternedString passName, RenderPassImplementation implementation)
        {
            preferredImplementationByPassName[passName] = implementation;
        }

        void clearPreferredImplementation(InternedString passName)
        {
            preferredImplementationByPassName.erase(passName);
        }

        std::optional<RenderPassImplementation> getPreferredImplementation(InternedString passName) const
        {
            auto it = preferredImplementationByPassName.find(passName);
            if (it == preferredImplementationByPassName.end())
            {
                return std::nullopt;
            }
            return it->second;
        }
    };

    enum class RenderPassPlacement : u8
    {
        RegistrationOrder,
        // After Composite and before later built-in passes, including the UI that samples offscreen view outputs.
        // Runs only when Composite is selected. In a per-view pass, viewTargetName(0) names the composed output.
        AfterComposite,
        // After Transparent, before exposure, temporal resolve, upscaling and bloom.
        // Omitted if Transparent is not selected. Per-view passes may load and write
        // SceneColor (R16G16B16A16_SFLOAT) and read DepthBuffer as an attachment.
        AfterTransparent
    };

    struct RenderPassModuleDefinition
    {
        InternedString passName;
        RenderPassImplementation implementation = RenderPassImplementation::Default;
        int priority = 0;
        // Part of every view's pass chain: setup runs once per view
        // inside RenderGraph's view scope. Otherwise it runs once, in view 0.
        bool perView = false;
        RenderPassPlacement placement = RenderPassPlacement::RegistrationOrder;
        // Also part of the pass chain of the view reserved for probe capture (RenderGraph::captureView), which
        // carries only the modules that draw and light a probe's face: false leaves the view out of the module.
        bool onCaptureView = false;

        std::function<bool(const RenderDeviceCapabilities&, const RenderPassSetupRuntimeConfig&)> isSupported;
        std::function<void(ServiceLocator&)> setup;
    };

    // Selects and runs the registered pass modules: per pass name the
    // highest-priority supported implementation wins, unless the runtime
    // config pins a preferred implementation or disables the pass. Modules
    // register under an owner (the module that holds their code) so a module
    // can be dropped and re-registered (hot reload). Lives in the
    // world services beside the RenderGraphGenerationSingleton.
    class SPITE_RENDERING_API RenderPassSetupService
    {
    public:
        explicit RenderPassSetupService(const RenderPassSetupRuntimeConfig& config = {});

        // A pass name keeps its first registration slot across drops, so a
        // re-registered module declares its passes in the original order; a
        // new name goes after the last slot its owner already has (a reloaded
        // build's added pass stays among that owner's passes).
        void registerModule(InternedString owner, RenderPassModuleDefinition definition);
        void registerPipelineQualification(InternedString owner, std::function<void(ServiceLocator&)> qualify);
        void qualifyPipelines(ServiceLocator& services) const;
        // Drops the owner's modules and the graph passes their setups declared;
        // the next finalize re-runs the remaining setups. false (and nothing to
        // re-run) when the owner registered none. A dropped pass's on/off switch (and a turn-off by command)
        // is put back on the pass of the same name at the next finalize.
        bool dropOwner(InternedString owner, ServiceLocator& services);
        [[nodiscard]] bool hasOwner(InternedString owner) const;
        // Declares every selected module's passes (after dropping the passes of
        // the previous finalize) and bumps the graph generation; no-op when
        // nothing was registered or dropped since the last finalize.
        void finalize(ServiceLocator& services);
        // Signal changes before frame planning, including removal of a module's contribution.
        void markViewContentChanged(u32 view);
        [[nodiscard]] u64 viewContentVersion(u32 view) const;

    private:
        friend struct RenderPassSetupTestAccess;

        struct OwnedModule
        {
            InternedString owner;
            RenderPassModuleDefinition definition;
        };

        struct PassSwitch
        {
            InternedString name;
            bool active = true;
            bool turnedOff = false;
        };

        u64 m_contentEpoch = 0;
        std::unordered_map<u32, u64> m_viewContentVersions;
        RenderPassSetupRuntimeConfig m_runtimeConfig{};
        RenderDeviceCapabilities m_capabilities{};

        std::vector<InternedString> m_registeredPassNames;
        std::vector<InternedString> m_slotOwners; // per m_registeredPassNames slot: its first registrant
        std::unordered_map<InternedString, std::vector<OwnedModule>, InternedString::hash> m_modulesByPassName;
        std::unordered_map<InternedString, std::function<void(ServiceLocator&)>, InternedString::hash> m_qualifiers;
        // owners whose passes the last finalize put into the graph
        std::vector<InternedString> m_declaredOwners;

        // the on/off switches of the passes dropped since the last finalize, put back on the passes of the same
        // names when they are declared again (a view mode's wireframe pass and a command's turn-off survive a reload)
        std::vector<PassSwitch> m_keptSwitches;

        bool m_isFinalized = false;
        bool m_capabilitiesDetected = false;

        void keepSwitches(const RenderGraph& graph, InternedString owner);
        bool isModuleSupported(const RenderPassModuleDefinition& definition) const;
        const OwnedModule* selectModule(InternedString passName) const;
    };
} // namespace spite
