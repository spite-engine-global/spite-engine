#include "EditorUiDocuments.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorDialogs.hpp"
#include "EditorFeedback.hpp"
#include "EditorPageScroll.hpp"
#include "EditorPanelList.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "engine/PlayWorld.hpp"
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

        constexpr std::string_view DOCS_VIEW = "editor.uidocuments";
        constexpr std::string_view DOCS_BAR = "uidocs.bar";
        constexpr std::string_view DOCS_FILTER = "uidocs.filter";
        constexpr std::string_view DOCS_ROWS = "uidocs.rows";
        constexpr std::string_view DOCS_LAYERS = "uidocs.layers";
        constexpr std::string_view DOCS_FONTS = "uidocs.fonts";
        constexpr std::string_view DOCS_PICK = "uidocs.pick";
        constexpr std::string_view DOCS_STATUS = "uidocs.status";
        constexpr std::string_view DOCS_TOOLBAR = "uidocs.toolbar";
        constexpr std::string_view DOCS_LAYERBAR = "uidocs.layerbar";
        constexpr std::string_view UI_WORLD = "editor";
        constexpr std::string_view BASE_LAYER = "base";
        // The document the editor cannot run without: its shell (menus, dock and panels' hosts).
        // An import that breaks one leaves the editor without a way to undo it from its own UI, so an Import into
        // one asks first (a command passes force=true).
        constexpr std::array<std::string_view, 1> GUARDED_VIEWS{"editor.shell"};
        constexpr std::array<std::string_view, 2> PAGES{"documents", "style"};
        constexpr float TABLE_SLOT_MIN =
            72.0f; // a 30 px header and a 42 px list: the least a page list is laid out for
        constexpr float STYLE_ROWS = 6.0f; // of the picked element's style the page keeps in view
        constexpr u32 COMMAND_REFRESH_FRAMES = 20;

        struct DocCells
        {
            std::string view;
            std::string file;
            std::string slots;
            std::string state;
            bool operator==(const DocCells&) const = default;
            SPITE_OBSERVED_SCHEMA(DocCells, view, file, slots, state)
        };

        struct LayerCells
        {
            std::string name;
            std::string file;
            u32 classes = 0;
            u32 kinds = 0;
            u32 tokens = 0;
            u32 elements = 0;
            std::string state;
            bool operator==(const LayerCells&) const = default;
            SPITE_OBSERVED_SCHEMA(LayerCells, name, file, classes, kinds, tokens, elements, state)
        };

        struct FontCells
        {
            std::string font;
            std::string fallbacks;
            u32 ready = 0;
            u32 pending = 0;
            u32 failed = 0;
            bool operator==(const FontCells&) const = default;
            SPITE_OBSERVED_SCHEMA(FontCells, font, fallbacks, ready, pending, failed)
        };

        struct StyleCells
        {
            std::string field;
            std::string value;
            std::string source;
            bool operator==(const StyleCells&) const = default;
            SPITE_OBSERVED_SCHEMA(StyleCells, field, value, source)
        };

        struct WidgetCells
        {
            std::string widget;
            std::string kind;
            bool operator==(const WidgetCells&) const = default;
            SPITE_OBSERVED_SCHEMA(WidgetCells, widget, kind)
        };

        UiTableColumn column(const std::string_view prefix, const std::string_view field, const float width)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString("editor-uidocuments." + std::string(prefix) + "." + std::string(field))};
        }

        std::string fileText(const std::string& path)
        {
            const sizet at = path.find("assets/ui/");
            return at == std::string::npos ? path : path.substr(at + 10);
        }

        enum class Act : u8
        {
            Reload,
            Export,
            Import,
            LoadLayer,
            UnloadLayer,
            PickPoint,
            PickId,
            Page,
            GameDocuments,
            EditSpacing,
            EditText,
            Preview,
            OpenGameEditor,
        };

        struct Action
        {
            Act act;
            std::string text; // a file under assets/ui, a layer name or an element id
            bool confirmed = false;
            Vec2 point{};
        };
    } // namespace

    struct EditorUiDocuments::State
    {
        State(Engine& engine, EditorDialogs& dialogs, EditorFeedback& feedback) :
            engine(&engine), dialogs(&dialogs), feedback(&feedback), control(&engine.control()),
            scope(editorUi(engine).ui()),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        EditorDialogs* dialogs;
        EditorFeedback* feedback;
        ControlServer* control;
        UiScope scope;
        DensityFollower density{scope};
        const Localization* localization;
        Panel root;
        PageScroll scroll; // the page scrolls where the panel is shorter than its bars and tables
        Button reloadButton;
        Button exportButton;
        Button importButton;
        Button loadButton;
        Button unloadButton;
        Button pickButton;
        Button pageButton;
        Button gameButton;
        Button spacingButton;
        Button textButton;
        Button previewButton;
        Button openGameEditorButton;
        std::vector<std::string> widgetPaths;
        std::vector<std::string> widgetIds;
        std::vector<std::string> widgetKeys;
        std::string selectedWidget;
        std::string widgetDocument;
        Widget slotRows;
        Widget slotLayers;
        Widget slotFonts;
        Widget slotPick;
        u32 quiet = 0;
        u32 appliedPage = ~0u;
        u64 appliedDocument = ~0ull;
        u32 page = 0; // 0: documents, layers and fonts; 1: the picked element's style
        Label note;
        Modal pickModal;
        Label pickHint;
        TextInput pickId;
        Panel highlight;
        Entity pickedEntity = Entity::undefined();
        u64 seenLayout = 0;
        u64 frameCommand = 0;
        bool outlined = false;
        bool repick = false;
        std::unique_ptr<FilteredTable> list;
        std::unique_ptr<RowTable<LayerCells>> layerTable;
        std::unique_ptr<RowTable<FontCells>> fontTable;
        std::unique_ptr<RowTable<StyleCells>> styleTable;
        std::unique_ptr<RowTable<WidgetCells>> widgetTable;
        std::unique_ptr<UiDocumentView> view;
        std::vector<std::string> names; // the views, parallel to cells
        std::vector<bool> watching;
        std::vector<DocCells> cells;
        std::vector<Entity> keys;
        std::unordered_map<std::string, u32> keyIndex;
        std::vector<LayerCells> layers;
        std::vector<bool> layerTheme;
        std::vector<Action> actions;
        std::vector<DocCells> shownCells;
        std::string selectedView;
        std::string selectedLayer;
        std::string pickedId; // the picked element's id; empty: nothing picked
        std::string noteShown;
        std::string actionNote;
        std::string actionError;
        std::string theme;
        u64 seenCommand = 0;
        u64 seenLocale = 0;
        u64 refreshes = 0;
        u32 sinceRefresh = 0;
        bool wasShown = false;
        bool due = false;
        bool stale = false;
        bool noteDirty = true;
        bool stateDirty = true;
        bool pickRequested = false;
        bool gameDocuments = false;
        bool wasPlaying = false;
        Vec2 pickPoint{};

        [[nodiscard]] std::string message(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            std::string scratch;
            return localization ? std::string(localization->resolve(InternedString(key), args, scratch))
                                : std::string(key);
        }

        [[nodiscard]] Json ask(const char* kind, const char* name, Json args = Json::object())
        {
            args["world"] = std::string(UI_WORLD);
            return request(*control, Json{{kind, name}, {"args", std::move(args)}});
        }

        [[nodiscard]] static bool guarded(const std::string_view name)
        {
            return std::ranges::contains(GUARDED_VIEWS, name);
        }

        [[nodiscard]] bool hasView(const std::string& name) const { return std::ranges::contains(names, name); }

        [[nodiscard]] const LayerCells* layer(const std::string& name) const
        {
            const auto at = std::ranges::find(layers, name, &LayerCells::name);
            return at == layers.end() ? nullptr : &*at;
        }

        void fail(const std::string& text)
        {
            actionError = text;
            actionNote.clear();
            const MessageArg args[]{{"reason", std::string_view(text)}};
            feedback->postToastKey(ToastLevel::Error, "editor-uidocuments.toast.failed", args);
            noteDirty = true;
        }

        void succeed(const std::string_view key, const std::string_view what, const bool toast = true)
        {
            actionError.clear();
            const MessageArg args[]{{"what", what}};
            actionNote = message(key, args);
            if (toast)
            {
                feedback->postToastKey(ToastLevel::Info, key, args);
            }
            noteDirty = true;
        }

        // the reply's result when it succeeded; else the failure is shown and the object is empty
        Json run(const char* kind, const char* name, Json args = Json::object())
        {
            const Json reply = ask(kind, name, std::move(args));
            if (!reply.value("ok", false))
            {
                fail(reply.value("error", std::string("failed")));
                return Json::object();
            }
            return reply.value("result", Json::object());
        }

        void refresh()
        {
            SPITE_ZONE("EditorUiDocuments.refresh");
            ++refreshes;
            due = false;
            names.clear();
            watching.clear();
            cells.clear();
            keys.clear();
            const Json docs =
                request(*control,
                        Json{{"query", "uiDocuments"}, {"args", {{"world", gameDocuments ? "play" : UI_WORLD}}}})
                    .value("result", Json::object());
            for (const Json& entry : docs.value("views", Json::array()))
            {
                if (gameDocuments && (!entry.value("watching", false) || !entry.value("fromDocument", false)))
                {
                    continue;
                }
                std::string slots;
                for (const Json& element : entry.value("contract", Json::array()))
                {
                    slots += (slots.empty() ? "" : ", ") + element.value("id", std::string{});
                }
                const u64 problems = entry.value("problems", Json::array()).size();
                std::string path = fileText(entry.value("path", std::string{}));
                std::string state;
                if (problems != 0)
                {
                    const MessageArg args[]{{"count", static_cast<std::int64_t>(problems)}};
                    state = message("editor-uidocuments.state.problems", args);
                }
                else
                {
                    state = message(entry.value("watching", false) ? "editor-uidocuments.state.watching"
                                                                   : "editor-uidocuments.state.static");
                }
                const std::string name = entry.value("name", std::string{});
                const auto [it, added] = keyIndex.try_emplace(name, static_cast<u32>(keyIndex.size() + 1));
                keys.push_back(Entity(it->second, 1u));
                names.push_back(name);
                watching.push_back(entry.value("watching", false));
                cells.push_back({name, path.empty() ? message("editor-uidocuments.code") : std::move(path),
                                 std::move(slots), std::move(state)});
            }
            if (cells != shownCells)
            {
                shownCells = cells;
                list->setItems(static_cast<u32>(cells.size()));
            }
            if (!selectedView.empty() && !hasView(selectedView))
            {
                selectedView.clear();
            }
            if (gameDocuments)
            {
                refreshWidgets();
                seenLocale = localization ? localization->revision() : 0;
                noteDirty = stateDirty = true;
                return;
            }

            const Json sheet = ask("query", "uiLayers").value("result", Json::object());
            theme = sheet.value("theme", std::string{});
            layers.clear();
            layerTheme.clear();
            for (const Json& entry : sheet.value("layers", Json::array()))
            {
                const std::string name = entry.value("name", std::string{});
                const bool isTheme = entry.value("theme", false);
                layerTheme.push_back(isTheme);
                layers.push_back({name, fileText(entry.value("path", std::string{})), entry.value("classes", 0u),
                                  entry.value("kinds", 0u), entry.value("tokens", 0u), entry.value("elements", 0u),
                                  message(name == BASE_LAYER ? "editor-uidocuments.layer.base"
                                              : isTheme      ? "editor-uidocuments.layer.theme"
                                                             : "editor-uidocuments.layer.loaded")});
            }
            layerTable->set(layers);
            if (!selectedLayer.empty() && !layer(selectedLayer))
            {
                selectedLayer.clear();
            }

            std::vector<FontCells> fonts;
            for (const Json& entry :
                 ask("query", "uiFonts").value("result", Json::object()).value("fonts", Json::array()))
            {
                std::string fallbacks;
                for (const Json& each : entry.value("fallbacks", Json::array()))
                {
                    fallbacks += (fallbacks.empty() ? "" : ", ") + each.get<std::string>();
                }
                const Json glyphs = entry.value("glyphs", Json::object());
                fonts.push_back({entry.value("name", std::string{}), std::move(fallbacks), glyphs.value("ready", 0u),
                                 glyphs.value("pending", 0u), glyphs.value("failed", 0u)});
            }
            fontTable->set(std::move(fonts));
            repick = !pickedId.empty(); // read again once no command shares the frame (update)
            seenLocale = localization ? localization->revision() : 0;
            noteDirty = stateDirty = true;
        }

        void clearPick()
        {
            pickedId.clear();
            pickedEntity = Entity::undefined();
            highlight.setVisible(false);
            outlined = false;
            repick = false;
            styleTable->set({});
        }

        // The outline follows the picked element while the panel shows: hidden with the panel, moved when the layout
        // moved (a resize), dropped with the element.
        void syncOutline(const bool panelShown)
        {
            const UiHierarchyManager& hierarchy = scope.hierarchy();
            if (!panelShown || pickedId.empty() || !hierarchy.isElement(pickedEntity) ||
                !hierarchy.isVisibleOf(pickedEntity))
            {
                if (outlined)
                {
                    highlight.setVisible(false);
                    outlined = false;
                }
                return;
            }
            if (outlined && hierarchy.layoutGeneration() == seenLayout)
            {
                return;
            }
            const UiRect rect = hierarchy.absoluteRectOf(pickedEntity);
            place(highlight, rect.position, rect.size);
            if (!outlined)
            {
                highlight.setVisible(true);
                outlined = true;
            }
            seenLayout = hierarchy.layoutGeneration();
        }

        void pickElement(const Json& hit)
        {
            const u64 entity = hit.value("entity", u64{0});
            if (entity == 0)
            {
                clearPick();
                return;
            }
            const Json style = ask("query", "uiStyle", {{"entity", entity}}).value("result", Json::object());
            pickedId = hit.value("id", std::string{});
            pickedEntity = Entity(entity);
            std::vector<StyleCells> rows;
            const Json rect = hit.value("rect", Json::object());
            const std::string text = std::format("{:.0f}, {:.0f}  {:.0f} x {:.0f}", rect.value("x", 0.0f),
                                                 rect.value("y", 0.0f), rect.value("w", 0.0f), rect.value("h", 0.0f));
            rows.push_back({message("editor-uidocuments.pick.id"), pickedId, hit.value("path", std::string{})});
            rows.push_back({message("editor-uidocuments.pick.kind"), hit.value("kind", std::string{}), {}});
            rows.push_back({message("editor-uidocuments.pick.rect"), text, {}});
            rows.push_back({message("editor-uidocuments.pick.class"), style.value("styleClass", std::string{}), {}});
            for (const auto& [name, field] : style.value("fields", Json::object()).items())
            {
                std::string source = field.value("level", std::string{});
                if (field.contains("layer"))
                {
                    source += " / " + field.value("layer", std::string{});
                }
                if (field.contains("token"))
                {
                    source += " $" + field.value("token", std::string{});
                }
                rows.push_back({name, field.value("value", Json{}).dump(), std::move(source)});
            }
            styleTable->set(std::move(rows));
            place(highlight, Vec2{rect.value("x", 0.0f), rect.value("y", 0.0f)},
                  Vec2{rect.value("w", 0.0f), rect.value("h", 0.0f)});
            syncOutline(true);
        }

        // the element id's own rect, class and style, or a note when no such element lives in the editor's UI
        void pickById(const std::string& id, const bool announce)
        {
            const Json tree = ask("query", "uiTree", {{"root", id}, {"depth", 0}});
            const Json nodes = tree.value("result", Json::object()).value("tree", Json::array());
            if (!tree.value("ok", false) || nodes.empty())
            {
                clearPick();
                if (announce)
                {
                    fail(tree.value("error", message("editor-uidocuments.pick.none")));
                }
                return;
            }
            Json hit = nodes[0];
            hit["path"] = id;
            pickElement(hit);
            if (announce)
            {
                succeed("editor-uidocuments.pick.done", pickedId, false);
            }
        }

        UiDocumentView* gameView(const std::string& name) const
        {
            PlayWorld* play = engine->playWorld();
            return play ? play->ui().documents().find(name) : nullptr;
        }

        void refreshWidgets()
        {
            std::string selectedKey;
            const auto previous = std::ranges::find(widgetPaths, selectedWidget);
            if (previous != widgetPaths.end())
            {
                selectedKey = widgetKeys[previous - widgetPaths.begin()];
            }
            std::vector<WidgetCells> rows;
            widgetPaths.clear();
            widgetIds.clear();
            widgetKeys.clear();
            widgetDocument.clear();
            if (UiDocumentView* target = gameView(selectedView))
            {
                widgetDocument = target->documentText();
                const Json doc = Json::parse(widgetDocument, nullptr, false);
                if (doc.is_object() && doc.contains("root"))
                {
                    const auto visit = [&](const auto& self, const Json& node, const std::string& path,
                                           const std::string& parentKey, u32 depth) -> void
                    {
                        if (!node.is_object())
                        {
                            return;
                        }
                        const std::string id = node.value("id", std::string{});
                        const std::string key = parentKey + std::to_string(id.size()) + "/" + id;
                        widgetPaths.push_back(path);
                        widgetIds.push_back(id);
                        widgetKeys.push_back(key);
                        rows.push_back({std::string(depth * 2, ' ') + id, node.value("kind", std::string{})});
                        if (node.contains("children") && node["children"].is_array())
                        {
                            for (sizet n = 0; n < node["children"].size(); ++n)
                            {
                                self(self, node["children"][n], path + "/children/" + std::to_string(n), key,
                                     depth + 1);
                            }
                        }
                    };
                    visit(visit, doc["root"], "/root", {}, 0);
                }
            }
            widgetTable->set(std::move(rows));
            const auto selected = std::ranges::find(widgetKeys, selectedKey);
            if (selected == widgetKeys.end())
            {
                selectedWidget.clear();
                widgetTable->table().setSelection({});
                return;
            }
            const u32 index = static_cast<u32>(selected - widgetKeys.begin());
            selectedWidget = widgetPaths[index];
            const Entity key(index + 1, 1u);
            widgetTable->table().setSelection(std::span(&key, 1));
        }

        void editSpacing(const bool textEdit = false)
        {
            const std::string name = selectedView;
            UiDocumentView* target = gameView(name);
            if (!target || !target->watching() || target->path().empty() || target->documentText().empty())
            {
                fail(message("editor-uidocuments.game.noDocument"));
                return;
            }
            const auto path = target->path();
            const std::string original = target->documentText();
            if (!selectedWidget.empty() && widgetDocument != original)
            {
                selectedWidget.clear();
                fail(message("editor-uidocuments.game.changed"));
                return;
            }
            auto document = nlohmann::ordered_json::parse(original, nullptr, false);
            if (!document.is_object() || !document.contains("root") || !document["root"].is_object())
            {
                fail(message("editor-uidocuments.game.noDocument"));
                return;
            }
            const std::string widget = selectedWidget == "/root" ? std::string{} : selectedWidget;
            const nlohmann::ordered_json::json_pointer pointer(widget.empty() ? "/root" : widget);
            if (!document.contains(pointer))
            {
                fail(message("editor-uidocuments.game.changed"));
                return;
            }
            auto& node = document[pointer];
            const auto layout = node.value("layout", nlohmann::ordered_json::object());
            if (widget.empty() && (!layout.is_object() || (layout.contains("gap") && !layout["gap"].is_number())))
            {
                fail(message("editor-uidocuments.game.noSpacing"));
                return;
            }
            if (textEdit &&
                (widget.empty() || !node.contains("text") || !node["text"].is_string() || node.contains("textKey")))
            {
                fail(message("editor-uidocuments.game.noText"));
                return;
            }
            if (!textEdit && !widget.empty() &&
                (!node.contains("size") || !node["size"].is_array() || node["size"].size() != 2 ||
                 !node["size"][0].is_number()))
            {
                fail(message("editor-uidocuments.game.noWidth"));
                return;
            }
            EditorPrompt prompt;
            prompt.title = message(
                textEdit ? "editor-uidocuments.game.text"
                         : (widget.empty() ? "editor-uidocuments.game.spacing" : "editor-uidocuments.game.width"));
            const MessageArg args[]{{"what", std::string_view(name)}};
            prompt.body = message(
                textEdit ? "editor-uidocuments.game.textPrompt"
                         : (widget.empty() ? "editor-uidocuments.game.prompt" : "editor-uidocuments.game.widthPrompt"),
                args);
            prompt.value = textEdit
                ? node["text"].get<std::string>()
                : (widget.empty() ? layout.value("gap", nlohmann::ordered_json(0)).dump() : node["size"][0].dump());
            prompt.confirmLabel = message("editor-uidocuments.game.save");
            dialogs->prompt(
                std::move(prompt),
                [this, name, path, original, widget, pointer, textEdit,
                 document = std::move(document)](std::optional<std::string> text) mutable
                {
                    if (!text)
                    {
                        return;
                    }
                    float gap = 0.0f;
                    const char* end = text->data() + text->size();
                    const auto parsed = std::from_chars(text->data(), end, gap);
                    if (!textEdit &&
                        (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(gap) || gap < 0.0f))
                    {
                        fail(message("editor-uidocuments.game.invalidSpacing"));
                        return;
                    }
                    const UiDocumentView* current = gameView(name);
                    std::string onDisk, why;
                    if (!current || current->path() != path || current->documentText() != original ||
                        !readUiDocumentFile(path, onDisk, why) || onDisk != original)
                    {
                        fail(message("editor-uidocuments.game.changed"));
                        return;
                    }
                    if (textEdit)
                    {
                        document[pointer]["text"] = *text;
                    }
                    else if (widget.empty())
                    {
                        document[pointer]["layout"]["gap"] = gap;
                    }
                    else
                    {
                        document[pointer]["size"][0] = gap;
                    }
                    if (const auto written = writeTextFileAtomically(path, document.dump(2) + "\n"); !written)
                    {
                        fail(message("editor-uidocuments.game.writeFailed"));
                        return;
                    }
                    succeed("editor-uidocuments.game.saved", fileText(utf8Path(path)));
                });
        }

        void perform(const Action& action)
        {
            if (gameDocuments && action.act != Act::GameDocuments && action.act != Act::EditSpacing &&
                action.act != Act::EditText && action.act != Act::Preview && action.act != Act::OpenGameEditor)
            {
                fail(message("editor-uidocuments.game.editorAction"));
                return;
            }
            switch (action.act)
            {
            case Act::Reload:
                {
                    if (run("command", "uiImport", {{"view", selectedView}}).empty())
                    {
                        return;
                    }
                    succeed("editor-uidocuments.done.reloaded", selectedView);
                    break;
                }
            case Act::Export:
                if (run("command", "uiExport",
                        {{"view", selectedView}, {"path", action.text}, {"overwrite", action.confirmed}})
                        .empty())
                {
                    return;
                }
                succeed("editor-uidocuments.done.exported", action.text);
                break;
            case Act::Import:
                if (run("command", "uiImport", {{"view", selectedView}, {"path", action.text}}).empty())
                {
                    return;
                }
                succeed("editor-uidocuments.done.imported", action.text);
                break;
            case Act::LoadLayer:
                {
                    const std::string name = utf8Path(pathFromUtf8(action.text).stem());
                    if (run("command", "loadLayer", {{"name", name}, {"path", action.text}}).empty())
                    {
                        return;
                    }
                    selectedLayer = name;
                    succeed("editor-uidocuments.done.loaded", name);
                    break;
                }
            case Act::UnloadLayer:
                if (run("command", "unloadLayer", {{"name", action.text}}).empty())
                {
                    return;
                }
                succeed("editor-uidocuments.done.unloaded", action.text);
                break;
            case Act::PickPoint:
                {
                    const Json hit = ask("query", "uiHit", {{"x", action.point.x}, {"y", action.point.y}});
                    const Json result = hit.value("result", Json::object());
                    if (!hit.value("ok", false) || result.value("entity", u64{0}) == 0)
                    {
                        pickElement(Json::object());
                        fail(hit.value("error", message("editor-uidocuments.pick.none")));
                        return;
                    }
                    pickElement(result);
                    page = 1;
                    stateDirty = true;
                    succeed("editor-uidocuments.pick.done", pickedId, false);
                    return;
                }
            case Act::PickId:
                pickById(action.text, true);
                if (!pickedId.empty())
                {
                    page = 1;
                    stateDirty = true;
                }
                return;
            case Act::Page:
                page = action.text == "style" ? 1u : 0u;
                stateDirty = true;
                return;
            case Act::GameDocuments:
                gameDocuments = !gameDocuments;
                page = 0;
                selectedView.clear();
                selectedWidget.clear();
                list->table().setSelection({});
                clearPick();
                actionError.clear();
                actionNote.clear();
                refresh();
                return;
            case Act::EditSpacing:
                if (gameDocuments)
                {
                    editSpacing();
                }
                return;
            case Act::Preview:
                {
                    const Json reply = request(*control, Json{{"command", "showGameUiPreview"}});
                    if (!reply.value("ok", false))
                    {
                        fail(reply.value("error", std::string("Could not show the Game panel")));
                    }
                }
                return;
            case Act::OpenGameEditor:
                if (gameDocuments)
                {
                    const Json reply = selectedView.empty()
                        ? request(*control, Json{{"command", "showGameUiEditor"}})
                        : request(*control, Json{{"command", "openGameUiEditor"}, {"args", {{"name", selectedView}}}});
                    if (!reply.value("ok", false))
                    {
                        fail(reply.value("error", message("editor-uidocuments.game.noDocument")));
                    }
                }
                return;
            case Act::EditText:
                if (gameDocuments)
                    editSpacing(true);
                return;
            }
            due = true;
        }

        // whether the action may run now; a guarded import asks first, a refused unload says why
        void submit(Action action)
        {
            if (action.act == Act::Import && guarded(selectedView) && !action.confirmed)
            {
                const MessageArg args[]{{"view", std::string_view(selectedView)}};
                EditorConfirm ask;
                ask.title = message("editor-uidocuments.confirm.title");
                ask.body = message("editor-uidocuments.confirm.body", args);
                ask.confirmLabel = message("editor-uidocuments.import");
                dialogs->confirm(std::move(ask),
                                 [this, action](const bool confirmed) mutable
                                 {
                                     if (confirmed)
                                     {
                                         action.confirmed = true;
                                         actions.push_back(std::move(action));
                                     }
                                 });
                return;
            }
            actions.push_back(std::move(action));
        }

        void chooseFile(const bool save, const Act act)
        {
            EditorPick pick;
            pick.mode = save ? EditorPickMode::SaveFile : EditorPickMode::OpenFile;
            pick.title = message(save ? "editor-uidocuments.export.title" : "editor-uidocuments.import.title");
            pick.roots = {"ui"};
            pick.root = "ui";
            pick.directory = "editor";
            pick.extensions = "json";
            if (save)
            {
                pick.name = selectedView + ".json";
            }
            dialogs->pickFile(std::move(pick),
                              [this, act](const std::optional<EditorPicked>& picked)
                              {
                                  if (picked)
                                  {
                                      submit(
                                          {act, picked->path, act == Act::Export}); // the save dialog named the replace
                                  }
                              });
        }

        void showState()
        {
            stateDirty = false;
            const bool docs = !gameDocuments && !selectedView.empty();
            spacingButton.setVisible(gameDocuments);
            previewButton.setVisible(gameDocuments);
            openGameEditorButton.setVisible(gameDocuments);
            textButton.setVisible(gameDocuments);
            const bool leafSelected = !selectedWidget.empty() && selectedWidget != "/root";
            textButton.setInteractable(gameDocuments && leafSelected);
            previewButton.setInteractable(gameDocuments && wasPlaying);
            openGameEditorButton.setInteractable(gameDocuments);
            spacingButton.setInteractable(gameDocuments && !selectedView.empty());
            spacingButton.setLocalizedText(
                InternedString(leafSelected ? "editor-uidocuments.game.width" : "editor-uidocuments.game.spacing"));
            spacingButton.setTooltip(InternedString(leafSelected ? "editor-uidocuments.game.width.tip"
                                                                 : "editor-uidocuments.game.spacing.tip"));
            styleTable->table().setVisible(!gameDocuments);
            widgetTable->table().setVisible(gameDocuments);
            gameButton.setLocalizedText(
                InternedString(gameDocuments ? "editor-uidocuments.game.editor" : "editor-uidocuments.game.play"));
            for (Button* button : {&loadButton, &unloadButton, &pickButton, &pageButton})
            {
                button->setVisible(!gameDocuments);
            }
            for (Button* button : {&reloadButton, &exportButton, &importButton})
            {
                if (button->isInteractable() != docs)
                {
                    button->setInteractable(docs);
                }
            }
            // two pages: the three lists, or the picked element's style with the whole height; written again only
            // when the page or the document (which may carry its own visibility) changed
            const u64 applies = view ? view->stats().applies : 0;
            const u32 visiblePage = gameDocuments ? 2u : page;
            if (appliedPage != visiblePage || appliedDocument != applies)
            {
                appliedPage = visiblePage;
                appliedDocument = applies;
                for (const auto& [slot, visible] :
                     {std::pair{&slotRows, visiblePage != 1}, std::pair{&slotLayers, visiblePage == 0},
                      std::pair{&slotFonts, visiblePage == 0},
                      std::pair{&slotPick, visiblePage == 1 || visiblePage == 2}})
                {
                    if (slot->isValid())
                    {
                        slot->setVisible(visible);
                    }
                }
                pageButton.setLocalizedText(
                    InternedString(page == 0 ? "editor-uidocuments.page.style" : "editor-uidocuments.page.documents"));
            }
            const LayerCells* selected = selectedLayer.empty() ? nullptr : layer(selectedLayer);
            const bool removable =
                selected && selected->name != BASE_LAYER && !layerTheme[static_cast<u32>(selected - layers.data())];
            if (unloadButton.isInteractable() != removable)
            {
                unloadButton.setInteractable(removable);
            }
        }

        void showNote()
        {
            noteDirty = false;
            std::string text = !actionError.empty() ? actionError : actionNote;
            if (text.empty())
            {
                const MessageArg args[]{{"documents", static_cast<std::int64_t>(cells.size())},
                                        {"layers", static_cast<std::int64_t>(layers.size())},
                                        {"theme", std::string_view(theme)}};
                text = message(gameDocuments
                                   ? (wasPlaying ? "editor-uidocuments.game.note" : "editor-uidocuments.game.startPlay")
                                   : "editor-uidocuments.note",
                               args);
            }
            if (text != noteShown)
            {
                noteShown = std::move(text);
                note.setText(noteShown);
            }
        }

        void registerCommands()
        {
            control->registerViewCommand("uidocumentsSelect", "editorUiDocuments",
                                         "selects a document view in the UI documents panel",
                                         {requiredArg("view", ArgKind::String, "a view name, as uiDocuments lists it")},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         {
                                             const std::string name = args.string("view").value_or("");
                                             const auto at = std::ranges::find(names, name);
                                             if (at == names.end())
                                             {
                                                 std::string known;
                                                 for (const std::string& each : names)
                                                 {
                                                     known += (known.empty() ? "" : ", ") + each;
                                                 }
                                                 return "no document view '" + name + "'; views: " + known;
                                             }
                                             const Entity key = keys[static_cast<u32>(at - names.begin())];
                                             list->table().setSelection(std::span(&key, 1));
                                             selectedView = name;
                                             stateDirty = true;
                                             return std::string{};
                                         });
            control->registerViewCommand("uidocumentsWidget", "editorUiDocuments",
                                         "Select a game document widget by id",
                                         {requiredArg("id", ArgKind::String, "document widget id")},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         {
                                             if (!gameDocuments)
                                             {
                                                 return std::string("select Game UI first");
                                             }
                                             refreshWidgets();
                                             const std::string id = args.string("id").value_or("");
                                             const auto found = std::ranges::find(widgetIds, id);
                                             if (found == widgetIds.end())
                                             {
                                                 return std::string("no such widget");
                                             }
                                             if (std::ranges::count(widgetIds, id) != 1)
                                             {
                                                 return std::string("widget id is ambiguous; select its row");
                                             }
                                             const u32 row = static_cast<u32>(found - widgetIds.begin());
                                             const Entity key(row + 1, 1u);
                                             widgetTable->table().setSelection(std::span(&key, 1));
                                             selectedWidget = widgetPaths[row];
                                             stateDirty = true;
                                             return std::string{};
                                         });
            control->registerViewCommand("uidocumentsLayer", "editorUiDocuments",
                                         "selects a stylesheet layer in the UI documents panel",
                                         {requiredArg("name", ArgKind::String, "a layer name, as uiLayers lists it")},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         {
                                             const std::string name = args.string("name").value_or("");
                                             const auto at = std::ranges::find(layers, name, &LayerCells::name);
                                             if (at == layers.end())
                                             {
                                                 return "no stylesheet layer '" + name + "'";
                                             }
                                             const Entity key(static_cast<u32>(at - layers.begin()) + 1u, 1u);
                                             layerTable->table().setSelection(std::span(&key, 1));
                                             selectedLayer = name;
                                             stateDirty = true;
                                             return std::string{};
                                         });
            control->registerViewCommand("uidocumentsReload", "editorUiDocuments",
                                         "re-reads the selected view's file (uiImport)", {},
                                         [this](const ControlArgs&, IFormatWriter&)
                                         {
                                             if (selectedView.empty())
                                             {
                                                 return std::string("no document view selected");
                                             }
                                             actions.push_back({Act::Reload, {}});
                                             return std::string{};
                                         });
            control->registerViewCommand(
                "uidocumentsExport", "editorUiDocuments",
                "writes the selected view as a document under assets/ui (uiExport); an existing file is replaced only "
                "with overwrite=true. The reply is at once: the result is the panel note and an error toast",
                {requiredPath("path", "ui", "a .json file under assets/ui"),
                 optionalArg("overwrite", ArgKind::Boolean, "replace an existing file", "false")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    if (selectedView.empty())
                    {
                        return std::string("no document view selected");
                    }
                    actions.push_back(
                        {Act::Export, args.string("path").value_or(""), args.boolean("overwrite").value_or(false)});
                    return std::string{};
                });
            control->registerViewCommand(
                "uidocumentsImport", "editorUiDocuments",
                "applies a document file under assets/ui to the selected view (uiImport, undoable); a document the "
                "editor cannot run without needs force=true (only this panel path is guarded: the raw uiImport is "
                "atomic and "
                "undoable). The reply is at once: the result is the panel note and an error toast",
                {requiredPath("path", "ui", "a .json file under assets/ui"),
                 optionalArg("force", ArgKind::Boolean, "imports into a guarded editor document", "false")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    if (selectedView.empty())
                    {
                        return std::string("no document view selected");
                    }
                    const bool confirmed = args.boolean("force").value_or(false);
                    if (guarded(selectedView) && !confirmed)
                    {
                        return "'" + selectedView + "' is a document the editor runs on: pass force=true";
                    }
                    actions.push_back({Act::Import, args.string("path").value_or(""), confirmed});
                    return std::string{};
                });
            control->registerViewCommand(
                "uidocumentsLoadLayer", "editorUiDocuments",
                "loads a stylesheet file under assets/ui as a layer named after the file (loadLayer)",
                {requiredPath("path", "ui", "a stylesheet .json file under assets/ui")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    actions.push_back({Act::LoadLayer, args.string("path").value_or("")});
                    return std::string{};
                });
            control->registerViewCommand("uidocumentsUnloadLayer", "editorUiDocuments",
                                         "removes the selected or named stylesheet layer (unloadLayer: the base sheet "
                                         "and the theme layers stay)",
                                         {optionalArg("name", ArgKind::String, "a layer name", "the selected layer")},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         {
                                             const std::string name = args.string("name").value_or(selectedLayer);
                                             const LayerCells* found = layer(name);
                                             if (!found)
                                             {
                                                 return name.empty() ? std::string("no layer selected")
                                                                     : "no stylesheet layer '" + name + "'";
                                             }
                                             actions.push_back({Act::UnloadLayer, name});
                                             return std::string{};
                                         });
            control->registerViewCommand(
                "uidocumentsPage", "editorUiDocuments",
                "shows the documents, layers and fonts, or the picked element's style (a pick shows the style)",
                {requiredArg("page", ArgKind::String, "documents or style", PAGES)},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const std::string name = args.string("page").value_or("");
                    if (!std::ranges::contains(PAGES, name))
                    {
                        return "page is documents or style, not '" + name + "'";
                    }
                    actions.push_back({Act::Page, name});
                    return std::string{};
                });
            control->registerViewCommand(
                "uidocumentsPick", "editorUiDocuments",
                "picks an element of the editor's UI by its UI-pixel point, or by element id: its id, rect, class "
                "and resolved style show in the style table and an overlay outlines it",
                {optionalArg("x", ArgKind::Number, "UI pixels from the left", "use id"),
                 optionalArg("y", ArgKind::Number, "UI pixels from the top", "use id"),
                 optionalArg("id", ArgKind::String, "an element id or id path", "use x and y")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    if (const std::optional<std::string> id = args.string("id"))
                    {
                        actions.push_back({Act::PickId, *id});
                        return std::string{};
                    }
                    const auto number = [&](const char* key)
                    {
                        const std::optional<std::string> text = args.json(key);
                        const Json value = text ? Json::parse(*text, nullptr, false) : Json();
                        return value.is_number() ? std::optional<float>(value.get<float>()) : std::nullopt;
                    };
                    const std::optional<float> x = number("x");
                    const std::optional<float> y = number("y");
                    if (!x || !y)
                    {
                        return std::string("name an element id, or x and y");
                    }
                    actions.push_back({Act::PickPoint, {}, false, Vec2{*x, *y}});
                    return std::string{};
                });
            control->registerQuery(
                "uiDocumentsView", "editorUiDocuments",
                "the UI documents panel: documents (view, file, slots, state), layers, fonts, the selected view and "
                "layer, the picked element and its style rows, the highlight, the note and the refresh counter",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    const auto table = [&](const char* name, const auto& rows, const StructSchema& schema)
                    {
                        out.beginArray(name, static_cast<u32>(rows.size()));
                        for (const auto& row : rows)
                        {
                            out.beginObject("");
                            writeValue(schema, &row, out, FieldSet::All);
                            out.endObject();
                        }
                        out.endArray();
                    };
                    table("documents", cells, DocCells::spiteSchema());
                    table("layers", layerTable->rows(), LayerCells::spiteSchema());
                    table("fonts", fontTable->rows(), FontCells::spiteSchema());
                    table("style", styleTable->rows(), StyleCells::spiteSchema());
                    out.writeString("selectedView", selectedView);
                    out.writeString("selectedLayer", selectedLayer);
                    out.writeString("picked", pickedId);
                    out.writeString("page", PAGES[page]);
                    out.writeBool("highlight",
                                  highlight.isValid() && scope.hierarchy().isVisibleOf(highlight.entity()));
                    out.writeString("note", noteShown);
                    out.writeBool("error", !actionError.empty());
                    out.writeU64("refreshes", refreshes);
                    list->writeView(out);
                    return std::string{};
                });
        }
    };

    EditorUiDocuments::EditorUiDocuments(Engine& engine, EditorDialogs& dialogs, EditorFeedback& feedback) :
        m_state(std::make_unique<State>(engine, dialogs, feedback))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.root = scope.createPanel("editor.panel", columnDesc());
        state.scroll =
            PageScroll(scope, InternedString("uidocs.scroll"), InternedString("editor-uidocuments.title"), state.root);
        // the picked element's style table keeps its header and STYLE_ROWS rows; the page scrolls rather than squeeze
        // it
        state.scroll.setLeast([&state](const Entity item, const Density& d)
                              { return item == state.slotPick.entity() ? d.control + STYLE_ROWS * d.row : 0.0f; });
        const auto button = [&](Button& target, const char* id, const char* key, std::function<void()> click)
        {
            target = state.density.track(scope.createButton(id, "", fitWidthDesc()), HEIGHT_CONTROL);
            bindFitPadding(target, SPACE_MD, SPACE_NONE);
            target.setLocalizedText(InternedString(key));
            target.setTooltip(InternedString(std::string(key) + ".tip"));
            target.setInteractable(false);
            target.onClick(std::move(click));
        };
        button(state.reloadButton, "uidocs.reload", "editor-uidocuments.reload",
               [&state] { state.submit({Act::Reload, {}}); });
        button(state.exportButton, "uidocs.export", "editor-uidocuments.export",
               [&state] { state.chooseFile(true, Act::Export); });
        button(state.importButton, "uidocs.import", "editor-uidocuments.import",
               [&state] { state.chooseFile(false, Act::Import); });
        button(state.loadButton, "uidocs.loadLayer", "editor-uidocuments.loadLayer",
               [&state] { state.chooseFile(false, Act::LoadLayer); });
        state.loadButton.setAccessibleNameKey(InternedString("editor-uidocuments.loadLayer.name"));
        state.loadButton.setInteractable(true);
        button(state.unloadButton, "uidocs.unloadLayer", "editor-uidocuments.unloadLayer",
               [&state] { state.submit({Act::UnloadLayer, state.selectedLayer}); });
        state.unloadButton.setAccessibleNameKey(InternedString("editor-uidocuments.unloadLayer.name"));
        button(state.pickButton, "uidocs.pickButton", "editor-uidocuments.pickElement",
               [&state]
               {
                   state.pickId.setText("");
                   state.pickModal.open(state.root);
               });
        state.pickButton.setInteractable(true);
        button(state.pageButton, "uidocs.page", "editor-uidocuments.page.style",
               [&state] { state.actions.push_back({Act::Page, state.page == 0 ? "style" : "documents"}); });
        state.pageButton.setInteractable(true);
        button(state.gameButton, "uidocs.game", "editor-uidocuments.game.play",
               [&state] { state.actions.push_back({Act::GameDocuments, {}}); });
        state.gameButton.setInteractable(true);
        button(state.spacingButton, "uidocs.spacing", "editor-uidocuments.game.spacing",
               [&state] { state.actions.push_back({Act::EditSpacing, {}}); });
        state.spacingButton.setVisible(false);
        button(state.textButton, "uidocs.text", "editor-uidocuments.game.text",
               [&state] { state.actions.push_back({Act::EditText, {}}); });
        state.textButton.setVisible(false);
        button(state.previewButton, "uidocs.preview", "editor-uidocuments.game.preview",
               [&state] { state.actions.push_back({Act::Preview, {}}); });
        state.previewButton.setVisible(false);
        button(state.openGameEditorButton, "uidocs.openGameEditor", "editor-uidocuments.game.openEditor",
               [&state] { state.actions.push_back({Act::OpenGameEditor, {}}); });
        state.openGameEditorButton.setVisible(false);
        UiRectDesc noteDesc = barDesc(Vec2{});
        noteDesc.layoutItem = fillItem();
        noteDesc.layoutItem->alignSelf = UiLayoutAlign::Stretch; // as high as the status row, whatever its size.row
        state.note = scope.createLabel("uidocs.note", "", noteDesc);
        state.note.setStyleClass(InternedString("uidocs.note"));

        state.pickModal = scope.createModal("uidocs.pickModal", {.padding = 12.0f});
        state.pickModal.onPress(
            [&state](const u8 buttonIndex, const Vec2 position)
            {
                if (buttonIndex == 0)
                {
                    state.pickModal.close();
                    state.actions.push_back({Act::PickPoint, {}, false, position});
                }
            });
        UiRectDesc hintDesc = topLeft({}, Vec2{360.0f, 0.0f});
        hintDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        state.pickHint = scope.createLabel("uidocs.pickHint", "", hintDesc);
        state.pickHint.setLocalizedText(InternedString("editor-uidocuments.pick.hint"));
        state.pickHint.setParent(state.pickModal.content());
        state.pickId =
            state.density.track(scope.createTextInput("uidocs.pickId", topLeft({}, Vec2{360.0f, 0.0f})), HEIGHT_ROW);
        state.pickId.setStyleClass(InternedString("tool.field"));
        state.pickId.setAccessibleNameKey(InternedString("editor-uidocuments.pick.byId"));
        state.pickId.setParent(state.pickModal.content());
        state.pickId.onSubmit(
            [&state](const std::string_view text)
            {
                if (!text.empty())
                {
                    state.pickModal.close();
                    state.actions.push_back({Act::PickId, std::string(text)});
                }
            });
        state.highlight = scope.createPanel("uidocs.highlight", topLeft());
        state.highlight.setStyleClass(InternedString("uidocs.highlight"));
        state.highlight.setInteractable(false);
        state.highlight.setVisible(false);

        state.list = std::make_unique<FilteredTable>(
            scope, *state.control,
            FilteredTableDesc{
                .id = "uidocs",
                .owner = "editorUiDocuments",
                .nameKey = InternedString("editor-uidocuments.table"),
                .schema = &DocCells::spiteSchema(),
                .columns = {column("column", "view", 180.0f), column("column", "file", 200.0f),
                            column("column", "slots", 240.0f), column("column", "state", 120.0f)},
                .row = [&state](const u32 item) -> const void* { return &state.cells[item]; },
                .key = [&state](const u32 item) { return state.keys[item]; },
                .matches =
                    [&state](const u32 item, const std::string_view needle)
                {
                    const DocCells& row = state.cells[item];
                    return containsIgnoringCase(row.view, needle) || containsIgnoringCase(row.file, needle) ||
                        containsIgnoringCase(row.slots, needle);
                },
                .headerHints = {{InternedString("slots"), InternedString("editor-uidocuments.hint.slots")}}});
        state.list->table().onSelect(
            [&state](const std::span<const Entity> picked)
            {
                const auto at = picked.empty() ? state.keys.end() : std::ranges::find(state.keys, picked[0]);
                state.selectedView = at == state.keys.end() ? std::string{} : state.names[at - state.keys.begin()];
                state.selectedWidget.clear();
                if (state.gameDocuments)
                    state.refreshWidgets();
                state.stateDirty = true;
            });
        state.layerTable = std::make_unique<RowTable<LayerCells>>(
            scope, "uidocs.layers.table", InternedString("editor-uidocuments.table.layers"),
            std::vector{column("layer", "name", 120.0f), column("layer", "file", 180.0f),
                        column("layer", "classes", 104.0f), column("layer", "kinds", 92.0f),
                        column("layer", "tokens", 104.0f), column("layer", "elements", 124.0f),
                        column("layer", "state", 110.0f)});
        state.layerTable->table().onSelect(
            [&state](const std::span<const Entity> picked)
            {
                const u32 row = picked.empty() ? NO_ROW : picked[0].id() - 1u;
                state.selectedLayer = row < state.layers.size() ? state.layers[row].name : std::string{};
                state.stateDirty = true;
            });
        state.fontTable = std::make_unique<RowTable<FontCells>>(
            scope, "uidocs.fonts.table", InternedString("editor-uidocuments.table.fonts"),
            std::vector{column("font", "font", 160.0f), column("font", "fallbacks", 240.0f),
                        column("font", "ready", 92.0f), column("font", "pending", 112.0f),
                        column("font", "failed", 112.0f)});
        state.styleTable = std::make_unique<RowTable<StyleCells>>(
            scope, "uidocs.pick.table", InternedString("editor-uidocuments.table.style"),
            std::vector{column("style", "field", 150.0f), column("style", "value", 240.0f),
                        column("style", "source", 240.0f)});
        state.widgetTable = std::make_unique<RowTable<WidgetCells>>(
            scope, "uidocs.widgets", InternedString("editor-uidocuments.game.hierarchy"),
            std::vector{column("game.column", "widget", 250.0f), column("game.column", "kind", 110.0f)});
        state.widgetTable->table().setVisible(false);
        state.widgetTable->table().onSelect(
            [&state](const std::span<const Entity> picked)
            {
                const u32 row = picked.empty() ? NO_ROW : picked[0].id() - 1u;
                state.selectedWidget = row < state.widgetPaths.size() ? state.widgetPaths[row] : std::string{};
                state.stateDirty = true;
            });
        state.registerCommands();
        scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(DOCS_BAR),
                      [&state](const Widget& slot)
                      {
                          for (Button* each : {&state.reloadButton, &state.exportButton, &state.importButton})
                          {
                              putIn(state.scope, slot, *each);
                          }
                      });
        contract.slot(InternedString(DOCS_FILTER), [&state](const Widget& slot) { state.list->fillFilter(slot); });
        contract.slot(InternedString(DOCS_ROWS),
                      [&state](const Widget& slot)
                      {
                          state.list->fillRows(slot);
                          state.slotRows = slot;
                      });
        contract.slot(InternedString(DOCS_LAYERBAR),
                      [&state](const Widget& slot)
                      {
                          for (Button* each : {&state.loadButton, &state.unloadButton, &state.pickButton,
                                               &state.pageButton, &state.gameButton, &state.spacingButton,
                                               &state.textButton, &state.previewButton, &state.openGameEditorButton})
                          {
                              putIn(state.scope, slot, *each);
                          }
                      });
        contract.slot(InternedString(DOCS_LAYERS),
                      [&state](const Widget& slot)
                      {
                          state.layerTable->fill(state.scope, slot);
                          state.slotLayers = slot;
                      });
        contract.slot(InternedString(DOCS_FONTS),
                      [&state](const Widget& slot)
                      {
                          state.fontTable->fill(state.scope, slot);
                          state.slotFonts = slot;
                      });
        contract.slot(InternedString(DOCS_PICK),
                      [&state](const Widget& slot)
                      {
                          state.styleTable->fill(state.scope, slot);
                          state.widgetTable->fill(state.scope, slot);
                          state.slotPick = slot;
                      });
        contract.slot(InternedString(DOCS_STATUS),
                      [&state](const Widget& slot) { putIn(state.scope, slot, state.note); });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                const auto row = [&](const std::string_view id) { return toolbarPanel(viewScope, id, root); };
                Panel toolbar = row(DOCS_TOOLBAR);
                fitBarPanel(viewScope, DOCS_BAR, toolbar);
                filterSlotPanel(viewScope, DOCS_FILTER, toolbar);
                const auto share = [&](const std::string_view id, const float grow, const float minHeight = 0.0f)
                {
                    Panel panel = fillSlotPanel(viewScope, id, root);
                    UiLayoutItemComponent item = fillItem();
                    item.grow = grow;
                    item.minSize.y = minHeight;
                    panel.setLayoutItem(item);
                };
                share(DOCS_ROWS, 2.0f, TABLE_SLOT_MIN);
                wrapRowPanel(viewScope, DOCS_LAYERBAR, root);

                share(DOCS_LAYERS, 1.0f, TABLE_SLOT_MIN);
                share(DOCS_FONTS, 1.0f, TABLE_SLOT_MIN);
                share(DOCS_PICK, 3.0f);
                statusPanel(viewScope, DOCS_STATUS, root);
            });
        state.view =
            openView(engine, DOCS_VIEW, "assets/ui/editor/uidocuments.json", std::move(contract), state.scroll.page());
    }

    EditorUiDocuments::~EditorUiDocuments() { m_state->control->unregisterOwner("editorUiDocuments"); }

    Widget EditorUiDocuments::content() const { return m_state->root; }

    void EditorUiDocuments::update()
    {
        State& state = *m_state;
        state.list->update();
        if (!shown(state.scope, state.root))
        {
            state.wasShown = false;
            state.syncOutline(false);
            return;
        }
        const bool playing = state.engine->playWorld() != nullptr;
        if (state.wasPlaying != playing)
        {
            state.wasPlaying = playing;
            state.due |= state.gameDocuments;
        }
        if (state.gameDocuments)
        {
            const UiDocumentView* target = state.gameView(state.selectedView);
            if (target && state.widgetDocument != target->documentText())
            {
                state.refreshWidgets();
                state.stateDirty = true;
            }
        }
        if (!state.actions.empty())
        {
            std::vector<Action> actions;
            actions.swap(state.actions);
            for (const Action& action : actions)
            {
                state.perform(action);
            }
            state.stale = true;
        }
        const u64 command = state.control->commandGeneration();
        // a command's effect is read once commands paused for a moment (an import and its undo are two commands),
        // and at most every COMMAND_REFRESH_FRAMES while they keep coming
        if (command != state.seenCommand)
        {
            state.seenCommand = command;
            state.stale = true;
            state.quiet = 0;
        }
        else
        {
            ++state.quiet;
        }
        ++state.sinceRefresh;
        if (!state.wasShown || state.due ||
            (state.stale && (state.quiet >= 2 || state.sinceRefresh >= COMMAND_REFRESH_FRAMES)) ||
            (state.localization && state.localization->revision() != state.seenLocale))
        {
            state.wasShown = true;
            state.stale = false;
            state.sinceRefresh = 0;
            state.refresh();
        }
        if (state.repick && command == state.frameCommand)
        {
            state.repick = false;
            state.pickById(state.pickedId, false);
        }
        state.frameCommand = command;
        state.syncOutline(true);
        state.scroll.fit(state.density.density());
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
            {.name = "uidocuments",
             .category = "project",
             .order = 180,
             .dock = {.target = "scene"},
             .zone = "EditorUiDocuments",
             .make = [](EditorPanelContext& c)
             { return makeEditorPanel<EditorUiDocuments>(c.engine, c.dialogs, c.feedback); }}};
    } // namespace
} // namespace spite
