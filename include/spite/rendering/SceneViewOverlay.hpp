#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "base/MathTypes.hpp"
#include "base/Platform.hpp"

namespace spite
{
    // The editor scene view's GPU work, a service on the scene world's services that the
    // SceneViewOverlay passes (renderpipeline, view 0 only) read and write while recording; the engine
    // reads and writes it between frames. Engine::beginFrame keeps each pass active only while it has work.
    struct SceneViewOverlay
    {
        static constexpr const char* PICK_PASS = "ScenePick";
        static constexpr const char* GRID_PASS = "SceneGrid";
        static constexpr const char* OUTLINE_ARGS_PASS = "SelectionOutlineArgs";
        static constexpr const char* OUTLINE_MASK_PASS = "SelectionOutlineMask";
        static constexpr const char* OUTLINE_PASS = "SelectionOutline";
        static constexpr const char* GIZMO_PASS = "TransformGizmo";
        static constexpr u32 NO_OBJECT = ~0u; // a pick over the background

        // The infinite grid of one axis plane the grid pass draws over the view's background and under the outline
        // and that background; the editor writes it from its settings, the gizmo's snap step
        // and the theme. Colours are RGBA8 as the UI styles hold them (sRGB, red in the low byte).
        struct Grid
        {
            enum class Plane : u32
            {
                Auto, // perspective: the ground (XZ); orthographic: the axis plane facing the view
                XZ,
                XY,
                YZ,
            };

            bool enabled = false; // the editor's settings turn it on; a service nobody wrote shows nothing
            f32 opacity = 0.6f; // 0 hides the lines
            f32 spacing = 1.0f; // metres between the lines of the finest level; it is the base of the levels
            f32 majorEvery = 10.0f; // the ratio between a level and the next (2 to 16)
            f32 fadeDistance = 1000.0f; // metres from the camera at which a perspective grid is gone; 0: no cut-off
            Plane plane = Plane::Auto;
            bool axes = true; // the plane's two axes are highlighted lines
            // alpha 0: the scene's own background stays
            u32 clearColor = 0;
            u32 minorColor = 0x73B0B0B0u;
            u32 majorColor = 0xB8D0D0D0u;
            u32 axisXColor = 0xFF4C53E5u;
            u32 axisYColor = 0xFF5AAB57u;
            u32 axisZColor = 0xFFF59B53u;

            // The camera position reduced for the grid shader: float32 cannot hold a position far from the
            // origin to a fraction of a line's width. The shader works relative to the camera and adds the position
            // back, per level: offset = position modulo the cell spacing * majorEvery^level (exact, in double) for the
            // levels up to `level`, the raw position above it. A cell of a level up to it divides that cell, so the
            // lines do not move; the cell is the first one not under 1/256 of the distance from the origin, so a
            // coarser level (cells of hundreds of pixels at least) is precise enough unreduced.
            struct CameraSnap
            {
                Vec3 offset;
                u32 level = 0;
            };
            [[nodiscard]] static CameraSnap snapCamera(const Vec3 position, const f32 spacing, const f32 majorEvery)
            {
                const double ratio = std::round(std::clamp(static_cast<double>(majorEvery), 2.0, 16.0));
                const double reach = std::max({std::abs(position.x), std::abs(position.y), std::abs(position.z)});
                double cell = std::max(static_cast<double>(spacing), 1e-4);
                CameraSnap snap;
                while (cell * 256.0 < reach && snap.level < 64)
                {
                    cell *= ratio;
                    ++snap.level;
                }
                snap.offset =
                    Vec3(static_cast<f32>(std::fmod(position.x, cell)), static_cast<f32>(std::fmod(position.y, cell)),
                         static_cast<f32>(std::fmod(position.z, cell)));
                return snap;
            }
        };
        Grid grid;

        // the grid pass has work: lines to draw or a background to paint
        [[nodiscard]] bool gridWork() const
        {
            return (grid.enabled && grid.opacity > 0.0f) || (grid.clearColor >> 24) != 0;
        }

        // the instance slots the outline passes draw around in view 0, sorted; a change bumps the generation
        std::vector<u32> outlined;
        u64 outlineGeneration = 0;

        // a thick line in view 0 pixels; the gizmo pass draws each as a quad over the outline
        struct GizmoSegment
        {
            Vec2 a;
            Vec2 b;
            f32 halfWidth = 1.0f; // pixels
            u32 color = 0; // RGBA8, R in the low byte
        };
        // the transform gizmo's segments; a change bumps the generation
        std::vector<GizmoSegment> gizmo;
        u64 gizmoGeneration = 0;
        // the Scene view owns these separately from the gizmo; either generation changes the combined draw
        std::vector<GizmoSegment> marquee;
        u64 marqueeGeneration = 0;
        std::vector<GizmoSegment> agentHighlight;
        u64 agentHighlightGeneration = 0;

        struct Pick
        {
            u64 id = 0;
            u32 x = 0; // view 0 pixel
            u32 y = 0;
            u32 x2 = 0;
            u32 y2 = 0;
            u32 slotCapacity = 0; // zero for a single-pixel pick
        };
        struct PickResult
        {
            u64 id = 0;
            u32 object = NO_OBJECT; // the visibility buffer's object id: an instance slot
            std::vector<u32> objects;
        };
        // queued picks; the pick pass takes one per frame slot it records
        std::vector<Pick> picks;
        // picks the pass dispatched whose results it has not harvested yet
        u32 picksInFlight = 0;
        // harvested once the slot's fence signaled; the engine drains them
        std::vector<PickResult> results;

        [[nodiscard]] bool pickWork() const { return !picks.empty() || picksInFlight > 0; }
    };
} // namespace spite
