#pragma once

#include <span>
#include <vector>

#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "engine/EngineExport.hpp"

namespace spite
{
    class World;
    struct ImageRange;

    enum class ComponentGizmoCategory : u8
    {
        Light,
        Collider,
        Camera,
        Custom
    };
    constexpr u32 componentGizmoBit(ComponentGizmoCategory category)
    {
        return static_cast<u32>(category) <= static_cast<u32>(ComponentGizmoCategory::Custom)
            ? 1u << static_cast<u32>(category)
            : 0u;
    }

    struct ComponentGizmoLine
    {
        Vec3 a, b; // world-space endpoints
        u32 color = 0xFFFFFFFFu; // RGBA8, red in the low byte
        float halfWidth = 1.0f; // Scene view pixels
    };

    // Bounded per-frame output. A full writer drops later lines; callbacks must not retain it.
    class SPITE_ENGINE_API ComponentGizmoWriter
    {
    public:
        explicit ComponentGizmoWriter(std::span<ComponentGizmoLine> storage) : m_storage(storage) {}
        void line(Vec3 a, Vec3 b, u32 color = 0xFFFFFFFFu, float halfWidth = 1.0f);
        void circle(Vec3 center, Vec3 axisU, Vec3 axisV, u32 color = 0xFFFFFFFFu, float halfWidth = 1.0f);
        void box(Vec3 center, Vec3 axisX, Vec3 axisY, Vec3 axisZ, u32 color = 0xFFFFFFFFu, float halfWidth = 1.0f);
        void sphere(Vec3 center, float radius, u32 color = 0xFFFFFFFFu, float halfWidth = 1.0f);
        void arrow(Vec3 from, Vec3 to, u32 color = 0xFFFFFFFFu, float halfWidth = 1.0f);
        // radius is distance from the apex to the spherical range boundary; angle is in radians.
        void cone(Vec3 apex, Vec3 direction, float radius, float angle, u32 color = 0xFFFFFFFFu,
                  float halfWidth = 1.0f);
        void frustum(const Mat4& rigidToWorld, float fov, float aspect, float nearPlane, float farPlane,
                     u32 color = 0xFFFFFFFFu, float halfWidth = 1.0f);
        [[nodiscard]] std::span<const ComponentGizmoLine> lines() const { return m_storage.first(m_count); }
        [[nodiscard]] bool full() const { return m_count == m_storage.size(); }

    private:
        std::span<ComponentGizmoLine> m_storage;
        sizet m_count = 0;
    };

    struct ComponentGizmoContext
    {
        Entity entity;
        const World& world; // read-only by contract; component pointers live only during the callback
        Mat4 localToWorld;
        Mat4 rigidToWorld; // world position and rotation without scale, for physics shapes
        float viewAspect = 1.0f;
        float worldUnitsPerPixel = 0.0f; // at the entity origin, in the current perspective or orthographic Scene view
    };

    // Engine-owned service: modules register in load under context.owner, and the host removes their entries before
    // unloading their code. Callbacks run synchronously on the frame thread and must only emit drawing commands.
    class SPITE_ENGINE_API ComponentGizmos
    {
    public:
        using DrawFn = void (*)(const void*, const ComponentGizmoContext&, ComponentGizmoWriter&);
        void registerGizmo(InternedString owner, ComponentID component, DrawFn draw,
                           ComponentGizmoCategory category = ComponentGizmoCategory::Custom);
        void unregisterOwner(InternedString owner);
        [[nodiscard]] u32 countCallbacksIn(const ImageRange& image) const;
        void draw(const ComponentGizmoContext& context, ComponentGizmoWriter& writer, u32 categories) const;
        [[nodiscard]] std::vector<ComponentID> components(u32 categories) const;
        [[nodiscard]] u64 revision() const { return m_revision; }

        template <typename T, void (*Draw)(const T&, const ComponentGizmoContext&, ComponentGizmoWriter&)>
        void registerGizmo(InternedString owner, ComponentGizmoCategory category = ComponentGizmoCategory::Custom)
        {
            registerGizmo(
                owner, getComponentId<T>(),
                [](const void* value, const ComponentGizmoContext& context, ComponentGizmoWriter& writer)
                { Draw(*static_cast<const T*>(value), context, writer); }, category);
        }

    private:
        struct Entry
        {
            InternedString owner;
            ComponentID component;
            DrawFn draw;
            ComponentGizmoCategory category;
        };
        std::vector<Entry> m_entries;
        u64 m_revision = 0;
    };
} // namespace spite
