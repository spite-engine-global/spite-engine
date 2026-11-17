#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "base/Expected.hpp"
#include "base/MathTypes.hpp"
#include "base/Schema.hpp"
#include "base/Platform.hpp"
#include "ecs/ComponentMetadata.hpp"
#include "ecs/EcsTypes.hpp"
#include "ui/UiExport.hpp"

namespace spite
{
    class World;
    class SystemContext;
    class UiHierarchyManager;

    // Easing curves over t in [0, 1]; every curve maps 0 to 0 and 1 to 1.
    enum class UiEase : u8
    {
        Linear,
        InQuad,
        OutQuad,
        InOutQuad,
        InCubic,
        OutCubic,
        InOutCubic,
        OutBack, // overshoots the end, settles on it
        Count
    };

    SPITE_UI_API std::string_view uiEaseName(UiEase ease);
    SPITE_UI_API std::optional<UiEase> uiEaseFromName(std::string_view name);
    SPITE_UI_API float evaluateUiEase(UiEase ease, float t);

    using UiTweenId = u32;
    inline constexpr UiTweenId NO_UI_TWEEN = 0;

    // One tween: `field` of `component` on `element` moves from `from` (default: its value when the tween
    // starts running, after the delay) to `to`. The field is f32, Vec2, Vec3 or Vec4 and the value's leading
    // lanes are the field's. Components: UiRect, UiVisual, UiContentSize, UiLayout, UiLayoutItem,
    // UiStyleAnim (a missing UiVisual is added; a style transition owns UiStyleAnim's fields). A tween writes in place,
    // so the layout system re-lays out or repaints it. Starting a tween on a (element, component, field) that already
    // has one replaces the older: it is dropped without an event and keeps the value it reached, so a repeated start (a
    // hover handler) never accumulates; tweens waiting on the replaced one start the frame after. The tweens a new one
    // waits on (its `after` chain) are kept, so a sequence on one field still runs in order.
    struct UiTweenDesc
    {
        Entity element = Entity::undefined();
        std::string_view component; // the registered component name
        std::string_view field;
        Vec4 to{0.0f, 0.0f, 0.0f, 0.0f};
        std::optional<Vec4> from;
        u32 lanes = 0; // when not 0, how many values the caller gave: refused unless it is the field's lane count
        float duration = 0.25f; // seconds of one play; 0 snaps to the end
        float delay = 0.0f; // seconds before the first play, counted from `after` finishing
        UiEase ease = UiEase::OutCubic;
        u32 repeat = 1; // plays; 0 repeats until cancelled
        bool yoyo = false; // every second play runs backwards
        UiTweenId after = NO_UI_TWEEN; // waits until this tween has finished or been cancelled: a sequence; an id never
                                       // issued is refused
    };

    // A resolved (component, field) a bulk caller starts many tweens on without a name lookup per start; from
    // UiTweens::field, valid for the process (component and field ids do not change).
    inline constexpr u32 NO_UI_TWEEN_INDEX = ~0u;

    struct UiTweenField
    {
        ComponentID component = INVALID_COMPONENT_ID;
        std::string_view componentName;
        std::string_view fieldName;
        u32 offset = 0; // of the field (or of the array element) in the component
        u32 lanes = 0;
        u32 index = NO_UI_TWEEN_INDEX; // the element of an array field named as `field[index]`
    };

    // A row of the "uiTweens" table of the UiContext service: one active tween.
    struct UiTweenRecord
    {
        u32 id = 0;
        Entity element = Entity::undefined();
        std::string component;
        std::string field;
        Vec4 from{0.0f, 0.0f, 0.0f, 0.0f}; // zero until the delay ends, unless given
        Vec4 to{0.0f, 0.0f, 0.0f, 0.0f};
        float duration = 0.0f;
        float delay = 0.0f;
        std::string ease;
        u32 repeat = 1;
        bool yoyo = false;
        u32 after = 0;
        std::string state; // waiting (on `after`), delay, running
        float elapsed = 0.0f; // seconds into the plays, after the delay
        SPITE_OBSERVED_SCHEMA(UiTweenRecord, id, element, component, field, from, to, duration, delay, ease, repeat,
                              yoyo, after, state, elapsed)
    };

    // The active tweens of one UI: a dense table the "UiTweens" system walks, so a UI with no tween
    // costs nothing. The system runs in the UPDATE stage with the world's frame delta: a paused world freezes
    // its tweens and a fixed delta replays them deterministically. A finished tween leaves the table and fires
    // a UiTweenFinishedEvent on the world; a cancelled one fires nothing and keeps the value it reached. start,
    // cancel and the reduced-motion switch run between world updates.
    class SPITE_UI_API UiTweens
    {
    public:
        UiTweens(World& world, UiHierarchyManager& hierarchy);
        ~UiTweens();
        UiTweens(const UiTweens&) = delete;
        UiTweens& operator=(const UiTweens&) = delete;

        // The new tween's id, or why it was refused (unknown element, component or field, a field that is not
        // a float or vector, a negative time, an `after` never issued). Replaces a tween on the same field.
        [[nodiscard]] Expected<UiTweenId> start(const UiTweenDesc& desc);
        // The tweenable field named by a desc's component and field strings, or why it is not one.
        [[nodiscard]] static Expected<UiTweenField> field(std::string_view component, std::string_view field);
        // start with the field already resolved (desc.component and desc.field are ignored): O(1) amortized in the
        // number of active tweens, the replacement of an older tween on the field included.
        [[nodiscard]] Expected<UiTweenId> start(const UiTweenField& field, const UiTweenDesc& desc);
        // False when no such tween runs. A tween others wait on releases them.
        bool cancel(UiTweenId id);
        // Cancels every tween on the element; how many.
        u32 cancelElement(Entity element);

        // Reduced motion: at the next update every tween snaps to the value its last play ends on (a yoyo of
        // an even number of plays ends back at `from`; a tween that repeats forever ends at `from`) and
        // finishes, and new tweens snap the same way.
        void setReducedMotion(bool reduced);
        [[nodiscard]] bool reducedMotion() const;

        [[nodiscard]] u32 activeCount() const;

        // The "UiTweens" system entry.
        void advance(SystemContext& ctx);
        // Disables the system once no tween is left (UiContext::processInput, the frame entry, calls it).
        void retireIdleSystem();

    private:
        friend class UiContext;

        // Adds the "uiTweens" table to the UiContext service; UiContext calls it once registered.
        void registerTable();

        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
