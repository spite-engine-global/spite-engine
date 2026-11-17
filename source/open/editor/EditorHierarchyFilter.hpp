#pragma once

#include <array>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "ecs/EcsTypes.hpp"
#include "editor/EditorExport.hpp"

namespace spite
{
    class ControlServer;
    class Engine;
    class IFormatWriter;
    class UiHierarchyManager;
    class World;

    // The search reads one find page of this many matches (find's limit); more are counted and reported truncated.
    inline constexpr unsigned HIERARCHY_FILTER_CAP = 10000;
    // The entities a pick by component selects; more are counted ("N of M selected").
    inline constexpr unsigned HIERARCHY_PICK_CAP = 2000;
    // Frames between two finds that a stream of commands (a gizmo drag) asks for; a structure change, a query and a new
    // text find at once.
    inline constexpr unsigned HIERARCHY_FILTER_REFRESH_FRAMES = 15;

    // The UI section's row is not an entity: the node a UI layer's elements hang under. No world hands out
    // this index, so it names no live entity.
    inline constexpr Entity HIERARCHY_UI_SECTION{0xFFFFFFFEu, 0xFFFFFFFFu};

    // Which rows the filter lists: the scene's entities, the shown world's UI elements, or both.
    enum class HierarchyScope : u8
    {
        Both,
        Scene,
        Ui
    };
    inline constexpr std::array<std::string_view, 3> HIERARCHY_SCOPE_NAMES{"both", "scene", "ui"};

    // What a view of the filter (the hierarchy panel) hooks in; every function runs inside the control command that
    // caused it, before its reply. One view per model (setting over a set one asserts).
    struct HierarchyFilterView
    {
        // the text or the scope was set (hierarchyFilter, or a locate that cleared it): settle the rows at once
        std::function<void()> changed;
        // hierarchyLocate found its target and the filter lets it show: open its ancestors and scroll to it
        std::function<void(Entity node)> reveal;
        // hierarchyPick selected `count` of `total` entities
        std::function<void(u64 count, u64 total)> picked;
    };

    // The hierarchy's search and scope (the editor model owns it, the hierarchy panel
    // is a view). The text and the scope are set only by the model's commands hierarchyFilter, hierarchyLocate and
    // hierarchyPick (owner editorModel, registered here); `revision` moves whenever the rows to list moved, so a view
    // compares it instead of copying the sets.
    //
    // The matches are a cache of find (text=) and the UI elements' ids, read again when asked: a view calls refresh()
    // or follow() and the hierarchy query refreshes before it answers. Those are const and write `mutable` members
    // only, so a view holding a const EditorModel& can ask; the text and the scope stay the commands'.
    class SPITE_EDITOR_API HierarchyFilter
    {
    public:
        // selectionWorld and selection are the model's (the selection locate targets); both outlive the filter.
        HierarchyFilter(Engine& engine, const std::string& selectionWorld, const std::vector<Entity>& selection);
        HierarchyFilter(const HierarchyFilter&) = delete;
        HierarchyFilter& operator=(const HierarchyFilter&) = delete;

        void registerCommands(std::string_view owner);
        // at most one view at a time; {} clears it
        void setView(HierarchyFilterView view);

        [[nodiscard]] const std::string& text() const { return m_text; }
        [[nodiscard]] HierarchyScope scope() const { return m_scope; }
        [[nodiscard]] bool searching() const { return !m_text.empty(); }
        [[nodiscard]] bool filtering() const { return searching() || m_scope != HierarchyScope::Both; }
        // `shown` is a text filter's: the rows listed are the matches and their ancestors
        [[nodiscard]] bool active() const { return m_active; }
        // the hierarchy's entities (and UI elements) the text found
        [[nodiscard]] const std::unordered_set<Entity>& matches() const { return m_matches; }
        // find's total, entities outside the hierarchy and past the cap included
        [[nodiscard]] u64 found() const { return m_found; }
        [[nodiscard]] u64 revision() const { return m_revision; }
        // the find queries run so far
        [[nodiscard]] u64 finds() const { return m_finds; }

        // whether the rows list the node: any, or under a text filter its matches and their ancestors
        [[nodiscard]] bool lists(const Entity node) const { return !m_active || m_shown.contains(node); }
        [[nodiscard]] bool scopeAllows(Entity node) const;
        // whether the model lists the UI section: the scope takes UI rows, there are UI elements, and under a text
        // filter one of them matches
        [[nodiscard]] bool sectionListed() const;
        // an entity that has a row in the scene's hierarchy or the UI's, whatever the scope
        [[nodiscard]] bool hasRow(Entity node) const;
        // the parent in the hierarchy the node is in: a UI root hangs under the UI section
        [[nodiscard]] Entity parentNow(Entity node) const;
        // the node Locate shows: the last of the selection that is in the shown world's hierarchy
        [[nodiscard]] Entity locateTarget() const;

        // Reads the matches again when the text is new (`force`), the structure or the world moved, or, with
        // `commands`, a command ran since. A change of the UI's structure alone waits for
        // HIERARCHY_FILTER_REFRESH_FRAMES unless `commands`.
        void refresh(bool force, bool commands = false) const;
        // The per-frame follow of a view that shows: refresh, and after a command at most once per interval.
        void follow() const;
        // {text, scope, matches, total, found, truncated}; total is the hierarchy's size the view counts
        void write(IFormatWriter& out, u64 total) const;

        // used by the model's commands: set the filter, then tell the view
        void set(std::string text, HierarchyScope scope);

    private:
        struct UiSeen
        {
            const UiHierarchyManager* manager = nullptr;
            u64 generation = 0;
            bool operator==(const UiSeen&) const = default;
        };

        [[nodiscard]] std::string_view worldName() const;
        [[nodiscard]] World& world() const;
        [[nodiscard]] const UiHierarchyManager& uiHierarchy() const;
        [[nodiscard]] UiSeen uiNow() const;
        void addUiNameMatches(std::unordered_set<Entity>& matches, u64& found) const;
        std::string pick(const std::string& component);
        std::string locate();

        Engine* m_engine;
        ControlServer* m_control;
        const std::string* m_selectionWorld;
        const std::vector<Entity>* m_selection;
        HierarchyFilterView m_view;
        std::string m_text;
        HierarchyScope m_scope = HierarchyScope::Both;
        // the cache below follows the world, not a command
        mutable bool m_active = false;
        mutable std::unordered_set<Entity> m_matches;
        mutable std::unordered_set<Entity> m_shown; // the matches and their ancestors
        mutable u64 m_found = 0;
        mutable u64 m_revision = 0;
        mutable u64 m_finds = 0;
        mutable u64 m_seenCommand = ~0ull; // what the matches were read at
        mutable u64 m_seenStructure = ~0ull;
        mutable UiSeen m_seenUi;
        mutable const World* m_seenWorld = nullptr;
        mutable u32 m_quiet = HIERARCHY_FILTER_REFRESH_FRAMES; // frames since the last find, up to the interval
    };
} // namespace spite
