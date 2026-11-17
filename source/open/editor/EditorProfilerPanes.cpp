#include "EditorProfilerPanes.hpp"
#include <spite/platform/LocalTime.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <format>

#include "EditorPanelUi.hpp"
#include <spite/profiling/Zone.hpp>
#include "ui/UiCanvas.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiChart.hpp"

namespace spite
{
    using namespace panelui;

    namespace
    {
        constexpr float CLOSE_WIDTH = 24.0f;
        constexpr std::array<std::string_view, PROFILER_VIEW_COUNT + 1> PANE_NAMES{"counters", "gpu",     "memory",
                                                                                   "calls",    "compare", "none"};
        constexpr std::array<const char*, PROFILER_VIEW_COUNT> PANE_ZONES{
            "EditorProfiler.pane.counters", "EditorProfiler.pane.gpu", "EditorProfiler.pane.memory",
            "EditorProfiler.pane.calls", "EditorProfiler.pane.compare"};
        constexpr std::array<std::string_view, PROFILER_VIEW_COUNT> TAB_KEYS{"pane.counters", "pane.gpu", "pane.memory",
                                                                             "pane.calls", "pane.compare"};
        constexpr std::array<std::string_view, PROFILER_VIEW_COUNT> TAB_NAME_KEYS{
            "pane.counters.name", "pane.gpu.name", "pane.memory.name", "pane.calls.name", "pane.compare.name"};

        // the length of the UTF-8 sequence starting at text[at] when it is well formed, else 0
        u32 sequenceLength(const std::string_view text, const sizet at)
        {
            const auto lead = static_cast<unsigned char>(text[at]);
            const u32 length = lead < 0x80 ? 1
                : (lead >> 5) == 0x6       ? 2
                : (lead >> 4) == 0xE       ? 3
                : (lead >> 3) == 0x1E      ? 4
                                           : 0;
            if (length == 0 || at + length > text.size())
            {
                return 0;
            }
            for (u32 i = 1; i < length; ++i)
            {
                if ((static_cast<unsigned char>(text[at + i]) & 0xC0) != 0x80)
                {
                    return 0;
                }
            }
            return lead == 0xC0 || lead == 0xC1 || lead > 0xF4 ? 0 : length;
        }
    } // namespace

    std::string profilerSafeText(const std::string_view text, const u32 limit)
    {
        std::string out;
        out.reserve(std::min<sizet>(text.size(), limit) + 3);
        for (sizet at = 0; at < text.size();)
        {
            const u32 length = sequenceLength(text, at);
            const std::string_view piece = length == 0 ? std::string_view("?") : text.substr(at, length);
            if (out.size() + piece.size() > limit)
            {
                out += "...";
                break;
            }
            const auto first = static_cast<unsigned char>(piece[0]);
            out += first < 0x20 || first == 0x7F ? std::string_view(" ") : piece;
            at += std::max<u32>(length, 1);
        }
        return out;
    }

    u32 profilerNameColor(const std::string_view name)
    {
        u32 hash = 2166136261u;
        for (const char c : name)
        {
            hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
        }
        const float hue = static_cast<float>(hash % 360u) / 60.0f;
        const float saturation = 0.5f, value = 0.85f;
        const float chroma = value * saturation;
        const float x = chroma * (1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f));
        float r = 0, g = 0, b = 0;
        switch (static_cast<int>(hue))
        {
        case 0:
            r = chroma, g = x;
            break;
        case 1:
            r = x, g = chroma;
            break;
        case 2:
            g = chroma, b = x;
            break;
        case 3:
            g = x, b = chroma;
            break;
        case 4:
            r = x, b = chroma;
            break;
        default:
            r = chroma, b = x;
            break;
        }
        const float m = value - chroma;
        const auto channel = [m](const float c) { return static_cast<u8>(std::clamp((c + m) * 255.0f, 0.0f, 255.0f)); };
        return uiCanvasColor(channel(r), channel(g), channel(b));
    }

    std::string profilerLocalTime(const std::int64_t unixMs)
    {
        const std::time_t seconds = static_cast<std::time_t>(unixMs / 1000);
        std::tm local{};
        spite::platform::toLocalTime(seconds, local);
        return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                           local.tm_hour, local.tm_min, local.tm_sec);
    }

    std::string profilerRoundedNumbers(const std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        const auto digit = [&](const sizet i) { return i < text.size() && text[i] >= '0' && text[i] <= '9'; };
        for (sizet at = 0; at < text.size();)
        {
            if (!digit(at) || (at > 0 && (digit(at - 1) || text[at - 1] == '.')))
            {
                out += text[at++];
                continue;
            }
            sizet end = at;
            while (digit(end))
            {
                ++end;
            }
            sizet fraction = end;
            if (end + 1 < text.size() && text[end] == '.' && digit(end + 1))
            {
                for (fraction = end + 1; digit(fraction); ++fraction)
                {
                }
            }
            if (fraction - end > 4)
            {
                out += std::format("{:.2f}", std::strtod(std::string(text.substr(at, fraction - at)).c_str(), nullptr));
            }
            else
            {
                out += text.substr(at, fraction - at);
            }
            at = fraction;
        }
        return out;
    }

    InternedString profilerKey(const std::string_view key)
    {
        return InternedString("editor-profiler." + std::string(key));
    }

    std::string profilerText(const UiScope& scope, const std::string_view key,
                             const std::initializer_list<MessageArg> args)
    {
        const Localization* localization = scope.hierarchy().localization();
        if (!localization && scope.world().services().contains<Localization>())
        {
            localization = &scope.world().services().get<Localization>();
        }
        const InternedString full = profilerKey(key);
        if (!localization)
        {
            return std::string(full.toView());
        }
        std::string scratch;
        return std::string(
            localization->resolve(full, std::span<const MessageArg>(args.begin(), args.size()), scratch));
    }

    Vec2 ProfilerViewHost::laidOutSize(const Widget& widget) const
    {
        const UiHierarchyManager& hierarchy = scope->hierarchy();
        const float scale = std::max(hierarchy.layoutScale(), 1e-3f);
        return hierarchy.absoluteRectOf(widget.entity()).size / scale;
    }

    std::string ProfilerViewHost::number(const double value) const
    {
        return uiChartFormat(locale ? *locale : std::string("en"),
                             std::isfinite(value) ? std::round(value * 100.0) / 100.0 : 0.0, 0.0);
    }

    std::string ProfilerViewHost::whole(const u64 value) const
    {
        return uiChartFormat(locale ? *locale : std::string("en"), static_cast<double>(value), 1.0);
    }

    std::string ProfilerViewHost::bytes(const u64 value) const
    {
        static constexpr const char* UNITS[] = {"B", "KB", "MB", "GB", "TB"};
        double scaled = static_cast<double>(value);
        u32 unit = 0;
        while (scaled >= 1024.0 && unit < 4)
        {
            scaled /= 1024.0;
            ++unit;
        }
        return (unit == 0 ? whole(value) : number(scaled)) + " " + UNITS[unit];
    }

    std::string ProfilerViewHost::precise(const double value) const
    {
        return uiChartFormat(locale ? *locale : std::string("en"), std::isfinite(value) ? value : 0.0, 0.0);
    }

    std::string ProfilerViewHost::ms(const double ns) const { return number(ns * 1e-6) + " ms"; }

    ProfilerPanes::ProfilerPanes(ProfilerViewHost host) : m_host(std::move(host))
    {
        UiScope& scope = *m_host.scope;
        DensityFollower& density = *m_host.density;
        m_root = stackPanel(scope, "profiler.panes", Vec2{}, UiLayoutDirection::Column, UiLayoutAlign::Stretch);
        density.apply([this](const Density&) { m_root.setSize(Vec2{0.0f, height()}); });
        Panel tabRow =
            density.track(stackPanel(scope, "profiler.paneTabRow", Vec2{}, UiLayoutDirection::Row), HEIGHT_BUTTON);
        tabRow.setParent(m_root);
        UiRectDesc tabsDesc = barDesc(Vec2{});
        tabsDesc.layoutItem = fillItem();
        m_tabs = density.track(scope.createTabBar("profiler.paneTabs", UiTabBarDesc{.rect = tabsDesc}), HEIGHT_BUTTON);
        m_tabs.setParent(tabRow);
        m_close = density.track(scope.createButton("profiler.paneClose", "x", barDesc(Vec2{CLOSE_WIDTH, 0.0f})),
                                HEIGHT_BUTTON);
        m_close.setParent(tabRow);
        m_close.setAccessibleNameKey(profilerKey("pane.close.name"));
        m_close.setVisible(false);
        m_close.onClick([this] { m_host.send({{"command", "profilerPane"}, {"args", {{"pane", "none"}}}}); });
        m_tabs.setAccessibleNameKey(profilerKey("pane.tabs.name"));
        m_content = scope.createPanel("profiler.paneContent",
                                      [&]
                                      {
                                          UiRectDesc desc = topLeft();
                                          desc.layout =
                                              stack(UiLayoutDirection::Column, 0.0f, {}, UiLayoutAlign::Stretch);
                                          desc.layoutItem = fillItem();
                                          return desc;
                                      }());
        m_content.setParent(m_root);
        m_views[static_cast<u32>(ProfilerViewId::Counters)] = makeCounterView(m_host);
        m_views[static_cast<u32>(ProfilerViewId::Gpu)] = makeGpuView(m_host);
        m_views[static_cast<u32>(ProfilerViewId::Memory)] = makeMemoryView(m_host);
        m_views[static_cast<u32>(ProfilerViewId::Calls)] = makeCallsView(m_host);
        m_views[static_cast<u32>(ProfilerViewId::Compare)] = makeCompareView(m_host);
        for (u32 i = 0; i < PROFILER_VIEW_COUNT; ++i)
        {
            if (!m_views[i])
            {
                continue;
            }
            Tab tab = m_tabs.addTab(InternedString(std::string("profiler.paneTab.") + std::string(PANE_NAMES[i])), "");
            tab.label().setLocalizedText(profilerKey(TAB_KEYS[i]));
            tab.setAccessibleNameKey(profilerKey(TAB_NAME_KEYS[i]));
            m_views[i]->root().setParent(m_content);
            tab.setContent(m_views[i]->root());
        }
        m_tabs.setActive(-1);
        m_tabs.onChange(
            [this](const u32 index)
            {
                m_host.send({{"command", "profilerPane"},
                             {"args", {{"pane", std::string(PANE_NAMES[std::min<u32>(index, PROFILER_VIEW_COUNT)])}}}});
            });
        m_tabs.onReselect([this](u32) { m_host.send({{"command", "profilerPane"}, {"args", {{"pane", "none"}}}}); });
        for (const std::unique_ptr<ProfilerView>& view : m_views)
        {
            if (view)
            {
                view->registerCommands();
            }
        }
        registerCommands();
    }

    ProfilerPanes::~ProfilerPanes() = default;

    void ProfilerPanes::setActive(const i32 index)
    {
        if (index == m_active)
        {
            return;
        }
        if (m_active >= 0)
        {
            m_views[static_cast<u32>(m_active)]->activate(false);
        }
        m_active = index;
        m_tabs.setActive(index);
        m_close.setVisible(index >= 0);
        m_preferred = index < 0 ? 0.0f : m_views[static_cast<u32>(index)]->preferredHeight();
        m_open = index < 0 ? 0.0f : std::max(m_preferred, m_viewport);
        m_root.setSize(Vec2{0.0f, height()});
        if (index >= 0)
        {
            m_views[static_cast<u32>(index)]->activate(true);
        }
    }

    void ProfilerPanes::setViewportHeight(const float viewport)
    {
        if (viewport == m_viewport)
        {
            return;
        }
        m_viewport = viewport;
        if (m_open > 0.0f)
        {
            m_open = std::max(m_preferred, m_viewport);
            m_root.setSize(Vec2{0.0f, height()});
        }
    }

    void ProfilerPanes::captureChanged(const CaptureId capture)
    {
        m_capture = capture;
        CaptureInfo info;
        const bool known = capture != 0 && m_host.store && m_host.store->info(capture, info);
        for (const std::unique_ptr<ProfilerView>& view : m_views)
        {
            if (view)
            {
                view->captureChanged(known ? capture : 0, info);
            }
        }
    }

    void ProfilerPanes::selectionChanged(const ProfilerSelectionComponent& selection)
    {
        for (const std::unique_ptr<ProfilerView>& view : m_views)
        {
            if (view)
            {
                view->selectionChanged(selection);
            }
        }
    }

    void ProfilerPanes::take(ProfilerEvent& event)
    {
        if (event.view < PROFILER_VIEW_COUNT && m_views[event.view])
        {
            m_views[event.view]->take(event);
        }
    }

    void ProfilerPanes::update()
    {
        if (m_active >= 0)
        {
            SPITE_ZONE(PANE_ZONES[static_cast<u32>(m_active)]);
            m_views[static_cast<u32>(m_active)]->update();
        }
    }

    bool ProfilerPanes::busy() const { return m_active >= 0 && m_views[static_cast<u32>(m_active)]->busy(); }

    void ProfilerPanes::writeState(IFormatWriter& out)
    {
        out.writeString("pane", m_active < 0 ? "none" : PANE_NAMES[static_cast<u32>(m_active)]);
        out.writeU32("capture", m_capture);
        // drawn figures (the lane's pass rects, the plot's events, the flame's rects) are those of the last frame the
        // view was drawn in; a hidden panel draws nothing, so they hold until it is shown again
        out.writeBool("shown",
                      m_host.scope->hierarchy().layout().isVisible(m_root.entity()) &&
                          m_host.laidOutSize(m_root).y > 0.0f);
        out.writeU64("tasksRequested", m_host.data ? m_host.data->tasksRequested() : 0);
        static constexpr std::array<const char*, PROFILER_VIEW_COUNT> KEYS{"counters", "gpu", "memory", "calls",
                                                                           "compare"};
        for (u32 i = 0; i < PROFILER_VIEW_COUNT; ++i)
        {
            if (m_views[i])
            {
                out.beginObject(KEYS[i]);
                m_views[i]->writeState(out);
                out.endObject();
            }
        }
    }

    void ProfilerPanes::registerCommands()
    {
        ControlServer& control = *m_host.control;
        control.registerViewCommand(
            "profilerPane", m_host.owner,
            "shows one of the Profiler panel's views under its graph (counters: counter tracks, gpu: the GPU lane, "
            "memory: per-tag totals and big allocations, calls: the flame graph and call tree, compare: two captures' "
            "frame time distributions and zone deltas) or none. A hidden "
            "view asks the capture store for nothing; the tab bar sends this same command",
            {requiredArg("pane", ArgKind::String, "the view to show", std::span<const std::string_view>(PANE_NAMES))},
            [this](const ControlArgs& args, IFormatWriter& out)
            {
                const std::string pane = args.string("pane").value_or("");
                const auto it = std::ranges::find(PANE_NAMES, std::string_view(pane));
                if (it == PANE_NAMES.end())
                {
                    return "no view named '" + profilerSafeText(pane, 32) + "'";
                }
                const auto index = static_cast<i32>(it - PANE_NAMES.begin());
                if (index < static_cast<i32>(PROFILER_VIEW_COUNT) && !m_views[static_cast<u32>(index)])
                {
                    return "the view '" + pane + "' is not available";
                }
                setActive(index < static_cast<i32>(PROFILER_VIEW_COUNT) ? index : -1);
                out.writeString("pane", pane);
                return std::string{};
            });
        control.registerQuery(
            "profilerPanes", m_host.owner,
            "what the Profiler panel's views show: the active view, the counter tracks (name, frames, total, max), the "
            "GPU lane's window and selected pass, the memory totals per tag, the call tree rows and the Compare view's "
            "comparison (`compare`: sides, verdict counts, errors, warnings, the first rows and the frame time "
            "distributions); the numbers are those profileCounterSeries, profileGpu, profileMemory, profileCallTree "
            "and profileCompare answer",
            {},
            [this](const ControlArgs&, IFormatWriter& out)
            {
                m_host.sync();
                writeState(out);
                return std::string{};
            });
    }
} // namespace spite
