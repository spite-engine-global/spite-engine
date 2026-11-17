#pragma once

// SPITE ECS shared vocabulary types.
//
// The single canonical definition of the types that cross every boundary: entity
// handles, the component base, and the query-argument tags. Both the ECS core
// (source/ecs) and the public C++ API headers include this header.

#include <cstdint>
#include <functional>
#include <type_traits>
#include <vector>

#include "base/Handle.hpp"
#include "base/Schema.hpp"

namespace spite
{
    // -- entity ------------------------------------------------------------------

    // The ECS identity wrapper over Handle; the bit format is Handle's, so
    // entity ids cross serialization and C-API boundaries as one u64.
    struct Entity
    {
    private:
        Handle m_handle;

    public:
        explicit constexpr Entity(const uint64_t entityId = 0) : m_handle(entityId) {}

        constexpr Entity(const uint32_t index, const uint32_t generation) : m_handle(index, generation) {}

        [[nodiscard]] constexpr uint32_t index() const { return m_handle.index(); }
        [[nodiscard]] constexpr uint32_t generation() const { return m_handle.generation(); }
        [[nodiscard]] constexpr uint64_t id() const { return m_handle.id; }
        [[nodiscard]] constexpr bool isValid() const { return m_handle.isValid(); }

        friend constexpr bool operator==(const Entity& lhs, const Entity& rhs) { return lhs.m_handle == rhs.m_handle; }
        friend constexpr auto operator<=>(const Entity& lhs, const Entity& rhs)
        {
            return lhs.m_handle <=> rhs.m_handle;
        }

        // index 0 is reserved: EntityFactory never hands it out, so undefined() cannot
        // collide with a live entity
        static constexpr Entity undefined() { return Entity(0); }
    };

    template <typename T>
    inline constexpr bool is_entity_v = std::is_same_v<std::decay_t<T>, Entity>;

    template <>
    struct SchemaFieldTraits<Entity> : detail::SchemaKind<FieldKind::Entity>
    {
    };

    template <>
    struct SchemaFieldTraits<std::vector<Entity>> : detail::SchemaKind<FieldKind::EntityList>
    {
    };
} // namespace spite

template <>
struct std::hash<spite::Entity>
{
    size_t operator()(const spite::Entity& entity) const noexcept { return std::hash<uint64_t>{}(entity.id()); }
};

namespace spite
{
    // -- components ---------------------------------------------------------------

    struct IComponent
    {
        using SpiteComponentMarker = void;
    };

    // Who owns a component and whether a person may author it; tools filter on it .
    //   Game: authored by a game or module, persisted in scenes, offered by the Inspector's Add Component. The default.
    //   Engine: engine-defined authored data (Transform, Camera, lights, Mesh, Rigidbody...); offered like Game,
    //     grouped apart, removal worth confirming.
    //   RuntimeDerived: computed or maintained by engine systems from other state (bounding boxes, GPU slots, physics
    //     state, event tags); never offered, never removed by hand.
    //   Ui: belongs to UI-layer entities (SpiteUi elements and their events, editor panel state); not for scene
    //     entities.
    enum class ComponentCategory : u8
    {
        Game,
        Engine,
        RuntimeDerived,
        Ui,
    };

    // A component type declares its category once, by deriving from one of these (or defining
    // SPITE_COMPONENT_CATEGORY itself); every registration of the type then agrees, wherever it registers.
    // Not derived from IComponent: MSVC 19.51 skips the default member initialisers of a component whose bases are
    // two levels deep when it is written `{}` inside an aggregate initializer (`CameraPose{.camera = {}}` held garbage
    // planes). The marker alias stands in for the base in t_component.
    template <ComponentCategory C>
    struct ICategorizedComponent
    {
        using SpiteComponentMarker = void;
        static constexpr ComponentCategory SPITE_COMPONENT_CATEGORY = C;
    };
    using IEngineComponent = ICategorizedComponent<ComponentCategory::Engine>;
    using IDerivedComponent = ICategorizedComponent<ComponentCategory::RuntimeDerived>;
    using IUiComponent = ICategorizedComponent<ComponentCategory::Ui>;

    // A concept for any type that can be stored in a chunk.
    template <typename T>
    concept t_component = requires { typename T::SpiteComponentMarker; } && std::is_move_constructible_v<T>;

    // -- events -------------------------------------------------------------------

    // marks every spawned event entity; fireEvent attaches it, the world's
    // per-update event cleanup (SystemManager::beginUpdate) queries it
    struct EventTag : IDerivedComponent
    {
        SPITE_SCHEMA(EventTag)
    };

    // On every entity that is disabled or has a disabled ancestor; never persisted. Every query skips it
    // unless the query names it or opts in with IncludeInactive / QueryBuilder::includeInactive().
    struct InactiveComponent : IDerivedComponent
    {
        SPITE_SCHEMA(InactiveComponent)
    };

    // Derive from one of these to mark a component as an event payload. Both are roots with no base of their own
    // (the one-level rule of ICategorizedComponent): the markers stand in for the derivation, and t_event requires
    // SpiteEventMarker instead of is_base_of_v<IEventComponent, T>.
    struct IEventComponent
    {
        using SpiteComponentMarker = void;
        using SpiteEventMarker = void;
    };

    struct IUiEventComponent
    {
        using SpiteComponentMarker = void;
        using SpiteEventMarker = void;
        static constexpr ComponentCategory SPITE_COMPONENT_CATEGORY = ComponentCategory::Ui;
    };

    template <typename T>
    concept t_event = requires { typename T::SpiteEventMarker; };

    // A root derived from another root would give its payload types a second base level (the MSVC aggregate default
    // hazard of ecs.md). A new root adds its own line here.
    static_assert(!std::is_base_of_v<IComponent, IEventComponent> &&
                      !std::is_base_of_v<IComponent, IUiEventComponent> &&
                      !std::is_base_of_v<IEventComponent, IUiEventComponent> &&
                      !std::is_base_of_v<IComponent, ICategorizedComponent<ComponentCategory::Game>> &&
                      !std::is_base_of_v<IEventComponent, ICategorizedComponent<ComponentCategory::Game>>,
                  "component root bases must not derive from one another: a payload type's base chain stays one level");

    // -- access wrappers ------------------------------------------------------------

    // Wrapper to request read-only access to a component in a query.
    template <t_component T>
    struct Read
    {
        using type = T;
    };

    // Wrapper to request read-write access to a component in a query.
    template <t_component T>
    struct Write
    {
        using type = T;
    };

    template <t_component T>
    struct Exclude
    {
        using type = T;
    };

    template <t_component T>
    struct Enabled
    {
        using type = T;
    };

    template <t_component T>
    struct Modified
    {
        using type = T;
    };

    // A buildQuery<> argument, not a component: the query also visits inactive entities (InactiveComponent).
    struct IncludeInactive
    {
    };

    template <typename T>
    struct is_read_wrapper : std::false_type
    {
    };

    template <typename U>
    struct is_read_wrapper<Read<U>> : std::true_type
    {
    };

    template <typename T>
    inline constexpr bool is_read_wrapper_v = is_read_wrapper<T>::value;

    template <typename T>
    struct is_write_wrapper : std::false_type
    {
    };

    template <typename U>
    struct is_write_wrapper<Write<U>> : std::true_type
    {
    };

    template <typename T>
    inline constexpr bool is_write_wrapper_v = is_write_wrapper<T>::value;

    template <typename T>
    struct is_exclude_wrapper : std::false_type
    {
    };

    template <typename U>
    struct is_exclude_wrapper<Exclude<U>> : std::true_type
    {
    };

    template <typename T>
    inline constexpr bool is_exclude_wrapper_v = is_exclude_wrapper<T>::value;

    template <typename T>
    struct is_enabled_wrapper : std::false_type
    {
    };

    template <typename U>
    struct is_enabled_wrapper<Enabled<U>> : std::true_type
    {
    };

    template <typename T>
    inline constexpr bool is_enabled_wrapper_v = is_enabled_wrapper<T>::value;

    template <typename T>
    struct is_modified_wrapper : std::false_type
    {
    };

    template <typename U>
    struct is_modified_wrapper<Modified<U>> : std::true_type
    {
    };

    template <typename T>
    inline constexpr bool is_modified_wrapper_v = is_modified_wrapper<T>::value;

    namespace detail
    {
        template <typename T, typename = void>
        struct get_component_type_sfinae
        {
        };

        template <typename T>
        struct get_component_type_sfinae<T, std::void_t<typename T::type>>
        {
            using type = typename T::type;
        };
    } // namespace detail

    template <typename T>
    using get_component_type = typename detail::get_component_type_sfinae<T>::type;
} // namespace spite

// The engine writes the ECS vocabulary as `ecs::X`; publish the expected aliases.
namespace spite::ecs
{
    using spite::Exclude;
    using spite::IncludeInactive;
    using spite::Read;
    using spite::Write;
} // namespace spite::ecs
