#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>

#include "EditorPanelUi.hpp"
#include "ProfilerViews.hpp"
#include "engine/CaptureCompare.hpp"
#include "ui/UiCanvas.hpp"

namespace spite
{
    using namespace panelui;

    namespace
    {
        constexpr u32 MAX_ERROR_CHARS = 200;
        constexpr u32 MAX_FLAG_CHARS = 260;
        constexpr u64 STATE_ROWS = 32;
        constexpr float PLOT_WIDTH = 320.0f;
        constexpr float DROPDOWN_WIDTH = 240.0f; // "#12  2026-10-09 11:47:03" and the arrow
        constexpr float PLOT_LEFT = 8.0f, PLOT_RIGHT = 8.0f, PLOT_TOP = 34.0f, PLOT_BOTTOM = 18.0f;
        constexpr std::array<std::string_view, COMPARE_SORTS> SORT_CHOICES{"impact", "mean", "p99", "name", "verdict"};
        constexpr std::array<std::string_view, 2> PLOT_CHOICES{"histogram", "cdf"};
        // the table's columns: metric, baseline, candidate, delta, change, p99 delta, cost, verdict. A column that
        // sorts names its sort; -1: the column does not sort
        constexpr std::array<i32, 8> SORT_OF_CELL{3, -1, -1, -1, 1, 2, 0, 4};
        constexpr std::array<u32, COMPARE_SORTS> CELL_OF_SORT{6, 4, 5, 0, 7};

        // the unix time of a file in ms; 0 when it cannot be read
        std::int64_t fileTimeMs(const std::filesystem::path& path)
        {
            std::error_code error;
            const auto time = std::filesystem::last_write_time(path, error);
            if (error)
            {
                return 0;
            }
            const auto system = std::chrono::clock_cast<std::chrono::system_clock>(time);
            return std::chrono::duration_cast<std::chrono::milliseconds>(system.time_since_epoch()).count();
        }

        struct Data
        {
            CompareOutcome outcome;
            std::string error;
        };

        u32 baselineColor(const UiScope& scope, const u8 alpha = 255)
        {
            return withAlpha(canvasColor(scope, "canvas.series.a"), alpha);
        }
        u32 candidateColor(const UiScope& scope, const u8 alpha = 255)
        {
            return withAlpha(canvasColor(scope, "canvas.series.b"), alpha);
        }

        // the table's verdict cell and the summary's counts
        const char* verdictKey(const CompareVerdict verdict)
        {
            switch (verdict)
            {
            case CompareVerdict::Regress:
                return "compare.verdict.regress";
            case CompareVerdict::Improve:
                return "compare.verdict.improve";
            case CompareVerdict::Flat:
                return "compare.verdict.flat";
            case CompareVerdict::Unknown:
                return "compare.verdict.unknown";
            case CompareVerdict::Info:
                return "compare.verdict.info";
            }
            return "compare.verdict.unknown";
        }

        class CompareView final : public ProfilerView
        {
        public:
            explicit CompareView(const ProfilerViewHost& host) :
                m_host(host), m_generation(std::make_shared<std::atomic<u64>>(0)),
                m_theme(*host.scope, [this] { m_dirty = true; })
            {
                UiScope& scope = *m_host.scope;
                DensityFollower& density = *m_host.density;
                m_root = stackPanel(scope, "profiler.compare", Vec2{}, UiLayoutDirection::Column,
                                    UiLayoutAlign::Stretch, true);
                m_root.setLayoutItem(fillItem());
                Panel sides = density.track(stackPanel(scope, "profiler.compare.sides", Vec2{}, UiLayoutDirection::Row),
                                            HEIGHT_CONTROL);
                sides.setParent(m_root);
                const auto dropdown = [&](Dropdown& out, const char* id, const char* arg)
                {
                    UiDropdownDesc desc;
                    desc.rect = barDesc(Vec2{DROPDOWN_WIDTH, 0.0f});
                    out = density.track(scope.createDropdown(id, desc), HEIGHT_ROW);
                    out.setParent(sides);
                    out.onChange(
                        [this, arg](const u32 index)
                        {
                            m_host.send({{"command", "profilerCompare"},
                                         {"args", {{arg, index == 0 || index > m_ids.size() ? 0 : m_ids[index - 1]}}}});
                        });
                };
                dropdown(m_baselineChoice, "profiler.compare.baseline", "baseline");
                dropdown(m_candidateChoice, "profiler.compare.candidate", "candidate");
                const auto button = [&](Panel& parent, Button& out, const char* textKey, const char* nameKey, Json args,
                                        const float width = 68.0f)
                {
                    out = density.track(scope.createButton("profiler.compare.button", "", barDesc(Vec2{width, 0.0f})),
                                        HEIGHT_ROW);
                    out.setLocalizedText(profilerKey(textKey));
                    out.setParent(parent);
                    out.setAccessibleNameKey(profilerKey(nameKey));
                    out.onClick([this, args = std::move(args)]
                                { m_host.send({{"command", "profilerCompare"}, {"args", args}}); });
                };
                button(sides, m_swap, "compare.swap", "compare.swap.name", Json{{"swap", true}});
                m_forceButton = density.track(
                    scope.createButton("profiler.compare.force", "", barDesc(Vec2{68.0f, 0.0f})), HEIGHT_ROW);
                m_forceButton.setLocalizedText(profilerKey("compare.force"));
                m_forceButton.setParent(sides);
                m_forceButton.setAccessibleNameKey(profilerKey("compare.force.name"));
                m_forceButton.setTooltip(profilerKey("compare.force.hint"));
                m_forceButton.onClick(
                    [this] { m_host.send({{"command", "profilerCompare"}, {"args", {{"force", !m_force}}}}); });
                Panel sorts = density.track(
                    stackPanel(scope, "profiler.compare.options", Vec2{}, UiLayoutDirection::Row), HEIGHT_CONTROL);
                sorts.setParent(m_root);
                m_allButton = density.track(scope.createButton("profiler.compare.all", "", barDesc(Vec2{76.0f, 0.0f})),
                                            HEIGHT_ROW);
                m_allButton.setLocalizedText(profilerKey("compare.all"));
                m_allButton.setParent(sorts);
                m_allButton.setAccessibleNameKey(profilerKey("compare.all.name"));
                m_allButton.onClick(
                    [this] { m_host.send({{"command", "profilerCompare"}, {"args", {{"all", !m_filter.all}}}}); });
                button(sorts, m_plotButton, "compare.plot", "compare.plot.name", Json{{"plot", "toggle"}}, 110.0f);
                m_summary = density.track(readOnlyText(scope, "profiler.compare.summary", barDesc(Vec2{})), HEIGHT_ROW);
                m_summary.setParent(m_root);
                m_summary.setAccessibleNameKey(profilerKey("compare.summary.name"));
                m_errors = density.track(readOnlyText(scope, "profiler.compare.errors", barDesc(Vec2{})), HEIGHT_ROW);
                m_errors.setParent(m_root);
                m_errors.setAccessibleNameKey(profilerKey("compare.errors.name"));
                m_warnings =
                    density.track(readOnlyText(scope, "profiler.compare.warnings", barDesc(Vec2{})), HEIGHT_ROW);
                m_warnings.setParent(m_root);
                m_warnings.setAccessibleNameKey(profilerKey("compare.warnings.name"));
                Panel body =
                    stackPanel(scope, "profiler.compare.body", Vec2{}, UiLayoutDirection::Row, UiLayoutAlign::Stretch);
                body.setLayoutItem(fillItem());
                body.setParent(m_root);
                m_table = std::make_unique<Table>(
                    scope,
                    TableDesc{.id = "profiler.compareTable",
                              .columns = {100.0f, 60.0f, 60.0f, 66.0f, 56.0f, 66.0f, 76.0f},
                              .cells = 8,
                              .read = [this](const u32 row, const std::span<std::string> cells, std::string&)
                              { readRow(m_order[row], cells); },
                              .headers = {{profilerKey("compare.col.metric"), true},
                                          {profilerKey("compare.col.baseline")},
                                          {profilerKey("compare.col.candidate")},
                                          {profilerKey("compare.col.delta")},
                                          {profilerKey("compare.col.change"), true},
                                          {profilerKey("compare.col.p99"), true},
                                          {profilerKey("compare.col.cost"), true},
                                          {profilerKey("compare.col.verdict"), true}},
                              .sort =
                                  [this](const u32 cell)
                              {
                                  m_host.send(
                                      {{"command", "profilerCompare"},
                                       {"args",
                                        {{"sort", std::string(SORT_CHOICES[static_cast<u32>(SORT_OF_CELL[cell])])},
                                         {"toggle", true}}}});
                              },
                              .headerName = [this](u32, const std::string_view title, const std::string_view order)
                              { return m_host.sortName(title, order); },
                              .lastMin = 90.0f});
                Widget table = m_table->widget();
                table.setParent(body);
                table.setLayoutItem(fillItem());
                m_table->list.setAccessibleNameKey(profilerKey("compare.list.name"));
                UiCanvasDesc canvasDesc;
                canvasDesc.rect = barDesc(Vec2{PLOT_WIDTH, 0.0f});
                canvasDesc.rect.layoutItem = fixedItem();
                canvasDesc.maxPrimitives = 4000;
                m_canvas = scope.createCanvas("profiler.compare.plot", canvasDesc);
                m_canvas.setParent(body);
                m_canvas.setAccessibleNameKey(profilerKey("compare.canvas.name"));
                syncChoices();
            }

            [[nodiscard]] Widget root() const override { return m_root; }

            void activate(const bool active) override
            {
                m_active = active;
                if (!active)
                {
                    m_generation->fetch_add(1);
                    m_loading = false;
                    return;
                }
                refreshCaptures();
                if (!m_loaded && m_baseline != 0 && m_candidate != 0)
                {
                    request();
                }
                m_dirty = true;
            }

            void captureChanged(const CaptureId, const CaptureInfo&) override {}
            void selectionChanged(const ProfilerSelectionComponent&) override {}

            bool take(ProfilerEvent& event) override
            {
                if (event.token != m_value || event.token != m_generation->load())
                {
                    return true;
                }
                m_loading = false;
                if (const auto data = std::static_pointer_cast<Data>(event.payload))
                {
                    m_data = std::move(*data);
                    m_loaded = m_data.error.empty();
                    m_data.error = profilerSafeText(m_data.error, MAX_ERROR_CHARS);
                }
                else if (!event.error.empty() && event.error != "superseded")
                {
                    m_data = {};
                    m_data.error = profilerSafeText(event.error, MAX_ERROR_CHARS);
                }
                rebuildRows();
                m_dirty = true;
                return true;
            }

            void update() override
            {
                if (m_host.store->generation() != m_storeGeneration)
                {
                    refreshCaptures();
                }
                const Vec2 size = m_host.laidOutSize(m_canvas);
                if (*m_host.locale != m_localeUsed)
                {
                    m_localeUsed = *m_host.locale;
                    refreshCaptures();
                    m_table->rebindVisible();
                    m_dirty = true;
                }
                if (m_dirty || size != m_drawnSize)
                {
                    redraw(size);
                }
            }

            [[nodiscard]] bool busy() const override { return m_loading; }

            [[nodiscard]] float preferredHeight() const override { return 380.0f; }

            void writeState(IFormatWriter& out) override
            {
                if (m_host.store->generation() != m_storeGeneration)
                {
                    refreshCaptures(); // a hidden view's query answers for the store as it is now
                }
                out.writeBool("loading", m_loading);
                out.writeBool("loaded", m_loaded);
                out.writeString("error", m_data.error);
                out.writeU32("baselineId", m_baseline);
                out.writeU32("candidateId", m_candidate);
                out.writeBool("force", m_force);
                out.writeString("plot", PLOT_CHOICES[m_plot]);
                out.writeU32("choices", static_cast<u32>(m_ids.size()));
                // the dropdowns' rows are cut to the control's width; these are whole
                out.beginArray("captures", static_cast<u32>(m_ids.size()));
                for (sizet i = 0; i < m_ids.size(); ++i)
                {
                    out.beginObject({});
                    out.writeU32("id", m_ids[i]);
                    out.writeString("label", m_shortLabels[i]);
                    out.writeString("full", m_fullLabels[i]);
                    out.endObject();
                }
                out.endArray();
                out.writeString("baselineLabel", labelOf(m_baseline));
                out.writeString("candidateLabel", labelOf(m_candidate));
                out.beginObject("list");
                writeTableStats(out, *m_table);
                out.endObject();
                out.writeU64("columnsDrawn", m_columnsDrawn);
                if (m_loaded)
                {
                    out.beginObject("result");
                    writeCompare(out, m_data.outcome, m_filter, 0, STATE_ROWS, true);
                    out.endObject();
                }
            }

            void registerCommands() override
            {
                m_host.control->registerViewCommand(
                    "profilerCompare", m_host.owner,
                    "sets what the Profiler's Compare view compares and how its zone table is ordered; the view runs "
                    "the same comparison as profileCompare (PerfCli compare on the two zones reports) and shows its "
                    "rows, warnings and the blocking environment differences (machine, scene, build, legacy profiler "
                    "attachment, Vulkan "
                    "layers) as PerfCli reports them; profilerPanes shows the result under `compare`. The dropdowns "
                    "and buttons send this same command",
                    {optionalArg("baseline", ArgKind::Integer, "capture id of the baseline (side A), 0: none",
                                 "unchanged"),
                     optionalArg("candidate", ArgKind::Integer, "capture id of the candidate (side B), 0: none",
                                 "unchanged"),
                     optionalArg("swap", ArgKind::Boolean, "exchange the two sides", "false"),
                     optionalArg("force", ArgKind::Boolean, "compare although the environments differ", "unchanged"),
                     optionalArg("sort", ArgKind::String, "order of the zone table", "unchanged", SORT_CHOICES),
                     optionalArg("toggle", ArgKind::Boolean, "with sort: the same sort again reverses the order",
                                 "false"),
                     optionalArg("descending", ArgKind::Boolean, "largest first (name: Z first)", "true; name: false"),
                     optionalArg("all", ArgKind::Boolean, "also the flat and info rows", "unchanged"),
                     optionalArg("filter", ArgKind::String, "case-insensitive substring of the metric", "unchanged"),
                     optionalArg("plot", ArgKind::String, "histogram, cdf or toggle", "unchanged")},
                    [this](const ControlArgs& args, IFormatWriter& out) { return apply(args, out); });
            }

        private:
            std::string apply(const ControlArgs& args, IFormatWriter& out)
            {
                CaptureId baseline = m_baseline, candidate = m_candidate;
                bool force = m_force;
                CompareFilter filter = m_filter;
                u32 plot = m_plot;
                for (const auto& [key, target] : {std::pair{"baseline", &baseline}, std::pair{"candidate", &candidate}})
                {
                    if (const std::optional<u64> id = args.integer(key))
                    {
                        *target = static_cast<CaptureId>(std::min<u64>(*id, UINT32_MAX));
                        CaptureInfo info;
                        if (*target != 0 &&
                            (!m_host.store->info(*target, info) || info.state != CaptureInfo::State::Ready ||
                             info.report.empty()))
                        {
                            return "capture " + std::to_string(*target) + " is not a ready capture with a zones report";
                        }
                    }
                }
                if (args.boolean("swap").value_or(false))
                {
                    std::swap(baseline, candidate);
                }
                if (baseline != 0 && baseline == candidate)
                {
                    return "baseline and candidate are the same capture";
                }
                force = args.boolean("force").value_or(force);
                if (const std::optional<std::string> sort = args.string("sort"))
                {
                    const auto it = std::ranges::find(SORT_CHOICES, std::string_view(*sort));
                    if (it == SORT_CHOICES.end())
                    {
                        return std::string("sort is impact, mean, p99, name or verdict");
                    }
                    const auto next = static_cast<CompareSort>(it - SORT_CHOICES.begin());
                    const bool same = next == filter.sort;
                    filter.sort = next;
                    filter.descending =
                        args.boolean("descending")
                            .value_or(same && args.boolean("toggle").value_or(false) ? !filter.descending
                                                                                     : next != CompareSort::Name);
                }
                else if (const std::optional<bool> descending = args.boolean("descending"))
                {
                    filter.descending = *descending;
                }
                filter.all = args.boolean("all").value_or(filter.all);
                if (const std::optional<std::string> name = args.string("filter"))
                {
                    if (name->size() > 256)
                    {
                        return std::string("filter is at most 256 bytes");
                    }
                    if (compareSafeText(*name, 256) != *name)
                    {
                        return std::string("filter is not printable UTF-8");
                    }
                    filter.name = *name;
                }
                if (const std::optional<std::string> shape = args.string("plot"))
                {
                    if (*shape == "toggle")
                    {
                        plot = 1 - plot;
                    }
                    else if (const auto it = std::ranges::find(PLOT_CHOICES, std::string_view(*shape));
                             it != PLOT_CHOICES.end())
                    {
                        plot = static_cast<u32>(it - PLOT_CHOICES.begin());
                    }
                    else
                    {
                        return std::string("plot is histogram, cdf or toggle");
                    }
                }
                const bool rerun = baseline != m_baseline || candidate != m_candidate || force != m_force;
                m_baseline = baseline;
                m_candidate = candidate;
                m_force = force;
                m_filter = std::move(filter);
                m_plot = plot;
                if (rerun)
                {
                    m_generation->fetch_add(1);
                    m_loading = false;
                    m_loaded = false;
                    m_data = {};
                    if (m_active)
                    {
                        request();
                    }
                }
                rebuildRows();
                syncChoices();
                m_dirty = true;
                out.writeU32("baseline", m_baseline);
                out.writeU32("candidate", m_candidate);
                out.writeBool("force", m_force);
                out.writeString("sort", compareSortName(m_filter.sort));
                out.writeBool("descending", m_filter.descending);
                out.writeBool("all", m_filter.all);
                out.writeString("plot", PLOT_CHOICES[m_plot]);
                return {};
            }

            void request()
            {
                if (!m_host.data || m_baseline == 0 || m_candidate == 0)
                {
                    return;
                }
                m_loading = true;
                m_value = m_generation->fetch_add(1) + 1;
                CaptureStore* store = m_host.store;
                const CaptureId baseline = m_baseline, candidate = m_candidate;
                const bool force = m_force;
                m_host.data->requestTask(
                    static_cast<u32>(ProfilerViewId::Compare), m_generation, m_value,
                    [store, baseline, candidate, force](const CaptureCancelFn& cancelled,
                                                        std::string&) -> std::shared_ptr<void>
                    {
                        auto data = std::make_shared<Data>();
                        data->error = compareCaptures(*store, baseline, candidate, force, cancelled, {}, data->outcome);
                        return data;
                    },
                    true);
            }

            // The captures a side can be: ready, with a zones report. A side that is no longer one is cleared.
            void refreshCaptures()
            {
                m_storeGeneration = m_host.store->generation();
                std::vector<CaptureId> ids;
                std::vector<std::string> shorts, fulls;
                std::vector<UiChoiceItem> items{{InternedString("none"), "(none)", profilerKey("compare.none"), true}};
                for (const CaptureInfo& info : m_host.store->list())
                {
                    if (info.state == CaptureInfo::State::Ready && !info.report.empty())
                    {
                        // the time and the id tell captures apart; a report name's long common prefix does not
                        const std::int64_t modified = fileTimeMs(info.path);
                        const std::string time = modified > 0 ? profilerLocalTime(modified) : info.name;
                        ids.push_back(info.id);
                        shorts.push_back(
                            m_host.text("compare.capture.label", {{"id", std::to_string(info.id)}, {"time", time}}));
                        fulls.push_back(m_host.text("compare.capture.full",
                                                    {{"id", std::to_string(info.id)},
                                                     {"time", time},
                                                     {"frames", m_host.whole(info.frames)},
                                                     {"report", profilerSafeText(info.report, 120)}}));
                        items.push_back({InternedString("capture" + std::to_string(info.id)), shorts.back(), {}, true});
                    }
                }
                const auto present = [&ids](const CaptureId id) { return std::ranges::find(ids, id) != ids.end(); };
                const bool lost =
                    (m_baseline != 0 && !present(m_baseline)) || (m_candidate != 0 && !present(m_candidate));
                if (lost)
                {
                    m_baseline = present(m_baseline) ? m_baseline : 0;
                    m_candidate = present(m_candidate) ? m_candidate : 0;
                    m_generation->fetch_add(1);
                    m_loading = false;
                    m_loaded = false;
                    m_data = {};
                    rebuildRows();
                    m_dirty = true;
                }
                if (ids != m_ids || shorts != m_shortLabels || fulls != m_fullLabels)
                {
                    m_ids = std::move(ids);
                    m_shortLabels = std::move(shorts);
                    m_fullLabels = std::move(fulls);
                    m_baselineChoice.setItems(items);
                    m_candidateChoice.setItems(items);
                }
                syncChoices();
            }

            // the capture's whole label, "(none)" for no capture
            [[nodiscard]] std::string labelOf(const CaptureId id) const
            {
                const auto it = std::ranges::find(m_ids, id);
                return it == m_ids.end() ? m_host.text("compare.none")
                                         : m_fullLabels[static_cast<sizet>(it - m_ids.begin())];
            }

            void syncChoices()
            {
                const auto index = [this](const CaptureId id)
                {
                    const auto it = std::ranges::find(m_ids, id);
                    return it == m_ids.end() ? 0 : static_cast<i32>(it - m_ids.begin()) + 1;
                };
                if (m_baselineChoice.selected() != index(m_baseline))
                {
                    m_baselineChoice.setSelected(index(m_baseline));
                }
                if (m_candidateChoice.selected() != index(m_candidate))
                {
                    m_candidateChoice.setSelected(index(m_candidate));
                }
                // the face shows the short label; the name says the whole one
                m_baselineChoice.setAccessibleName(
                    m_host.text("compare.side",
                                {{"side", m_host.text("compare.baseline.name")}, {"capture", labelOf(m_baseline)}}));
                m_candidateChoice.setAccessibleName(
                    m_host.text("compare.side",
                                {{"side", m_host.text("compare.candidate.name")}, {"capture", labelOf(m_candidate)}}));
            }

            void rebuildRows()
            {
                m_order.clear();
                if (m_loaded)
                {
                    m_order = compareOrder(m_data.outcome.result, m_filter);
                }
                m_table->setSort(static_cast<i32>(CELL_OF_SORT[static_cast<u32>(m_filter.sort)]), m_filter.descending);
                m_table->setRowCount(static_cast<u32>(m_order.size()));
                m_table->rebindVisible();
            }

            // A value in its kind's unit: ms below 0.1 as microseconds, memory in MB, ratios as x
            [[nodiscard]] std::string value(const std::string& kind, const double v, const bool sign) const
            {
                const std::string prefix = sign && v > 0.0 ? "+" : "";
                if (kind == "ms" || kind == "msFine")
                {
                    return std::fabs(v) > 0.0 && std::fabs(v) < 0.1 ? prefix + m_host.precise(v * 1000.0) + " \xC2\xB5s"
                                                                    : prefix + m_host.precise(v) + " ms";
                }
                if (kind == "mb")
                {
                    return prefix + m_host.precise(v) + " MB";
                }
                if (kind == "ratio")
                {
                    return prefix + m_host.precise(v) + "x";
                }
                if (kind == "perFrame")
                {
                    return prefix + m_host.precise(v) + "/frame";
                }
                return prefix + m_host.precise(v);
            }

            void readRow(const u32 index, const std::span<std::string> cells) const
            {
                const CompareRow& row = m_data.outcome.result.rows[index];
                cells[0] = profilerSafeText(row.metric, 80);
                cells[1] = row.absentA ? "-" : value(row.kind, row.a, false);
                cells[2] = row.absentB ? "-" : value(row.kind, row.b, false);
                cells[3] = value(row.kind, row.delta, true);
                // the change, the p99 delta and the cost have their unit in the header
                cells[4] = (row.rel > 0.0 ? "+" : "") + m_host.precise(std::round(row.rel * 1000.0) / 10.0);
                cells[5] = row.p99A && row.p99B
                    ? (*row.p99B > *row.p99A ? "+" : "") + m_host.precise(*row.p99B - *row.p99A)
                    : "-";
                cells[6] = row.deltaMsPerFrame
                    ? (*row.deltaMsPerFrame > 0.0 ? "+" : "") + m_host.precise(*row.deltaMsPerFrame)
                    : "-";
                cells[7] = m_host.text(verdictKey(row.verdict)) +
                    (row.floored ? " " + m_host.text("compare.floored") : std::string()) +
                    (row.renamedTo.empty() ? "" : " > " + profilerSafeText(row.renamedTo, 40));
            }

            [[nodiscard]] std::string joined(const std::vector<std::string>& texts, const u32 limit) const
            {
                std::string text;
                for (const std::string& item : texts)
                {
                    text += (text.empty() ? "" : " | ") + item;
                }
                return profilerSafeText(profilerRoundedNumbers(text), limit);
            }

            void redraw(const Vec2 size)
            {
                m_dirty = false;
                m_drawnSize = size;
                std::string summary, errors, warnings;
                if (m_baseline == 0 || m_candidate == 0)
                {
                    summary = m_host.text("compare.choose");
                }
                else if (!m_data.error.empty())
                {
                    summary = m_host.text("status.error", {{"error", m_data.error}});
                }
                else if (!m_loaded)
                {
                    summary = m_host.text("compare.comparing");
                }
                else
                {
                    const CompareResult& result = m_data.outcome.result;
                    std::string counts;
                    for (std::size_t v = 0; v < COMPARE_VERDICTS; ++v)
                    {
                        counts += std::string(v ? ", " : "") +
                            m_host.text("compare.count",
                                        {{"verdict", m_host.text(verdictKey(static_cast<CompareVerdict>(v)))},
                                         {"count", m_host.whole(result.counts[v])}});
                    }
                    summary = m_host.text("compare.summary",
                                          {{"group", profilerSafeText(result.group, 80)},
                                           {"counts", counts},
                                           {"sets", m_host.whole(result.replicateSets)}});
                    if (!result.errors.empty())
                    {
                        errors = m_host.text(result.blocked ? "compare.blocked" : "compare.forced",
                                             {{"flags", joined(result.errors, MAX_FLAG_CHARS)}});
                    }
                    if (!result.warnings.empty())
                    {
                        warnings =
                            m_host.text("compare.warnings", {{"flags", joined(result.warnings, MAX_FLAG_CHARS)}});
                    }
                }
                show(m_summary, summary);
                show(m_errors, errors);
                show(m_warnings, warnings);
                if (size.x <= PLOT_LEFT + PLOT_RIGHT + 8.0f || size.y <= PLOT_TOP + PLOT_BOTTOM + 8.0f)
                {
                    m_canvas.clear();
                    m_columnsDrawn = 0;
                    return;
                }
                drawPlot(size);
            }

            void drawPlot(const Vec2 size)
            {
                const float width = size.x - PLOT_LEFT - PLOT_RIGHT;
                const float height = size.y - PLOT_TOP - PLOT_BOTTOM;
                m_columnsDrawn = 0;
                m_canvas.update(
                    [&](UiCanvasPainter& painter)
                    {
                        if (!m_loaded)
                        {
                            return;
                        }
                        const CompareDistribution& d = m_data.outcome.distribution;
                        const UiScope& scope = *m_host.scope;
                        const u32 text = canvasColor(scope, "canvas.text");
                        const u32 axis = canvasColor(scope, "canvas.axis");
                        const CompareFrames* sides[2] = {&d.baseline, &d.candidate};
                        const char* labels[2] = {"compare.plot.baseline", "compare.plot.candidate"};
                        for (int side = 0; side < 2; ++side)
                        {
                            const CompareFrames& f = *sides[side];
                            painter.rect(Vec2{PLOT_LEFT, 4.0f + static_cast<float>(side) * 14.0f + 3.0f},
                                         Vec2{8.0f, 8.0f}, side ? candidateColor(scope) : baselineColor(scope));
                            painter.text(Vec2{PLOT_LEFT + 12.0f, 2.0f + static_cast<float>(side) * 14.0f},
                                         Vec2{width - 12.0f, 14.0f},
                                         m_host.text(labels[side],
                                                     {{"count", m_host.whole(f.count)},
                                                      {"mean", m_host.precise(f.meanMs)},
                                                      {"p99", m_host.precise(f.p99Ms)}}) +
                                             (f.capped ? " " + m_host.text("compare.plot.capped") : std::string()),
                                         text, UiCanvasAlign::Left, 10.0f);
                        }
                        painter.rect(Vec2{PLOT_LEFT, PLOT_TOP + height}, Vec2{width, 1.0f}, axis);
                        painter.text(Vec2{PLOT_LEFT, PLOT_TOP + height + 3.0f}, Vec2{60.0f, 14.0f}, "0", text,
                                     UiCanvasAlign::Left, 10.0f);
                        painter.text(Vec2{PLOT_LEFT + width - 90.0f, PLOT_TOP + height + 3.0f}, Vec2{90.0f, 14.0f},
                                     m_host.precise(d.rangeMs) + " ms", text, UiCanvasAlign::Right, 10.0f);
                        if (d.baseline.count == 0 && d.candidate.count == 0)
                        {
                            painter.text(Vec2{PLOT_LEFT + 8.0f, PLOT_TOP + height * 0.4f}, Vec2{width, 16.0f},
                                         m_host.text("compare.plot.noFrames"), text, UiCanvasAlign::Left, 11.0f);
                            return;
                        }
                        // one column per ~3 pixels, never more than the bins: the primitives follow the width
                        const u32 columns =
                            std::clamp<u32>(static_cast<u32>(width / 3.0f), 1, static_cast<u32>(COMPARE_BINS));
                        const float step = width / static_cast<float>(columns);
                        std::vector<std::array<double, 2>> value(columns);
                        double top = 0.0;
                        for (int side = 0; side < 2; ++side)
                        {
                            const CompareFrames& f = *sides[side];
                            std::vector<u64> merged(columns, 0);
                            for (u32 bin = 0; bin < f.bins.size(); ++bin)
                            {
                                merged[std::min<u32>(
                                    columns - 1, static_cast<u32>(static_cast<u64>(bin) * columns / COMPARE_BINS))] +=
                                    f.bins[bin];
                            }
                            double run = 0.0;
                            for (u32 c = 0; c < columns; ++c)
                            {
                                run += static_cast<double>(merged[c]);
                                value[c][side] = m_plot == 0 ? static_cast<double>(merged[c])
                                                             : run / std::max(1.0, static_cast<double>(f.count));
                                top = std::max(top, value[c][side]);
                            }
                        }
                        top = std::max(top, 1e-12);
                        for (int side = 0; side < 2; ++side)
                        {
                            if (m_plot == 0)
                            {
                                for (u32 c = 0; c < columns; ++c)
                                {
                                    const float h = static_cast<float>(value[c][side] / top) * height;
                                    if (h > 0.0f)
                                    {
                                        painter.rect(
                                            Vec2{PLOT_LEFT + static_cast<float>(c) * step, PLOT_TOP + height - h},
                                            Vec2{std::max(1.0f, step - 0.5f), h},
                                            side ? candidateColor(scope, 140) : baselineColor(scope, 140));
                                    }
                                }
                            }
                            else
                            {
                                std::vector<Vec2> points;
                                points.reserve(columns + 1);
                                points.push_back(Vec2{PLOT_LEFT, PLOT_TOP + height});
                                for (u32 c = 0; c < columns; ++c)
                                {
                                    points.push_back(
                                        Vec2{PLOT_LEFT + static_cast<float>(c + 1) * step,
                                             PLOT_TOP + height - static_cast<float>(value[c][side] / top) * height});
                                }
                                painter.polyline(points.data(), static_cast<u32>(points.size()), 1.5f,
                                                 side ? candidateColor(scope) : baselineColor(scope));
                            }
                            // the p99 of each side
                            const double p99 = sides[side]->p99Ms;
                            if (p99 > 0.0 && p99 < d.rangeMs)
                            {
                                painter.rect(Vec2{PLOT_LEFT + static_cast<float>(p99 / d.rangeMs) * width, PLOT_TOP},
                                             Vec2{1.0f, height},
                                             side ? candidateColor(scope, 200) : baselineColor(scope, 200));
                            }
                        }
                        m_columnsDrawn = columns;
                    });
            }

            ProfilerViewHost m_host;
            Panel m_root;
            Dropdown m_baselineChoice, m_candidateChoice;
            Button m_swap, m_forceButton, m_allButton, m_plotButton;
            TextInput m_summary, m_errors, m_warnings;
            std::unique_ptr<Table> m_table;
            Canvas m_canvas;
            std::shared_ptr<std::atomic<u64>> m_generation;
            u64 m_value = 0;
            Data m_data;
            CompareFilter m_filter;
            std::vector<u32> m_order; // rows shown, indexes into the result
            std::vector<CaptureId> m_ids; // the choices, after "(none)"
            std::vector<std::string> m_shortLabels, m_fullLabels; // per choice: the dropdown's row, the whole label
            u64 m_storeGeneration = ~u64{0};
            Vec2 m_drawnSize{};
            CaptureId m_baseline = 0, m_candidate = 0;
            std::string m_localeUsed;
            u32 m_plot = 0;
            u64 m_columnsDrawn = 0;
            bool m_force = false;
            bool m_active = false;
            bool m_loading = false;
            bool m_loaded = false;
            bool m_dirty = true;
            ThemeObserver m_theme; // after m_dirty, which its callback sets
        };
    } // namespace

    std::unique_ptr<ProfilerView> makeCompareView(const ProfilerViewHost& host)
    {
        return std::make_unique<CompareView>(host);
    }
} // namespace spite
