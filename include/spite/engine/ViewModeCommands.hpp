#pragma once

#include <array>
#include <string_view>

#include "base/Platform.hpp"
#include "engine/EngineToolsExport.hpp"

namespace spite
{
    class ControlServer;
    class ServiceLocator;

    // Registers on control (owner "engine"), independently for each render view:
    //   viewMode query: view, mode (a VIEW_MODES name, or custom when the debug passes were switched one by one),
    //     available (the modes this renderer can show now), unavailable (the others, each with its reason), passes (the
    //     wireframe debug passes with their active flags) and shading (what the Light pass shows: none or the mode)
    //   setViewMode mode=<VIEW_MODES> [view=0] -> the viewMode fields and previous: an undo step; the debug passes
    //     and the Light pass's shading switch as one set, so leaving a mode leaves nothing of it on
    inline constexpr std::array<std::string_view, 19> VIEW_MODES{
        "lit",       "wireframe", "solid",        "albedo",    "normals", "depth",      "roughness",        "metallic",
        "occlusion", "emissive",  "entityId",     "meshletId", "cascade", "shadowMask", "lightsPerCluster", "mip",
        "lod",       "ao",        "materialClass"};

    // What the viewMode query reads, for a caller that polls it each frame (a toolbar): the VIEW_MODES index of the
    // current mode (VIEW_MODES.size() for custom) and one bit per VIEW_MODES entry the renderer can show now. No
    // render graph: lit, none available.
    struct ViewModeStatus
    {
        u32 mode = 0;
        u32 available = 0;
        friend bool operator==(const ViewModeStatus&, const ViewModeStatus&) = default;
    };
    SPITE_ENGINE_TOOLS_API ViewModeStatus viewModeStatus(ServiceLocator& worldServices, u32 view = 0);
    // Why the renderer cannot show a VIEW_MODES entry when it lacks it (what setViewMode refuses with); empty for a
    // name that is not one.
    SPITE_ENGINE_TOOLS_API std::string_view viewModeUnavailableReason(std::string_view mode);
    SPITE_ENGINE_TOOLS_API void registerViewModeCommands(ControlServer& control, ServiceLocator& worldServices);
} // namespace spite
