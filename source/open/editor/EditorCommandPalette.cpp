#include "EditorCommandPalette.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <cctype>
#include <utility>
#include <string_view>

#include <nlohmann/json.hpp>

#include "EditorActions.hpp"
#include "EditorDialogs.hpp"
#include "EditorFeedback.hpp"
#include "EditorPanelUi.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        constexpr std::string_view OWNER = "editorCommandPalette";
        constexpr sizet MAX_DOC = 110; // bytes of a command's first sentence a row shows
        constexpr sizet CHORD_GAP = 3; // spaces between an action's name and its hotkey

        enum class Kind : u8
        {
            Header,
            Action,
            Panel,
            Command,
        };
        constexpr std::array<std::string_view, 4> KIND_NAMES{"header", "action", "panel", "command"};

        struct ArgInfo
        {
            std::string name;
            std::string kind;
            std::string doc;
            std::vector<std::string> choices;
            bool required = false;
        };

        struct Entry
        {
            Kind kind = Kind::Header;
            std::string id; // what uiActivate and the query name the row by
            std::string name; // the command's name, the action's hotkey action, the panel's name
            std::string description;
            std::string hotkey;
            std::string label; // the row's text: what the search matches
            std::string searchTerms;
            std::string exactSearch;
            bool enabled = true;
            EditorActions::Action action = EditorActions::Action::Count;
            std::vector<ArgInfo> args;

            [[nodiscard]] bool needsArgs() const
            {
                return std::ranges::any_of(args, [](const ArgInfo& arg) { return arg.required; });
            }
        };

        // the first sentence of a doc, cut inside a whole code point with an ellipsis
        std::string firstSentence(const std::string_view doc)
        {
            std::string_view line = doc.substr(0, std::min(doc.find(". "), doc.size()));
            if (line.size() <= MAX_DOC)
            {
                return std::string(line);
            }
            sizet end = MAX_DOC;
            while (end > 0 && (static_cast<u8>(line[end]) & 0xC0u) == 0x80u)
            {
                --end;
            }
            return std::string(line.substr(0, end)) + "...";
        }

        // `a=1 b="two words" c=[1,2]`, or one JSON object. A quoted value is the text inside the quotes (a backslash
        // stays, so a Windows path needs no escaping; \" is a quote) and always a string. An unquoted value follows the
        // argument's declared kind: a string or component argument gets the text as it is, the others get JSON when it
        // parses (a number, true, [1]) and the text otherwise, which the server refuses by name. A name with no value
        // is left out, so the server's own refusal names the missing argument.
        bool parseArgs(const std::string_view text, const std::span<const ArgInfo> declared, Json& out,
                       std::string& problem)
        {
            out = Json::object();
            const sizet first = text.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
            {
                return true;
            }
            if (text[first] == '{')
            {
                Json parsed = Json::parse(text.substr(first), nullptr, false);
                if (!parsed.is_object())
                {
                    problem = "not a JSON object";
                    return false;
                }
                out = std::move(parsed);
                return true;
            }
            struct Token
            {
                std::string raw; // as typed
                std::string text; // quotes taken off
                bool quoted = false;
            };
            std::vector<Token> tokens;
            Token token;
            bool inQuotes = false;
            for (sizet i = 0; i < text.size(); ++i)
            {
                const char c = text[i];
                if (c == '"')
                {
                    inQuotes = !inQuotes;
                    token.quoted = true;
                    token.raw += c;
                    continue;
                }
                if (!inQuotes && (c == ' ' || c == '\t' || c == '\r' || c == '\n'))
                {
                    if (!token.raw.empty())
                    {
                        tokens.push_back(std::move(token));
                        token = {};
                    }
                    continue;
                }
                token.raw += c;
                if (inQuotes && c == '\\' && i + 1 < text.size() && text[i + 1] == '"')
                {
                    token.text += '"';
                    token.raw += '"';
                    ++i;
                    continue;
                }
                token.text += c;
            }
            if (inQuotes)
            {
                problem = "a quote is not closed";
                return false;
            }
            if (!token.raw.empty())
            {
                tokens.push_back(std::move(token));
            }
            for (const Token& each : tokens)
            {
                const sizet equals = each.raw.find('=');
                if (equals == std::string::npos || equals == 0 || each.raw.find('"') < equals)
                {
                    problem = "'" + each.raw + "' is not name=value";
                    return false;
                }
                const std::string name = each.raw.substr(0, equals);
                const std::string value = each.text.substr(equals + 1);
                if (value.empty())
                {
                    continue;
                }
                const auto kind = std::ranges::find(declared, name, &ArgInfo::name);
                const bool asText =
                    each.quoted || (kind != declared.end() && (kind->kind == "string" || kind->kind == "component"));
                Json parsed = asText ? Json() : Json::parse(value, nullptr, false);
                out[name] = asText || parsed.is_discarded() || parsed.is_null() ? Json(value) : std::move(parsed);
            }
            return true;
        }
    } // namespace

    struct EditorCommandPalette::State
    {
        State(Engine& engine, UiScope& scope, Button field, Widget fallback, EditorDialogs& dialogs,
              EditorActions& actions, EditorFeedback& feedback, PanelsFn panels) :
            engine(&engine), control(&engine.control()), scope(&scope), field(field), fallback(fallback),
            dialogs(&dialogs), actions(&actions), feedback(&feedback), panels(std::move(panels)),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        UiScope* scope;
        Button field;
        Widget fallback;
        EditorDialogs* dialogs;
        EditorActions* actions;
        EditorFeedback* feedback;
        PanelsFn panels;
        const Localization* localization;
        PopupList list;
        std::vector<Entry> entries; // the list's rows, headers included: a choice's index is an index here
        std::string fieldText;
        u64 seenLanguage = ~0ull;
        u64 seenBindings = ~0ull;

        [[nodiscard]] std::string say(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            if (!localization)
            {
                return std::string(key);
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(key), args, scratch));
        }

        [[nodiscard]] std::string chordOf(const EditorActions::Action action) const
        {
            const std::vector<std::string> chords = actions->shortcuts(action);
            return chords.empty() ? std::string{} : chords.front();
        }

        // -- rows -------------------------------------------------------------------------------------------------

        static void header(std::vector<Entry>& out, const std::string_view id, std::string label)
        {
            out.push_back({.kind = Kind::Header, .id = "palette.group." + std::string(id), .label = std::move(label)});
        }

        std::vector<Entry> build() const
        {
            std::vector<Entry> out;
            header(out, "actions", say("editor-palette.actions"));
            for (u32 i = 0; i < static_cast<u32>(EditorActions::Action::Count); ++i)
            {
                const auto action = static_cast<EditorActions::Action>(i);
                if (action == EditorActions::Action::DismissToast || action == EditorActions::Action::CommandPalette)
                {
                    continue;
                }
                const std::string_view name = EditorActions::ACTION_NAMES[i];
                Entry entry{.kind = Kind::Action,
                            .id = "palette.action." + std::string(name),
                            .name = std::string(name),
                            .hotkey = chordOf(action),
                            .enabled = actions->enabled(action),
                            .action = action};
                entry.description = say("editor.shortcut." + std::string(name));
                entry.label = entry.description;
                if (!entry.hotkey.empty())
                {
                    entry.label.append(CHORD_GAP, ' ');
                    entry.label += entry.hotkey;
                }
                out.push_back(std::move(entry));
            }
            if (const std::vector<PanelRow> shown = panels ? panels() : std::vector<PanelRow>{}; !shown.empty())
            {
                header(out, "panels", say("editor-palette.panels"));
                for (const PanelRow& panel : shown)
                {
                    const std::array<MessageArg, 1> args{{{"title", std::string_view(panel.title)}}};
                    Entry entry{.kind = Kind::Panel, .id = "palette.panel." + panel.name, .name = panel.name};
                    entry.label = say("editor-palette.show", args);
                    entry.description = entry.label;
                    entry.exactSearch = panel.title;
                    const InternedString alias("editor-palette.alias." + panel.name);
                    if (localization && localization->contains(alias))
                    {
                        entry.searchTerms = say(alias.toView());
                    }
                    out.push_back(std::move(entry));
                }
            }
            header(out, "commands", say("editor-palette.commands"));
            const Json reply = Json::parse(control->handle(R"({"query":"describe"})"), nullptr, false);
            if (!reply.is_object() || !reply.value("ok", false))
            {
                return out;
            }
            std::vector<Entry> commands;
            for (const Json& item : reply["result"].value("entries", Json::array()))
            {
                // shutdown ends the run unasked; the Quit action row asks about unsaved changes first
                if (item.value("kind", std::string()) != "command" || item.value("name", std::string()) == "shutdown")
                {
                    continue;
                }
                Entry entry{.kind = Kind::Command, .name = item.value("name", std::string())};
                entry.id = "palette.command." + entry.name;
                entry.description = firstSentence(item.value("doc", std::string()));
                for (const Json& arg : item.value("args", Json::array()))
                {
                    ArgInfo info{.name = arg.value("name", std::string()),
                                 .kind = arg.value("kind", std::string("string")),
                                 .doc = arg.value("doc", std::string()),
                                 .required = arg.value("required", false)};
                    for (const Json& choice : arg.value("choices", Json::array()))
                    {
                        info.choices.push_back(choice.get<std::string>());
                    }
                    entry.args.push_back(std::move(info));
                }
                entry.label = entry.name;
                if (!entry.description.empty())
                {
                    entry.label += " - " + entry.description;
                }
                if (std::string names; entry.needsArgs())
                {
                    for (const ArgInfo& arg : entry.args)
                    {
                        names += arg.required ? (names.empty() ? "" : ", ") + arg.name : std::string{};
                    }
                    const std::array<MessageArg, 1> args{{{"names", std::string_view(names)}}};
                    entry.label += "  (" + say("editor-palette.needs", args) + ")";
                }
                commands.push_back(std::move(entry));
            }
            std::ranges::sort(commands, {}, &Entry::name);
            out.insert(out.end(), std::make_move_iterator(commands.begin()), std::make_move_iterator(commands.end()));
            return out;
        }

        void refresh()
        {
            entries = build();
            std::vector<UiChoiceItem> items;
            items.reserve(entries.size());
            std::vector<std::string> terms, exact;
            terms.reserve(entries.size());
            exact.reserve(entries.size());
            for (const Entry& entry : entries)
            {
                items.push_back({.id = InternedString(entry.id),
                                 .label = entry.label,
                                 .enabled = entry.enabled,
                                 .header = entry.kind == Kind::Header});
                terms.push_back(entry.searchTerms);
                exact.push_back(entry.exactSearch);
            }
            list.setSearchMetadata(terms, exact);
            list.setItems(items);
        }

        // -- the field --------------------------------------------------------------------------------------------

        void showField()
        {
            const std::string chord = chordOf(EditorActions::Action::CommandPalette);
            panelui::setTip(field, chord);
            std::string text = say("editor.bar.palette.short");
            if (text != fieldText)
            {
                fieldText = std::move(text);
                field.setText(fieldText);
            }
        }

        // -- running ----------------------------------------------------------------------------------------------

        void toast(const ToastLevel level, const std::string_view key, const std::span<const MessageArg> args)
        {
            feedback->postToastKey(level, key, args, level == ToastLevel::Info ? 2.0 : 0.0);
        }

        void run(const std::string& name, const Json& args)
        {
            Json line{{"command", name}};
            if (!args.empty())
            {
                line["args"] = args;
            }
            control->runAtFrameStart(OWNER, line.dump(),
                                     [this, name](const std::string& reply)
                                     {
                                         const Json parsed = Json::parse(reply, nullptr, false);
                                         if (parsed.is_object() && parsed.value("ok", false))
                                         {
                                             if (name == "dock" || name == "resetLayout")
                                                 return;
                                             const std::array<MessageArg, 1> args{{{"name", std::string_view(name)}}};
                                             toast(ToastLevel::Info, "editor-palette.ran", args);
                                             return;
                                         }
                                         const std::string error = parsed.is_object() && parsed.contains("error") &&
                                                 parsed["error"].is_string()
                                             ? parsed["error"].get<std::string>()
                                             : reply;
                                         const std::array<MessageArg, 2> args{
                                             {{"name", std::string_view(name)}, {"error", std::string_view(error)}}};
                                         toast(ToastLevel::Warn, "editor-palette.failed", args);
                                     });
        }

        void ask(const Entry& entry)
        {
            std::string required;
            std::string optional;
            std::string start;
            for (const ArgInfo& arg : entry.args)
            {
                if (arg.required)
                {
                    std::string kind = arg.kind;
                    for (sizet i = 0; i < arg.choices.size(); ++i)
                    {
                        kind += (i == 0 ? ": " : "|") + arg.choices[i];
                    }
                    required +=
                        (required.empty() ? "" : "\n") + std::string("  ") + arg.name + " (" + kind + ") " + arg.doc;
                    start += arg.name + "= ";
                }
                else
                {
                    optional += (optional.empty() ? "" : ", ") + arg.name;
                }
            }
            if (optional.empty())
            {
                optional = say("editor-palette.prompt.none");
            }
            const std::array<MessageArg, 1> titleArgs{{{"name", std::string_view(entry.name)}}};
            const std::array<MessageArg, 3> bodyArgs{{{"doc", std::string_view(entry.description)},
                                                      {"required", std::string_view(required)},
                                                      {"optional", std::string_view(optional)}}};
            dialogs->prompt({.title = say("editor-palette.prompt.title", titleArgs),
                             .body = say("editor-palette.prompt.body", bodyArgs),
                             .value = std::move(start),
                             .confirmLabel = say("editor-palette.prompt.run")},
                            [this, name = entry.name, declared = entry.args](std::optional<std::string> text)
                            {
                                if (!text)
                                {
                                    return;
                                }
                                Json args;
                                if (std::string problem; !parseArgs(*text, declared, args, problem))
                                {
                                    const std::array<MessageArg, 2> bad{
                                        {{"name", std::string_view(name)}, {"problem", std::string_view(problem)}}};
                                    toast(ToastLevel::Warn, "editor-palette.badLine", bad);
                                    return;
                                }
                                run(name, args);
                            });
        }

        void choose(const u32 index)
        {
            if (index >= entries.size())
            {
                return;
            }
            const Entry entry = entries[index];
            switch (entry.kind)
            {
            case Kind::Action:
                actions->request(entry.action);
                break;
            case Kind::Panel:
                run("dock", Json{{"kind", "show"}, {"panel", entry.name}});
                break;
            case Kind::Command:
                if (entry.needsArgs())
                {
                    ask(entry);
                }
                else
                {
                    run(entry.name, Json::object());
                }
                break;
            case Kind::Header:
                break;
            }
        }

        void open()
        {
            if (list.isOpen())
            {
                list.close();
                return;
            }
            refresh();
            list.open(scope->hierarchy().isVisibleOf(field.entity()) ? Widget(field) : fallback);
        }

        // -- control ----------------------------------------------------------------------------------------------

        void registerControl()
        {
            control->registerQuery(
                "commandPalette", OWNER,
                "the command palette: whether it is open, the search text, how many rows it shows and the one it "
                "highlights "
                "(a row id), and every row (id, kind header|action|panel|command, name, label, description, hotkey, "
                "enabled, needsArgs, "
                "args with name, kind, required, doc and choices); the rows are built now, as opening builds them",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    const UiDropdownComponent& state =
                        scope->world().getConstComponent<UiDropdownComponent>(list.entity());
                    out.writeBool("open", state.open);
                    out.writeString("filter", state.filter);
                    out.writeString("field", fieldText);
                    const std::vector<Entry> rows = build();
                    // what the list shows now: the rows (never a group heading) whose text holds the search, and
                    // the one it highlights
                    const auto holds = [&](const std::string_view text)
                    {
                        return std::ranges::search(
                                   text, state.filter, [](const char x, const char y)
                                   { return std::tolower(static_cast<u8>(x)) == std::tolower(static_cast<u8>(y)); })
                                   .begin() != text.end() ||
                            state.filter.empty();
                    };
                    out.writeU32(
                        "shown",
                        state.open
                            ? static_cast<u32>(std::ranges::count_if(
                                  rows, [&](const Entry& row)
                                  { return row.kind != Kind::Header && (holds(row.label) || holds(row.searchTerms)); }))
                            : 0u);
                    const bool lit = state.open && state.highlighted >= 0 &&
                        std::cmp_less(state.highlighted, state.count) && std::cmp_less(state.highlighted, rows.size());
                    out.writeString("highlighted",
                                    lit ? rows[static_cast<sizet>(state.highlighted)].id : std::string{});
                    out.writeU32("count", static_cast<u32>(rows.size()));
                    out.beginArray("rows", static_cast<u32>(rows.size()));
                    for (const Entry& row : rows)
                    {
                        out.beginObject("");
                        out.writeString("id", row.id);
                        out.writeString("kind", KIND_NAMES[static_cast<u8>(row.kind)]);
                        out.writeString("name", row.name);
                        out.writeString("label", row.label);
                        out.writeString("description", row.description);
                        out.writeString("hotkey", row.hotkey);
                        out.writeBool("enabled", row.enabled);
                        out.writeBool("needsArgs", row.needsArgs());
                        out.beginArray("args", static_cast<u32>(row.args.size()));
                        for (const ArgInfo& arg : row.args)
                        {
                            out.beginObject("");
                            out.writeString("name", arg.name);
                            out.writeString("kind", arg.kind);
                            out.writeBool("required", arg.required);
                            out.writeString("doc", arg.doc);
                            out.beginArray("choices", static_cast<u32>(arg.choices.size()));
                            for (const std::string& choice : arg.choices)
                            {
                                out.writeString("", choice);
                            }
                            out.endArray();
                            out.endObject();
                        }
                        out.endArray();
                        out.endObject();
                    }
                    out.endArray();
                    return std::string{};
                });
            control->registerViewCommand(
                "commandPaletteOpen", OWNER,
                "opens or closes the command palette (the Ctrl+K list); refused while a dialog is open. Once open, "
                "injectText types into its search box, and uiActivate setValue on the element editor.palette with a "
                "row "
                "id (the commandPalette query lists them) runs that row",
                {optionalArg("open", ArgKind::Boolean, "true opens it, false closes it", "true")},
                [this](const ControlArgs& args, IFormatWriter& out)
                {
                    const bool want = args.boolean("open").value_or(true);
                    if (want != list.isOpen())
                    {
                        open();
                    }
                    if (want && !list.isOpen())
                    {
                        return std::string("the palette did not open: a dialog is open, or the toolbar is hidden");
                    }
                    out.writeBool("open", list.isOpen());
                    return std::string{};
                });
        }
    };

    EditorCommandPalette::EditorCommandPalette(Engine& engine, UiScope& scope, Button field, Widget fallback,
                                               EditorDialogs& dialogs, EditorActions& actions, EditorFeedback& feedback,
                                               PanelsFn panels) :
        m_state(std::make_unique<State>(engine, scope, field, fallback, dialogs, actions, feedback, std::move(panels)))
    {
        State& state = *m_state;
        state.list = scope.createPopupList(
            "editor.palette", UiDropdownDesc{.rect = {.size = {520.0f, 0.0f}}, .maxRows = 12, .searchable = true});
        state.list.setAccessibleNameKey(InternedString("editor-palette.name"));
        state.list.onChoose([&state](const u32 index) { state.choose(index); });
        field.onPress(
            [&state](const u8 mouse, Vec2)
            {
                if (mouse == 0)
                {
                    state.open();
                }
            });
        state.showField();
        state.registerControl();
    }

    EditorCommandPalette::~EditorCommandPalette() { m_state->control->unregisterOwner(OWNER); }

    void EditorCommandPalette::open() { m_state->open(); }

    void EditorCommandPalette::close() { m_state->list.close(); }

    bool EditorCommandPalette::isOpen() const { return m_state->list.isOpen(); }

    void EditorCommandPalette::update()
    {
        State& state = *m_state;
        const u64 language = state.localization ? state.localization->revision() : 0;
        const u64 bindings = state.actions->bindingRevision();
        if (language == state.seenLanguage && bindings == state.seenBindings)
        {
            return;
        }
        state.seenLanguage = language;
        state.seenBindings = bindings;
        state.showField();
        if (state.list.isOpen())
        {
            state.refresh();
        }
    }
} // namespace spite
