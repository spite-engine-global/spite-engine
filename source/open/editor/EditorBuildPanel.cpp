#include "EditorBuildState.hpp"
#include "EditorContext.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorPanelUi.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "base/io/FileSystem.hpp"
#include "application/Shell.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <utility>
#include <chrono>
#include <deque>
#include <fstream>
#include <future>
#include <sstream>

namespace spite
{
    namespace
    {
        using namespace panelui;
        constexpr auto OWNER = "editorBuildPanel";
        constexpr u32 NO_DIAGNOSTIC = std::numeric_limits<u32>::max();

        struct OutputTail
        {
            std::filesystem::path path;
            std::string text;
            std::vector<std::string> lines;
            u64 size = 0;
            bool unchanged = false, failed = false;
        };
        OutputTail readOutputTail(std::filesystem::path path, u64 previousSize)
        {
            OutputTail result{.path = std::move(path)};
            std::ifstream file(result.path, std::ios::binary | std::ios::ate);
            if (!file || file.tellg() < 0)
            {
                result.failed = true;
                return result;
            }
            const std::streamoff size = file.tellg();
            result.size = static_cast<u64>(size);
            if (result.size == previousSize)
            {
                result.unchanged = true;
                return result;
            }
            constexpr std::streamoff MAX_BYTES = 128 * 1024;
            const auto start = std::max<std::streamoff>(0, size - MAX_BYTES);
            file.seekg(start);
            result.text.resize(static_cast<sizet>(size - start));
            file.read(result.text.data(), static_cast<std::streamsize>(result.text.size()));
            result.text.resize(static_cast<sizet>(file.gcount()));
            if (start > 0)
            {
                const auto newline = result.text.find('\n');
                if (newline != std::string::npos)
                    result.text.erase(0, newline + 1);
            }
            std::deque<std::string> lines;
            std::istringstream input(result.text);
            for (std::string line; std::getline(input, line);)
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.size() > 4096)
                {
                    sizet end = 4096;
                    while (end && (static_cast<unsigned char>(line[end]) & 0xc0) == 0x80)
                        --end;
                    line.resize(end);
                    line += "…";
                }
                lines.push_back(std::move(line));
                if (lines.size() > 1000)
                    lines.pop_front();
            }
            result.lines.assign(std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
            return result;
        }

        class BuildPanel
        {
        public:
            BuildPanel(Engine& engine, EditorBuildState& build) :
                engine(engine), build(build), scope(editorUi(engine).ui()), density(Density::read(scope))
            {
                root = scope.createPanel("build.root", columnDesc(density.xs, Vec4{density.xs}));
                root.setLayoutItem(fillItem());
                bindGap(root, SPACE_XS);
                bindPadding(root, SPACE_XS, SPACE_XS, SPACE_XS, SPACE_XS);
                auto toolbarDesc = barDesc({}, stack(UiLayoutDirection::Row, density.xs));
                toolbarDesc.layout->wrap = true;
                toolbarDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                toolbar = scope.createPanel("build.toolbar", toolbarDesc);
                toolbar.setParent(root);
                bindGap(toolbar, SPACE_XS);
                UiDropdownDesc choice;
                choice.rect = barDesc({240, density.control});
                choice.rect.layoutItem = UiLayoutItemComponent{.grow = 1, .basis = 240, .minSize = {160, 0}};
                choice.searchable = true;
                projectChoice = scope.createDropdown("build.project", choice);
                projectChoice.setParent(toolbar);
                projectChoice.setAccessibleNameKey("editor-build.project");
                projectChoice.setPlaceholderKey("editor-build.chooseProject");
                projectChoice.onChange(
                    [this](u32 index)
                    {
                        if (index < projects.size())
                            queue("buildPanelSelect", {{"project", projects[index].project}});
                    });
                buildButton = button("build.start", "editor-build.build", toolbar);
                buildButton.onClick([this] { queue("buildPanelRun"); });
                refreshButton = button("build.refresh", "editor-build.refresh", toolbar);
                refreshButton.setIcon("arrows-clockwise");
                refreshButton.setAccessibleNameKey("editor-build.refresh");
                refreshButton.setTooltip(InternedString("editor-build.refresh"));
                refreshButton.setSize({density.control, density.control});
                refreshButton.onClick([this] { queue("buildPanelRefresh"); });
                reloadButton = button("build.reload", "editor-build.reload", toolbar);
                reloadButton.onClick([this] { queue("buildPanelReload"); });
                headline = label("build.headline", root, "build.headline");
                summary = label("build.summary", root, "modules.detail");
                hint = label("build.hint", root, "modules.detail");
                tabs = scope.createTabBar("build.tabs");
                tabs.setLayoutItem(fixedItem());
                tabs.setParent(root);
                tabs.addTab("build.tab.output", "").label().setLocalizedText("editor-build.output");
                tabs.addTab("build.tab.diagnostics", "").label().setLocalizedText("editor-build.diagnostics");
                tabs.setActive(1);
                tabs.onChange([this](u32 tab) { queue("buildPanelView", {{"output", tab == 0}}); });
                auto outputDesc = columnDesc(density.xs);
                outputDesc.layoutItem = fillItem();
                outputPane = scope.createPanel("build.output.pane", outputDesc);
                outputPane.setParent(root);
                auto logBar = scope.createPanel("build.output.actions", toolbarDesc);
                logBar.setParent(outputPane);
                followBox = scope.createCheckbox("build.output.follow", "", barDesc({0, density.control}));
                followBox.label().setLocalizedText("editor-build.followOutput");
                followBox.setParent(logBar);
                followBox.setChecked(true);
                followBox.onChange([this](bool value) { queue("buildPanelFollow", {{"follow", value}}); });
                copyOutputButton = button("build.output.copy", "editor-build.copyOutput", logBar);
                copyOutputButton.onClick([this] { queue("buildPanelCopyOutput"); });
                revealOutputButton = button("build.output.reveal", "editor-build.revealOutput", logBar);
                revealOutputButton.onClick([this] { queue("buildPanelRevealOutput"); });
                outputStatus = label("build.output.status", outputPane, "modules.detail");
                outputTable =
                    std::make_unique<Table>(scope,
                                            TableDesc{.id = "build.output.lines",
                                                      .cells = 1,
                                                      .read = [this](u32 row, std::span<std::string> cells,
                                                                     std::string&) { cells[0] = outputLines[row]; },
                                                      .cellNameKeys = {InternedString("editor-build.outputLine")},
                                                      .ellipsizeLast = true});
                outputTable->widget().setParent(outputPane);
                outputTable->widget().setLayoutItem(fillItem());
                filters = scope.createPanel("build.filters", toolbarDesc);
                filters.setParent(root);
                bindGap(filters, SPACE_XS);
                UiDropdownDesc filterDesc;
                filterDesc.rect = barDesc({180, density.control});
                filterChoice = scope.createDropdown("build.filter", filterDesc);
                filterChoice.setParent(filters);
                filterChoice.setAccessibleNameKey("editor-build.filter");
                filterChoice.onChange([this](u32 index) { queue("buildPanelFilter", {{"severity", index}}); });
                copyButton = button("build.copy", "editor-build.copy", filters);
                copyButton.onClick([this] { queue("buildPanelCopy"); });
                diagnostics = std::make_unique<Table>(
                    scope,
                    TableDesc{.id = "build.diagnostics",
                              .columns = {85, 180},
                              .cells = 3,
                              .read =
                                  [this](u32 row, std::span<std::string> cells, std::string&)
                              {
                                  const auto& item = record.diagnostics[visibleRows[row]];
                                  cells[0] = item.severity == "error" ? tr("error")
                                      : item.severity == "warning"    ? tr("warning")
                                                                      : tr("message");
                                  cells[1] = utf8Path(pathFromUtf8(item.file).filename());
                                  if (item.line)
                                      cells[1] += ":" + std::to_string(item.line);
                                  cells[2] = item.code.empty() ? item.message : item.code + ": " + item.message;
                              },
                              .select = [this](u32 row) { queue("buildPanelDiagnostic", {{"row", visibleRows[row]}}); },
                              .headers = {{InternedString("editor-build.severity"), false},
                                          {InternedString("editor-build.location"), false},
                                          {InternedString("editor-build.message"), false}},
                              .lastMin = 120,
                              .ellipsizeLast = true,
                              .isSelected = [this](u32 row) { return visibleRows[row] == selectedDiagnostic; }});
                diagnostics->widget().setParent(root);
                diagnostics->widget().setLayoutItem(fillItem());
                empty = label("build.empty", root, "modules.detail");
                empty.setLayoutItem(fillItem());
                detailScroll = scope.createScrollView("build.detail", topLeft());
                detailScroll.setLayoutItem(basisItem(density.row * 5));
                detailScroll.setParent(root);
                auto detailDesc = columnDesc(density.xs);
                detailDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                detailColumn = scope.createPanel("build.detail.column", detailDesc);
                detailColumn.setParent(detailScroll.content());
                detail = label("build.detail.text", detailColumn, "modules.output");
                registerCommands();
                densityObserver = scope.observeLayoutTokens(
                    [this]
                    {
                        density = Density::read(scope);
                        for (Widget widget : {static_cast<Widget>(projectChoice), static_cast<Widget>(filterChoice),
                                              static_cast<Widget>(buildButton), static_cast<Widget>(reloadButton),
                                              static_cast<Widget>(copyButton), static_cast<Widget>(copyOutputButton),
                                              static_cast<Widget>(revealOutputButton)})
                            widget.setSize({widget.size().x, density.control});
                        refreshButton.setSize({density.control, density.control});
                        detailScroll.setLayoutItem(basisItem(density.row * 5));
                        dirty = true;
                    });
                scope.commit();
            }
            ~BuildPanel()
            {
                scope.unobserveLayoutTokens(densityObserver);
                engine.control().unregisterOwner(OWNER);
            }
            Widget content() const { return root; }
            void update()
            {
                for (const auto& line : std::exchange(pending, {}))
                    (void)engine.control().handle(line);
                const bool visible = shown(scope, root);
                build.update(visible);
                if (!visible)
                {
                    wasShown = false;
                    return;
                }
                const auto revision = build.displayRevision();
                const auto locale =
                    engine.services().contains<Localization>() ? engine.services().get<Localization>().revision() : 0;
                if (!wasShown || dirty || revision != seenRevision || locale != seenLocale)
                {
                    refresh();
                    seenRevision = revision;
                    seenLocale = locale;
                    dirty = false;
                }
                const bool running = build.startedBuild(project).has_value();
                if (running != ownBuild)
                {
                    ownBuild = running;
                    dirty = true;
                }
                diagnostics->followText();
                outputTable->followText();
                pollOutput();
                const float width = std::max(0.0f, resolvedSize(scope, detailScroll).x - density.md);
                if (detailColumn.size().x != width)
                    detailColumn.setSize({width, detailColumn.size().y});
                detailScroll.setContentSize({width, scope.hierarchy().layoutSizeOf(detailColumn.entity()).y});
                wasShown = true;
            }

        private:
            void pollOutput()
            {
                const auto& path = build.outputPath(project);
                if (path != outputPath)
                {
                    outputPath = path;
                    outputText.clear();
                    outputLines.clear();
                    outputSize = ~0ull;
                    outputTable->setRowCount(0);
                    outputTable->rebindVisible();
                    outputFailed = false;
                    nextOutputRead = {};
                    scrollOutput = true;
                    outputSettled = false;
                    refreshOutputControls();
                }
                if (outputWorker.valid() && outputWorker.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                {
                    auto result = outputWorker.get();
                    if (result.path == outputPath)
                    {
                        bool changed = outputFailed != result.failed;
                        outputFailed = result.failed;
                        if (!result.failed && !result.unchanged)
                        {
                            outputSize = result.size;
                            outputText = std::move(result.text);
                            outputLines = std::move(result.lines);
                            outputTable->setRowCount(static_cast<u32>(outputLines.size()));
                            outputTable->rebindVisible();
                            scrollOutput = true;
                            changed = true;
                        }
                        outputSettled = !result.failed && result.unchanged && !build.startedBuild(project);
                        if (changed)
                            refreshOutputControls();
                    }
                }
                if (!showOutput || outputPath.empty())
                    return;
                const auto now = std::chrono::steady_clock::now();
                if (!outputSettled && !outputWorker.valid() && now >= nextOutputRead)
                {
                    outputWorker = std::async(std::launch::async, readOutputTail, outputPath, outputSize);
                    nextOutputRead = now + std::chrono::milliseconds(250);
                }
                if (followOutput && scrollOutput)
                {
                    outputTable->list.setScrollOffset(
                        {0, static_cast<float>(outputLines.size()) * outputTable->list.rowHeight()});
                    scrollOutput = false;
                }
            }
            void refreshOutputControls()
            {
                if (copyOutputButton.isInteractable() != !outputText.empty())
                    copyOutputButton.setInteractable(!outputText.empty());
                if (revealOutputButton.isInteractable() != !outputPath.empty())
                    revealOutputButton.setInteractable(!outputPath.empty());
                if (followBox.checked() != followOutput)
                    followBox.setChecked(followOutput);
                const auto status = tr(outputPath.empty()       ? "outputUnavailable"
                                           : outputFailed       ? "outputError"
                                           : outputText.empty() ? "outputWaiting"
                                                                : "outputTail");
                if (outputStatus.text() != status)
                    outputStatus.setText(status);
            }
            std::string tr(std::string_view key, const std::vector<MessageArg>& args = {}) const
            {
                const InternedString id("editor-build." + std::string(key));
                if (!engine.services().contains<Localization>())
                    return std::string(id.toView());
                std::string scratch;
                return std::string(engine.services().get<Localization>().resolve(id, args, scratch));
            }
            Button button(const char* id, const char* key, const Widget& parent)
            {
                auto desc = fitWidthDesc();
                desc.size.y = density.control;
                auto result = scope.createButton(id, "", desc);
                result.setLocalizedText(key);
                result.setParent(parent);
                bindFitPadding(result, SPACE_XS, SPACE_NONE);
                return result;
            }
            Label label(const char* id, const Widget& parent, const char* style)
            {
                auto desc = topLeft();
                desc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                desc.layoutItem = fixedItem();
                auto result = scope.createLabel(id, "", desc);
                result.setStyleClass(style);
                result.setParent(parent);
                return result;
            }
            void queue(const char* command, Json args = Json::object())
            {
                pending.push_back(Json{{"command", command}, {"args", std::move(args)}}.dump());
            }
            std::string diagnosticText() const
            {
                if (selectedDiagnostic >= record.diagnostics.size())
                    return {};
                const auto& item = record.diagnostics[selectedDiagnostic];
                return item.file + (item.line ? ":" + std::to_string(item.line) : "") + "\n" + item.severity +
                    (item.code.empty() ? "" : " " + item.code) + ": " + item.message;
            }
            void refresh()
            {
                projects = build.others([](std::string_view) { return false; });
                if (project.empty() && !projects.empty())
                {
                    const auto rank = [](const BuildRecord& row)
                    {
                        return row.state == "building" && !row.abandoned ? 0
                            : row.state == "failed"                      ? 1
                            : row.needsReload                            ? 2
                                                                         : 3;
                    };
                    const auto best = std::ranges::min_element(projects,
                                                               [&](const auto& a, const auto& b)
                                                               {
                                                                   return rank(a) != rank(b) ? rank(a) < rank(b)
                                                                                             : a.ageSec.value_or(1e30) <
                                                                           b.ageSec.value_or(1e30);
                                                               });
                    project = best->project;
                }
                if (!project.empty() && std::ranges::find(projects, project, &BuildRecord::project) == projects.end())
                    projects.push_back(BuildRecord{.project = project, .state = "none"});
                std::ranges::sort(projects, {}, &BuildRecord::project);
                std::vector<UiChoiceItem> choices;
                for (const auto& row : projects)
                    choices.push_back({.id = InternedString(row.project), .label = row.project});
                projectChoice.setItems(choices);
                const auto chosen = std::ranges::find(projects, project, &BuildRecord::project);
                projectChoice.setSelected(chosen == projects.end() ? -1 : static_cast<i32>(chosen - projects.begin()));
                record = chosen == projects.end() ? BuildRecord{.state = "none"} : *chosen;
                const auto started = build.startedBuild(project);
                ownBuild = started.has_value();
                if (started && (record.state != "building" || record.pid != *started))
                {
                    record.diagnostics.clear();
                    record.diagnosticsTotal = 0;
                    selectedDiagnostic = NO_DIAGNOSTIC;
                }
                const bool busy = ownBuild || (record.state == "building" && !record.abandoned);
                const bool hostOwned =
                    project == "SpiteEngine" || project == "SpiteEditor" || project == "SpiteEditorHost";
                buildButton.setLocalizedText(ownBuild ? "editor-build.cancel" : "editor-build.build");
                buildButton.setInteractable(!project.empty() && (ownBuild || (!busy && !hostOwned)));
                const bool canReload = record.needsReload || (project == "SpiteRenderPipeline" && record.state == "ok");
                reloadButton.setVisible(canReload && !hostOwned && !busy);
                reloadButton.setInteractable(!busy);
                headline.setText(project.empty() ? tr("chooseProject")
                                                 : tr(busy                           ? "building"
                                                          : record.abandoned         ? "interrupted"
                                                          : record.state == "ok"     ? "succeeded"
                                                          : record.state == "failed" ? "failed"
                                                                                     : "notBuilt"));
                headline.setStyleClass(!busy && record.state == "failed" ? "build.headline form.error"
                                                                         : "build.headline");
                u32 errors = 0, warnings = 0;
                visibleRows.clear();
                for (u32 i = 0; i < record.diagnostics.size(); ++i)
                {
                    const auto& item = record.diagnostics[i];
                    errors += item.severity == "error";
                    warnings += item.severity == "warning";
                    if (severity == 0 || item.severity == (severity == 1 ? "error" : "warning"))
                        visibleRows.push_back(i);
                }
                const std::string configuration = record.configuration.empty()
                    ? utf8Path(engine.modules().searchDirectory().filename())
                    : record.configuration;
                std::string age;
                if (record.ageSec && !busy)
                {
                    const auto value = buildAgeOf(*record.ageSec);
                    age = value.unit == BuildAgeUnit::Now ? tr("now")
                                                          : std::to_string(value.count) + " " +
                            tr(value.unit == BuildAgeUnit::Minutes     ? "minutesAgo"
                                   : value.unit == BuildAgeUnit::Hours ? "hoursAgo"
                                                                       : "daysAgo");
                }
                summary.setText(project.empty() ? ""
                                                : configuration + (age.empty() ? "" : " · " + age) + " · " +
                                        tr("counts",
                                           {{"errors", static_cast<std::int64_t>(errors)},
                                            {"warnings", static_cast<std::int64_t>(warnings)}}));
                std::string note = hostOwned ? tr("restart") : record.needsReload && !busy ? tr("reloadHint") : "";
                if (busy && !ownBuild)
                    note = tr("externalBuild");
                if (record.untracked && !busy)
                    note += (note.empty() ? "" : " ") + tr("untracked");
                if (record.diagnosticsTotal > record.diagnostics.size())
                    note += (note.empty() ? "" : " ") + tr("truncated");
                hint.setText(note);
                hint.setVisible(!note.empty());
                const std::array<UiChoiceItem, 3> filterItems{
                    {{.label = tr("all")}, {.label = tr("errors")}, {.label = tr("warnings")}}};
                filterChoice.setItems(filterItems);
                filterChoice.setSelected(severity);
                diagnostics->setRowCount(static_cast<u32>(visibleRows.size()));
                diagnostics->rebindVisible();
                diagnostics->widget().setVisible(!visibleRows.empty());
                empty.setVisible(visibleRows.empty());
                empty.setText(tr(project.empty()              ? "empty"
                                     : busy                   ? "waiting"
                                     : record.state == "none" ? "noBuild"
                                     : record.state == "failed" && record.diagnostics.empty()
                                     ? "failureWithoutDiagnostics"
                                     : severity != 0 ? "noMatches"
                                                     : "noDiagnostics"));
                if (selectedDiagnostic >= record.diagnostics.size())
                    selectedDiagnostic = NO_DIAGNOSTIC;
                copyButton.setInteractable(selectedDiagnostic != NO_DIAGNOSTIC);
                detailScroll.setVisible(selectedDiagnostic != NO_DIAGNOSTIC);
                detail.setText(diagnosticText());
                tabs.setActive(showOutput ? 0 : 1);
                outputPane.setVisible(showOutput);
                filters.setVisible(!showOutput);
                diagnostics->widget().setVisible(!showOutput && !visibleRows.empty());
                empty.setVisible(!showOutput && visibleRows.empty());
                detailScroll.setVisible(!showOutput && selectedDiagnostic != NO_DIAGNOSTIC);
                refreshOutputControls();
            }
            void registerCommands()
            {
                auto& control = engine.control();
                control.registerViewCommand("buildPanelView", OWNER, "Chooses live output or diagnostics",
                                            {requiredArg("output", ArgKind::Boolean, "show live output")},
                                            [this](const ControlArgs& args, IFormatWriter&)
                                            {
                                                showOutput = args.boolean("output").value_or(false);
                                                dirty = true;
                                                return std::string{};
                                            });
                control.registerViewCommand("buildPanelFollow", OWNER, "Follows new build output",
                                            {requiredArg("follow", ArgKind::Boolean, "follow output")},
                                            [this](const ControlArgs& args, IFormatWriter&)
                                            {
                                                followOutput = args.boolean("follow").value_or(true);
                                                scrollOutput = true;
                                                dirty = true;
                                                return std::string{};
                                            });
                control.registerViewCommand("buildPanelCopyOutput", OWNER, "Copies the displayed build-output tail", {},
                                            [this](const ControlArgs&, IFormatWriter&)
                                            {
                                                editorUi(engine).ui().setClipboardText(outputText);
                                                return std::string{};
                                            });
                control.registerViewCommand("buildPanelRevealOutput", OWNER, "Reveals the complete build log", {},
                                            [this](const ControlArgs&, IFormatWriter&)
                                            {
                                                return !outputPath.empty() && revealInFileManager(outputPath)
                                                    ? std::string{}
                                                    : std::string("build log unavailable");
                                            });
                control.registerViewCommand("buildPanelSelect", OWNER, "Selects a project in Build",
                                            {requiredArg("project", ArgKind::String, "project name")},
                                            [this](const ControlArgs& args, IFormatWriter&)
                                            {
                                                project = args.string("project").value_or("");
                                                if (build.startedBuild(project))
                                                    showOutput = true;
                                                selectedDiagnostic = NO_DIAGNOSTIC;
                                                dirty = true;
                                                return std::string{};
                                            });
                control.registerViewCommand("buildPanelFilter", OWNER, "Filters build diagnostics",
                                            {requiredArg("severity", ArgKind::Integer, "0 all, 1 errors, 2 warnings")},
                                            [this](const ControlArgs& args, IFormatWriter&)
                                            {
                                                const auto value = args.integer("severity").value_or(0);
                                                if (value > 2)
                                                    return std::string("invalid diagnostic filter");
                                                severity = static_cast<u32>(value);
                                                selectedDiagnostic = NO_DIAGNOSTIC;
                                                dirty = true;
                                                return std::string{};
                                            });
                control.registerViewCommand("buildPanelDiagnostic", OWNER, "Selects a diagnostic for full details",
                                            {requiredArg("row", ArgKind::Integer, "diagnostic index")},
                                            [this](const ControlArgs& args, IFormatWriter&)
                                            {
                                                const auto row = args.integer("row").value_or(NO_DIAGNOSTIC);
                                                if (row >= record.diagnostics.size())
                                                    return std::string("diagnostic unavailable");
                                                selectedDiagnostic = static_cast<u32>(row);
                                                dirty = true;
                                                return std::string{};
                                            });
                control.registerViewCommand("buildPanelCopy", OWNER, "Copies the selected diagnostic", {},
                                            [this](const ControlArgs&, IFormatWriter&)
                                            {
                                                if (selectedDiagnostic >= record.diagnostics.size())
                                                    return std::string("select a diagnostic first");
                                                editorUi(engine).ui().setClipboardText(diagnosticText());
                                                return std::string{};
                                            });
                control.registerViewCommand("buildPanelRefresh", OWNER, "Refreshes build records", {},
                                            [this](const ControlArgs&, IFormatWriter&)
                                            {
                                                build.refreshNow();
                                                outputSettled = false;
                                                nextOutputRead = {};
                                                dirty = true;
                                                return std::string{};
                                            });
                control.registerViewCommand(
                    "buildPanelRun", OWNER, "Builds the selected project or cancels its editor-started build", {},
                    [this](const ControlArgs&, IFormatWriter&)
                    {
                        if (project.empty())
                            return std::string("select a project first");
                        if (project == "SpiteEngine" || project == "SpiteEditor" || project == "SpiteEditorHost")
                            return tr("restart");
                        if (!build.startedBuild(project))
                            showOutput = true;
                        const auto reply =
                            Json::parse(engine.control().handle(Json{
                                            {"command", build.startedBuild(project) ? "cancelBuild" : "buildProject"},
                                            {"args", {{"project", project}}}}
                                                                    .dump()),
                                        nullptr, false);
                        dirty = true;
                        return reply.is_object() && reply.value("ok", false)
                            ? std::string{}
                            : reply.value("error", std::string("build request failed"));
                    });
                control.registerViewCommand(
                    "buildPanelReload", OWNER, "Reloads the selected project's loaded module", {},
                    [this](const ControlArgs&, IFormatWriter&)
                    {
                        build.refreshNow();
                        const auto selected = build.record(project);
                        const bool pipeline = project == "SpiteRenderPipeline";
                        if (!pipeline && (!selected || selected->module.empty()))
                            return std::string("project module is not loaded or its mapping is ambiguous");
                        const auto reply =
                            Json::parse(engine.control().handle(
                                            Json{{"command", pipeline ? "reloadRenderPipeline" : "reloadModule"},
                                                 {"args", pipeline ? Json::object() : Json{{"name", selected->module}}}}
                                                .dump()),
                                        nullptr, false);
                        dirty = true;
                        return reply.is_object() && reply.value("ok", false)
                            ? std::string{}
                            : reply.value("error", std::string("reload failed"));
                    });
                control.registerQuery("buildPanelState", OWNER, "The selected build project and displayed diagnostics",
                                      {},
                                      [this](const ControlArgs&, IFormatWriter& out)
                                      {
                                          build.refreshNow();
                                          refresh();
                                          out.writeString("project", project);
                                          out.writeString("state", ownBuild ? "building" : record.state);
                                          out.writeU32("diagnostics", static_cast<u32>(record.diagnostics.size()));
                                          out.writeU32("visibleDiagnostics", static_cast<u32>(visibleRows.size()));
                                          out.writeU32("severity", severity);
                                          out.writeBool("output", showOutput);
                                          out.writeU32("outputLines", static_cast<u32>(outputLines.size()));
                                          out.writeString("outputPath", utf8Path(outputPath));
                                          return std::string{};
                                      });
            }
            Engine& engine;
            EditorBuildState& build;
            UiScope scope;
            Density density;
            Panel root, toolbar, detailColumn;
            Panel outputPane, filters;
            TabBar tabs;
            Checkbox followBox;
            Dropdown projectChoice, filterChoice;
            Button buildButton, refreshButton, reloadButton, copyButton;
            Button copyOutputButton, revealOutputButton;
            Label headline, summary, hint, empty, detail;
            Label outputStatus;
            ScrollView detailScroll;
            std::unique_ptr<Table> diagnostics;
            std::unique_ptr<Table> outputTable;
            std::filesystem::path outputPath;
            std::vector<std::string> outputLines;
            std::string outputText;
            std::future<OutputTail> outputWorker;
            std::chrono::steady_clock::time_point nextOutputRead{};
            u64 outputSize = ~0ull;
            bool showOutput = false, followOutput = true, scrollOutput = true, outputFailed = false,
                 outputSettled = false;
            std::vector<BuildRecord> projects;
            BuildRecord record;
            std::vector<u32> visibleRows;
            std::vector<std::string> pending;
            std::string project;
            u32 severity = 0, selectedDiagnostic = NO_DIAGNOSTIC;
            u32 densityObserver = 0;
            u64 seenRevision = ~0ull, seenLocale = ~0ull;
            bool dirty = true, wasShown = false, ownBuild = false;
        };
        const EditorPanelRegistrar REGISTRATION{
            {.name = "build",
             .category = "project",
             .order = 125,
             .dock = {.target = "console"},
             .zone = "EditorBuild",
             .make = [](EditorPanelContext& c)
             { return makeEditorPanel<BuildPanel>(c.engine, EditorModelAccess::build(c.model)); },
             .openByDefault = false}};
    } // namespace
} // namespace spite
