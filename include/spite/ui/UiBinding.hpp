#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "base/Expected.hpp"
#include "base/InternedString.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"
#include "ui/UiExport.hpp"

namespace spite
{
    class World;
    class UiContext;
    class UiHierarchyManager;
    class UiRouter;
    class UiScope;

    // What a binding writes with the source field's value.
    enum class UiBindTarget : u8
    {
        // A label or button's text: the message `format` of the Localization service with the value as its argument
        // (a plural or number format follows the locale), or, without a format, the value as text.
        Text,
        // The target's state: a progress bar's, slider's or number field's value, a toggle's checked (non-zero), a
        // radio group's, tab bar's, dropdown's or combo's selected index. Written as a setter does: normalized,
        // applied at once, no onChange.
        Value,
        // The target's visibility (a non-zero number, true or a non-empty string shows it).
        Visible,
    };

    struct UiBindingDesc
    {
        Entity source = Entity::undefined(); // an entity of the UI's world
        std::string_view component; // its registered component name
        std::string_view field; // a scalar field of the component's schema: bool, integer, float, string
        UiBindTarget target = UiBindTarget::Text;
        InternedString format{}; // Text: a localization message key; invalid: the value as text
        std::string_view argument = "value"; // Text: the name the message receives the value as (no '=' or line break)
    };

    // A row of the "uiBindings" table of the UiContext service: one live binding.
    struct UiBindingRecord
    {
        u32 id = 0;
        Entity source = Entity::undefined();
        std::string component;
        std::string field;
        Entity target = Entity::undefined();
        std::string targetId; // the target element's id
        std::string kind; // text, value, visible
        std::string format; // the message key, or empty
        std::string value; // the last value written to the target, as text
        u64 updates = 0; // times the field changed after the binding was made
        SPITE_OBSERVED_SCHEMA(UiBindingRecord, id, source, component, field, target, targetId, kind, format, value,
                              updates)
    };

    // What the last frame's visit did: the proof a quiescent UI reads nothing.
    struct UiBindingStats
    {
        u32 chunksVisited = 0; // chunks of a bound component the change summaries named
        u32 rowsChanged = 0; // rows written this frame in them
        u32 lookups = 0; // written rows looked up, or bound sources probed when those are fewer than the written rows
        u32 applied = 0; // bindings whose field value differed and whose target was written
        double visitMs = 0.0; // the chunk walk and compares
        double applyMs = 0.0; // the target writes
        bool blocked = false; // visits suspended: a system in the visit's stage writes a bound component
    };

    // The data bindings of one UI: a UI field follows an entity component field of the same world
    // without per-frame polling code. Bound components are visited through the chunk change summaries only (the
    // "write bits" the layout system's value-widget scan uses): a frame in which no bound component was written
    // visits no chunk, a written row costs one lookup of its bindings and a compare with the last value, and only a
    // differing value writes the target. A binding is cleared when its source entity (or the component) is
    // destroyed, its target element is destroyed, or its scope goes. The visit runs in the layout system's update
    // (BEGIN_FRAME, before the layout resolve and the widget scan), so a source written in UPDATE shows in that
    // frame, and a paused world still follows edits made over control.
    // The source components are read without a declared dependency (they are chosen at run time), which is race-free
    // only while no other system in the visit's stage (BEGIN_FRAME) declares a write on one. That rule is enforced:
    // add refuses a component such a system writes, and a system registered later that does suspends the visits
    // (logged, UiBindingStats::blocked) until the schedule is clean. A number field or combo being edited keeps its
    // typing: a bound write to the focused one is dropped.
    class SPITE_UI_API UiBindings
    {
    public:
        UiBindings(World& world, UiHierarchyManager& hierarchy, const UiRouter& router);
        ~UiBindings();
        UiBindings(const UiBindings&) = delete;
        UiBindings& operator=(const UiBindings&) = delete;

        // The new binding's id (non-zero, never reused) or why it was refused: an unknown or dead source, a missing
        // or non-scalar field, a target that cannot take the kind. The target takes the source's current value at
        // once. Replaces the binding of the same target and kind.
        [[nodiscard]] Expected<u32> add(UiScope& scope, Entity target, const UiBindingDesc& desc);
        bool remove(u32 id);
        // Every binding the scope owns, or whose target is the element.
        void removeOwner(const UiScope* scope);
        void removeTarget(Entity target);

        [[nodiscard]] u32 count() const;
        [[nodiscard]] UiBindingStats stats() const;

        // The frame entry (UiContext's update hook).
        void update();

    private:
        friend class UiContext;

        // Adds the "uiBindings" table to the UiContext service; UiContext calls it once registered.
        void registerTable();

        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
