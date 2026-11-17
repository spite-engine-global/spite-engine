#pragma once

#include <array>
#include <functional>
#include <string_view>

#include "base/MathFunctions.hpp"

namespace spite
{
    class IFormatWriter;

    enum class GizmoMode : u8
    {
        Translate,
        Rotate,
        Scale,
    };

    inline constexpr std::array<std::string_view, 3> GIZMO_MODE_NAMES{"translate", "rotate", "scale"}; // by GizmoMode

    enum class GizmoSpace : u8
    {
        World,
        Local,
    };

    inline constexpr std::array<std::string_view, 2> GIZMO_SPACE_NAMES{"world", "local"}; // by GizmoSpace

    // The transform gizmo's settings: editor state of the EditorModel, set by gizmoMode and read by the
    // gizmo, the Scene toolbar, the grid and the shell's toolbar through EditorModelAccess::gizmo.
    struct GizmoSettings
    {
        GizmoMode mode = GizmoMode::Translate;
        GizmoSpace space = GizmoSpace::World;
        bool snap = false;
        bool enabled = true; // false: no gizmo, no handle to grab (the Scene toolbar's Overlays)
        u32 componentCategories = 1u; // light overlays on by default
        bool selectedComponentGizmos = true;
        f32 translateStep = 0.5f; // world units
        f32 rotateStep = 15.0f; // degrees
        f32 scaleStep = 0.1f; // of the scale factor

        // The snap step of a mode.
        [[nodiscard]] f32 step(const GizmoMode of) const
        {
            return of == GizmoMode::Translate ? translateStep : of == GizmoMode::Rotate ? rotateStep : scaleStep;
        }
    };

    // What the model asks of the gizmo that draws and drags (EditorGizmo sets it, one per model, and clears it in its
    // destructor): whether a drag runs (gizmoMode refuses a mode or visibility change then) and the gizmo query's
    // view part (visible, pivot, axes, size, handles, drag). Without one the model answers visible=false and no drag.
    struct GizmoView
    {
        std::function<bool()> dragging;
        std::function<void(IFormatWriter&)> write;
    };
} // namespace spite
