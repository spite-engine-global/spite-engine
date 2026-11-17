#include "EditorFileMenu.hpp"
#include "EditorContext.hpp"
#include "EditorSceneFlows.hpp"
#include "editor/EditorModel.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <string>
#include <system_error>

#include "engine/Engine.hpp"
#include "engine/PlayWorld.hpp"
#include "localization/Localization.hpp"

namespace spite
{
    namespace
    {
        using Action = EditorActions::Action;
        using Op = EditorSceneFlows::Op;

        constexpr std::string_view TABLE = "editor-file.";
        constexpr std::string_view RECENT_PREFIX = "file.recent.";

        struct Entry
        {
            std::string_view id;
            std::string_view label; // the key under editor-file.menu.
            Op op;
            Action hotkey; // Action::Count: none
        };

        constexpr std::array<Entry, 11> ENTRIES{
            Entry{"file.new", "new", Op::New, Action::FileNew},
            Entry{"file.open", "open", Op::Open, Action::FileOpen},
            Entry{"file.save", "save", Op::Save, Action::FileSave},
            Entry{"file.saveAs", "saveAs", Op::SaveAs, Action::FileSaveAs},
            Entry{"file.saveAll", "saveAll", Op::SaveAll, Action::Count},
            Entry{"file.openWorkspace", "openWorkspace", Op::OpenWorkspace, Action::Count},
            Entry{"file.saveWorkspace", "saveWorkspace", Op::SaveWorkspace, Action::Count},
            Entry{"file.replaceWorkspace", "replaceWorkspace", Op::Replace, Action::Count},
            Entry{"file.resetWorkspace", "resetWorkspace", Op::Reset, Action::Count},
            Entry{"file.import", "import", Op::Import, Action::Count},
            Entry{"file.quit", "quit", Op::Quit, Action::FileQuit},
        };

        // code points, not bytes: what a monospace column counts
        sizet width(const std::string_view text)
        {
            return static_cast<sizet>(std::ranges::count_if(
                text, [](const char c) { return (static_cast<unsigned char>(c) & 0xC0u) != 0x80u; }));
        }
    } // namespace

    struct EditorFileMenu::State
    {
        State(Engine& engine, EditorModel& model, const EditorActions& actions) :
            engine(&engine), model(&model), flows(&EditorModelAccess::flows(model)), actions(&actions),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        EditorModel* model; // the recent scenes
        EditorSceneFlows* flows; // the model's: the menu requests, they run
        const EditorActions* actions;
        const Localization* localization;

        // the list as the open menu shows it: an entry opens the path it showed, not what its index holds by then
        mutable std::vector<std::string> shownRecent;

        [[nodiscard]] std::string tr(const std::string_view key) const
        {
            const std::string full = std::string(TABLE) + std::string(key);
            if (!localization)
            {
                return full;
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(full), scratch));
        }

        // -- menu ------------------------------------------------------------------------------------------------

        [[nodiscard]] std::string firstChord(const Action action) const
        {
            if (action == Action::Count)
            {
                return {};
            }
            const std::vector<std::string> chords = actions->shortcuts(action);
            return chords.empty() ? std::string{} : chords.front();
        }
    };

    EditorFileMenu::EditorFileMenu(Engine& engine, EditorModel& model, const EditorActions& actions) :
        m_state(std::make_unique<State>(engine, model, actions))
    {
    }

    EditorFileMenu::~EditorFileMenu() = default;

    std::vector<UiMenuItem> EditorFileMenu::items() const
    {
        const State& state = *m_state;
        const bool editing = state.engine->playWorld() == nullptr;
        const auto changesWorkspace = [](const Op op)
        { return op == Op::New || op == Op::Open || op == Op::OpenWorkspace || op == Op::Replace || op == Op::Reset; };

        // a label and its chord in two monospace columns
        std::array<std::string, ENTRIES.size()> labels;
        std::array<std::string, ENTRIES.size()> chords;
        sizet column = 0;
        for (sizet i = 0; i < ENTRIES.size(); ++i)
        {
            labels[i] = state.tr("menu." + std::string(ENTRIES[i].label));
            chords[i] = state.firstChord(ENTRIES[i].hotkey);
            if (!chords[i].empty())
            {
                column = std::max(column, width(labels[i]));
            }
        }
        std::vector<UiMenuItem> items;
        const auto add = [&](const std::string_view id)
        {
            const sizet index = static_cast<sizet>(std::ranges::find(ENTRIES, id, &Entry::id) - ENTRIES.begin());
            const Entry& entry = ENTRIES[index];
            std::string label = std::move(labels[index]);
            if (!chords[index].empty())
            {
                label.append(column - width(label) + 3, ' ');
                label += chords[index];
            }
            items.push_back(UiMenuItem{.id = InternedString(entry.id),
                                       .label = std::move(label),
                                       .enabled = editing || !changesWorkspace(entry.op)});
        };
        add("file.new");
        add("file.open");
        const std::vector<std::string>& scenes = state.model->recentScenes();
        UiMenuItem recent{.id = InternedString("file.recent"),
                          .label = state.tr("menu.recent"),
                          .enabled = editing && !scenes.empty()};
        for (sizet i = 0; i < scenes.size(); ++i)
        {
            recent.children.push_back(
                UiMenuItem{.id = InternedString(std::string(RECENT_PREFIX) + std::to_string(i)), .label = scenes[i]});
        }
        state.shownRecent = scenes;
        items.push_back(std::move(recent));
        items.push_back(UiMenuItem{.separator = true});
        add("file.save");
        add("file.saveAs");
        add("file.saveAll");
        items.push_back(UiMenuItem{.separator = true});
        add("file.openWorkspace");
        add("file.saveWorkspace");
        add("file.replaceWorkspace");
        add("file.resetWorkspace");
        items.push_back(UiMenuItem{.separator = true});
        add("file.import");
        items.push_back(UiMenuItem{.separator = true});
        add("file.quit");
        return items;
    }

    bool EditorFileMenu::activate(const InternedString item)
    {
        State& state = *m_state;
        const std::string_view id = item.toView();
        if (id.starts_with(RECENT_PREFIX))
        {
            const std::string_view number = id.substr(RECENT_PREFIX.size());
            sizet index = 0;
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), index);
            if (parsed.ec == std::errc{} && index < state.shownRecent.size())
            {
                state.flows->request(Op::OpenRecent, state.shownRecent[index]);
            }
            return true;
        }
        const auto entry = std::ranges::find(ENTRIES, id, &Entry::id);
        if (entry == ENTRIES.end())
        {
            return false;
        }
        state.flows->request(entry->op);
        return true;
    }

    void EditorFileMenu::openRecent(const std::string_view path)
    {
        m_state->flows->request(Op::OpenRecent, std::string(path));
    }

    void EditorFileMenu::request(const EditorActions::Action action)
    {
        const auto entry = std::ranges::find(ENTRIES, action, &Entry::hotkey);
        if (entry != ENTRIES.end() && action != Action::Count)
        {
            m_state->flows->request(entry->op);
        }
    }

    void EditorFileMenu::update() { m_state->flows->update(); }
} // namespace spite
