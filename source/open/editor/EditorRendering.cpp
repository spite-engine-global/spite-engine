#include "EditorRendering.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorEntityLabel.hpp"
#include "EditorFeedback.hpp"
#include "EditorPanelList.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/ControlServer.hpp"
#include "ecs/World.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "renderruntime/RenderRuntime.hpp"
#include "rendering/ShaderManager.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using namespace panelui;

        constexpr std::string_view RENDERING_VIEW = "editor.rendering";
        constexpr std::string_view RENDERING_TOOLBAR = "rendering.toolbar";
        constexpr std::string_view RENDERING_ROWS = "rendering.rows";
        constexpr std::string_view RENDERING_DETAIL = "rendering.detail";
        constexpr std::string_view OWNER = "editorRendering";
        constexpr std::string_view TABLE = "editor-rendering.";
        constexpr std::array<std::string_view, 3> TABS{"pipeline", "views", "shadows"};
        constexpr u32 REFRESH_FRAMES = 30; // the Views and Shadows tabs follow the engine this often at most
        constexpr u32 COMMAND_REFRESH_FRAMES = 20; // a stream of state commands is read this often at most
        constexpr u32 MAX_REPORT_LINES = 60; // shader report rows; the rest is counted in a last row
        constexpr u32 MAX_CAMERAS = 64; // the chooser lists this many; the rest is counted
        constexpr float CAMERA_WIDTH = 150.0f;

        // a status row: section, item, value; valueClass "warn" marks a refusal or a failure
        struct StatusCells
        {
            std::string section;
            std::string item;
            std::string value;
            std::string valueClass;
            bool operator==(const StatusCells&) const = default;
            SPITE_OBSERVED_SCHEMA(StatusCells, section, item, value, valueClass)
        };

        struct ViewCells
        {
            u32 view = 0;
            std::string world;
            std::string camera;
            std::string extent;
            std::string swapchain;
            std::string follows;
            bool operator==(const ViewCells&) const = default;
            SPITE_OBSERVED_SCHEMA(ViewCells, view, world, camera, extent, swapchain, follows)
        };

        struct CascadeCells
        {
            u32 view = 0;
            u32 cascade = 0;
            std::string range;
            std::string radius;
            std::string texel;
            std::string dirty;
            u64 rendered = 0;
            u32 visible = 0;
            u32 dropped = 0;
            bool operator==(const CascadeCells&) const = default;
            SPITE_OBSERVED_SCHEMA(CascadeCells, view, cascade, range, radius, texel, dirty, rendered, visible, dropped)
        };

        UiTableColumn column(const std::string_view field, const float width)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString(std::string(TABLE) + "column." + std::string(field)),
                    .sortable = false,
                    .classField = field == "value" ? InternedString("valueClass") : InternedString{}};
        }

        std::string fixed(const double value, const int digits) { return std::format("{:.{}f}", value, digits); }
    } // namespace

    struct EditorRendering::State
    {
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
        Panel root;
        std::array<Button, TABS.size()> tabs;
        Button reloadPipeline;
        Button reloadShaders;
        Button swapchain;
        Button invalidate;
        Dropdown camera;
        Label detail;
        Panel spacer; // pushes the actions to the right of the tabs
        std::unique_ptr<RowTable<StatusCells>> status;
        std::unique_ptr<RowTable<ViewCells>> views;
        std::unique_ptr<RowTable<CascadeCells>> cascades;
        std::unique_ptr<UiDocumentView> view;
        std::vector<std::function<void()>> pending;
        u32 tab = 0;
        bool wasShown = false;
        bool commanded = false; // this panel ran a command: read once more
        bool stale = false; // a state command ran since the last read
        bool truncated = false; // the shader report has rows beyond MAX_REPORT_LINES
        bool selectionMoved = false;
        u32 sinceRefresh = 0;
        u64 seenCommand = 0;
        u64 seenLocale = 0;
        u64 seenReloads = 0;
        std::string seenError; // renderPipeline lastError as read
        u64 refreshes = 0;
        // the Views tab's camera chooser and the controls' states, written when they change
        std::vector<u64> cameraIds;
        std::vector<std::string> cameraNames;
        u64 camerasTotal = 0;
        std::string cameraState;
        std::array<i32, 2> enabled{-1, -1}; // the swapchain button and the camera chooser as last written
        std::string detailText;

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

        [[nodiscard]] ShaderManager& shaders() const { return engine->services().rootServices().get<ShaderManager>(); }

        [[nodiscard]] static std::optional<u32> pickedRow(const TableView& table)
        {
            const std::span<const Entity> picked = table.selection();
            return picked.empty() ? std::nullopt : std::optional(picked.front().index() - 1);
        }

        [[nodiscard]] std::optional<u32> selectedView() const { return pickedRow(views->table()); }

        // the shown tab's table and its row count
        [[nodiscard]] TableView shownTable() const
        {
            return tab == 0 ? status->table() : tab == 1 ? views->table() : cascades->table();
        }

        [[nodiscard]] u32 shownRows() const
        {
            return static_cast<u32>(tab == 0       ? status->rows().size()
                                        : tab == 1 ? views->rows().size()
                                                   : cascades->rows().size());
        }

        [[nodiscard]] std::string_view worldOf(const u32 index, const Json& entry) const
        {
            return index == engine->gameView() || entry.value("followsGame", false) ? engine->gameWorldName()
                                                                                    : std::string_view("scene");
        }

        // -- the three tabs' rows ---------------------------------------------------------------------------------

        std::vector<StatusCells> pipelineRows()
        {
            const Json pipeline = result("renderPipeline");
            const Json report = result("shaderStatus");
            std::vector<StatusCells> rows;
            // a section names its first row only: the rows under it read as its group
            std::string lastSection;
            const auto add = [&rows, &lastSection](const std::string& section, std::string item, std::string value,
                                                   const bool warn = false)
            {
                rows.push_back({section == lastSection ? std::string{} : section, std::move(item), std::move(value),
                                warn ? "warn" : ""});
                lastSection = section;
            };
            const std::string pipelineSection = message("section.pipeline");
            const std::string shaderSection = message("section.shaders");
            const std::string error = pipeline.value("lastError", std::string{});
            add(pipelineSection, message("item.dll"), displayPath(pipeline.value("source", std::string{})));
            add(pipelineSection, message("item.reloads"), std::to_string(pipeline.value("reloads", 0u)));
            add(pipelineSection, message("item.auto"),
                message(pipeline.value("watching", false) ? "value.on" : "value.off"));
            add(pipelineSection, message("item.refused"), error.empty() ? message("value.none") : error,
                !error.empty());
            const bool on = report.value("enabled", false);
            add(shaderSection, message("item.hotReload"),
                message(!on                                   ? "value.off"
                            : report.value("watching", false) ? "value.watching"
                                                              : "value.notWatching"));
            if (on)
            {
                const u32 reloads = report.value("reloads", 0u);
                add(shaderSection, message("item.source"), displayPath(report.value("source", std::string{})));
                add(shaderSection, message("item.reloads"), std::to_string(reloads));
                const MessageArg args[]{
                    {"compiled", static_cast<std::int64_t>(report.value("compiled", Json::array()).size())},
                    {"failed", static_cast<std::int64_t>(report.value("failed", Json::array()).size())},
                    {"refused", static_cast<std::int64_t>(report.value("refused", Json::array()).size())},
                    {"rebuilt", static_cast<std::int64_t>(report.value("pipelinesRebuilt", 0u))},
                    {"kept", static_cast<std::int64_t>(report.value("pipelinesKept", 0u))}};
                add(shaderSection, message("item.last"),
                    reloads == 0 ? message("value.noneYet") : message("value.last", args));
            }
            // one row per line of a file's text: a compile error spans many; failures first
            u32 lines = 0;
            u32 more = 0;
            for (const auto& [key, item, warn] : {std::tuple{"failed", "item.failed", true},
                                                  {"refused", "item.refusedShader", true},
                                                  {"compiled", "item.compiled", false}})
            {
                for (const Json& entry : report.value(key, Json::array()))
                {
                    const std::string text = entry.get<std::string>();
                    bool first = true;
                    for (sizet from = 0; from < text.size();)
                    {
                        const sizet end = std::min(text.find('\n', from), text.size());
                        std::string line = text.substr(from, end - from);
                        from = end + 1;
                        std::erase(line, '\r');
                        if (line.empty())
                        {
                            continue;
                        }
                        if (lines < MAX_REPORT_LINES)
                        {
                            add(shaderSection, first ? message(item) : std::string{}, std::move(line), warn);
                            ++lines;
                        }
                        else
                        {
                            ++more;
                        }
                        first = false;
                    }
                }
            }
            truncated = more > 0;
            if (truncated)
            {
                const MessageArg args[]{{"count", static_cast<std::int64_t>(more)}};
                add(shaderSection, message("item.more"), message("value.more", args), true);
            }
            seenError = error;
            return rows;
        }

        std::string cameraLabel(const Json& entry, const std::string_view worldName) const
        {
            if (entry.value("pose", false))
            {
                return message("view.pose");
            }
            const Entity camera(entry.value("camera", u64{0}));
            if (!camera.isValid())
            {
                return message("view.hidden");
            }
            const World& world = worldName == "play" ? engine->gameWorld() : engine->world();
            std::string scratch;
            return world.isEntityAlive(camera) ? std::string(entityLabel(scratch, world, camera))
                                               : message("view.removed");
        }

        std::vector<ViewCells> viewRows(const Json& reply) const
        {
            std::vector<ViewCells> rows;
            const Json swapchainView = reply.value("swapchainView", Json());
            for (const Json& entry : reply.value("views", Json::array()))
            {
                const u32 index = entry.value("view", 0u);
                const std::string world(worldOf(index, entry));
                const u32 width = entry.value("width", 0u);
                const u32 height = entry.value("height", 0u);
                rows.push_back(
                    {index, world, cameraLabel(entry, world),
                     width > 0 && height > 0 ? std::format("{} x {}", width, height) : message("view.hidden"),
                     message(swapchainView.is_number() && swapchainView.get<u32>() == index ? "value.yes" : "value.no"),
                     message(entry.value("followsGame", false) ? "value.yes" : "value.no")});
            }
            return rows;
        }

        [[nodiscard]] std::string dirtyText(const u32 reasons) const
        {
            std::string text;
            constexpr std::array<std::string_view, 5> NAMES{"camera", "light", "casters", "graph", "forced"};
            for (u32 bit = 0; bit < NAMES.size(); ++bit)
            {
                if (reasons & (1u << bit))
                {
                    text += (text.empty() ? "" : "+") + message("dirty." + std::string(NAMES[bit]));
                }
            }
            return text.empty() ? message("value.no") : text;
        }

        std::vector<CascadeCells> cascadeRows() const
        {
            std::vector<CascadeCells> rows;
            for (const Json& entry : result("shadowCascades").value("cascades", Json::array()))
            {
                rows.push_back(
                    {entry.value("view", 0u), entry.value("cascade", 0u),
                     fixed(entry.value("splitNear", 0.0), 2) + " - " + fixed(entry.value("splitFar", 0.0), 2),
                     fixed(entry.value("radius", 0.0), 2), fixed(entry.value("texelWorldSize", 0.0), 4),
                     dirtyText(entry.value("dirtyReasons", 0u)), entry.value("renderedFrames", u64{0}),
                     entry.value("visibleMeshlets", 0u), entry.value("overflowed", 0u)});
            }
            return rows;
        }

        // -- the Views tab's camera chooser ----------------------------------------------------------------------

        void readCameras(const std::string_view worldName)
        {
            cameraIds.clear();
            cameraNames.clear();
            const Json found = result(
                "find", Json{{"with", "CameraComponent"}, {"world", std::string(worldName)}, {"limit", MAX_CAMERAS}});
            camerasTotal = found.value("total", u64{0});
            const World& world = worldName == "play" ? engine->gameWorld() : engine->world();
            std::string scratch;
            for (const Json& row : found.value("entities", Json::array()))
            {
                const u64 id = row.value("id", u64{0});
                std::string name = row.value("name", std::string{});
                cameraIds.push_back(id);
                cameraNames.push_back(name.empty() ? std::string(entityLabel(scratch, world, Entity(id))) : name);
            }
            const std::vector<std::string> plain = cameraNames;
            for (u32 i = 0; i < cameraNames.size(); ++i)
            {
                if (std::ranges::count(plain, plain[i]) > 1)
                {
                    cameraNames[i] += std::format(" #{}", Entity(cameraIds[i]).index());
                }
            }
        }

        void showCameras(const u32 viewIndex, const Json& entry, const std::string_view worldName)
        {
            readCameras(worldName);
            const u64 bound = entry.value("camera", u64{0});
            std::string signature = std::format("{}|{}|{}|{}", viewIndex, worldName, bound, camerasTotal);
            for (const std::string& name : cameraNames)
            {
                signature += "|" + name;
            }
            if (signature == cameraState)
            {
                return;
            }
            cameraState = std::move(signature);
            std::vector<UiChoiceItem> items;
            items.push_back({.id = InternedString("none"), .key = InternedString(std::string(TABLE) + "camera.none")});
            for (u32 i = 0; i < cameraIds.size(); ++i)
            {
                items.push_back({.id = InternedString(std::to_string(cameraIds[i])), .label = cameraNames[i]});
            }
            if (camerasTotal > cameraIds.size())
            {
                const MessageArg args[]{{"shown", static_cast<std::int64_t>(cameraIds.size())},
                                        {"total", static_cast<std::int64_t>(camerasTotal)}};
                items.push_back({.label = message("camera.truncated", args), .enabled = false});
            }
            camera.setItems(items);
            const auto at = std::ranges::find(cameraIds, bound);
            camera.setSelected(!Entity(bound).isValid()    ? 0
                                   : at == cameraIds.end() ? -1
                                                           : static_cast<i32>(at - cameraIds.begin()) + 1);
        }

        // -- refresh ----------------------------------------------------------------------------------------------

        void setEnabled(const u32 slot, Widget widget, const bool on)
        {
            if (enabled[slot] != static_cast<i32>(on))
            {
                enabled[slot] = static_cast<i32>(on);
                widget.setInteractable(on);
            }
        }

        void showDetail(const std::string& text)
        {
            if (detail.text() != text)
            {
                detail.setText(text);
            }
        }

        // The tab's table from the engine, the controls it enables and the line under it.
        void refresh()
        {
            SPITE_ZONE("EditorRendering.refresh");
            ++refreshes;
            selectionMoved = false;
            seenLocale = localization ? localization->revision() : 0;
            seenReloads = reloads();
            if (tab == 0)
            {
                status->set(pipelineRows());
                // a long path or error is cut by its column: the picked row is read in full below
                const std::optional<u32> picked = pickedRow(status->table());
                showDetail(picked && *picked < status->rows().size() ? status->rows()[*picked].value
                                                                     : message("detail.pipeline"));
            }
            else if (tab == 1)
            {
                const Json reply = result("views");
                views->set(viewRows(reply));
                const std::optional<u32> picked = selectedView();
                const Json entries = reply.value("views", Json::array());
                const bool known = picked && *picked < entries.size();
                setEnabled(0, swapchain, known);
                // a tool's view draws from its own pose: a camera entity would change nothing
                setEnabled(1, camera, known && !entries[*picked].value("pose", false));
                if (known)
                {
                    const Json& entry = entries[*picked];
                    const std::string world(worldOf(*picked, entry));
                    showCameras(*picked, entry, world);
                    const std::string label = cameraLabel(entry, world);
                    const MessageArg args[]{{"view", static_cast<std::int64_t>(*picked)},
                                            {"world", std::string_view(world)},
                                            {"camera", std::string_view(label)}};
                    showDetail(message("detail.views", args));
                }
                else
                {
                    showDetail(message("detail.views.none"));
                }
            }
            else
            {
                const ShadowConfig& graphics = engine->config().graphics.shadows;
                std::vector<CascadeCells> rows = cascadeRows();
                const bool none = rows.empty();
                cascades->set(std::move(rows));
                const MessageArg args[]{{"count", static_cast<std::int64_t>(graphics.cascadeCount)},
                                        {"resolution", static_cast<std::int64_t>(graphics.resolution)},
                                        {"distance", static_cast<double>(graphics.maxDistance)},
                                        {"lambda", static_cast<double>(graphics.splitLambda)},
                                        {"pcf", static_cast<std::int64_t>(graphics.pcfRadius)},
                                        {"blend", static_cast<double>(graphics.blendBand)},
                                        {"constant", static_cast<double>(graphics.depthBiasConstant)},
                                        {"slope", static_cast<double>(graphics.depthBiasSlope)},
                                        {"normal", static_cast<double>(graphics.normalOffset)}};
                showDetail((none ? message("detail.shadows.none") : std::string{}) + message("detail.shadows", args));
            }
        }

        // what a reload moves without a command of ours: the counters a frame compares
        [[nodiscard]] u64 reloads() const
        {
            return u64{engine->render().pipelineStatus().reloads} << 32 | shaders().reloadCount();
        }

        [[nodiscard]] bool errorMoved() const { return engine->render().pipelineStatus().lastError != seenError; }

        // -- tabs, controls and actions ---------------------------------------------------------------------------

        void showTabs()
        {
            for (u32 i = 0; i < tabs.size(); ++i)
            {
                tabs[i].setLocalizedText(InternedString(std::string(TABLE) + "tab." + std::string(TABS[i])));
                showSelected(tabs[i], i == tab);
            }
            status->table().setVisible(tab == 0);
            views->table().setVisible(tab == 1);
            cascades->table().setVisible(tab == 2);
            reloadPipeline.setVisible(tab == 0);
            reloadShaders.setVisible(tab == 0);
            swapchain.setVisible(tab == 1);
            camera.setVisible(tab == 1);
            invalidate.setVisible(tab == 2);
        }

        void setTab(const u32 index)
        {
            tab = index;
            showTabs();
            refresh();
        }

        void toast(const ToastLevel level, const std::string_view key, const std::span<const MessageArg> args = {})
        {
            feedback->postToastKey(level, std::string(TABLE) + std::string(key), args);
        }

        // runs a command; a refusal is a toast with the engine's reason
        std::optional<Json> run(const char* command, const Json& args)
        {
            const Json reply = request(*control, Json{{"command", command}, {"args", args}});
            if (reply.value("ok", false))
            {
                return reply.value("result", Json::object());
            }
            const std::string error = reply.value("error", std::string{});
            const MessageArg values[]{{"command", std::string_view(command)}, {"error", std::string_view(error)}};
            toast(ToastLevel::Warn, "toast.refused", values);
            return std::nullopt;
        }

        void reloadPipelineNow()
        {
            const Json reply = request(*control, Json{{"command", "reloadRenderPipeline"}, {"args", Json::object()}});
            if (reply.value("ok", false))
            {
                const MessageArg args[]{{"ms", reply["result"].value("ms", 0.0)}};
                toast(ToastLevel::Info, "toast.pipelineReloaded", args);
                return;
            }
            const std::string error = reply.value("error", std::string{});
            const MessageArg args[]{{"error", std::string_view(error)}};
            toast(ToastLevel::Error, "toast.pipelineRefused", args);
        }

        void reloadShadersNow()
        {
            const Json reply = request(*control, Json{{"command", "reloadShaders"}, {"args", Json::object()}});
            const bool ok = reply.value("ok", false);
            const Json report = ok ? reply.value("result", Json::object()) : reply.value("details", Json::object());
            if (!ok && report.empty())
            {
                const std::string error = reply.value("error", std::string{});
                const MessageArg args[]{{"error", std::string_view(error)}};
                toast(ToastLevel::Warn, "toast.shadersOff", args);
                return;
            }
            const auto count = [&report](const char* key)
            { return static_cast<std::int64_t>(report.value(key, Json::array()).size()); };
            if (!ok)
            {
                const MessageArg args[]{{"failed", count("failed")}, {"refused", count("refused")}};
                toast(ToastLevel::Error, "toast.shadersFailed", args);
            }
            else if (count("compiled") == 0)
            {
                toast(ToastLevel::Info, "toast.shadersUpToDate");
            }
            else
            {
                const MessageArg args[]{{"compiled", count("compiled")},
                                        {"rebuilt", static_cast<std::int64_t>(report.value("pipelinesRebuilt", 0u))}};
                toast(ToastLevel::Info, "toast.shadersReloaded", args);
            }
        }

        void bind(const u64 id)
        {
            const std::optional<u32> picked = selectedView();
            if (!picked)
            {
                return;
            }
            const Json entries = result("views").value("views", Json::array());
            if (*picked < entries.size())
            {
                run("bindCamera",
                    Json{{"view", *picked}, {"id", id}, {"world", std::string(worldOf(*picked, entries[*picked]))}});
            }
        }

        void select(const u32 row)
        {
            const Entity key(row + 1, 1u);
            shownTable().setSelection(std::span<const Entity>(&key, 1));
            refresh();
        }

        void registerEntries()
        {
            control->registerViewCommand("renderingTab", OWNER, "sets the tab the rendering panel shows",
                                         {requiredArg("tab", ArgKind::String, "pipeline, views or shadows", TABS)},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         {
                                             const auto at = std::ranges::find(TABS, args.string("tab").value_or(""));
                                             if (at == TABS.end())
                                             {
                                                 return std::string("tab is pipeline, views or shadows");
                                             }
                                             setTab(static_cast<u32>(at - TABS.begin()));
                                             return std::string{};
                                         });
            control->registerViewCommand(
                "renderingSelect", OWNER,
                "selects a row of the tab the rendering panel shows: on Views the row is the render view, and Set "
                "Swapchain View and the camera chooser act on it; on Pipeline the row is shown in full under the "
                "table",
                {requiredArg("row", ArgKind::Integer, "the row, from 0, as renderingView lists them")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    refresh(); // the rows of a hidden panel are read now
                    const u64 index = args.integer("row").value_or(0);
                    if (index >= shownRows())
                    {
                        return std::format("row {} out of range: the {} tab has {} row(s)", index, TABS[tab],
                                           shownRows());
                    }
                    select(static_cast<u32>(index));
                    return std::string{};
                });
            control->registerQuery(
                "renderingView", OWNER,
                "the rendering panel's current tab read from the engine, shown or hidden: pipeline (section, item, "
                "value rows of the render pipeline build and shader hot reload; truncated when the shader report "
                "has rows beyond 60), views (view, world, camera, extent, swapchain, follows rows; selected, and the "
                "cameras the chooser lists), shadows (view, cascade, range, radius, texel, dirty, rendered, visible "
                "and dropped rows), the line under the table (detail) and the reads made (refreshes)",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    refresh();
                    out.writeString("tab", TABS[tab]);
                    const auto rows = [&out](const auto& table)
                    {
                        using Row = std::decay_t<decltype(table->rows().front())>;
                        out.beginArray("rows", static_cast<u32>(table->rows().size()));
                        for (const Row& row : table->rows())
                        {
                            out.beginObject("");
                            writeValue(Row::spiteSchema(), &row, out, FieldSet::All);
                            out.endObject();
                        }
                        out.endArray();
                    };
                    if (tab == 0)
                    {
                        rows(status);
                        out.writeBool("truncated", truncated);
                    }
                    else if (tab == 1)
                    {
                        rows(views);
                        out.writeU64("camerasTotal", camerasTotal);
                        out.beginArray("cameras", static_cast<u32>(cameraIds.size()));
                        for (u32 i = 0; i < cameraIds.size(); ++i)
                        {
                            out.beginObject("");
                            out.writeU64("id", cameraIds[i]);
                            out.writeString("name", cameraNames[i]);
                            out.endObject();
                        }
                        out.endArray();
                    }
                    else
                    {
                        rows(cascades);
                    }
                    const std::optional<u32> picked = pickedRow(shownTable());
                    out.writeI32("selected", picked ? static_cast<i32>(*picked) : -1);
                    out.writeString("detail", detail.text());
                    out.writeU64("refreshes", refreshes);
                    return std::string{};
                });
        }
    };

    EditorRendering::EditorRendering(Engine& engine, EditorFeedback& feedback) :
        m_state(std::make_unique<State>(engine, feedback))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        state.root = scope.createPanel("editor.panel", columnDesc());
        const auto fitButton = [&state, &scope](const char* id, const std::string_view key, const std::string_view tip)
        {
            UiRectDesc desc = barDesc(Vec2{});
            desc.contentSize = UiContentSizeComponent{
                .fitWidth = true, .fitHeight = false, .padding = Vec2{state.density.density().lg, 0.0f}};
            Button button = state.density.track(scope.createButton(id, "", desc), HEIGHT_CONTROL);
            bindFitPadding(button, SPACE_LG, SPACE_NONE);
            button.setLocalizedText(InternedString(std::string(TABLE) + std::string(key)));
            if (!tip.empty())
            {
                button.setTooltip(InternedString(std::string(TABLE) + std::string(tip)));
            }
            return button;
        };
        for (u32 i = 0; i < state.tabs.size(); ++i)
        {
            state.tabs[i] = fitButton("rendering.tab", std::string("tab.") + std::string(TABS[i]), "");
            state.tabs[i].setAccessibleNameKey(InternedString(std::string(TABLE) + "tab." + std::string(TABS[i])));
            state.tabs[i].onClick(
                [&state, i]
                {
                    state.pending.push_back(
                        [&state, i]
                        {
                            (void)request(*state.control,
                                          Json{{"command", "renderingTab"}, {"args", {{"tab", std::string(TABS[i])}}}});
                        });
                });
        }
        state.reloadPipeline = fitButton("rendering.reloadPipeline", "action.reloadPipeline", "tip.reloadPipeline");
        state.reloadPipeline.onClick([&state] { state.pending.push_back([&state] { state.reloadPipelineNow(); }); });
        state.reloadShaders = fitButton("rendering.reloadShaders", "action.reloadShaders", "tip.reloadShaders");
        state.reloadShaders.onClick([&state] { state.pending.push_back([&state] { state.reloadShadersNow(); }); });
        state.swapchain = fitButton("rendering.swapchain", "action.swapchain", "tip.swapchain");
        state.swapchain.onClick(
            [&state]
            {
                state.pending.push_back(
                    [&state]
                    {
                        if (const std::optional<u32> picked = state.selectedView())
                        {
                            if (state.run("setSwapchainView", Json{{"view", std::to_string(*picked)}}))
                            {
                                const MessageArg args[]{{"view", static_cast<std::int64_t>(*picked)}};
                                state.toast(ToastLevel::Info, "toast.swapchainSet", args);
                            }
                        }
                    });
            });
        state.invalidate = fitButton("rendering.invalidate", "action.invalidate", "tip.invalidate");
        state.invalidate.onClick(
            [&state]
            {
                state.pending.push_back(
                    [&state]
                    {
                        if (const std::optional<Json> done = state.run("shadowInvalidate", Json::object()))
                        {
                            const MessageArg args[]{{"count", static_cast<std::int64_t>(done->value("cascades", 0u))}};
                            state.toast(ToastLevel::Info, "toast.invalidated", args);
                        }
                    });
            });
        UiDropdownDesc cameraDesc;
        cameraDesc.rect = barDesc(Vec2{CAMERA_WIDTH, 0.0f});
        state.camera = state.density.track(scope.createDropdown("rendering.camera", cameraDesc), HEIGHT_CONTROL);
        state.camera.setAccessibleNameKey(InternedString(std::string(TABLE) + "action.camera"));
        state.camera.setTooltip(InternedString(std::string(TABLE) + "tip.camera"));
        state.camera.onChange(
            [&state](const u32 index)
            {
                if (index == 0 || index <= state.cameraIds.size())
                {
                    state.pending.push_back([&state, id = index == 0 ? u64{0} : state.cameraIds[index - 1]]
                                            { state.bind(id); });
                }
            });
        const std::string tables = std::string(TABLE) + "table.";
        state.status = std::make_unique<RowTable<StatusCells>>(
            scope, "rendering.status", InternedString(tables + "pipeline"),
            std::vector{column("section", 90.0f), column("item", 110.0f), column("value", 640.0f)});
        state.views = std::make_unique<RowTable<ViewCells>>(
            scope, "rendering.views", InternedString(tables + "views"),
            std::vector{column("view", 56.0f), column("world", 72.0f), column("camera", 170.0f),
                        column("extent", 110.0f), column("swapchain", 96.0f), column("follows", 110.0f)});
        state.cascades = std::make_unique<RowTable<CascadeCells>>(
            scope, "rendering.cascades", InternedString(tables + "shadows"),
            std::vector{column("view", 56.0f), column("cascade", 72.0f), column("range", 130.0f),
                        column("radius", 96.0f), column("texel", 84.0f), column("dirty", 150.0f),
                        column("rendered", 90.0f), column("visible", 90.0f), column("dropped", 84.0f)});
        const auto picked = [&state](const std::span<const Entity>) { state.selectionMoved = true; };
        state.status->table().onSelect(picked);
        state.views->table().onSelect(picked);
        UiRectDesc detailDesc = barDesc(Vec2{});
        detailDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        UiRectDesc spacerDesc = barDesc(Vec2{});
        spacerDesc.layoutItem = fillItem();
        state.spacer = scope.createPanel("rendering.spacer", spacerDesc);
        state.spacer.setInteractable(false);
        state.detail = scope.createLabel("rendering.detail.text", "", detailDesc);
        state.detail.setStyleClass(InternedString("settings.noteWrap"));
        state.detail.setInteractable(false);
        state.registerEntries();
        state.showTabs();
        scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(RENDERING_TOOLBAR),
                      [&state](const Widget& slot)
                      {
                          for (Widget widget : std::initializer_list<Widget>{
                                   state.tabs[0], state.tabs[1], state.tabs[2], state.spacer, state.reloadPipeline,
                                   state.reloadShaders, state.swapchain, state.camera, state.invalidate})
                          {
                              putIn(state.scope, slot, widget);
                          }
                      });
        contract.slot(InternedString(RENDERING_ROWS),
                      [&state](const Widget& slot)
                      {
                          state.status->fill(state.scope, slot);
                          state.views->fill(state.scope, slot);
                          state.cascades->fill(state.scope, slot);
                      });
        contract.slot(InternedString(RENDERING_DETAIL),
                      [&state](const Widget& slot) { putIn(state.scope, slot, state.detail); });
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
                Panel toolbar = viewScope.createPanel(InternedString(RENDERING_TOOLBAR), toolbarDesc);
                bindGap(toolbar, SPACE_XS);
                bindPadding(toolbar, SPACE_XS, SPACE_XXS, SPACE_XS, SPACE_XXS);
                toolbar.setParent(root);
                fillSlotPanel(viewScope, RENDERING_ROWS, root);
                UiRectDesc detailDesc = barDesc(Vec2{},
                                                stack(UiLayoutDirection::Column, 0.0f,
                                                      Vec4{density.xs, density.hairline, density.xs, density.hairline},
                                                      UiLayoutAlign::Stretch));
                detailDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                Panel detail = viewScope.createPanel(InternedString(RENDERING_DETAIL), detailDesc);
                bindPadding(detail, SPACE_XS, SPACE_HAIRLINE, SPACE_XS, SPACE_HAIRLINE);
                detail.setParent(root);
            });
        state.view =
            openView(engine, RENDERING_VIEW, "assets/ui/editor/rendering.json", std::move(contract), state.root);
    }

    EditorRendering::~EditorRendering() { m_state->control->unregisterOwner(OWNER); }

    Widget EditorRendering::content() const { return m_state->root; }

    void EditorRendering::update()
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
            state.commanded = true; // the person's own action shows at once
        }
        if (!shown(state.scope, state.root))
        {
            state.wasShown = false;
            return;
        }
        const u64 command = state.control->stateGeneration();
        if (command != state.seenCommand)
        {
            state.seenCommand = command;
            state.stale = true;
        }
        ++state.sinceRefresh;
        const bool followed = state.tab != 0 && state.sinceRefresh >= REFRESH_FRAMES;
        if (!state.wasShown || state.commanded || state.selectionMoved || followed ||
            (state.stale && state.sinceRefresh >= COMMAND_REFRESH_FRAMES) || state.reloads() != state.seenReloads ||
            state.errorMoved() || (state.localization && state.localization->revision() != state.seenLocale))
        {
            state.wasShown = true;
            state.commanded = false;
            state.stale = false;
            state.sinceRefresh = 0;
            state.refresh();
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "rendering",
             .category = "debug",
             .order = 95,
             .dock = {.target = "project"},
             .zone = "EditorRendering",
             .make = [](EditorPanelContext& c) { return makeEditorPanel<EditorRendering>(c.engine, c.feedback); }}};
    } // namespace
} // namespace spite
