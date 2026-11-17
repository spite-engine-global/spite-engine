#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class Widget;

    // The Components panel: every registered component as a table on the panel list pattern (name,
    // category, size, field count and, for the selected one, its entity count), with the selected component's fields
    // (type and flags) below it. The filter matches a component's name or its category (game, engine,
    // runtimeDerived, ui). Show Entities finds the entities with the selected component (`find with=`) and selects
    // them, which the Hierarchy reveals.
    //
    // The list comes from the `components` query without counts, again only when the panel becomes shown, on Refresh,
    // on a locale change and when the registry's component count moved (a module's load); the selected component's
    // entity count is one `components name=` read, when it is selected and on Refresh. A hidden or idle panel costs
    // counter compares. Control (owner editorComponents): query `componentsView` (rows, selection, fields, usage,
    // note, counters, filter and sort), view commands `componentsSelect name=`, `componentsShow [name=]`,
    // `componentsRefresh`, and the list's `componentsFilter` and `componentsTable`.
    class EditorComponents
    {
    public:
        explicit EditorComponents(Engine& engine);
        ~EditorComponents();
        EditorComponents(const EditorComponents&) = delete;
        EditorComponents& operator=(const EditorComponents&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
