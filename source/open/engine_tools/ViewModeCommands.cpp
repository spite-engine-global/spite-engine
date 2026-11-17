#include "engine/ViewModeCommands.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "base/ServiceLocator.hpp"
#include "control/ControlServer.hpp"
#include "rendering/RenderGraph.hpp"
#include "rendering/RenderingSingletons.hpp"
#include "serialization/JsonFormat.hpp"

namespace spite
{
    namespace
    {
        constexpr std::string_view LIGHT_PASS = "Light";
        constexpr std::array<std::string_view, 4> DEBUG_PASSES{"Wireframe_PrepareArgs", "Wireframe_Visibility",
                                                               "Wireframe_SolidResolve", "Wireframe_LineResolve"};
        using PassSet = std::array<bool, DEBUG_PASSES.size()>;

        struct ModeDef
        {
            std::string_view name;
            PassSet passes; // the debug passes that run
            ShadingDebug shading;
        };

        // Wireframe draws edges over the lit image; solid is the wireframe module's flat gray resolve; the rest show
        // one input of the Light pass unlit through it (its ShadingDebug is the mode's name).
        constexpr std::array<ModeDef, VIEW_MODES.size()> MODES{
            {{"lit", {}, ShadingDebug::NONE},
             {"wireframe", {true, true, false, true}, ShadingDebug::NONE},
             {"solid", {false, false, true, false}, ShadingDebug::NONE},
             {"albedo", {}, ShadingDebug::ALBEDO},
             {"normals", {}, ShadingDebug::NORMALS},
             {"depth", {}, ShadingDebug::DEPTH},
             {"roughness", {}, ShadingDebug::ROUGHNESS},
             {"metallic", {}, ShadingDebug::METALLIC},
             {"occlusion", {}, ShadingDebug::OCCLUSION},
             {"emissive", {}, ShadingDebug::EMISSIVE},
             {"entityId", {}, ShadingDebug::ENTITY_ID},
             {"meshletId", {}, ShadingDebug::MESHLET_ID},
             {"cascade", {}, ShadingDebug::CASCADE},
             {"shadowMask", {}, ShadingDebug::SHADOW_MASK},
             {"lightsPerCluster", {}, ShadingDebug::LIGHTS_PER_CLUSTER},
             {"mip", {}, ShadingDebug::MIP},
             {"lod", {}, ShadingDebug::LOD},
             {"ao", {}, ShadingDebug::AO},
             {"materialClass", {}, ShadingDebug::MATERIAL_CLASS}}};

        constexpr std::string_view shadingName(const ShadingDebug shading)
        {
            for (const ModeDef& mode : MODES)
            {
                if (shading != ShadingDebug::NONE && mode.shading == shading)
                {
                    return mode.name;
                }
            }
            return "none";
        }

        struct Current
        {
            PassSet passes{};
            ShadingDebug shading = ShadingDebug::NONE;
        };

        // interned once: the editor's toolbar reads the state every frame
        const InternedString& debugPass(const sizet index)
        {
            static const std::array<InternedString, DEBUG_PASSES.size()> ids{
                InternedString(DEBUG_PASSES[0]), InternedString(DEBUG_PASSES[1]), InternedString(DEBUG_PASSES[2]),
                InternedString(DEBUG_PASSES[3])};
            return ids[index];
        }

        const InternedString& lightPass()
        {
            static const InternedString id(LIGHT_PASS);
            return id;
        }

        bool declared(const RenderGraph& graph, const InternedString& pass) { return graph.findPass(pass) != nullptr; }

        // The mode's passes are all declared: a device without mesh shaders has no wireframe passes, a headless
        // engine no Light pass.
        bool available(const RenderGraph& graph, const ModeDef& mode, const u32 view)
        {
            for (sizet i = 0; i < DEBUG_PASSES.size(); ++i)
            {
                if (mode.passes[i] && !declared(graph, viewScopedName(debugPass(i), view)))
                {
                    return false;
                }
            }
            return mode.shading == ShadingDebug::NONE || declared(graph, viewScopedName(lightPass(), view));
        }

        std::vector<std::string_view> availableModes(const RenderGraph& graph, const u32 view)
        {
            std::vector<std::string_view> modes;
            for (const ModeDef& mode : MODES)
            {
                if (available(graph, mode, view))
                {
                    modes.push_back(mode.name);
                }
            }
            return modes;
        }

        Current read(const RenderGraph& graph, const ShadingDebugSingleton& shading, const u32 view)
        {
            Current current;
            for (sizet i = 0; i < DEBUG_PASSES.size(); ++i)
            {
                const RGPass* pass = graph.findPass(viewScopedName(debugPass(i), view));
                current.passes[i] = pass && pass->isActive();
            }
            current.shading = view < shading.modes.size() ? shading.modes[view] : ShadingDebug::NONE;
            return current;
        }

        std::string_view modeOf(const Current& current)
        {
            const auto found =
                std::ranges::find_if(MODES, [&](const ModeDef& mode)
                                     { return mode.passes == current.passes && mode.shading == current.shading; });
            return found == MODES.end() ? "custom" : found->name;
        }

        void writeState(IFormatWriter& out, const RenderGraph& graph, const Current& current, const u32 view)
        {
            out.writeString("mode", modeOf(current));
            out.writeString("shading", shadingName(current.shading));
            std::vector<sizet> present;
            for (sizet i = 0; i < DEBUG_PASSES.size(); ++i)
            {
                if (declared(graph, viewScopedName(debugPass(i), view)))
                {
                    present.push_back(i);
                }
            }
            out.beginArray("passes", static_cast<u32>(present.size()));
            for (const sizet i : present)
            {
                out.beginObject("");
                out.writeString("pass", DEBUG_PASSES[i]);
                out.writeBool("active", current.passes[i]);
                out.endObject();
            }
            out.endArray();
        }

        constexpr std::string_view reasonOf(const ModeDef& mode)
        {
            return mode.shading == ShadingDebug::NONE
                ? "the render graph lacks the wireframe debug passes (they need mesh shaders)"
                : "the render graph lacks the Light pass";
        }

    } // namespace

    ViewModeStatus viewModeStatus(ServiceLocator& worldServices, const u32 view)
    {
        ViewModeStatus status{.mode = 0, .available = 0};
        if (!worldServices.contains<RenderGraph>() || !worldServices.contains<ShadingDebugSingleton>())
        {
            return status;
        }
        const auto graph = worldServices.read<RenderGraph>();
        status.mode = static_cast<u32>(
            std::ranges::find(MODES, modeOf(read(*graph, *worldServices.read<ShadingDebugSingleton>(), view)),
                              &ModeDef::name) -
            MODES.begin());
        for (u32 i = 0; i < MODES.size(); ++i)
        {
            status.available |= available(*graph, MODES[i], view) ? 1u << i : 0u;
        }
        return status;
    }

    std::string_view viewModeUnavailableReason(const std::string_view mode)
    {
        const auto found = std::ranges::find(MODES, mode, &ModeDef::name);
        return found == MODES.end() ? std::string_view{} : reasonOf(*found);
    }

    void registerViewModeCommands(ControlServer& control, ServiceLocator& worldServices)
    {
        control.registerQuery(
            "viewMode", "engine",
            "the shading mode of a render view: mode (a setViewMode choice, or custom when "
            "the debug passes were switched one by one), available (the modes this renderer can show now), "
            "unavailable (the others, each with its reason), shading (what the Light pass shows: none or the "
            "mode's name) and passes (the wireframe debug passes with their active flags)",
            {optionalArg("view", ArgKind::Integer, "the render view", "0")},
            [&worldServices](const ControlArgs& args, IFormatWriter& out)
            {
                const u64 requestedView = args.integer("view").value_or(0);
                if (!worldServices.contains<RenderGraph>() || !worldServices.contains<ShadingDebugSingleton>())
                {
                    return std::string("no render graph");
                }
                const auto graph = worldServices.read<RenderGraph>();
                if (requestedView >= graph->viewCount())
                    return std::string("render view out of range");
                const u32 view = static_cast<u32>(requestedView);
                writeState(out, *graph, read(*graph, *worldServices.read<ShadingDebugSingleton>(), view), view);
                const std::vector<std::string_view> modes = availableModes(*graph, view);
                out.beginArray("available", static_cast<u32>(modes.size()));
                for (const std::string_view name : modes)
                {
                    out.writeString("", name);
                }
                out.endArray();
                out.beginArray("unavailable", static_cast<u32>(VIEW_MODES.size() - modes.size()));
                for (const ModeDef& mode : MODES)
                {
                    if (!available(*graph, mode, view))
                    {
                        out.beginObject("");
                        out.writeString("mode", mode.name);
                        out.writeString("reason", reasonOf(mode));
                        out.endObject();
                    }
                }
                out.endArray();
                return std::string{};
            });
        // inverse: setViewMode with the previous mode (lit when it was custom)
        control.registerUndoableViewCommand(
            "setViewMode", "engine",
            "sets the shading mode of a render view through the render graph's debug "
            "passes and the Light pass: lit, wireframe (edges over the lit image), solid (flat gray), or one input of "
            "the Light pass shown unlit: albedo (the base colour), normals (n * 0.5 + 0.5), depth (the view-space "
            "distance d "
            "along the camera's forward as d / (d + 16): 0 at the camera, toward 1 far), roughness, metallic and "
            "occlusion (grey), emissive (the emitted radiance e as e / (1 + e): 1 reads one half), "
            "entityId (one colour per object slot, pixelInfo names the entity; a see-through surface at or above "
            "graphics.pickAlphaThreshold shows its own colour over what is behind it), meshletId (one colour per "
            "meshlet: the overdraw of a visibility buffer is not recoverable; see-through surfaces as entityId), "
            "cascade (the sun cascade of the pixel: red, "
            "green, blue, yellow for 0 to 3; dark grey past the last split or without a sun) and shadowMask (the "
            "sun's visibility, white without a sun) and lightsPerCluster (how many point, spot and area lights "
            "the pixel's light cluster lists: black none, blue through green and yellow to red at 32, magenta where "
            "the "
            "cluster is full and Light loops every light instead, dark grey where the view has no lists), mip (the "
            "texture mip level the material resolve sampled, the coarsest over the material's textures: a grey whose "
            "byte is the level times 16, fractional between two levels) and lod (the cluster LOD level of the pixel's "
            "meshlet: level 0 blue, then cyan, green, yellow to red for the coarsest; needs graphics.lod, else every "
            "pixel is level 0) and ao (the ambient occlusion visibility Light multiplies into the indirect light: "
            "white open, "
            "dark where nearby geometry blocks it; white while graphics.ao is off) and materialClass (the material "
            "class of the "
            "pixel's meshlet: grey opaque, green mask, orange blend). Each byte "
            "shows the value as stored (these views skip "
            "the camera's exposure and the tone map, and the sRGB swapchain encodes once). The previous mode's passes "
            "go off; a mode the renderer cannot show is refused, naming the ones "
            "it can",
            {requiredArg("mode", ArgKind::String, "the shading mode", VIEW_MODES),
             optionalArg("view", ArgKind::Integer, "the render view", "0")},
            [&control, &worldServices](const ControlArgs& args, IFormatWriter& out)
            {
                const u64 requestedView = args.integer("view").value_or(0);
                if (!worldServices.contains<RenderGraph>() || !worldServices.contains<ShadingDebugSingleton>())
                {
                    return std::string("no render graph");
                }
                const auto graph = worldServices.write<RenderGraph>();
                if (requestedView >= graph->viewCount())
                    return std::string("render view out of range");
                const u32 view = static_cast<u32>(requestedView);
                const std::string requested = args.string("mode").value_or("");
                const ModeDef& mode = *std::ranges::find(MODES, requested, &ModeDef::name);
                if (!available(*graph, mode, view))
                {
                    std::string valid;
                    for (const std::string_view name : availableModes(*graph, view))
                    {
                        valid += (valid.empty() ? "" : ", ") + std::string(name);
                    }
                    return "view mode '" + requested + "' is unavailable: " + std::string(reasonOf(mode)) +
                        "; available: " + valid;
                }
                const auto shading = worldServices.write<ShadingDebugSingleton>();
                const std::string_view previous = modeOf(read(*graph, *shading, view));
                for (sizet i = 0; i < DEBUG_PASSES.size(); ++i)
                {
                    if (declared(*graph, viewScopedName(debugPass(i), view)))
                    {
                        // off by the mode, not by a command: the Passes panel does not count it as turned off
                        graph->setPassActiveExact(viewScopedName(debugPass(i), view), mode.passes[i], false);
                    }
                }
                shading->modes.resize(std::max<sizet>(shading->modes.size(), view + 1));
                shading->modes[view] = mode.shading;
                JsonFragmentWriter inverse;
                inverse.writeU32("view", view);
                inverse.writeString("mode", previous == "custom" ? "lit" : previous);
                std::string inverseArgs;
                inverse.dump(inverseArgs);
                control.recordInverse("setViewMode", inverseArgs);
                writeState(out, *graph, read(*graph, *shading, view), view);
                out.writeString("previous", previous);
                return std::string{};
            });
    }
} // namespace spite
