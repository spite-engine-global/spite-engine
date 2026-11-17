#include "EditorConsole.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorFeedback.hpp"
#include "EditorPanelUi.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/Logging.hpp"
#include "control/ControlServer.hpp"
#include "ecs/ComponentMetadata.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include "rendering/RenderGraph.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiScope.hpp"
#include "ui/UiTextMeasurer.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using namespace panelui;

        // HID scancodes (the UiKeyTransition domain)
        constexpr u32 RETURN_KEY = 40;
        constexpr u32 ESCAPE_KEY = 41;
        constexpr u32 TAB_KEY = 43;
        constexpr u32 DOWN_KEY = 81;
        constexpr u32 UP_KEY = 82;

        constexpr std::string_view CONSOLE_VIEW = "editor.console";
        constexpr std::string_view CONSOLE_LINES = "console.lines";
        constexpr std::string_view CONSOLE_PROMPT = "console.prompt";
        constexpr std::string_view CONSOLE_FIELD = "console.field";
        constexpr std::string_view CONSOLE_ACTIONS = "console.actions";
        constexpr float MEASURE_FALLBACK = 7.0f; // a character's width without a text measurer
        // the one verb whose `path` is a component field path (Component.field); the others' are files, sections
        constexpr std::string_view FIELD_PATH_VERB = "set";

        // -- console helpers ---------------------------------------------------------

        std::vector<std::string> names(ControlServer& control, const bool commandsOnly)
        {
            std::vector<std::string> out;
            const Json reply = request(control, {{"query", "list"}});
            for (const char* kind : {"commands", "queries"})
            {
                if (commandsOnly && std::string_view(kind) == "queries")
                {
                    continue;
                }
                for (const Json& entry : reply["result"].value(kind, Json::array()))
                {
                    out.push_back(entry.value("name", std::string()));
                }
            }
            return out;
        }

        // `name key=value ...` (the spite CLI's syntax) or a JSON request: a
        // value that parses as JSON (number, array, object, true/false) is sent
        // as JSON, anything else as a string; registered command names are sent
        // as commands, the rest as queries.
        std::optional<Json> parseLine(ControlServer& control, const std::string_view line, std::string& error)
        {
            if (line.front() == '{')
            {
                Json parsed = Json::parse(line, nullptr, false);
                if (!parsed.is_object())
                {
                    error = "not a JSON object";
                    return std::nullopt;
                }
                return parsed;
            }
            std::vector<std::string> words;
            for (size_t at = 0; at < line.size();)
            {
                const size_t end = std::min(line.find(' ', at), line.size());
                if (end > at)
                {
                    words.emplace_back(line.substr(at, end - at));
                }
                at = end + 1;
            }
            Json args = Json::object();
            for (size_t i = 1; i < words.size(); ++i)
            {
                const size_t equals = words[i].find('=');
                const std::string key = words[i].substr(0, equals);
                const std::string value = equals == std::string::npos ? "" : words[i].substr(equals + 1);
                Json parsed = Json::parse(value, nullptr, false);
                args[key] = parsed.is_discarded() || parsed.is_null() ? Json(value) : std::move(parsed);
            }
            const std::vector<std::string> commands = names(control, true);
            const bool command = std::ranges::find(commands, words.front()) != commands.end();
            return Json{{command ? "command" : "query", words.front()}, {"args", std::move(args)}};
        }

        // the command or query name a request carries; a JSON line may name neither as a string
        std::string verbOf(const Json& request)
        {
            for (const char* key : {"command", "query"})
            {
                const auto it = request.find(key);
                if (it != request.end() && it->is_string())
                {
                    return it->get<std::string>();
                }
            }
            return "request";
        }

        // the args a verb declares (describe) that the line does not name yet
        std::vector<std::string> unusedArgs(ControlServer& control, const std::string& line)
        {
            std::vector<std::string> out;
            const std::string verb = line.substr(0, line.find(' '));
            const Json reply = request(control, {{"query", "describe"}, {"args", {{"name", verb}}}});
            if (!reply.value("ok", false))
            {
                return out;
            }
            for (const Json& arg : reply["result"]["entries"][0].value("args", Json::array()))
            {
                std::string name = arg.value("name", std::string());
                if (line.find(" " + name + "=") == std::string::npos)
                {
                    out.push_back(std::move(name));
                }
            }
            return out;
        }

        // the values describe lists for a verb's argument
        std::vector<std::string> declaredChoices(ControlServer& control, const std::string& line,
                                                 const std::string& key)
        {
            std::vector<std::string> out;
            const std::string verb = line.substr(0, line.find(' '));
            const Json reply = request(control, {{"query", "describe"}, {"args", {{"name", verb}}}});
            if (!reply.value("ok", false))
            {
                return out;
            }
            for (const Json& arg : reply["result"]["entries"][0].value("args", Json::array()))
            {
                if (arg.value("name", std::string()) == key)
                {
                    for (const Json& choice : arg.value("choices", Json::array()))
                    {
                        out.push_back(choice.get<std::string>());
                    }
                }
            }
            return out;
        }

        void addPaths(const StructSchema& schema, const std::string& prefix, std::vector<std::string>& out,
                      const u32 depth)
        {
            for (const FieldDesc& field : schema.fields)
            {
                const std::string path = prefix + "." + std::string(field.name);
                out.push_back(path);
                if (field.kind == FieldKind::Struct && field.arrayCount == 0 && depth < 4)
                {
                    addPaths(*field.structSchema, path, out, depth + 1);
                }
            }
        }
    } // namespace

    // -- console ------------------------------------------------------------------------

    struct EditorConsole::State
    {
        static constexpr sizet MAX_LINES = 2000; // output lines kept; older ones go
        static constexpr sizet MAX_REPLY_LINES = 400; // one result shows this many lines, then the marker
        static constexpr sizet MAX_REPLY_BYTES = 64 * 1024; // or this many bytes, a longer line cut inside
        static constexpr sizet MAX_BYTES = 4 * 1024 * 1024; // bytes of every line kept; older ones go
        static constexpr sizet MAX_HISTORY = 200;
        static constexpr u32 MAX_CANDIDATES = 48;
        static constexpr sizet MAX_ECHO = 60; // bytes of a typed line a failure row names

        State(Engine& engine, EditorFeedback& feedback) :
            engine(&engine), control(&engine.control()), feedback(&feedback), scope(editorUi(engine).ui()),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        EditorFeedback* feedback;
        UiScope scope;
        DensityFollower density{scope};
        const Localization* localization; // not owned: the engine's service
        Panel root; // the docked content: the document view's root goes in it
        std::unique_ptr<Table> output;
        Widget field; // the slot holding the input, read for its width each frame
        TextInput input;
        Label prompt;
        std::array<Button, 2> actions; // clear, copy
        PopupList completions;
        std::deque<std::string> lines; // what ran and came back, one entry per line
        std::vector<std::string> rows; // `lines` wrapped to the output's width: what the list shows
        std::vector<std::string> history;
        sizet browse = 0; // history index Up/Down shows; history.size(): the draft
        std::string draft;
        std::vector<std::string> pending; // console requests the GUI queued
        std::vector<std::string> queued; // lines the console command took, run at update
        std::string lastResult; // the last reply's result, whole
        std::string lastError; // why the last line failed, as the engine said it; empty after one that ran
        u32 truncated = 0; // lines of the last result the output does not show
        Vec2 size{-1.0f, -1.0f}; // the field size the caret was last fitted to
        float scrollX = 0.0f;
        float charWidth = 0.0f; // a letter's width in the output
        u64 measuredAt = 0; // the layout generation it was measured at
        Entity measuredIn = Entity::undefined(); // and the element it was measured in
        u32 columns = 0; // characters a wrapped row holds
        std::string followedText;
        u32 followedCaret = NO_ROW;
        u64 runs = 0;
        // the open completions: what the word at the caret becomes for each item
        struct Offer
        {
            sizet wordStart = 0;
            std::string key; // `name=` of a value
            bool verb = false;
            bool argName = false;
            std::vector<std::string> matches;
        } offer;
        bool navigated = false; // Up or Down moved in the list: Enter and Tab choose from it
        std::unique_ptr<UiDocumentView> view;

        [[nodiscard]] std::string say(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            if (!localization)
            {
                return std::string(key);
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(key), args, scratch));
        }

        void append(std::string line) { lines.push_back(std::move(line)); }

        // the typed text as a log row's name: a long one keeps its start (whole code points) and an ellipsis
        static std::string clipped(const std::string_view line)
        {
            if (line.size() <= MAX_ECHO)
            {
                return std::string(line);
            }
            sizet end = MAX_ECHO;
            while (end > 0 && (static_cast<u8>(line[end]) & 0xC0u) == 0x80u)
            {
                --end;
            }
            return std::string(line.substr(0, end)) + "...";
        }

        // a failed line shows in the output and, as one warning, in the engine log (the Log panel, the status bar
        // and logView see it)
        void fail(const std::string_view name, const std::string& reason)
        {
            const std::array<MessageArg, 1> args{{{"reason", std::string_view(reason)}}};
            lastError = reason;
            append(say("editor-console.error", args));
            SWARNING_LOG("console: {}: {}", clipped(name), reason);
        }

        void run(const std::string& line)
        {
            ++runs;
            lastError.clear();
            append("> " + line);
            truncated = 0;
            std::string error;
            const std::optional<Json> parsed = parseLine(*control, line, error);
            if (!parsed)
            {
                fail(line, error);
                return;
            }
            const Json reply = request(*control, *parsed);
            if (!reply.value("ok", false))
            {
                fail(verbOf(*parsed), reply.value("error", reply.dump()));
                return;
            }
            const Json& result = reply.contains("result") ? reply["result"] : Json::object();
            if (result.empty())
            {
                lastResult.clear();
                append(say("editor-console.ok"));
                return;
            }
            lastResult = result.dump(2);
            const auto cutFrom = [this](const size_t from)
            {
                truncated = static_cast<u32>(std::count(lastResult.begin() + from, lastResult.end(), '\n') + 1);
                const std::array<MessageArg, 1> args{{{"lines", static_cast<std::int64_t>(truncated)}}};
                append(say("editor-console.truncated", args));
            };
            sizet shown = 0;
            for (size_t at = 0, count = 0; at < lastResult.size(); ++count)
            {
                if (count == MAX_REPLY_LINES || shown >= MAX_REPLY_BYTES)
                {
                    cutFrom(at);
                    break;
                }
                const size_t end = std::min(lastResult.find('\n', at), lastResult.size());
                size_t take = std::min(end - at, MAX_REPLY_BYTES - shown);
                while (take > 0 && at + take < end && (static_cast<u8>(lastResult[at + take]) & 0xC0u) == 0x80u)
                {
                    --take; // not inside a UTF-8 sequence
                }
                append(lastResult.substr(at, take));
                shown += take;
                if (at + take < end)
                {
                    cutFrom(at + take); // a line longer than what is left
                    break;
                }
                at = end + 1;
            }
        }

        // -- the input --

        // the field slot's resolved size in layout units (a panel's own size is what was authored, not what the
        // container gave it)
        [[nodiscard]] Vec2 fieldSize() const { return resolvedSize(scope, field); }

        void layout()
        {
            if (!field.isValid())
            {
                return;
            }
            const Vec2 now = fieldSize();
            if (now == size)
            {
                return;
            }
            size = now;
            followedCaret = NO_ROW; // re-fit the input to the field
            followCaret();
        }

        // Scrolls the input inside its clipping field so the caret shows.
        void followCaret()
        {
            if (!field.isValid())
            {
                return;
            }
            const std::string_view text = input.value();
            const u32 caret = input.caret();
            if (caret == followedCaret && text == followedText)
            {
                return;
            }
            followedCaret = caret;
            followedText = text;
            const float width =
                std::max(0.0f, fieldSize().x - 2.0f * density.density().xs); // the slot less the input's inset
            const UiTextMeasurer* measurer = scope.hierarchy().textMeasurer();
            const float textWidth = measurer ? measurer->measureText(input.entity(), text).x : 0.0f;
            const float caretX = measurer ? measurer->measureText(input.entity(), text.substr(0, caret)).x : 0.0f;
            scrollX = std::max(std::min(scrollX, caretX), caretX + 2.0f - width);
            scrollX = std::clamp(scrollX, 0.0f, std::max(0.0f, textWidth + 2.0f - width));
            const Density& now = density.density();
            place(input, Vec2{now.xs - scrollX, now.xxs}, Vec2{std::max(width, textWidth + 2.0f), now.row});
        }

        void recall(const bool older)
        {
            if (history.empty() || (older && browse == 0) || (!older && browse >= history.size()))
            {
                return;
            }
            if (browse == history.size())
            {
                draft = input.value();
            }
            browse = older ? browse - 1 : browse + 1;
            input.setText(browse == history.size() ? draft : history[browse]);
        }

        // -- completion --

        std::vector<std::string> candidates(const std::string& line, const std::string& key, const bool verb)
        {
            if (verb)
            {
                return names(*control, false);
            }
            std::vector<std::string> out = declaredChoices(*control, line, key);
            if (!out.empty())
            {
                return out;
            }
            if (key == "world")
            {
                for (const Json& world : request(*control, {{"query", "worlds"}})["result"]["worlds"])
                {
                    out.push_back(world.value("name", std::string()));
                }
            }
            else if (key == "component" || (key == "path" && line.substr(0, line.find(' ')) == FIELD_PATH_VERB))
            {
                std::vector<ComponentID> ids(getRegisteredComponentCount());
                ids.resize(std::min<sizet>(ids.size(), getComponentIds(ids.data(), static_cast<u32>(ids.size()))));
                for (const ComponentID id : ids)
                {
                    const ComponentMetadata& metadata = getComponentMetadata(id);
                    if (!metadata.schema)
                    {
                        continue;
                    }
                    const std::string name(metadata.name.toView());
                    if (key == "component")
                    {
                        out.push_back(name);
                    }
                    else
                    {
                        addPaths(*metadata.schema, name, out, 0);
                    }
                }
            }
            else if (key == "pass")
            {
                engine->services().queryTable("RenderGraph", "passes", [&out](const void* row)
                                              { out.push_back(static_cast<const RenderGraphPassRow*>(row)->pass); });
            }
            return out;
        }

        // The word at the caret and what it can become, from what the engine has registered (list, describe), the
        // worlds, components and passes: sorted, without repeats.
        [[nodiscard]] Offer wordAtCaret()
        {
            Offer found;
            const std::string text(input.value());
            const u32 caret = input.caret();
            const size_t start = text.rfind(' ', caret == 0 ? 0 : caret - 1);
            found.wordStart = start == std::string::npos || caret == 0 ? 0 : start + 1;
            const std::string word = text.substr(found.wordStart, caret - found.wordStart);
            found.verb = found.wordStart == 0;
            const size_t equals = word.find('=');
            found.argName = !found.verb && equals == std::string::npos;
            found.key = found.verb || found.argName ? std::string() : word.substr(0, equals);
            const std::string partial = found.verb || found.argName ? word : word.substr(equals + 1);
            for (std::string& candidate :
                 found.argName ? unusedArgs(*control, text) : candidates(text, found.key, found.verb))
            {
                if (candidate.starts_with(partial))
                {
                    found.matches.push_back(std::move(candidate));
                }
            }
            std::ranges::sort(found.matches);
            found.matches.erase(std::ranges::unique(found.matches).begin(), found.matches.end());
            return found;
        }

        // the word at the caret becomes `completion` (a lone verb takes a space after it, a lone arg name an `=`)
        void accept(const Offer& at, const std::string& completion, const bool alone)
        {
            const std::string text(input.value());
            const u32 caret = input.caret();
            const std::string completed =
                (at.verb || at.argName ? "" : at.key + "=") + completion + (alone ? (at.argName ? "=" : " ") : "");
            input.setText(text.substr(0, at.wordStart) + completed + text.substr(caret));
            const auto end = static_cast<u32>(at.wordStart + completed.size());
            input.select(end, end);
        }

        // Tab: completes the word at the caret; several matches extend to their common prefix (the list under the
        // input shows them).
        void complete()
        {
            const Offer at = wordAtCaret();
            if (at.matches.empty())
            {
                return;
            }
            std::string common = at.matches.front();
            for (const std::string& match : at.matches)
            {
                common.resize(std::ranges::mismatch(common, match).in1 - common.begin());
            }
            accept(at, common, at.matches.size() == 1);
        }

        // The list under the input: the word's matches while there are some besides the word itself.
        void refreshCompletions()
        {
            navigated = false;
            if (input.value().empty() || scope.router().focused() != input.entity())
            {
                completions.close();
                return;
            }
            offer = wordAtCaret();
            const std::string_view text = input.value();
            const std::string_view word = text.substr(std::min(offer.wordStart, text.size()),
                                                      input.caret() - std::min<sizet>(input.caret(), offer.wordStart));
            if (offer.matches.empty() || (offer.matches.size() == 1 && word.ends_with(offer.matches.front())))
            {
                completions.close();
                return;
            }
            offer.matches.resize(std::min<sizet>(offer.matches.size(), MAX_CANDIDATES));
            std::vector<UiChoiceItem> items;
            for (const std::string& match : offer.matches)
            {
                items.push_back({.label = match});
            }
            completions.setItems(items);
            completions.setSelected(-1);
            if (!completions.isOpen())
            {
                completions.open(field);
            }
        }

        // -- the output --

        // `line` in pieces of at most `columns` characters: a continuation is indented under its line
        void wrap(const std::string& line)
        {
            if (line.size() <= columns)
            {
                rows.push_back(line);
                return;
            }
            const sizet lead = line.find_first_not_of(' ');
            const sizet indent = std::min<sizet>(lead == std::string::npos ? 0 : lead + 2, columns / 2);
            sizet at = 0;
            while (at < line.size())
            {
                const sizet room = at == 0 ? columns : columns - indent;
                sizet end = std::min(at + room, line.size());
                if (end < line.size())
                {
                    const sizet space = line.find_last_of(" ,", end - 1);
                    if (space != std::string::npos && space > at + room / 2)
                    {
                        end = space + 1;
                    }
                    while (end > at + 1 && (static_cast<u8>(line[end]) & 0xC0) == 0x80)
                    {
                        --end; // not inside a UTF-8 sequence
                    }
                }
                rows.push_back((at == 0 ? std::string() : std::string(indent, ' ')) + line.substr(at, end - at));
                at = end;
            }
        }

        // characters that fit the output's width, by the widest letter so that a row never clips; the letter is
        // measured in the element that paints the rows, again when the layout changed (a text scale, a letter
        // spacing) or the rows were created
        [[nodiscard]] u32 columnsNow()
        {
            const Entity painting = output->textElement();
            if (const u64 generation = scope.hierarchy().layoutGeneration();
                charWidth <= 0.0f || generation != measuredAt || painting != measuredIn)
            {
                measuredAt = generation;
                measuredIn = painting;
                const UiTextMeasurer* measurer = scope.hierarchy().textMeasurer();
                constexpr std::string_view SAMPLE = "MMMMMMMMMMMMMMMM"; // a literal: every relayout measures it
                charWidth =
                    measurer ? measurer->measureText(painting, SAMPLE).x / static_cast<float>(SAMPLE.size()) : 0.0f;
                charWidth = charWidth > 0.0f ? charWidth : MEASURE_FALLBACK;
            }
            const float width = resolvedSize(scope, output->list).x - THUMB - 2.0f * density.density().xs;
            return static_cast<u32>(std::max(16.0f, width / charWidth));
        }

        void showLines()
        {
            sizet bytes = 0;
            for (const std::string& line : lines)
            {
                bytes += line.size();
            }
            while (lines.size() > MAX_LINES || bytes > MAX_BYTES)
            {
                bytes -= lines.front().size();
                lines.pop_front();
            }
            columns = columnsNow();
            rows.clear();
            for (const std::string& line : lines)
            {
                wrap(line);
            }
            output->setRowCount(static_cast<u32>(rows.size()));
            output->scrollToBottom();
            output->rebindVisible(); // wrapped and capped lines shift the rows
        }

        std::string copy(IFormatWriter& out)
        {
            if (lastResult.empty())
            {
                return say("editor-console.nothingToCopy");
            }
            editorUi(*engine).ui().setClipboardText(lastResult);
            const auto count = static_cast<std::int64_t>(std::count(lastResult.begin(), lastResult.end(), '\n') + 1);
            feedback->postToastKey(ToastLevel::Info, "editor-console.copied",
                                   std::vector<MessageArg>{{"lines", count}});
            out.writeU64("bytes", lastResult.size());
            out.writeU64("lines", static_cast<u64>(count));
            return {};
        }

        void registerCommands()
        {
            control->registerCommand("console", "editorConsole", "runs a line in the editor console, as typed",
                                     {requiredArg("line", ArgKind::String, "`name key=value ...` or a JSON request")},
                                     [this](const ControlArgs& args, IFormatWriter&)
                                     {
                                         std::optional<std::string> line = args.string("line");
                                         if (!line || line->find_first_not_of(' ') == std::string::npos)
                                         {
                                             return std::string("console needs a non-empty line");
                                         }
                                         line->erase(0, line->find_first_not_of(' '));
                                         line->erase(line->find_last_not_of(' ') + 1);
                                         if (history.empty() || history.back() != *line)
                                         {
                                             history.push_back(*line);
                                             if (history.size() > MAX_HISTORY)
                                             {
                                                 history.erase(history.begin());
                                             }
                                         }
                                         browse = history.size();
                                         queued.push_back(std::move(*line));
                                         return std::string{};
                                     });
            control->registerViewCommand(
                "consoleClear", "editorConsole",
                "empties the console's output (the history and the last result stay): the Clear button", {},
                [this](const ControlArgs&, IFormatWriter&)
                {
                    lines.clear();
                    showLines();
                    return std::string{};
                });
            control->registerViewCommand(
                "consoleCopy", "editorConsole",
                "copies the last result the console printed, whole and as formatted JSON, to the clipboard: the Copy "
                "button; the answer is its size",
                {}, [this](const ControlArgs&, IFormatWriter& out) { return copy(out); });
            control->registerQuery(
                "consoleView", "editorConsole",
                "the console's history, output lines (each as it ran, before wrapping; a failure's line is in the "
                "shown language, lastError is its reason alone), the input, the open completions and the output "
                "list's rows",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    out.beginArray("history", static_cast<u32>(history.size()));
                    for (const std::string& line : history)
                    {
                        out.writeString("", line);
                    }
                    out.endArray();
                    out.beginArray("output", static_cast<u32>(lines.size()));
                    for (const std::string& line : lines)
                    {
                        out.writeString("", line);
                    }
                    out.endArray();
                    out.writeString("input", input.value());
                    out.writeU64("runs", runs);
                    out.writeString("lastError", lastError);
                    out.writeU64("wrapped", rows.size());
                    out.writeU32("columns", columns);
                    out.writeU32("truncated", truncated);
                    out.writeU64("lastResultBytes", lastResult.size());
                    out.writeBool("placeholder", input.value().empty());
                    out.beginObject("completions");
                    out.writeBool("open", completions.isOpen());
                    out.writeBool("navigated", navigated);
                    out.beginArray("items", completions.isOpen() ? static_cast<u32>(offer.matches.size()) : 0);
                    if (completions.isOpen())
                    {
                        for (const std::string& match : offer.matches)
                        {
                            out.writeString("", match);
                        }
                    }
                    out.endArray();
                    out.endObject();
                    writeTableStats(out, *output);
                    return std::string{};
                });
        }
    };

    EditorConsole::EditorConsole(Engine& engine, EditorFeedback& feedback) :
        m_state(std::make_unique<State>(engine, feedback))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.root = scope.createPanel("editor.panel", columnDesc());
        state.output =
            std::make_unique<Table>(scope,
                                    TableDesc{.id = "console.output",
                                              .read = [&state](const u32 row, const std::span<std::string> cells,
                                                               std::string&) { cells[0] = state.rows[row]; }});
        state.output->list.setLayoutItem(fillItem());
        state.output->list.setAccessibleNameKey(InternedString("editor-console.output"));
        state.input = scope.createTextInput("console.input", topLeft());
        state.density.apply([&state](const Density&) { state.size = Vec2{-1.0f, -1.0f}; }); // the caret re-fits
        state.input.setAccessibleNameKey(InternedString("editor.access.console"));
        state.prompt = state.density.track(scope.createLabel("console.prompt.text", "", fitWidthDesc()), HEIGHT_FIELD);
        state.prompt.setLocalizedText(InternedString("editor-console.prompt"));
        state.prompt.setAccessibleRole(UiRole::Presentation);
        state.input.setLocalizedPlaceholder(InternedString("editor-console.placeholder"));
        constexpr std::array<std::string_view, 2> ACTION_KEYS{"clear", "copy"};
        constexpr std::array<std::string_view, 2> ACTION_COMMANDS{"consoleClear", "consoleCopy"};
        for (u32 i = 0; i < state.actions.size(); ++i)
        {
            state.actions[i] =
                state.density.track(scope.createButton("console.action", "", fitWidthDesc()), HEIGHT_FIELD);
            bindFitPadding(state.actions[i], SPACE_MD, SPACE_NONE);
            state.actions[i].setLocalizedText(InternedString("editor-console." + std::string(ACTION_KEYS[i])));
            state.actions[i].onClick(
                [&state, command = ACTION_COMMANDS[i]]
                {
                    state.pending.push_back(Json{{"command", std::string(command)}}.dump());
                    state.scope.router().requestFocus(state.input.entity()); // typing goes on
                });
        }
        state.completions =
            scope.createPopupList("console.completions", UiDropdownDesc{.maxRows = 8, .keepFocus = true});
        state.completions.setAccessibleNameKey(InternedString("editor-console.completions"));
        state.completions.onChoose(
            [&state](const u32 index)
            {
                if (index < state.offer.matches.size())
                {
                    state.accept(state.offer, state.offer.matches[index], true);
                    state.refreshCompletions();
                }
            });
        state.input.onChange([&state](std::string_view) { state.refreshCompletions(); });
        state.input.onSubmit(
            [&state](const std::string_view line)
            {
                if (state.completions.isOpen() && state.navigated)
                {
                    state.completions.keyDown(RETURN_KEY); // chooses the highlighted completion
                    return;
                }
                state.completions.close();
                if (line.find_first_not_of(' ') == std::string_view::npos)
                {
                    return;
                }
                state.pending.push_back(Json{{"command", "console"}, {"args", {{"line", std::string(line)}}}}.dump());
                state.input.setText("");
            });
        state.input.onKey(
            [&state](const u32 scancode, u8)
            {
                if (scancode == UP_KEY || scancode == DOWN_KEY)
                {
                    if (!state.completions.isOpen())
                    {
                        state.recall(scancode == UP_KEY);
                        return;
                    }
                    // the first Down takes the list's first item, which is where its highlight starts
                    if (state.navigated || scancode == UP_KEY)
                    {
                        state.completions.keyDown(scancode);
                    }
                    state.navigated = true;
                }
                else if (scancode == TAB_KEY)
                {
                    if (state.completions.isOpen() && state.navigated)
                    {
                        state.completions.keyDown(RETURN_KEY);
                    }
                    else
                    {
                        state.complete();
                        state.refreshCompletions();
                    }
                }
            });
        // Escape closes an open list and leaves the line and the focus; the next one leaves the input
        state.input.onCancelRequest([&state] { return state.completions.keyDown(ESCAPE_KEY); });
        state.registerCommands();
        scope.commit();

        // the document owns the panel: the output list, the prompt, the input's clipping field and the buttons are
        // its slots; the input is placed from the field's resolved width
        UiViewContract contract;
        contract.slot(InternedString(CONSOLE_LINES),
                      [&state](const Widget& slot) { fillList(state.scope, slot, *state.output); });
        contract.slot(InternedString(CONSOLE_PROMPT),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.prompt);
                          state.prompt.setStyleClass(slotFields(state.scope, slot).childClass);
                      });
        contract.slot(InternedString(CONSOLE_FIELD),
                      [&state](const Widget& slot)
                      {
                          state.field = slot;
                          putIn(state.scope, slot, state.input);
                          const InternedString base = slotFields(state.scope, slot).childClass;
                          state.input.setStyleClass(base.isValid() ? base : InternedString("console.input"));
                          state.size = Vec2{-1.0f, -1.0f}; // the caret re-fits to the field
                      });
        contract.slot(InternedString(CONSOLE_ACTIONS),
                      [&state](const Widget& slot)
                      {
                          for (Button& action : state.actions)
                          {
                              putIn(state.scope, slot, action);
                              action.setStyleClass(slotFields(state.scope, slot).childClass);
                          }
                      });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                const Density density = Density::read(viewScope);
                columnRoot(root, density.xxs, Vec4{0.0f, 0.0f, 0.0f, density.xxs});
                bindGap(root, SPACE_XXS);
                bindPadding(root, SPACE_NONE, SPACE_NONE, SPACE_NONE, SPACE_XXS);
                fillSlotPanel(viewScope, CONSOLE_LINES, root);
                const float fieldHeight = HEIGHT_FIELD.of(density);
                Panel row = viewScope.createPanel(
                    "console.row",
                    barDesc(Vec2{0.0f, fieldHeight},
                            stack(UiLayoutDirection::Row, density.xs, Vec4{density.xs, 0.0f, density.xs, 0.0f},
                                  UiLayoutAlign::Stretch)));
                bindGap(row, SPACE_XS);
                bindPadding(row, SPACE_XS, SPACE_NONE, SPACE_XS, SPACE_NONE);
                bindBasis(row, SIZE_CONTROL);
                row.setParent(root);
                UiRectDesc promptDesc = barDesc(Vec2{0.0f, fieldHeight}, stack(UiLayoutDirection::Row, 0.0f));
                promptDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = false};
                Panel promptSlot = viewScope.createPanel(InternedString(CONSOLE_PROMPT), promptDesc);
                bindFitPadding(promptSlot, SPACE_NONE, SPACE_NONE);
                promptSlot.setParent(row);
                UiRectDesc fieldDesc = topLeft({}, Vec2{0.0f, fieldHeight});
                fieldDesc.layoutItem = fillItem();
                Panel field = viewScope.createPanel(InternedString(CONSOLE_FIELD), fieldDesc);
                viewScope.world().getComponent<UiStateComponent>(field.entity()).clipsChildren = true;
                field.setParent(row);
                UiRectDesc actionsDesc = barDesc(Vec2{0.0f, fieldHeight}, stack(UiLayoutDirection::Row, density.xs));
                actionsDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = false};
                Panel actionsSlot = viewScope.createPanel(InternedString(CONSOLE_ACTIONS), actionsDesc);
                bindGap(actionsSlot, SPACE_XS);
                bindFitPadding(actionsSlot, SPACE_NONE, SPACE_NONE);
                actionsSlot.setParent(row);
            });
        state.view = openView(engine, CONSOLE_VIEW, "assets/ui/editor/console.json", std::move(contract), state.root);
    }

    EditorConsole::~EditorConsole() { m_state->control->unregisterOwner("editorConsole"); }

    Widget EditorConsole::content() const { return m_state->root; }

    void EditorConsole::update()
    {
        State& state = *m_state;
        if (!state.pending.empty())
        {
            dispatch(*state.control, state.pending, "console");
        }
        if (!state.queued.empty())
        {
            std::vector<std::string> lines;
            lines.swap(state.queued);
            for (const std::string& line : lines)
            {
                state.run(line);
            }
            state.showLines();
        }
        state.layout();
        // a resize re-wraps the output; the list has a width once the panel is laid out
        if (state.output->list.size().x > 0.0f && state.columnsNow() != state.columns)
        {
            state.showLines();
        }
        if (state.scope.router().focused() == state.input.entity())
        {
            state.followCaret();
        }
        else if (state.completions.isOpen())
        {
            state.completions.close();
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "console",
             .category = "debug",
             .order = 60,
             .dock = {.target = "project"},
             .zone = "EditorConsole",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<EditorConsole>(c.engine, c.feedback); }}};
    } // namespace
} // namespace spite
