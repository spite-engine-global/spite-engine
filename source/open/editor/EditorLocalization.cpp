#include "EditorLocalization.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorPageScroll.hpp"
#include "EditorPanelList.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "serialization/FormatWalk.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using namespace panelui;

        constexpr std::string_view LOC_VIEW = "editor.localization";
        constexpr std::string_view LOC_BAR = "loc.bar";
        constexpr std::string_view LOC_FILTER = "loc.filter";
        constexpr std::string_view LOC_ROWS = "loc.rows";
        constexpr std::string_view LOC_EDIT = "loc.edit";
        constexpr std::string_view LOC_PREVIEW = "loc.preview";
        constexpr std::string_view LOC_STATUS = "loc.status";
        constexpr std::string_view LOC_TOOLBAR = "loc.toolbar";
        constexpr std::string_view LOC_SEARCH_BAR = "loc.searchbar";
        constexpr std::string_view PSEUDO = "en-XA"; // derived from the default locale: read-only
        constexpr u32 MAX_KEYS = 5000; // locStrings' own cap: one read lists the table
        constexpr float BUTTON_PAD = 16.0f; // a button is as wide as its text and this much
        constexpr float ARGS_WIDTH = 160.0f;
        constexpr float LOCALE_WIDTH = 128.0f; // "Locale: en-XA"

        UiRectDesc fitted()
        {
            UiRectDesc desc = barDesc(Vec2{});
            desc.contentSize =
                UiContentSizeComponent{.fitWidth = true, .fitHeight = false, .padding = Vec2{BUTTON_PAD, 0.0f}};
            return desc;
        }

        struct StringCells
        {
            std::string key;
            std::string group;
            std::string value;
            std::string forms;
            std::string state;
            bool operator==(const StringCells&) const = default;
            SPITE_OBSERVED_SCHEMA(StringCells, key, group, value, forms, state)
        };

        struct Entry
        {
            std::string key;
            std::string group;
            std::string text; // the locale's own message, else the default locale's
            bool missing = false;
        };

        UiTableColumn column(const std::string_view field, const float width)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString("editor-localization.column." + std::string(field))};
        }

        // The plural categories a message source names ("{n, plural, one {..} other {..}}" -> "one, other"); "" when
        // it has none. Nested selects and plurals add their own, once each.
        std::string pluralForms(const std::string_view source)
        {
            std::string forms;
            for (sizet at = source.find("plural"); at != std::string_view::npos; at = source.find("plural", at + 1))
            {
                sizet i = at + 6;
                while (i < source.size() && source[i] == ' ')
                {
                    ++i;
                }
                if (i >= source.size() || source[i] != ',')
                {
                    continue;
                }
                std::string current;
                std::string last;
                u32 depth = 0;
                for (++i; i < source.size(); ++i)
                {
                    const char c = source[i];
                    if (c == '{')
                    {
                        if (depth == 0)
                        {
                            const std::string& form = current.empty() ? last : current;
                            if (!form.empty() && !(", " + forms + ", ").contains(", " + form + ", "))
                            {
                                forms += (forms.empty() ? "" : ", ") + form;
                            }
                            current.clear();
                        }
                        ++depth;
                    }
                    else if (c == '}')
                    {
                        if (depth == 0)
                        {
                            break;
                        }
                        --depth;
                    }
                    else if (depth == 0)
                    {
                        if (std::isspace(static_cast<unsigned char>(c)))
                        {
                            if (!current.empty())
                            {
                                last = std::move(current);
                                current.clear();
                            }
                        }
                        else
                        {
                            current += c;
                        }
                    }
                }
            }
            return forms;
        }
    } // namespace

    struct EditorLocalization::State
    {
        explicit State(Engine& engine) :
            engine(&engine), control(&engine.control()), scope(editorUi(engine).ui()),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        UiScope scope;
        DensityFollower density{scope};
        const Localization* localization;
        Panel root;
        PageScroll scroll; // the page scrolls where the panel is shorter than its bars and list
        Dropdown localeChoice;
        Button missingOnlyButton;
        Button useButton;
        Button reloadButton;
        Button saveButton;
        Label editLabel;
        Label previewLabel;
        Label note;
        TextInput edit;
        TextInput args;
        TextInput preview;
        std::unique_ptr<FilteredTable> list;
        std::unique_ptr<UiDocumentView> view;
        std::vector<std::string> locales; // the editable ones, in the order the dropdown lists them
        std::vector<std::string> shownLocales; // what the dropdown holds
        u64 shownLocalesRevision = ~0ull; // the strings revision its "Locale: <name>" items were written in
        std::string locale; // the one the list shows
        std::vector<Entry> entries; // sorted by key
        std::vector<StringCells> cells; // parallel to entries
        std::vector<u32> items; // entries the list shows (all, or the missing ones)
        std::vector<StringCells> shownCells; // what the list holds, to write nothing when a read found no change
        std::vector<Entity> keys; // parallel to entries
        std::unordered_map<std::string, u32> keyIndex; // key -> entity id, stable for the session
        std::vector<std::string> pending;
        std::string noteShown;
        std::string actionNote; // the last save or refusal, until the selection or locale moves
        std::string errorNote; // a table file that failed to load, from the locales query
        std::string previewShown;
        std::string previewLabelShown;
        std::string loadedText; // the text the edit field was last filled with
        std::string previewLocale;
        u64 total = 0; // the locale's own messages
        u64 missingCount = 0;
        bool missingTruncated = false;
        u64 seenRevision = 0;
        u64 refreshes = 0;
        Entity selected = Entity::undefined();
        bool missingOnly = false;
        bool wasShown = false;
        bool due = false;
        bool saveDue = false;
        bool previewDue = false;
        bool noteDirty = true;
        bool stateDirty = true;

        [[nodiscard]] std::string message(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            std::string scratch;
            return localization ? std::string(localization->resolve(InternedString(key), args, scratch))
                                : std::string(key);
        }

        [[nodiscard]] Json read(const char* name, const Json& arguments = Json::object()) const
        {
            const Json reply = request(*control, Json{{"query", name}, {"args", arguments}});
            return reply.value("result", Json::object());
        }

        [[nodiscard]] const Entry* current() const
        {
            const auto at = std::ranges::find(keys, selected);
            return at == keys.end() ? nullptr : &entries[static_cast<u32>(at - keys.begin())];
        }

        [[nodiscard]] StringCells cellsOf(const Entry& entry) const
        {
            return {entry.key, entry.group, entry.text,
                    [&]
                    {
                        std::string forms = pluralForms(entry.text);
                        return forms.empty() ? std::string("-") : forms;
                    }(),
                    message(entry.missing ? "editor-localization.state.missing" : "editor-localization.state.own")};
        }

        void applyItems()
        {
            items.clear();
            for (u32 i = 0; i < entries.size(); ++i)
            {
                if (!missingOnly || entries[i].missing)
                {
                    items.push_back(i);
                }
            }
            std::vector<StringCells> next;
            next.reserve(items.size());
            for (const u32 index : items)
            {
                next.push_back(cells[index]);
            }
            if (next != shownCells)
            {
                shownCells = std::move(next);
                list->setItems(static_cast<u32>(items.size()));
            }
        }

        void refresh()
        {
            SPITE_ZONE("EditorLocalization.refresh");
            ++refreshes;
            due = false;
            const Json state = read("locales");
            const std::string defaultLocale = state.value("defaultLocale", std::string{});
            const std::string active = state.value("locale", std::string{});
            locales.clear();
            for (const Json& entry : state.value("locales", Json::array()))
            {
                if (const std::string name = entry.value("locale", std::string{}); name != PSEUDO)
                {
                    locales.push_back(name);
                }
            }
            if (!std::ranges::contains(locales, locale))
            {
                locale = std::ranges::contains(locales, active) ? active : defaultLocale;
            }
            errorNote.clear();
            const Json errors = state.value("errors", Json::array());
            if (!errors.empty())
            {
                // named strings: a MessageArg holds a view, and a temporary dies at the end of this declaration
                const std::string file = errors[0].value("file", std::string{});
                const std::string reason = errors[0].value("reason", std::string{});
                const MessageArg messageArgs[]{{"count", static_cast<std::int64_t>(errors.size())},
                                               {"file", std::string_view(file)},
                                               {"line", static_cast<std::int64_t>(errors[0].value("line", 0u))},
                                               {"reason", std::string_view(reason)}};
                errorNote = message("editor-localization.note.errors", messageArgs);
            }
            previewLocale = active;
            seenRevision = localization ? localization->revision() : 0;

            entries.clear();
            const Json own = read("locStrings", {{"locale", locale}, {"limit", MAX_KEYS}});
            total = own.value("total", u64{0});
            for (const Json& row : own.value("messages", Json::array()))
            {
                entries.push_back({row.value("key", std::string{}), row.value("namespace", std::string{}),
                                   row.value("text", std::string{}), false});
            }
            missingCount = 0;
            missingTruncated = false;
            const Json gaps = locale.empty() ? Json::object() : read("locMissing", {{"locale", locale}});
            const Json gapRows = gaps.value("locales", Json::array());
            if (!gapRows.empty() && !gapRows[0].value("keys", Json::array()).empty())
            {
                missingCount = gapRows[0].value("count", u64{0});
                missingTruncated = gapRows[0].value("truncated", false);
                // a missing key shows the default locale's text, the language the strings were authored in
                std::unordered_map<std::string, std::string> authored;
                if (locale != defaultLocale)
                {
                    const Json base = read("locStrings", {{"locale", defaultLocale}, {"limit", MAX_KEYS}});
                    for (const Json& row : base.value("messages", Json::array()))
                    {
                        authored[row.value("key", std::string{})] = row.value("text", std::string{});
                    }
                }
                for (const Json& key : gapRows[0]["keys"])
                {
                    const std::string name = key.get<std::string>();
                    const auto text = authored.find(name);
                    entries.push_back({name, name.substr(0, name.find('.')),
                                       text == authored.end() ? std::string{} : text->second, true});
                }
            }
            std::ranges::sort(entries, {}, &Entry::key);
            cells.clear();
            keys.clear();
            for (const Entry& entry : entries)
            {
                cells.push_back(cellsOf(entry));
                const auto [it, added] = keyIndex.try_emplace(entry.key, static_cast<u32>(keyIndex.size() + 1));
                keys.push_back(Entity(it->second, 1u));
            }
            if (selected.isValid() && !current())
            {
                selected = Entity::undefined();
            }
            applyItems();
            syncEdit(false);
            previewDue = true;
            noteDirty = true;
            stateDirty = true;
        }

        // The value field follows the selection; a text the person has not touched follows a reload too.
        void syncEdit(const bool force)
        {
            const Entry* entry = current();
            const std::string text = entry ? entry->text : std::string{};
            if (force || (text != loadedText && edit.value() == loadedText))
            {
                loadedText = text;
                show(edit, text);
                stateDirty = true;
            }
            if (edit.isInteractable() != (entry != nullptr))
            {
                edit.setInteractable(entry != nullptr);
            }
        }

        void resolvePreview()
        {
            previewDue = false;
            const Entry* entry = current();
            std::string text;
            if (entry)
            {
                Json arguments = {{"key", entry->key}};
                bool usable = true;
                if (const std::string_view typed = args.value(); !typed.empty())
                {
                    arguments["args"] = Json::parse(typed, nullptr, false);
                    usable = arguments["args"].is_object();
                }
                const Json reply =
                    usable ? request(*control, Json{{"query", "locResolve"}, {"args", arguments}}) : Json::object();
                text = !usable                 ? message("editor-localization.preview.badArgs")
                    : reply.value("ok", false) ? reply["result"].value("text", std::string{})
                                               : reply.value("error", std::string{});
            }
            if (show(preview, text))
            {
                previewShown = text;
            }
            const MessageArg labelArgs[]{{"locale", std::string_view(previewLocale)}};
            if (std::string label = message("editor-localization.preview", labelArgs); label != previewLabelShown)
            {
                previewLabelShown = std::move(label);
                previewLabel.setText(previewLabelShown);
            }
        }

        void showState()
        {
            stateDirty = false;
            const Entry* entry = current();
            const bool editable = entry != nullptr;
            if (const bool saveable = editable && (entry->missing || edit.value() != entry->text);
                saveButton.isInteractable() != saveable)
            {
                saveButton.setInteractable(saveable);
            }
            const bool differs = !locale.empty() && localization && locale != localization->locale();
            if (useButton.isInteractable() != differs)
            {
                useButton.setInteractable(differs);
            }
            missingOnlyButton.setLocalizedText(InternedString("editor-localization.missingOnly"));
            showSelected(missingOnlyButton, missingOnly);
            if (const u64 revision = localization ? localization->revision() : 0;
                locales != shownLocales || revision != shownLocalesRevision)
            {
                shownLocales = locales;
                shownLocalesRevision = revision;
                std::vector<UiChoiceItem> choices;
                for (const std::string& each : locales)
                {
                    const MessageArg args[]{{"locale", std::string_view(each)}};
                    choices.push_back(
                        {.id = InternedString(each), .label = message("editor-localization.localeItem", args)});
                }
                localeChoice.setItems(choices);
            }
            const auto at = std::ranges::find(locales, locale);
            if (at != locales.end() && localeChoice.selected() != static_cast<i32>(at - locales.begin()))
            {
                localeChoice.setSelected(static_cast<i32>(at - locales.begin()));
            }
        }

        void showNote()
        {
            noteDirty = false;
            std::string text = actionNote;
            if (text.empty())
            {
                const MessageArg messageArgs[]{{"locale", std::string_view(locale)},
                                               {"keys", static_cast<std::int64_t>(total)},
                                               {"missing", static_cast<std::int64_t>(missingCount)},
                                               {"total", static_cast<std::int64_t>(total)},
                                               {"shown", static_cast<std::int64_t>(MAX_KEYS)}};
                text = message(total > MAX_KEYS ? "editor-localization.note.truncated" : "editor-localization.note",
                               messageArgs);
                if (missingTruncated)
                {
                    text += ' ' + message("editor-localization.note.missingCapped", messageArgs);
                }
            }
            if (!errorNote.empty())
            {
                text += ' ' + errorNote; // a table that failed to load is never hidden by an earlier refusal
            }
            if (text != noteShown)
            {
                noteShown = std::move(text);
                note.setText(noteShown);
            }
        }

        void select(const Entity key)
        {
            selected = key;
            actionNote.clear();
            syncEdit(true);
            previewDue = true;
            noteDirty = true;
            stateDirty = true;
        }

        void save()
        {
            saveDue = false;
            const Entry* entry = current();
            if (!entry)
            {
                return;
            }
            const std::string key = entry->key;
            const std::string text(edit.value());
            const Json reply = request(
                *control,
                Json{{"command", "setLocString"}, {"args", {{"locale", locale}, {"key", key}, {"text", text}}}});
            if (reply.value("ok", false))
            {
                const MessageArg messageArgs[]{{"key", std::string_view(key)}, {"locale", std::string_view(locale)}};
                actionNote = message("editor-localization.note.saved", messageArgs);
                loadedText = text; // the reload below shows this very text: not a change to follow
            }
            else
            {
                actionNote = reply.value("error", std::string("setLocString failed"));
            }
            due = true;
            noteDirty = true;
        }

        void registerCommands()
        {
            control->registerViewCommand(
                "localizationLocale", "editorLocalization", "sets the locale the localization panel lists",
                {requiredArg("locale", ArgKind::String, "a loaded locale, as the locales query lists them")},
                [this](const ControlArgs& arguments, IFormatWriter&)
                {
                    const std::string name = arguments.string("locale").value_or("");
                    if (!std::ranges::contains(locales, name))
                    {
                        std::string known;
                        for (const std::string& each : locales)
                        {
                            known += (known.empty() ? "" : ", ") + each;
                        }
                        return "no editable locale '" + name + "'; locales: " + known;
                    }
                    locale = name;
                    actionNote.clear();
                    due = true;
                    return std::string{};
                });
            control->suggestArgValues("localizationLocale", "locale", [this] { return locales; });
            control->registerViewCommand("localizationSelect", "editorLocalization",
                                         "selects a key of the listed locale and loads its text",
                                         {requiredArg("key", ArgKind::String, "a key, as the panel's rows list them")},
                                         [this](const ControlArgs& arguments, IFormatWriter&)
                                         {
                                             const std::string name = arguments.string("key").value_or("");
                                             const auto at = std::ranges::find(entries, name, &Entry::key);
                                             if (at == entries.end())
                                             {
                                                 return "no key '" + name + "' in locale '" + locale + "'";
                                             }
                                             const Entity key = keys[static_cast<u32>(at - entries.begin())];
                                             list->table().setSelection(std::span(&key, 1));
                                             list->table().scrollTo(key);
                                             select(key);
                                             return std::string{};
                                         });
            control->registerViewCommand("localizationMissingOnly", "editorLocalization",
                                         "lists only the keys the locale lacks, or all of them",
                                         {requiredArg("enabled", ArgKind::Boolean, "on or off")},
                                         [this](const ControlArgs& arguments, IFormatWriter&)
                                         {
                                             missingOnly = arguments.boolean("enabled").value_or(false);
                                             applyItems();
                                             stateDirty = true;
                                             return std::string{};
                                         });
            control->registerViewCommand("localizationEdit", "editorLocalization",
                                         "sets the text of the selected key's value field (localizationSave writes it)",
                                         {requiredArg("text", ArgKind::String, "the message")},
                                         [this](const ControlArgs& arguments, IFormatWriter&)
                                         {
                                             if (!current())
                                             {
                                                 return std::string("no key selected");
                                             }
                                             edit.setText(arguments.string("text").value_or(""));
                                             stateDirty = true;
                                             return std::string{};
                                         });
            control->registerViewCommand("localizationSave", "editorLocalization",
                                         "writes the value field as the selected key's message (setLocString, "
                                         "undoable); the panel's note carries a refusal",
                                         {},
                                         [this](const ControlArgs&, IFormatWriter&)
                                         {
                                             if (!current())
                                             {
                                                 return std::string("no key selected");
                                             }
                                             saveDue = true;
                                             return std::string{};
                                         });
            control->registerViewCommand(
                "localizationPreview", "editorLocalization",
                "sets the sample arguments of the preview: a JSON object of names to integers, numbers or strings",
                {optionalArg("args", ArgKind::Json, "the JSON object, or its text", "none")},
                [this](const ControlArgs& arguments, IFormatWriter&)
                {
                    std::string text = arguments.json("args").value_or("");
                    if (const Json parsed = Json::parse(text, nullptr, false); parsed.is_string())
                    {
                        text = parsed.get<std::string>();
                    }
                    args.setText(text);
                    previewDue = true;
                    return std::string{};
                });
            control->registerQuery(
                "localizationView", "editorLocalization",
                "the localization panel: the locale and the editable locales, rows (key, group, value, forms, state), "
                "the selected key, its value field, the preview (text, locale), the note, the totals and the list's "
                "filter and sort",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    out.writeString("locale", locale);
                    out.beginArray("locales", static_cast<u32>(locales.size()));
                    for (const std::string& each : locales)
                    {
                        out.writeString("", each);
                    }
                    out.endArray();
                    out.writeString("selectedKey", current() ? current()->key : std::string{});
                    out.writeString("edit", edit.value());
                    out.writeString("args", args.value());
                    out.writeString("preview", previewShown);
                    out.writeString("previewLocale", previewLocale);
                    out.writeString("note", noteShown);
                    out.writeBool("missingOnly", missingOnly);
                    out.writeU64("total", total);
                    out.writeU64("missing", missingCount);
                    out.writeBool("missingTruncated", missingTruncated);
                    out.beginArray("rows", static_cast<u32>(items.size()));
                    for (const u32 index : items)
                    {
                        out.beginObject("");
                        writeValue(StringCells::spiteSchema(), &cells[index], out, FieldSet::All);
                        out.writeBool("missing", entries[index].missing);
                        out.endObject();
                    }
                    out.endArray();
                    out.writeU64("refreshes", refreshes);
                    list->writeView(out);
                    return std::string{};
                });
        }
    };

    EditorLocalization::EditorLocalization(Engine& engine) : m_state(std::make_unique<State>(engine))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.root = scope.createPanel("editor.panel", columnDesc());
        state.scroll =
            PageScroll(scope, InternedString("loc.scroll"), InternedString("editor-localization.title"), state.root);

        UiDropdownDesc dropdown;
        dropdown.rect = barDesc(Vec2{LOCALE_WIDTH, 0.0f});
        state.localeChoice = state.density.track(scope.createDropdown("loc.locale", dropdown), HEIGHT_CONTROL);
        state.localeChoice.setAccessibleNameKey(InternedString("editor-localization.locale"));
        state.localeChoice.onChange(
            [&state](const u32 index)
            {
                if (index < state.locales.size())
                {
                    state.pending.push_back(
                        Json{{"command", "localizationLocale"}, {"args", {{"locale", state.locales[index]}}}}.dump());
                }
            });
        const auto bar = [&](Button& button, const char* id, const char* key, std::function<Json()> command)
        {
            button = state.density.track(scope.createButton(id, "", fitted()), HEIGHT_CONTROL);
            button.setLocalizedText(InternedString(key));
            button.setTooltip(InternedString(std::string(key) + ".tip"));
            button.onClick([&state, command] { state.pending.push_back(command().dump()); });
        };
        bar(state.missingOnlyButton, "loc.missingOnly", "editor-localization.missingOnly", [&state]
            { return Json{{"command", "localizationMissingOnly"}, {"args", {{"enabled", !state.missingOnly}}}}; });
        bar(state.useButton, "loc.use", "editor-localization.use",
            [&state] { return Json{{"command", "setLocale"}, {"args", {{"locale", state.locale}}}}; });
        bar(state.reloadButton, "loc.reload", "editor-localization.reload",
            [&state]
            {
                state.actionNote.clear(); // the read below may show a table that no longer loads
                state.due = state.noteDirty = true;
                return Json{{"command", "locReload"}, {"args", {{"force", true}}}};
            });
        bar(state.saveButton, "loc.save", "editor-localization.save",
            [] { return Json{{"command", "localizationSave"}, {"args", Json::object()}}; });

        state.editLabel = state.density.track(scope.createLabel("loc.editLabel", "", fitted()), HEIGHT_ROW);
        state.editLabel.setLocalizedText(InternedString("editor-localization.edit"));
        UiRectDesc inputDesc = barDesc(Vec2{});
        inputDesc.layoutItem = fillItem();
        state.edit = state.density.track(scope.createTextInput("loc.edit.input", inputDesc), HEIGHT_ROW);
        state.edit.setStyleClass(InternedString("tool.field"));
        state.edit.setAccessibleNameKey(InternedString("editor-localization.edit"));
        state.edit.setInteractable(false);
        state.edit.onChange([&state](std::string_view) { state.stateDirty = true; });
        state.edit.onSubmit([&state](std::string_view)
                            { state.pending.push_back(Json{{"command", "localizationSave"}}.dump()); });
        state.previewLabel = state.density.track(scope.createLabel("loc.previewLabel", "", fitted()), HEIGHT_ROW);
        state.args =
            state.density.track(scope.createTextInput("loc.args", barDesc(Vec2{ARGS_WIDTH, 0.0f})), HEIGHT_ROW);
        state.args.setStyleClass(InternedString("tool.field"));
        state.args.setAccessibleNameKey(InternedString("editor-localization.args"));
        state.args.setTooltip(InternedString("editor-localization.args.tip"));
        state.args.onCommit([&state](std::string_view) { state.previewDue = true; });
        state.preview = state.density.track(readOnlyText(scope, "loc.preview.text", inputDesc), HEIGHT_ROW);
        state.preview.setStyleClass(InternedString("tool.field"));
        state.preview.setAccessibleNameKey(InternedString("editor-localization.previewText"));
        UiRectDesc noteDesc = barDesc(Vec2{});
        noteDesc.layoutItem = fillItem();
        noteDesc.layoutItem->alignSelf = UiLayoutAlign::Stretch; // as high as the status row, whatever its size.row
        state.note = scope.createLabel("loc.note", "", noteDesc);
        state.note.setStyleClass(InternedString("loc.note"));

        state.list = std::make_unique<FilteredTable>(
            scope, *state.control,
            FilteredTableDesc{
                .id = "loc",
                .owner = "editorLocalization",
                .nameKey = InternedString("editor-localization.table"),
                .schema = &StringCells::spiteSchema(),
                .columns = {column("key", 240.0f), column("group", 120.0f), column("value", 300.0f),
                            column("forms", 140.0f), column("state", 90.0f)},
                .row = [&state](const u32 item) -> const void* { return &state.cells[state.items[item]]; },
                .key = [&state](const u32 item) { return state.keys[state.items[item]]; },
                .matches =
                    [&state](const u32 item, const std::string_view needle)
                {
                    const StringCells& row = state.cells[state.items[item]];
                    return containsIgnoringCase(row.key, needle) || containsIgnoringCase(row.value, needle) ||
                        containsIgnoringCase(row.group, needle);
                },
                .headerHints = {{InternedString("forms"), InternedString("editor-localization.hint.forms")},
                                {InternedString("state"), InternedString("editor-localization.hint.state")}}});
        state.list->table().onSelect([&state](const std::span<const Entity> picked)
                                     { state.select(picked.empty() ? Entity::undefined() : picked[0]); });
        state.registerCommands();
        scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(LOC_BAR),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.localeChoice);
                          for (Button* button : {&state.missingOnlyButton, &state.useButton, &state.reloadButton})
                          {
                              putIn(state.scope, slot, *button);
                              setOwnClass(*button, slotFields(state.scope, slot).childClass);
                          }
                      });
        contract.slot(InternedString(LOC_FILTER), [&state](const Widget& slot) { state.list->fillFilter(slot); });
        contract.slot(InternedString(LOC_ROWS), [&state](const Widget& slot) { state.list->fillRows(slot); });
        contract.slot(InternedString(LOC_EDIT),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.editLabel);
                          putIn(state.scope, slot, state.edit);
                          putIn(state.scope, slot, state.saveButton);
                          state.saveButton.setStyleClass(slotFields(state.scope, slot).childClass);
                      });
        contract.slot(InternedString(LOC_PREVIEW),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.previewLabel);
                          putIn(state.scope, slot, state.args);
                          putIn(state.scope, slot, state.preview);
                      });
        contract.slot(InternedString(LOC_STATUS),
                      [&state](const Widget& slot) { putIn(state.scope, slot, state.note); });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                // the edit and preview rows only pad: a text line and the padding above and below it
                const auto inset = [&](const std::string_view id)
                {
                    const Density density = Density::read(viewScope);
                    Panel panel =
                        viewScope.createPanel(InternedString(id),
                                              barDesc(Vec2{0.0f, HEIGHT_FIELD.of(density)},
                                                      stack(UiLayoutDirection::Row, density.xs,
                                                            Vec4{density.xs, density.xxs, density.xs, density.xxs})));
                    bindGap(panel, SPACE_XS);
                    bindPadding(panel, SPACE_XS, SPACE_XXS, SPACE_XS, SPACE_XXS);
                    panel.setParent(root);
                };
                wrapBarPanel(viewScope, LOC_TOOLBAR, LOC_BAR, root);
                Panel searchBar = toolbarPanel(viewScope, LOC_SEARCH_BAR, root);
                filterSlotPanel(viewScope, LOC_FILTER, searchBar);
                fillSlotPanel(viewScope, LOC_ROWS, root);
                inset(LOC_EDIT);
                inset(LOC_PREVIEW);
                statusPanel(viewScope, LOC_STATUS, root);
            });
        state.view =
            openView(engine, LOC_VIEW, "assets/ui/editor/localization.json", std::move(contract), state.scroll.page());
    }

    EditorLocalization::~EditorLocalization() { m_state->control->unregisterOwner("editorLocalization"); }

    Widget EditorLocalization::content() const { return m_state->root; }

    void EditorLocalization::update()
    {
        State& state = *m_state;
        if (!state.pending.empty())
        {
            dispatch(*state.control, state.pending, "localization");
        }
        state.list->update();
        if (!shown(state.scope, state.root))
        {
            state.wasShown = false;
            return;
        }
        state.scroll.fit(state.density.density());
        if (state.saveDue)
        {
            state.save();
        }
        if (!state.wasShown || state.due ||
            (state.localization && state.localization->revision() != state.seenRevision))
        {
            state.wasShown = true;
            state.refresh();
        }
        if (state.previewDue)
        {
            state.resolvePreview();
        }
        if (state.stateDirty)
        {
            state.showState();
        }
        if (state.noteDirty)
        {
            state.showNote();
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "localization",
             .category = "tools",
             .order = 170,
             .dock = {.target = "scene"},
             .zone = "EditorLocalization",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<EditorLocalization>(c.engine); }}};
    } // namespace
} // namespace spite
