#include "EditorStateViewer.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorDialogs.hpp"
#include "EditorFeedback.hpp"
#include "EditorPageScroll.hpp"
#include "EditorPanelList.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/ControlServer.hpp"
#include "ecs/World.hpp"
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

        constexpr std::string_view STATE_VIEW = "editor.state";
        constexpr std::string_view STATE_TOOLBAR = "state.toolbar";
        constexpr std::string_view STATE_SOURCES = "state.sources";
        constexpr std::string_view STATE_ROWS = "state.rows";
        constexpr std::string_view STATE_DETAIL = "state.detail";
        constexpr std::string_view OWNER = "editorState";
        constexpr std::string_view TABLE = "editor-state.";
        constexpr std::string_view CAPTURES_ROOT = "captures";
        constexpr std::array<u32, 4> PAGE_SIZES{25, 50, 100, 250};
        constexpr u32 DEFAULT_SIZE = 50;
        constexpr u32 LIVE_FRAMES = 15; // a Live table is read this often at most (4 Hz at 60 fps)
        constexpr sizet CELL_BYTES = 96; // a cell is cut here; the selected row is read in full under the table
        constexpr sizet DETAIL_BYTES = 600;
        constexpr float FILTER_MIN_WIDTH = 120.0f;
        constexpr float PAGE_SIZE_WIDTH = 96.0f; // "250 rows"
        constexpr float SOURCE_NAME_WIDTH = 150.0f;

        struct SourceCells
        {
            std::string scope;
            std::string name;
            std::string id; // not a column: what a click on the row acts on
            bool operator==(const SourceCells&) const = default;
            SPITE_OBSERVED_SCHEMA(SourceCells, scope, name, id)
        };

        // A CPU service table of a world, or of the root container (queried through the first world), or a GPU buffer
        struct Source
        {
            std::string id; // scene/EcsTables.archetypes, root/LogHistory.entries, gpu/ObjectBuffer
            std::string scope; // scene, play, root, gpu
            std::string world; // the world its query names
            std::string service;
            std::string table; // the table, or the GPU buffer
            bool gpu = false;
        };

        // A GPU buffer as gpuTables lists it
        struct GpuInfo
        {
            u64 capacityRows = 0;
            std::optional<u64> liveRows;
            u32 stride = 0;
            std::vector<std::string> fields;
        };

        // The answer of a gpuBuffer read, left by the reply sink for the next frame: the ticket of the read it answers
        struct Mail
        {
            bool arrived = false;
            u64 ticket = 0;
            std::string text;
        };

        enum class GpuState : u8
        {
            Idle,
            Pending,
            Ready,
            Refused
        };

        // What a reply answered: the page shown is always one of these, never derived from the request.
        struct Page
        {
            std::string id;
            u64 first = 0;
            u32 size = 0; // the page size it was asked for
            u32 span = 0; // rows the query returned (GPU: rows read, before the filter)
            u64 matched = 0; // CPU: rows the filter keeps; GPU: rows in the buffer
            u64 total = 0; // CPU: every row of the table
            std::optional<u64> inUse; // GPU: the rows in use where the engine counts them
            std::string filter;
            std::vector<std::string> fields;
            std::vector<Json> rows;
        };

        // The panel's filter text: `field:text` searches one field when the part before the colon names a column
        struct FieldFilter
        {
            std::string column;
            std::string needle;
        };

        FieldFilter parseFilter(const std::string& text, const std::vector<std::string>& fields)
        {
            if (const sizet colon = text.find(':'); colon != std::string::npos && colon > 0)
            {
                const std::string name = text.substr(0, colon);
                for (const std::string& field : fields)
                {
                    if (field.size() == name.size() && containsIgnoringCase(field, name))
                    {
                        return {field, text.substr(colon + 1)};
                    }
                }
            }
            return {{}, text};
        }

        std::string clip(std::string text, const sizet limit, bool& cut)
        {
            if (text.size() <= limit)
            {
                return text;
            }
            sizet end = limit;
            while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
            {
                --end;
            }
            text.resize(end);
            cut = true;
            return text + "...";
        }

        std::string valueText(const Json& value)
        {
            if (value.is_string())
            {
                return value.get<std::string>();
            }
            if (value.is_number_float())
            {
                return std::format("{:.6g}", value.get<double>());
            }
            if (value.is_array() && std::ranges::all_of(value, [](const Json& item) { return item.is_number(); }))
            {
                std::string text = "(";
                for (const Json& item : value)
                {
                    text += (text.size() > 1 ? ", " : "") + valueText(item);
                }
                return text + ")";
            }
            return value.dump(-1, ' ', false, Json::error_handler_t::replace);
        }

        // The cells of a row, in the page's column order
        std::vector<std::string> rowTexts(const Json& row, const std::vector<std::string>& fields)
        {
            std::vector<std::string> texts;
            for (const std::string& field : fields)
            {
                const auto at = row.find(field);
                texts.push_back(at == row.end() ? std::string{} : valueText(*at));
            }
            return texts;
        }

        // One table of string cells over a schema built for its columns
        struct Grid
        {
            std::vector<std::string> names;
            std::vector<FieldDesc> fields;
            StructSchema schema;
            std::vector<std::string> cells;
            TableView table;
        };

        // wide enough for the title and the longest of the rows read, at about 8 px a character
        float columnWidth(const sizet characters)
        {
            return std::clamp(16.0f + 8.0f * static_cast<float>(characters), 64.0f, 240.0f);
        }
    } // namespace

    struct EditorStateViewer::State
    {
        State(Engine& engine, EditorFeedback& feedback, EditorDialogs& dialogs) :
            engine(&engine), control(&engine.control()), feedback(&feedback), dialogs(&dialogs),
            scope(editorUi(engine).ui()),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        EditorFeedback* feedback;
        EditorDialogs* dialogs;
        UiScope scope;
        DensityFollower density{scope};
        const Localization* localization;
        Panel root;
        PageScroll scroll; // the page scrolls where the panel is shorter than its toolbar, lists and detail
        TextInput filterBox;
        Button prev;
        Button next;
        Button refresh;
        Button exportButton;
        Button copyButton;
        Checkbox liveBox;
        Dropdown sizeBox;
        Label status;
        Label detail;
        Label emptyBody; // the rows area's note while no page has rows: what the footer lines say then
        Widget rowsSlot;
        std::unique_ptr<RowTable<SourceCells>> sourceTable;
        std::map<std::string, std::unique_ptr<Grid>> grids;
        Grid* grid = nullptr;
        std::unique_ptr<UiDocumentView> view;
        std::vector<std::function<void()>> pending;
        std::optional<std::string> typed; // the box's latest text, sent once per frame

        std::vector<Source> sources;
        std::map<std::string, GpuInfo> gpuInfo;
        bool gpuListed = false;
        bool sourcesRead = false;
        std::string sourceId; // the picked source; empty: none
        u64 first = 0; // the wanted page: its offset, size and filter
        u32 pageSize = DEFAULT_SIZE;
        std::string filterText;
        Page page; // the last answer; page.id empty: none shown
        std::string problem; // why the picked table shows nothing
        std::optional<u32> selected; // a row of page.rows
        bool clipped = false;
        bool detailCut = false;
        bool liveOn = false;
        bool wasShown = false;
        u32 sinceLive = 0;
        u64 reads = 0;
        u64 seenLocale = 0;

        // the page sizes as "<n> rows" in the current locale; the selection keeps its index
        void fillSizes()
        {
            std::vector<UiChoiceItem> sizes;
            for (const u32 size : PAGE_SIZES)
            {
                const MessageArg args[]{{"count", static_cast<std::int64_t>(size)}};
                sizes.push_back({.id = InternedString(std::to_string(size)), .label = message("size.rows", args)});
            }
            sizeBox.setItems(sizes);
        }

        // The GPU read: one in flight, asked for flightId at flightFirst with flightSize rows. Its answer arrives
        // through the mailbox in a later frame and counts only when it is the newest ticket and the page still wanted.
        GpuState gpuState = GpuState::Idle;
        std::string gpuReason;
        std::shared_ptr<Mail> mail = std::make_shared<Mail>();
        u64 ticket = 0;
        bool inFlight = false;
        std::string flightId;
        u64 flightFirst = 0;
        u32 flightSize = 0;
        std::vector<Json> gpuRaw; // the rows read, before the filter

        [[nodiscard]] std::string message(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            const std::string full = std::string(TABLE) + std::string(key);
            if (!localization)
            {
                return full;
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(full), args, scratch));
        }

        [[nodiscard]] Json result(const char* query, const Json& args = Json::object()) const
        {
            const Json reply = request(*control, Json{{"query", query}, {"args", args}});
            return reply.contains("result") && reply["result"].is_object() ? reply["result"] : Json::object();
        }

        void toast(const ToastLevel level, const std::string_view key, const std::span<const MessageArg> args = {})
        {
            feedback->postToastKey(level, std::string(TABLE) + std::string(key), args);
        }

        [[nodiscard]] const Source* find(const std::string_view id) const
        {
            const auto at = std::ranges::find(sources, id, &Source::id);
            return at == sources.end() ? nullptr : &*at;
        }

        [[nodiscard]] const Source* picked() const { return find(sourceId); }

        // No play world runs: editing, or paused (the pause-and-inspect state a Live table follows)
        [[nodiscard]] bool settled() const
        {
            PlayWorld* play = engine->playWorld();
            return !play || engine->gameWorld().simulationState().paused;
        }

        // -- sources ----------------------------------------------------------------------------------------------

        void readSources()
        {
            sourcesRead = true;
            sources.clear();
            gpuInfo.clear();
            std::vector<std::string> worlds;
            for (const Json& entry : result("worlds").value("worlds", Json::array()))
            {
                worlds.push_back(entry.value("name", std::string{}));
            }
            for (u32 w = 0; w < worlds.size(); ++w)
            {
                for (const Json& service : result("services", {{"world", worlds[w]}}).value("services", Json::array()))
                {
                    const bool own = service.value("depth", 0u) == 0;
                    if (!own && w > 0)
                    {
                        continue; // the root container's tables are listed once, with the first world
                    }
                    for (const Json& table : service.value("tables", Json::array()))
                    {
                        Source source{.scope = own ? worlds[w] : "root",
                                      .world = worlds[w],
                                      .service = service.value("name", std::string{}),
                                      .table = table.get<std::string>()};
                        source.id = source.scope + "/" + source.service + "." + source.table;
                        sources.push_back(std::move(source));
                    }
                }
            }
            // a headless run has no gpuTables: no GPU sources, and the status says so
            const Json gpu = request(*control, Json{{"query", "gpuTables"}, {"args", Json::object()}});
            gpuListed = gpu.value("ok", false);
            for (const Json& table : gpuListed ? gpu["result"].value("tables", Json::array()) : Json::array())
            {
                if (!table.value("present", false))
                {
                    continue;
                }
                GpuInfo info{.capacityRows = table.value("capacityRows", u64{0}), .stride = table.value("stride", 0u)};
                if (table.contains("liveRows"))
                {
                    info.liveRows = table["liveRows"].get<u64>();
                }
                for (const Json& field : table.value("fields", Json::array()))
                {
                    // padding keeps the layout aligned and says nothing: gpuBuffer still replies it, no column shows it
                    std::string name = field.value("name", std::string{});
                    if (!name.starts_with("_pad"))
                    {
                        info.fields.push_back(std::move(name));
                    }
                }
                const std::string buffer = table.value("buffer", std::string{});
                gpuInfo[buffer] = std::move(info);
                sources.push_back({.id = "gpu/" + buffer, .scope = "gpu", .table = buffer, .gpu = true});
            }
            std::vector<SourceCells> rows;
            sizet longest = 0;
            for (const Source& source : sources)
            {
                const bool known =
                    source.scope == "scene" || source.scope == "play" || source.scope == "root" || source.gpu;
                const std::string name = source.gpu ? source.table : source.service + "." + source.table;
                longest = std::max(longest, name.size());
                rows.push_back({known ? message("scope." + source.scope) : source.scope, name, source.id});
            }
            sourceTable->set(std::move(rows));
            // a long name scrolls sideways in the list instead of being cut at the column
            (void)sourceTable->table().apply({.kind = UiTableOpKind::Resize,
                                              .column = InternedString("name"),
                                              .width = std::max(SOURCE_NAME_WIDTH, columnWidth(longest))});
            syncSourceSelection();
        }

        void syncSourceSelection()
        {
            const auto& rows = sourceTable->rows();
            const auto at = std::ranges::find(rows, sourceId, &SourceCells::id);
            const Entity key(static_cast<u32>(at - rows.begin()) + 1, 1u);
            sourceTable->table().setSelection(at == rows.end() ? std::span<const Entity>{}
                                                               : std::span<const Entity>(&key, 1));
        }

        // -- grids ------------------------------------------------------------------------------------------------

        void attach(Grid& g)
        {
            if (!rowsSlot.isValid())
            {
                return;
            }
            putIn(scope, rowsSlot, g.table);
            const UiSlotComponent& fields = slotFields(scope, rowsSlot);
            g.table.setScrollEnabled(fields.scroll);
            g.table.setRowClass(fields.childClass.isValid() ? fields.childClass : InternedString(LIST_ROW_CLASS));
        }

        Grid& gridFor(const std::vector<std::string>& columns, const std::vector<Json>& rows)
        {
            std::string key = sourceId;
            for (const std::string& column : columns)
            {
                key += "|" + column;
            }
            std::unique_ptr<Grid>& slot = grids[key];
            if (slot)
            {
                return *slot;
            }
            slot = std::make_unique<Grid>();
            Grid& g = *slot;
            g.names = columns; // never resized: the schema's field names point into it
            std::vector<UiTableColumn> shown;
            std::vector<sizet> longest;
            for (const std::string& name : g.names)
            {
                longest.push_back(name.size());
            }
            for (u32 r = 0; r < rows.size() && r < 50; ++r)
            {
                const std::vector<std::string> texts = rowTexts(rows[r], columns);
                for (u32 i = 0; i < texts.size(); ++i)
                {
                    longest[i] = std::max(longest[i], std::min(texts[i].size(), CELL_BYTES));
                }
            }
            for (u32 i = 0; i < g.names.size(); ++i)
            {
                g.fields.push_back({.name = g.names[i],
                                    .offset = static_cast<u32>(i * sizeof(std::string)),
                                    .kind = FieldKind::String,
                                    .flags = fieldFlags::OBSERVED});
                shown.push_back({.field = InternedString(g.names[i]),
                                 .title = g.names[i],
                                 .width = columnWidth(longest[i]),
                                 .sortable = false});
            }
            g.schema = {"StateRow", static_cast<u32>(g.names.size() * sizeof(std::string)), g.fields};
            const Density now = density.density();
            UiTableDesc desc;
            desc.rect = topLeft();
            desc.rect.layoutItem = fillItem();
            desc.rowHeight = now.row;
            desc.headerHeight = now.control;
            desc.padding = now.xs;
            desc.schema = &g.schema;
            desc.columns = std::move(shown);
            desc.rowClass = InternedString(LIST_ROW_CLASS);
            desc.row = [&g](const u32 row) -> const void* { return g.cells.data() + sizet{row} * g.names.size(); };
            desc.key = [](const u32 row) { return Entity(row + 1, 1u); };
            g.table = scope.createTable("state.grid", std::move(desc));
            g.table.setAccessibleNameKey(InternedString(std::string(TABLE) + "table.rows"));
            g.table.onSelect(
                [this, &g](const std::span<const Entity> keys)
                {
                    if (&g == grid)
                    {
                        selected = keys.empty() ? std::nullopt : std::optional<u32>(keys.front().index() - 1);
                        showChrome();
                    }
                });
            g.table.setVisible(false);
            attach(g);
            return g;
        }

        // -- pages ------------------------------------------------------------------------------------------------

        void clearPage()
        {
            page = {};
            selected.reset();
            clipped = false;
            if (grid)
            {
                grid->cells.clear();
                grid->table.refresh(0);
                grid->table.setSelection({});
            }
        }

        // Shows a reply's rows. The selection stays only on the same page (source, offset, size and filter).
        void showPage(Page answered)
        {
            const bool same = answered.id == page.id && answered.first == page.first && answered.size == page.size &&
                answered.filter == page.filter;
            page = std::move(answered);
            Grid& g = gridFor(page.fields, page.rows);
            if (grid != &g)
            {
                if (grid)
                {
                    grid->table.setVisible(false);
                }
                grid = &g;
                g.table.setVisible(true);
            }
            std::vector<std::string> cells;
            clipped = false;
            for (const Json& row : page.rows)
            {
                for (std::string& text : rowTexts(row, page.fields))
                {
                    cells.push_back(clip(std::move(text), CELL_BYTES, clipped));
                }
            }
            if (cells != g.cells)
            {
                g.cells = std::move(cells);
                g.table.refresh(static_cast<u32>(page.rows.size()));
            }
            if (!same || (selected && *selected >= page.rows.size()))
            {
                selected.reset();
                g.table.setSelection({});
            }
            showChrome();
        }

        void readCpu(const Source& source)
        {
            FieldFilter filter =
                parseFilter(filterText, page.id == sourceId ? page.fields : std::vector<std::string>{});
            const auto ask = [&]
            {
                Json args{{"world", source.world},
                          {"service", source.service},
                          {"table", source.table},
                          {"limit", pageSize},
                          {"first", first}};
                if (!filter.needle.empty())
                {
                    args["filter"] = filter.needle;
                    if (!filter.column.empty())
                    {
                        args["column"] = filter.column;
                    }
                }
                return request(*control, Json{{"query", "tableRows"}, {"args", args}});
            };
            Json reply = ask();
            // the table shrank under the offset: the last page that has rows
            if (reply.value("ok", false) && first > 0 && reply["result"]["rows"].empty() &&
                reply["result"]["matched"].get<u64>() > 0)
            {
                first = (reply["result"]["matched"].get<u64>() - 1) / pageSize * pageSize;
                reply = ask();
            }
            if (!reply.value("ok", false))
            {
                problem = reply.value("error", std::string{});
                clearPage();
                showChrome();
                return;
            }
            problem.clear();
            const Json& answer = reply["result"];
            Page answered{.id = source.id,
                          .first = answer.value("first", u64{0}),
                          .size = pageSize,
                          .matched = answer.value("matched", u64{0}),
                          .total = answer.value("total", u64{0}),
                          .filter = filterText};
            for (const Json& field : answer.value("fields", Json::array()))
            {
                answered.fields.push_back(field.get<std::string>());
            }
            for (const Json& row : answer.value("rows", Json::array()))
            {
                answered.rows.push_back(row);
            }
            answered.span = static_cast<u32>(answered.rows.size());
            showPage(std::move(answered));
        }

        // -- GPU pages --------------------------------------------------------------------------------------------

        [[nodiscard]] bool keeps(const Json& row, const std::vector<std::string>& fields,
                                 const FieldFilter& filter) const
        {
            if (filter.needle.empty())
            {
                return true;
            }
            const std::vector<std::string> texts = rowTexts(row, fields);
            for (u32 i = 0; i < texts.size(); ++i)
            {
                if ((filter.column.empty() || fields[i] == filter.column) &&
                    containsIgnoringCase(texts[i], filter.needle))
                {
                    return true;
                }
            }
            return false;
        }

        // The page as the filter leaves the rows read: the filter never asks the GPU again
        void showGpu()
        {
            const GpuInfo& info = gpuInfo[sourceId.substr(sourceId.find('/') + 1)];
            Page answered{.id = sourceId,
                          .first = flightFirst,
                          .size = flightSize,
                          .span = static_cast<u32>(gpuRaw.size()),
                          .matched = info.capacityRows,
                          .total = info.capacityRows,
                          .inUse = info.liveRows,
                          .filter = filterText};
            answered.fields.push_back("row");
            answered.fields.insert(answered.fields.end(), info.fields.begin(), info.fields.end());
            const FieldFilter filter = parseFilter(filterText, answered.fields);
            for (const Json& row : gpuRaw)
            {
                if (keeps(row, answered.fields, filter))
                {
                    answered.rows.push_back(row);
                }
            }
            problem.clear();
            showPage(std::move(answered));
        }

        void settleGpu(const Json& reply)
        {
            if (!reply.is_object() || !reply.value("ok", false))
            {
                gpuState = GpuState::Refused;
                gpuReason = reply.is_object() ? reply.value("error", std::string{}) : std::string("unparsable reply");
                gpuRaw.clear();
                clearPage();
                showChrome();
                return;
            }
            gpuState = GpuState::Ready;
            gpuReason.clear();
            gpuRaw.clear();
            const Json& answer = reply["result"];
            for (const Json& row : answer.value("rows", Json::array()))
            {
                gpuRaw.push_back(row);
            }
            GpuInfo& info = gpuInfo[sourceId.substr(sourceId.find('/') + 1)];
            info.capacityRows = answer.value("capacityRows", u64{0});
            info.liveRows = answer.contains("liveRows") ? std::optional(answer["liveRows"].get<u64>()) : std::nullopt;
            first = flightFirst;
            showGpu();
        }

        // Asks for the wanted page. One read is in flight at a time: a change meanwhile waits for its answer, which
        // is dropped when it answers a page that is no longer wanted.
        void requestGpu()
        {
            const Source* source = picked();
            if (!source)
            {
                return;
            }
            // a page that is not the wanted one is not shown as current while its answer is awaited
            if (page.id != sourceId || page.first != first || page.size != pageSize)
            {
                clearPage();
            }
            gpuState = GpuState::Pending;
            gpuReason.clear();
            showChrome();
            if (inFlight)
            {
                return;
            }
            flightId = sourceId;
            flightFirst = first;
            flightSize = pageSize;
            const u64 mine = ++ticket;
            inFlight = true;
            const std::string line = Json{{"query", "gpuBuffer"},
                                          {"args", {{"buffer", source->table}, {"first", first}, {"limit", pageSize}}}}
                                         .dump();
            // gpuBuffer defers its reply to a frame that renders: the sink only leaves it in the mailbox
            const std::string immediate = control->handle(line, [box = mail, mine](const std::string_view text)
                                                          { *box = {true, mine, std::string(text)}; });
            if (!immediate.empty())
            {
                inFlight = false;
                settleGpu(Json::parse(immediate, nullptr, false));
            }
        }

        // The answer of the read in flight, if one arrived: applied when it is the newest ticket and the page it read
        // is the one wanted, else dropped and the wanted page asked next
        void takeMail()
        {
            if (!mail->arrived)
            {
                return;
            }
            const Mail got = std::exchange(*mail, Mail{});
            if (!inFlight || got.ticket != ticket)
            {
                return;
            }
            inFlight = false;
            const Source* source = picked();
            if (!source || !source->gpu)
            {
                return;
            }
            if (flightId == sourceId && flightFirst == first && flightSize == pageSize)
            {
                settleGpu(Json::parse(got.text, nullptr, false));
            }
            else
            {
                requestGpu();
            }
        }

        void refreshPage()
        {
            SPITE_ZONE("EditorStateViewer.refresh");
            ++reads;
            sinceLive = 0;
            const Source* source = picked();
            if (!source)
            {
                if (!sourceId.empty())
                {
                    const MessageArg args[]{{"name", std::string_view(sourceId)}};
                    problem = message("status.gone", args);
                }
                clearPage();
                showChrome();
            }
            else if (source->gpu)
            {
                requestGpu();
            }
            else
            {
                readCpu(*source);
            }
        }

        // -- chrome -----------------------------------------------------------------------------------------------

        void setText(Label& label, const std::string& text)
        {
            if (label.text() != text)
            {
                label.setText(text);
            }
        }

        [[nodiscard]] u32 gpuStride() const
        {
            const auto at = gpuInfo.find(sourceId.substr(sourceId.find('/') + 1));
            return at == gpuInfo.end() ? 0 : at->second.stride;
        }

        [[nodiscard]] std::string statusText() const
        {
            if (!problem.empty())
            {
                return problem;
            }
            const Source* source = picked();
            if (!source)
            {
                return message("status.none") +
                    (gpuListed || !sourcesRead ? std::string{} : " " + message("status.noGpu"));
            }
            const std::string name = source->gpu ? source->table : source->service + "." + source->table;
            if (page.id != sourceId)
            {
                const MessageArg args[]{{"buffer", std::string_view(name)}, {"reason", std::string_view(gpuReason)}};
                if (!source->gpu)
                {
                    return {};
                }
                return gpuState == GpuState::Refused ? message("gpu.refused", args)
                    : gpuState == GpuState::Pending  ? message("gpu.pending", args)
                                                     : std::string{};
            }
            const auto count = [](const u64 value) { return static_cast<std::int64_t>(value); };
            const MessageArg args[]{{"table", std::string_view(name)},
                                    {"from", count(page.rows.empty() ? 0 : page.first + 1)},
                                    {"to", count(page.first + page.rows.size())},
                                    {"matched", count(page.matched)},
                                    {"total", count(page.total)},
                                    {"readFrom", count(page.first + 1)},
                                    {"readTo", count(page.first + page.span)},
                                    {"shown", count(page.rows.size())},
                                    {"inUse", count(page.inUse.value_or(0))},
                                    {"kib", count((u64{page.size} * gpuStride() + 1023) / 1024)}};
            std::string text;
            if (source->gpu)
            {
                text = message(!page.filter.empty() ? "status.gpuFiltered"
                                   : page.inUse     ? "status.gpuLive"
                                                    : "status.gpu",
                               args) +
                    " " + message("gpu.cost", args);
            }
            else
            {
                text = message(page.rows.empty() ? (page.filter.empty() ? "status.empty" : "status.noMatch")
                                                 : (page.filter.empty() ? "status.cpu" : "status.cpuFiltered"),
                               args);
            }
            return text + (clipped || detailCut ? " " + message("status.clipped") : std::string{}) +
                (liveOn && !settled() ? " " + message("status.liveWait") : std::string{});
        }

        // the selected row's fields in one line; cut is set when the row is longer than the line holds
        [[nodiscard]] std::string detailText(bool& cut) const
        {
            cut = false;
            if (!selected || *selected >= page.rows.size())
            {
                return message("detail.hint");
            }
            std::string fields;
            const Json& row = page.rows[*selected];
            for (const std::string& name : page.fields)
            {
                const auto at = row.find(name);
                fields +=
                    (fields.empty() ? "" : ", ") + name + "=" + (at == row.end() ? std::string{} : valueText(*at));
            }
            const std::string shownFields = clip(std::move(fields), DETAIL_BYTES, cut);
            const MessageArg args[]{{"row", static_cast<std::int64_t>(page.first + *selected)},
                                    {"fields", std::string_view(shownFields)}};
            return message("detail.row", args);
        }

        void setEnabled(Widget widget, const bool on)
        {
            if (widget.isInteractable() != on)
            {
                widget.setInteractable(on);
            }
        }

        void showChrome()
        {
            const std::string detailLine = detailText(detailCut);
            const std::string statusLine = statusText();
            setText(status, statusLine);
            setText(detail, detailLine);
            const Source* source = picked();
            const bool shownPage = source && page.id == sourceId;
            // no rows to show: the area where they go says why (and what to pick) instead of the footer lines
            const std::string note = shownPage && !page.rows.empty() ? std::string{}
                : source                                             ? statusLine
                                                                     : statusLine + " " + detailLine;
            const bool noteOn = !note.empty();
            setText(emptyBody, note);
            const auto showIf = [](Widget widget, const bool on)
            {
                if (widget.isVisible() != on)
                {
                    widget.setVisible(on);
                }
            };
            showIf(emptyBody, noteOn);
            showIf(status, !noteOn);
            showIf(detail, !noteOn);
            if (grid)
            {
                showIf(grid->table, !noteOn);
            }
            const u64 end = page.first + page.span;
            setEnabled(prev, shownPage && page.first > 0);
            setEnabled(next, shownPage && end < page.matched);
            setEnabled(exportButton, source && !source->gpu);
            setEnabled(copyButton, selected && *selected < page.rows.size());
        }

        // -- actions ----------------------------------------------------------------------------------------------

        std::string selectSource(const std::string& id)
        {
            // a source that appeared since the last read (play/... after play) is found by one more read
            const bool fresh = !sourcesRead;
            if (fresh)
            {
                readSources();
            }
            if (!find(id) && !fresh)
            {
                readSources();
            }
            if (!find(id))
            {
                std::string known;
                for (u32 i = 0; i < sources.size() && i < 12; ++i)
                {
                    known += " " + sources[i].id;
                }
                return "no source '" + id + "'; sources:" + known + (sources.size() > 12 ? " ..." : "");
            }
            sourceId = id;
            first = 0;
            filterText.clear();
            showFilter("");
            problem.clear();
            clearPage();
            gpuState = GpuState::Idle;
            gpuReason.clear();
            syncSourceSelection();
            refreshPage();
            return {};
        }

        std::string goTo(const u64 offset)
        {
            if (!picked())
            {
                return "no source picked (stateSource id=)";
            }
            const u64 bound = picked()->gpu ? gpuInfo[picked()->table].capacityRows
                : page.id == sourceId       ? page.matched
                                            : ~u64{0};
            if (offset > 0 && offset >= bound)
            {
                return std::format("first {} is past the {} rows", offset, bound);
            }
            first = offset;
            refreshPage();
            return {};
        }

        void showFilter(const std::string& text) { show(filterBox, text); }

        std::string setFilter(const std::string& text)
        {
            if (!picked())
            {
                return "no source picked (stateSource id=)";
            }
            filterText = text;
            showFilter(text);
            if (picked()->gpu)
            {
                // the rows read, filtered again: the filter never reads the GPU again, and a read in flight takes it
                // when it answers
                if (gpuState == GpuState::Ready && !inFlight)
                {
                    showGpu();
                }
                return {};
            }
            first = 0;
            refreshPage();
            return {};
        }

        std::string copyRow(IFormatWriter& out)
        {
            if (!selected || *selected >= page.rows.size())
            {
                return "no row selected (stateSelectRow row=<n>)";
            }
            const std::string text = page.rows[*selected].dump(-1, ' ', false, Json::error_handler_t::replace);
            UiContext& ui = editorUi(*engine).ui();
            ui.setClipboardText(text);
            if (ui.clipboardText() != text)
            {
                return "the clipboard did not take the text";
            }
            const MessageArg args[]{{"row", static_cast<std::int64_t>(page.first + *selected)}};
            toast(ToastLevel::Info, "toast.copied", args);
            out.writeString("text", text);
            return {};
        }

        void askExport()
        {
            const Source* source = picked();
            if (!source || source->gpu)
            {
                return;
            }
            const std::string id = source->id;
            const std::string name = std::format("state-{}-{}_{}", source->scope, source->service, source->table);
            dialogs->pickFile({.mode = EditorPickMode::SaveFile,
                               .title = message("export.title"),
                               .roots = {std::string(CAPTURES_ROOT)},
                               .name = name,
                               .extensions = "sqlite"},
                              [this, id](const std::optional<EditorPicked> choice)
                              {
                                  if (choice)
                                  {
                                      pending.push_back([this, id, path = choice->path] { exportTable(id, path); });
                                  }
                              });
        }

        // Identity, not position: the source picked when Export was pressed, or a refusal if it is gone
        void exportTable(const std::string& id, const std::string& path)
        {
            const Source* source = find(id);
            const MessageArg gone[]{{"name", std::string_view(id)}};
            const Json reply = source
                ? request(*control,
                          Json{{"query", "snapshot"},
                               {"args",
                                {{"world", source->world},
                                 {"path", path},
                                 {"tables", Json::array({source->service + "_" + source->table})}}}})
                : Json{{"ok", false}, {"error", message("status.gone", gone)}};
            if (reply.value("ok", false))
            {
                const MessageArg args[]{{"path", std::string_view(path)}};
                toast(ToastLevel::Info, "toast.exported", args);
                return;
            }
            const std::string why = reply.value("error", std::string{});
            const MessageArg args[]{{"error", std::string_view(why)}};
            toast(ToastLevel::Warn, "toast.exportRefused", args);
        }

        void setLive(const bool on)
        {
            liveOn = on;
            liveBox.setChecked(on);
            sinceLive = 0;
            showChrome();
        }

        void setSize(const u32 size)
        {
            pageSize = size;
            first = 0;
            const auto at = std::ranges::find(PAGE_SIZES, size);
            sizeBox.setSelected(static_cast<i32>(at - PAGE_SIZES.begin()));
            if (picked())
            {
                refreshPage();
            }
        }

        void registerEntries()
        {
            control->registerViewCommand(
                "stateSource", OWNER,
                "picks the table the state viewer shows (a source as stateView lists them: scene/EcsTables.archetypes, "
                "root/LogHistory.entries, gpu/ObjectBuffer) and reads its first page; the filter clears. A source that "
                "is not listed is looked for once more before the refusal; a GPU buffer is asked for now and shows "
                "when its answer arrives in a later frame (stateView: gpu.state)",
                {requiredArg("id", ArgKind::String, "the source, as stateView lists it")},
                [this](const ControlArgs& args, IFormatWriter&)
                { return selectSource(args.string("id").value_or("")); });
            control->registerViewCommand(
                "stateFilter", OWNER,
                "filters the picked table's rows by a field's text (ignoring case), or one column with field:text, and "
                "returns to the first page; a CPU table filters in the query, so the offset counts matched rows; a GPU "
                "buffer filters the rows already read, with no new readback, and stays on its page",
                {requiredArg("text", ArgKind::String, "the text; empty clears")},
                [this](const ControlArgs& args, IFormatWriter&)
                { return setFilter(args.string("text").value_or("")); });
            control->registerViewCommand("statePage", OWNER,
                                         "reads the picked table's page at an explicit offset (among the matched rows; "
                                         "the buffer's rows for a GPU buffer); refused past the end of the "
                                         "rows last seen",
                                         {requiredArg("first", ArgKind::Integer, "the offset of the page's first row")},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         { return goTo(args.integer("first").value_or(0)); });
            control->registerViewCommand("statePageSize", OWNER,
                                         "sets the rows per page (25, 50, 100 or 250) and returns to the first page",
                                         {requiredArg("size", ArgKind::Integer, "25, 50, 100 or 250")},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         {
                                             const u64 size = args.integer("size").value_or(0);
                                             if (std::ranges::find(PAGE_SIZES, size) == PAGE_SIZES.end())
                                             {
                                                 return std::string("size is 25, 50, 100 or 250");
                                             }
                                             setSize(static_cast<u32>(size));
                                             return std::string{};
                                         });
            control->registerViewCommand(
                "stateLive", OWNER,
                "follows the picked table: with on it reads again every 15 frames while no play world runs (paused or "
                "editing); a running world is never read per frame. Off by default",
                {requiredArg("on", ArgKind::Boolean, "follow or not")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    setLive(args.boolean("on").value_or(false));
                    return std::string{};
                });
            control->registerViewCommand(
                "stateRefresh", OWNER,
                "reads the sources and the picked table again, now: the Refresh button; a GPU buffer "
                "is asked for and answers in a later frame",
                {},
                [this](const ControlArgs&, IFormatWriter&)
                {
                    readSources();
                    refreshPage();
                    return std::string{};
                });
            control->registerViewCommand(
                "stateSelectRow", OWNER, "selects a row of the page shown; the row is read in full under the table",
                {requiredArg("row", ArgKind::Integer, "the row of the page, from 0, as stateView lists them")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const u64 row = args.integer("row").value_or(0);
                    if (row >= page.rows.size() || !grid)
                    {
                        return std::format("row {} is out of range: the page shows {} row(s)", row, page.rows.size());
                    }
                    selected = static_cast<u32>(row);
                    const Entity key(static_cast<u32>(row) + 1, 1u);
                    grid->table.setSelection(std::span(&key, 1));
                    showChrome();
                    return std::string{};
                });
            control->registerViewCommand("stateCopyRow", OWNER,
                                         "copies the selected row of the state viewer to the clipboard as JSON: the "
                                         "Copy Row button; the answer is the text",
                                         {}, [this](const ControlArgs&, IFormatWriter& out) { return copyRow(out); });
            control->registerQuery(
                "stateView", OWNER,
                "the state viewer: source (the picked id), sources (id, scope, name), gpu (listed: gpuTables answered; "
                "state idle|pending|ready|refused of the GPU read; reason: why it was refused), the page (first, size, "
                "span, matched, total, inUse) it last read, filter, columns, rows (the cells of the page), selected, "
                "live and settled, status, detail, whether a cell was cut (clipped) and the reads made (reads). A CPU "
                "table is read now, shown or hidden; a GPU buffer never (its read answers in a later frame)",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    if (!sourcesRead)
                    {
                        readSources();
                    }
                    if (const Source* source = picked(); source && !source->gpu)
                    {
                        refreshPage();
                    }
                    out.writeString("source", sourceId);
                    out.beginArray("sources", static_cast<u32>(sourceTable->rows().size()));
                    for (const SourceCells& row : sourceTable->rows())
                    {
                        out.beginObject("");
                        out.writeString("id", row.id);
                        out.writeString("scope", row.scope);
                        out.writeString("name", row.name);
                        out.endObject();
                    }
                    out.endArray();
                    out.beginObject("gpu");
                    out.writeBool("listed", gpuListed);
                    constexpr std::array<std::string_view, 4> STATES{"idle", "pending", "ready", "refused"};
                    out.writeString("state", STATES[static_cast<u32>(gpuState)]);
                    out.writeString("reason", gpuReason);
                    out.endObject();
                    out.beginObject("page");
                    out.writeU64("first", page.first);
                    out.writeU32("size", pageSize);
                    out.writeU32("span", page.span);
                    out.writeU64("matched", page.matched);
                    out.writeU64("total", page.total);
                    if (page.inUse)
                    {
                        out.writeU64("inUse", *page.inUse);
                    }
                    out.endObject();
                    out.writeString("filter", filterText);
                    out.beginArray("columns", static_cast<u32>(page.fields.size()));
                    for (const std::string& field : page.fields)
                    {
                        out.writeString("", field);
                    }
                    out.endArray();
                    out.beginArray("rows", static_cast<u32>(page.rows.size()));
                    for (const Json& row : page.rows)
                    {
                        out.beginObject("");
                        bool cut = false;
                        for (const std::string& field : page.fields)
                        {
                            const auto at = row.find(field);
                            out.writeString(field,
                                            clip(at == row.end() ? std::string{} : valueText(*at), CELL_BYTES, cut));
                        }
                        out.endObject();
                    }
                    out.endArray();
                    out.writeI32("selected", selected ? static_cast<i32>(*selected) : -1);
                    out.writeBool("live", liveOn);
                    out.writeBool("settled", settled());
                    out.writeString("status", status.text());
                    out.writeString("detail", detail.text());
                    out.writeBool("clipped", clipped || detailCut);
                    out.writeU64("reads", reads);
                    return std::string{};
                });
        }
    };

    EditorStateViewer::EditorStateViewer(Engine& engine, EditorFeedback& feedback, EditorDialogs& dialogs) :
        m_state(std::make_unique<State>(engine, feedback, dialogs))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.root = scope.createPanel("editor.panel", columnDesc());
        state.scroll =
            PageScroll(scope, InternedString("state.scroll"), InternedString("editor-state.title"), state.root);
        const auto fitButton = [&state, &scope](const char* id, const std::string& key)
        {
            UiRectDesc desc = barDesc(Vec2{});
            desc.contentSize = UiContentSizeComponent{
                .fitWidth = true, .fitHeight = false, .padding = Vec2{state.density.density().lg, 0.0f}};
            Button button = state.density.track(scope.createButton(id, "", desc), HEIGHT_CONTROL);
            bindFitPadding(button, SPACE_LG, SPACE_NONE);
            button.setLocalizedText(InternedString(std::string(TABLE) + "action." + key));
            button.setAccessibleNameKey(InternedString(std::string(TABLE) + "action." + key));
            button.setTooltip(InternedString(std::string(TABLE) + "tip." + key));
            return button;
        };
        const Json none = Json::object();
        const auto command = [&state](const char* name, const Json& args)
        {
            state.pending.push_back(
                [&state, name, args]
                {
                    const Json reply = request(*state.control, Json{{"command", name}, {"args", args}});
                    if (!reply.value("ok", false))
                    {
                        const std::string why = reply.value("error", std::string{});
                        const MessageArg values[]{{"command", std::string_view(name)},
                                                  {"error", std::string_view(why)}};
                        state.toast(ToastLevel::Warn, "toast.refused", values);
                    }
                });
        };
        state.prev = fitButton("state.prev", "prev");
        state.prev.onClick(
            [&state]
            {
                // the offsets come from the page shown when the click runs, not from one captured earlier
                state.pending.push_back(
                    [&state]
                    {
                        const u64 back = state.page.first > state.pageSize ? state.page.first - state.pageSize : 0;
                        (void)request(*state.control, Json{{"command", "statePage"}, {"args", {{"first", back}}}});
                    });
            });
        state.next = fitButton("state.next", "next");
        state.next.onClick(
            [&state]
            {
                state.pending.push_back(
                    [&state]
                    {
                        (void)request(
                            *state.control,
                            Json{{"command", "statePage"}, {"args", {{"first", state.page.first + state.page.span}}}});
                    });
            });
        state.refresh = fitButton("state.refresh", "refresh");
        state.refresh.onClick([command, none] { command("stateRefresh", none); });
        state.exportButton = fitButton("state.export", "export");
        state.exportButton.onClick([&state] { state.askExport(); });
        state.copyButton = fitButton("state.copy", "copy");
        state.copyButton.onClick([command, none] { command("stateCopyRow", none); });
        state.liveBox = scope.createCheckbox("state.live", "", barDesc(Vec2{}), false);
        state.liveBox.label().setLocalizedText(InternedString(std::string(TABLE) + "action.live"));
        state.liveBox.setTooltip(InternedString(std::string(TABLE) + "tip.live"));
        state.liveBox.onChange([command](const bool checked) { command("stateLive", {{"on", checked}}); });
        UiDropdownDesc sizeDesc;
        sizeDesc.rect = barDesc(Vec2{PAGE_SIZE_WIDTH, 0.0f});
        state.sizeBox = state.density.track(scope.createDropdown("state.size", sizeDesc), HEIGHT_CONTROL);
        state.fillSizes();
        state.sizeBox.setSelected(static_cast<i32>(std::ranges::find(PAGE_SIZES, DEFAULT_SIZE) - PAGE_SIZES.begin()));
        state.sizeBox.setAccessibleNameKey(InternedString(std::string(TABLE) + "action.size"));
        state.sizeBox.setTooltip(InternedString(std::string(TABLE) + "tip.size"));
        state.sizeBox.onChange(
            [command](const u32 index)
            {
                if (index < PAGE_SIZES.size())
                {
                    command("statePageSize", {{"size", PAGE_SIZES[index]}});
                }
            });
        UiRectDesc filterDesc = barDesc(Vec2{});
        filterDesc.layoutItem = fillItem();
        filterDesc.layoutItem->minSize = Vec2{FILTER_MIN_WIDTH, 0.0f};
        filterDesc.layoutItem->alignSelf = UiLayoutAlign::Center;
        state.filterBox = state.density.track(scope.createTextInput("state.filter", filterDesc), HEIGHT_CONTROL);
        bindInputPadding(state.filterBox);
        state.filterBox.setStyleClass(InternedString("tool.field"));
        state.filterBox.setLocalizedPlaceholder(InternedString("engine.ui.list.placeholder"));
        state.filterBox.setAccessibleNameKey(InternedString(std::string(TABLE) + "action.filter"));
        state.filterBox.setTooltip(InternedString(std::string(TABLE) + "tip.filter"));
        state.filterBox.onChange([&state](const std::string_view text) { state.typed = std::string(text); });

        UiRectDesc noteDesc = barDesc(Vec2{});
        noteDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        state.status = scope.createLabel("state.status.text", "", noteDesc);
        state.status.setStyleClass(InternedString("settings.noteWrap"));
        state.status.setInteractable(false);
        state.detail = scope.createLabel("state.detail.text", "", noteDesc);
        state.detail.setStyleClass(InternedString("settings.note")); // one line: Copy Row has the whole row
        state.detail.setInteractable(false);
        state.emptyBody = emptyNote(scope, "state.empty");

        state.sourceTable = std::make_unique<RowTable<SourceCells>>(
            scope, "state.sources.table", InternedString(std::string(TABLE) + "table.sources"),
            std::vector<UiTableColumn>{{.field = InternedString("scope"),
                                        .width = 56.0f,
                                        .titleKey = InternedString(std::string(TABLE) + "column.scope"),
                                        .sortable = false},
                                       {.field = InternedString("name"),
                                        .width = 236.0f,
                                        .titleKey = InternedString(std::string(TABLE) + "column.name"),
                                        .sortable = false}});
        state.sourceTable->table().onSelect(
            [&state](const std::span<const Entity> keys)
            {
                // by identity: the id of the row shown now, which the command checks against the sources again
                const auto& rows = state.sourceTable->rows();
                if (keys.size() == 1 && keys.front().index() - 1 < rows.size())
                {
                    state.pending.push_back(
                        [&state, id = rows[keys.front().index() - 1].id]
                        { (void)request(*state.control, Json{{"command", "stateSource"}, {"args", {{"id", id}}}}); });
                }
            });
        state.density.apply(
            [&state](const Density& now)
            {
                for (const auto& [key, g] : state.grids)
                {
                    g->table.setMetrics(now.row, now.control, now.xs);
                }
            });
        state.registerEntries();
        scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(STATE_TOOLBAR),
                      [&state](const Widget& slot)
                      {
                          for (Widget widget : std::initializer_list<Widget>{
                                   state.filterBox, state.prev, state.next, state.sizeBox, state.liveBox, state.refresh,
                                   state.exportButton, state.copyButton})
                          {
                              putIn(state.scope, slot, widget);
                          }
                      });
        contract.slot(InternedString(STATE_SOURCES),
                      [&state](const Widget& slot) { state.sourceTable->fill(state.scope, slot); });
        contract.slot(InternedString(STATE_ROWS),
                      [&state](const Widget& slot)
                      {
                          state.rowsSlot = slot;
                          putIn(state.scope, slot, state.emptyBody);
                          for (const auto& [key, g] : state.grids)
                          {
                              state.attach(*g);
                          }
                      });
        contract.slot(InternedString(STATE_DETAIL),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.status);
                          putIn(state.scope, slot, state.detail);
                      });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                const Density density = Density::read(viewScope);
                UiLayoutComponent line =
                    stack(UiLayoutDirection::Row, density.xs, Vec4{density.xs, density.xxs, density.xs, density.xxs},
                          UiLayoutAlign::Center);
                line.wrap = true;
                UiRectDesc toolbarDesc = barDesc(Vec2{}, line);
                toolbarDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                Panel toolbar = viewScope.createPanel(InternedString(STATE_TOOLBAR), toolbarDesc);
                bindToolbar(toolbar);
                toolbar.setLayoutItem(fixedItem());
                toolbar.setParent(root);
                UiRectDesc bodyDesc = barDesc(Vec2{}, stack(UiLayoutDirection::Row, 0.0f, {}, UiLayoutAlign::Stretch));
                bodyDesc.layoutItem = fillItem();
                Panel body = viewScope.createPanel("state.body", bodyDesc);
                body.setParent(root);
                UiRectDesc sourcesDesc =
                    barDesc(Vec2{}, stack(UiLayoutDirection::Column, 0.0f, {}, UiLayoutAlign::Stretch));
                // the longest table names fit
                sourcesDesc.layoutItem = UiLayoutItemComponent{.shrink = 0.0f, .basis = 300.0f};
                Panel sources = viewScope.createPanel(InternedString(STATE_SOURCES), sourcesDesc);
                sources.setParent(body);
                fillSlotPanel(viewScope, STATE_ROWS, body);
                UiRectDesc detailDesc = barDesc(Vec2{},
                                                stack(UiLayoutDirection::Column, 0.0f,
                                                      Vec4{density.xs, density.hairline, density.xs, density.hairline},
                                                      UiLayoutAlign::Stretch));
                detailDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                Panel detail = viewScope.createPanel(InternedString(STATE_DETAIL), detailDesc);
                bindPadding(detail, SPACE_XS, SPACE_HAIRLINE, SPACE_XS, SPACE_HAIRLINE);
                detail.setParent(root);
            });
        state.view =
            openView(engine, STATE_VIEW, "assets/ui/editor/state.json", std::move(contract), state.scroll.page());
    }

    EditorStateViewer::~EditorStateViewer() { m_state->control->unregisterOwner(OWNER); }

    Widget EditorStateViewer::content() const { return m_state->root; }

    void EditorStateViewer::update()
    {
        State& state = *m_state;
        if (!state.pending.empty())
        {
            std::vector<std::function<void()>> actions;
            actions.swap(state.pending);
            for (const std::function<void()>& action : actions)
            {
                action();
            }
        }
        if (state.typed)
        {
            const std::string text = *state.typed;
            state.typed.reset();
            if (text != state.filterText && state.picked())
            {
                (void)request(*state.control, Json{{"command", "stateFilter"}, {"args", {{"text", text}}}});
            }
        }
        state.takeMail();
        if (!shown(state.scope, state.root))
        {
            state.wasShown = false;
            return;
        }
        state.scroll.fit(state.density.density());
        const u64 locale = state.localization ? state.localization->revision() : 0;
        if (!state.wasShown || locale != state.seenLocale)
        {
            state.wasShown = true;
            state.seenLocale = locale;
            state.fillSizes();
            state.readSources();
            state.refreshPage();
            return;
        }
        if (state.liveOn && state.picked() && state.settled() && !state.inFlight && ++state.sinceLive >= LIVE_FRAMES)
        {
            state.refreshPage();
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "state",
             .category = "world",
             .order = 155,
             .dock = {.target = "scene"},
             .zone = "EditorStateViewer",
             .make = [](EditorPanelContext& c)
             { return makeEditorPanel<EditorStateViewer>(c.engine, c.feedback, c.dialogs); }}};
    } // namespace
} // namespace spite
