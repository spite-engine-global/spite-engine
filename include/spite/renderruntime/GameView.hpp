#pragma once

#include <functional>
#include <optional>

#include "base/MathTypes.hpp"
#include "base/ServiceLocator.hpp"
#include "ecs/EcsTypes.hpp"
#include "renderruntime/RenderRuntimeExport.hpp"

namespace spite
{
    class InputSystem;

    // The mouse pointer in the view that draws a world's game camera (the view Play binds), for gameplay that casts
    // rays through the cursor.
    struct GameViewPointer
    {
        // the camera entity the view draws
        Entity camera = Entity::undefined();
        // the view's width over height (1 when it has no size yet)
        float aspect = 1.0f;
        // the pointer in the view as ndc (x right, y up, both -1..1); valid only when inside
        Vec2 ndc{};
        // false when the pointer is outside the view, or the window or view has no size to place it in
        bool inside = false;
    };

    // The pointer in the game view of the world whose services these are; empty when no active view draws that
    // world (no render runtime, headless, Play not bound). The editor's Scene view drawing the same world does not
    // count.
    SPITE_RENDER_RUNTIME_API std::optional<GameViewPointer> gameViewPointer(ServiceLocator& services,
                                                                            const InputSystem& input);

    // What the game view drew at a picked point: the entity whose surface is nearest the camera there (a see-through
    // surface a click selects in front of an opaque one, as the editor's pick), as drawn that frame: skinned and
    // moving entities pick where they were drawn.
    struct GamePickResult
    {
        Entity entity = Entity::undefined(); // undefined for the sky, or when the entity left its slot meanwhile
        Vec3 worldPosition{}; // of the picked surface; meaningless without hit
        bool hit = false;
    };

    // How a game asks which entity is under a point of its game view (the windowed engine registers it in the root
    // services; a headless one has none). The GPU reads the point back after the next frame renders, so an answer
    // comes a few frames later; no request costs nothing. Both calls are thread-safe.
    struct GamePicker
    {
        // Queues a pick at ndc (GameViewPointer::ndc) in the game view drawing the world whose services these are:
        // a ticket, 0 when no active game view draws that world.
        std::function<u64(const ServiceLocator& worldServices, Vec2 ndc)> request;
        // The ticket's answer once read back (and forgotten): empty while pending. A pick that could not be read
        // (a view that changed world, a minimized window, a pipeline reload) answers without hit.
        std::function<std::optional<GamePickResult>(u64 ticket)> take;
    };
} // namespace spite
