#include "EditorHierarchyFilter.hpp"

#include <algorithm>
#include <optional>
#include <span>
#include <utility>

#include <nlohmann/json.hpp>

#include "base/Assert.hpp"
#include "control/ControlServer.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "engine/Engine.hpp"
#include "engine/PlayWorld.hpp"
#include <spite/profiling/Zone.hpp>
#include "serialization/FormatWalk.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "world/EntityTransformManager.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using Scope = HierarchyScope;

        std::optional<Scope> parseScope(const std::string_view text)
        {
            const auto at = std::ranges::find(HIERARCHY_SCOPE_NAMES, text);
            return at == HIERARCHY_SCOPE_NAMES.end()
                ? std::nullopt
                : std::optional(static_cast<Scope>(at - HIERARCHY_SCOPE_NAMES.begin()));
        }

        bool containsIgnoringCase(const std::string_view text, const std::string_view part)
        {
            const auto lower = [](const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
            return part.empty() || std::ranges::search(text, part, {}, lower, lower).begin() != text.end();
        }

        Json ask(ControlServer& control, const Json& line)
        {
            Json reply = Json::parse(control.handle(line.dump()), nullptr, false);
            return reply.is_object() ? reply : Json{{"ok", false}, {"error", "unparsable reply"}};
        }
    } // namespace

    HierarchyFilter::HierarchyFilter(Engine& engine, const std::string& selectionWorld,
                                     const std::vector<Entity>& selection) :
        m_engine(&engine), m_control(&engine.control()), m_selectionWorld(&selectionWorld), m_selection(&selection)
    {
    }

    void HierarchyFilter::setView(HierarchyFilterView view)
    {
        SASSERTM(!(view.changed || view.reveal || view.picked) || !(m_view.changed || m_view.reveal || m_view.picked),
                 "A hierarchy filter view is already set: clear it first.");
        m_view = std::move(view);
    }

    std::string_view HierarchyFilter::worldName() const { return m_engine->playWorld() ? "play" : "scene"; }

    World& HierarchyFilter::world() const
    {
        PlayWorld* play = m_engine->playWorld();
        return play ? play->world() : m_engine->world();
    }

    const UiHierarchyManager& HierarchyFilter::uiHierarchy() const
    {
        PlayWorld* play = m_engine->playWorld();
        return (play ? play->ui() : m_engine->sceneUi()).hierarchy();
    }

    HierarchyFilter::UiSeen HierarchyFilter::uiNow() const
    {
        const UiHierarchyManager& ui = uiHierarchy();
        return {&ui, ui.structureGeneration()};
    }

    bool HierarchyFilter::scopeAllows(const Entity node) const
    {
        return m_scope == Scope::Both ||
            (m_scope == Scope::Scene) == (node != HIERARCHY_UI_SECTION && !uiHierarchy().isElement(node));
    }

    bool HierarchyFilter::sectionListed() const
    {
        return m_scope != Scope::Scene && uiHierarchy().elementCount() > 0 &&
            (!m_active || m_shown.contains(HIERARCHY_UI_SECTION));
    }

    bool HierarchyFilter::hasRow(const Entity node) const { return world().isEntityAlive(node); }

    Entity HierarchyFilter::parentNow(const Entity node) const
    {
        if (node == HIERARCHY_UI_SECTION)
        {
            return Entity::undefined();
        }
        if (uiHierarchy().isElement(node))
        {
            const Entity parent = uiHierarchy().parentOf(node);
            return parent.isValid() ? parent : HIERARCHY_UI_SECTION;
        }
        PlayWorld* play = m_engine->playWorld();
        return (play ? play->transforms() : m_engine->transforms()).parentOf(node);
    }

    Entity HierarchyFilter::locateTarget() const
    {
        if (*m_selectionWorld == worldName())
        {
            for (auto it = m_selection->rbegin(); it != m_selection->rend(); ++it)
            {
                if (hasRow(*it))
                {
                    return *it;
                }
            }
        }
        return Entity::undefined();
    }

    // The UI elements whose id contains the text (find matches names and component names, not a UI element's id).
    // Past HIERARCHY_FILTER_CAP matches they are counted in `found`, not listed.
    void HierarchyFilter::addUiNameMatches(std::unordered_set<Entity>& matches, u64& found) const
    {
        const World& scene = world();
        for (const Entity element : uiHierarchy().compiledOrder())
        {
            const auto* identity =
                scene.isEntityAlive(element) ? scene.tryGetConstComponent<UiIdentityComponent>(element) : nullptr;
            if (identity && containsIgnoringCase(identity->id.toView(), m_text) && !matches.contains(element))
            {
                ++found;
                if (matches.size() < HIERARCHY_FILTER_CAP)
                {
                    matches.insert(element);
                }
            }
        }
    }

    void HierarchyFilter::refresh(const bool force, const bool commands) const
    {
        if (!searching())
        {
            if (m_active)
            {
                m_active = false;
                m_matches.clear();
                m_shown.clear();
                m_found = 0;
                ++m_revision;
            }
            return;
        }
        PlayWorld* play = m_engine->playWorld();
        EntityTransformManager& manager = play ? play->transforms() : m_engine->transforms();
        const u64 command = m_control->stateGeneration();
        const u64 structure = manager.structureGeneration();
        const World* target = &world();
        const UiSeen ui = m_scope == Scope::Scene ? UiSeen{} : uiNow(); // the UI is only read in this scope
        const bool uiWaits = !commands && m_quiet < HIERARCHY_FILTER_REFRESH_FRAMES;
        if (!force && m_active && (!commands || command == m_seenCommand) && structure == m_seenStructure &&
            target == m_seenWorld && (ui == m_seenUi || uiWaits))
        {
            return;
        }
        m_quiet = 0;
        ++m_finds;
        m_seenCommand = command;
        m_seenStructure = structure;
        m_seenUi = ui;
        m_seenWorld = target;
        SPITE_ZONE("EditorHierarchy.find");
        const Json query{{"query", "find"},
                         {"args", {{"world", worldName()}, {"text", m_text}, {"limit", HIERARCHY_FILTER_CAP}}}};
        const Json reply = Json::parse(m_control->handle(query.dump()), nullptr, false);
        manager.compile();
        std::unordered_set<Entity> matches;
        u64 found = 0;
        if (reply.is_object() && reply.value("ok", false))
        {
            const Json& result = reply["result"];
            found = result.value("total", u64{0});
            for (const Json& one : result.value("entities", Json::array()))
            {
                const Entity entity(one.value("id", u64{0}));
                if ((m_scope != Scope::Ui && !uiHierarchy().isElement(entity)) ||
                    (m_scope != Scope::Scene && uiHierarchy().isElement(entity)))
                {
                    matches.insert(entity);
                }
            }
        }
        if (m_scope != Scope::Scene)
        {
            addUiNameMatches(matches, found);
        }
        if (m_active && found == m_found && matches == m_matches)
        {
            return;
        }
        m_active = true;
        m_found = found;
        m_matches = std::move(matches);
        m_shown = m_matches;
        for (const Entity match : m_matches)
        {
            // an ancestor already listed has its own ancestors listed; a UI root's is the UI section
            for (Entity at = parentNow(match); at.isValid() && m_shown.insert(at).second; at = parentNow(at))
            {
            }
        }
        ++m_revision;
    }

    void HierarchyFilter::follow() const
    {
        refresh(false); // the structure or the world moved
        if (!searching())
        {
            return;
        }
        if (m_quiet < HIERARCHY_FILTER_REFRESH_FRAMES)
        {
            ++m_quiet;
        }
        if (m_control->stateGeneration() != m_seenCommand && m_quiet >= HIERARCHY_FILTER_REFRESH_FRAMES)
        {
            refresh(false, true); // a command ran: a name or a component may match now; not every frame of a drag
        }
    }

    void HierarchyFilter::write(IFormatWriter& out, const u64 total) const
    {
        out.beginObject("filter");
        out.writeString("text", m_text);
        out.writeString("scope", HIERARCHY_SCOPE_NAMES[static_cast<u8>(m_scope)]);
        out.writeU32("matches", static_cast<u32>(m_matches.size()));
        out.writeU32("total", static_cast<u32>(total));
        out.writeU64("found", m_found);
        out.writeBool("truncated", m_found > HIERARCHY_FILTER_CAP);
        out.endObject();
    }

    void HierarchyFilter::set(std::string text, const HierarchyScope scope)
    {
        if (scope != m_scope)
        {
            ++m_revision; // the rows to list differ even when the text finds the same
        }
        m_text = std::move(text);
        m_scope = scope;
        refresh(true);
        if (m_view.changed)
        {
            m_view.changed();
        }
    }

    // Shows the selection's row: a filter that hides it is cleared first.
    std::string HierarchyFilter::locate()
    {
        const Entity target = locateTarget();
        if (!target.isValid())
        {
            return "nothing in the hierarchy is selected";
        }
        refresh(false, true);
        if (!scopeAllows(target) || (m_active && !m_shown.contains(target)))
        {
            set({}, Scope::Both);
        }
        if (m_view.reveal)
        {
            m_view.reveal(target);
        }
        return {};
    }

    // find with=<component>, then select what it returned
    std::string HierarchyFilter::pick(const std::string& component)
    {
        if (getComponentIdByName(component.c_str()) == INVALID_COMPONENT_ID)
        {
            return "unknown component '" + component + "': the `components` query lists the registered ones";
        }
        const std::string world(worldName());
        const Json found = ask(
            *m_control,
            Json{{"query", "find"}, {"args", {{"world", world}, {"with", component}, {"limit", HIERARCHY_PICK_CAP}}}});
        if (!found.value("ok", false))
        {
            return found.value("error", std::string("find refused"));
        }
        const Json& result = found["result"];
        Json ids = Json::array();
        for (const Json& one : result.value("entities", Json::array()))
        {
            ids.push_back(one.value("id", u64{0}));
        }
        if (!ids.empty())
        {
            const Json selected =
                ask(*m_control, Json{{"command", "select"}, {"args", {{"world", world}, {"ids", ids}}}});
            if (!selected.value("ok", false))
            {
                return selected.value("error", std::string("select refused"));
            }
        }
        if (m_view.picked)
        {
            m_view.picked(ids.size(), result.value("total", static_cast<u64>(ids.size())));
        }
        return {};
    }

    void HierarchyFilter::registerCommands(const std::string_view owner)
    {
        m_control->registerViewCommand(
            "hierarchyFilter", owner,
            "filters the hierarchy's rows by text and scope: the entities whose name or a component's name "
            "contains the text (find text=) and the UI elements whose id does, with their ancestors; the scope "
            "lists the scene's entities, the shown world's UI elements (the read-only UI section) or both; "
            "neither argument clears the filter",
            {optionalArg("text", ArgKind::String, "the text to find",
                         "empty: no text filter (unchanged when only a scope is given)"),
             optionalArg("scope", ArgKind::String, "scene, ui or both: which rows to list",
                         "both (unchanged when only a text is given)")},
            [this](const ControlArgs& args, IFormatWriter&)
            {
                const std::optional<std::string> text = args.string("text");
                const std::optional<std::string> named = args.string("scope");
                const std::optional<Scope> chosen = named ? parseScope(*named) : std::nullopt;
                if (named && !chosen)
                {
                    return "scope must be scene, ui or both, not '" + *named + "'";
                }
                const bool cleared = !text && !named;
                set(text ? *text : cleared ? std::string{} : m_text, cleared ? Scope::Both : chosen.value_or(m_scope));
                return std::string{};
            });
        m_control->registerViewCommand(
            "hierarchyPick", owner,
            "selects every entity that has the component (find with=, at most 2000) and shows how many in the "
            "hierarchy's count",
            {requiredArg("component", ArgKind::String, "a registered component name")},
            [this](const ControlArgs& args, IFormatWriter&) { return pick(args.string("component").value_or("")); });
        m_control->registerViewCommand(
            "hierarchyLocate", owner,
            "scrolls the hierarchy to the selected entity and opens its ancestors; a filter that hides it clears", {},
            [this](const ControlArgs&, IFormatWriter&) { return locate(); });
    }
} // namespace spite
