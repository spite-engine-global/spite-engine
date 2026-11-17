#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

#include "EditorPanelUi.hpp"
#include "ProfilerViews.hpp"
#include "ui/UiCanvas.hpp"
#include "ui/UiChart.hpp"
#include "ui/UiTextMeasurer.hpp"

namespace spite
{
    using namespace panelui;

    namespace
    {
        constexpr u64 DEFAULT_FRAMES = 64; // the window when no frame range is selected: the capture's first frames
        constexpr u64 MAX_FRAMES = 256; // CPU frames of the window at most (a longer selection shows its first ones)
        constexpr u64 MAX_GPU_ROWS = 20000;
        constexpr u32 MAX_ERROR_CHARS = 160;
        constexpr u32 MAX_LABELS = 300;
        constexpr float AXIS_LEFT = 44.0f;
        constexpr float AXIS_RIGHT = 6.0f;
        constexpr float AXIS_Y = 2.0f;
        constexpr float CPU_Y = 24.0f, CPU_H = 20.0f;
        constexpr float GPU_Y = 50.0f, GPU_H = 14.0f;
        constexpr float PASS_Y = 68.0f, PASS_H = 40.0f;
        constexpr u32 STATE_FRAMES = 16;
        constexpr u32 STATE_RECTS = 48;
        constexpr float HOVER_PAD = 4.0f; // the hover label's room around its text

        // one window of a capture: its CPU frames and the GPU frames (with passes) that start inside their time span
        struct Window
        {
            CaptureId capture = 0;
            bool calibrated = false;
            bool hasFrames = false;
            u64 firstFrame = 0, lastFrame = 0; // the CPU frames shown
            u64 requestedLast = 0; // the range asked for, before the cap
            std::int64_t t0 = 0,
                         t1 = 1; // the time axis: the CPU frames' span, extended over the GPU frames that start in it
            std::int64_t cpuEnd = 1; // the last CPU frame's end
            std::vector<CaptureFrame> cpu;
            std::vector<CaptureGpuFrame> gpu;
            bool gpuTruncated = false;
            std::string error;
        };

        struct Hit
        {
            float x0, x1;
            u64 frame;
            u32 pass;
        };

        class GpuView final : public ProfilerView
        {
        public:
            explicit GpuView(const ProfilerViewHost& host) :
                m_host(host), m_generation(std::make_shared<std::atomic<u64>>(0)),
                m_theme(*host.scope, [this] { m_dirty = true; })
            {
                UiScope& scope = *m_host.scope;
                DensityFollower& density = *m_host.density;
                m_root =
                    stackPanel(scope, "profiler.gpu", Vec2{}, UiLayoutDirection::Column, UiLayoutAlign::Stretch, true);
                m_root.setLayoutItem(fillItem());
                m_summary = density.track(readOnlyText(scope, "profiler.gpu.summary", barDesc(Vec2{})), HEIGHT_ROW);
                m_summary.setParent(m_root);
                m_summary.setAccessibleNameKey(profilerKey("gpu.summary.name"));
                // the lane is a canvas a keyboard cannot point into: the step buttons walk the passes in time order
                Panel bar = density.track(stackPanel(scope, "profiler.gpu.bar", Vec2{}, UiLayoutDirection::Row),
                                          HEIGHT_CONTROL);
                bar.setParent(m_root);
                static constexpr const char* STEP_TEXT[2] = {"gpu.prev", "gpu.next"};
                static constexpr const char* STEP_NAME[2] = {"gpu.prev.name", "gpu.next.name"};
                for (u32 i = 0; i < 2; ++i)
                {
                    m_steps[i] = density.track(scope.createButton("profiler.gpu.step", "", barDesc(Vec2{64.0f, 0.0f})),
                                               HEIGHT_ROW);
                    m_steps[i].setLocalizedText(profilerKey(STEP_TEXT[i]));
                    m_steps[i].setParent(bar);
                    m_steps[i].setAccessibleNameKey(profilerKey(STEP_NAME[i]));
                    m_steps[i].onClick([this, i] { stepPass(i == 0 ? -1 : 1); });
                }
                UiRectDesc detail = topLeft();
                detail.layoutItem = fillItem();
                m_detail = density.track(readOnlyText(scope, "profiler.gpu.detail", detail), HEIGHT_ROW);
                m_detail.setParent(bar);
                m_detail.setAccessibleNameKey(profilerKey("gpu.detail.name"));
                UiCanvasDesc canvasDesc;
                canvasDesc.rect = topLeft();
                canvasDesc.rect.layoutItem = fillItem();
                canvasDesc.maxPrimitives = 60000;
                m_canvas = scope.createCanvas("profiler.gpu.lane", canvasDesc);
                m_canvas.setParent(m_root);
                m_canvas.setAccessibleNameKey(profilerKey("gpu.lane.name"));
                m_canvas.onPointer([this](const UiCanvasPointer& pointer) { pointerEvent(pointer); });
            }

            [[nodiscard]] Widget root() const override { return m_root; }

            void activate(const bool active) override
            {
                m_active = active;
                if (!active)
                {
                    m_generation->fetch_add(1);
                    m_loading = false;
                    m_key = {};
                    return;
                }
                m_dirty = true;
                refresh();
            }

            void captureChanged(const CaptureId capture, const CaptureInfo& info) override
            {
                m_capture = capture;
                m_infoFrames = capture != 0 ? info.frames : 0;
                m_selection = {};
                m_data = {};
                m_key = {};
                m_generation->fetch_add(1);
                m_loading = false;
                m_dirty = true;
                if (m_active)
                {
                    refresh();
                }
            }

            void selectionChanged(const ProfilerSelectionComponent& selection) override
            {
                const bool window = selection.hasFrames != m_selection.hasFrames ||
                    selection.firstFrame != m_selection.firstFrame || selection.lastFrame != m_selection.lastFrame;
                m_selection = selection;
                m_dirty = true; // the selected pass or the frames highlighted moved
                if (window && m_active)
                {
                    refresh();
                }
            }

            bool take(ProfilerEvent& event) override
            {
                if (event.token != m_value || event.token != m_generation->load())
                {
                    return true;
                }
                m_loading = false;
                if (const auto data = std::static_pointer_cast<Window>(event.payload))
                {
                    m_data = std::move(*data);
                }
                else if (!event.error.empty() && event.error != "superseded")
                {
                    m_data = {};
                    m_data.error = profilerSafeText(event.error, MAX_ERROR_CHARS);
                }
                m_dirty = true;
                return true;
            }

            void update() override
            {
                const Vec2 size = m_host.laidOutSize(m_canvas);
                if (*m_host.locale != m_localeUsed)
                {
                    m_localeUsed = *m_host.locale;
                    m_dirty = true;
                }
                if (m_dirty || size != m_drawnSize)
                {
                    redraw(size);
                }
            }

            [[nodiscard]] bool busy() const override { return m_loading; }

            void writeState(IFormatWriter& out) override
            {
                out.writeBool("loading", m_loading);
                out.writeString("error", m_data.error);
                out.writeBool("calibrated", m_data.calibrated);
                out.beginObject("window");
                out.writeU32("capture", m_data.capture);
                out.writeU64("firstFrame", m_data.firstFrame);
                out.writeU64("lastFrame", m_data.lastFrame);
                out.writeBool("fromSelection", m_data.hasFrames);
                out.writeBool("clipped", m_data.requestedLast > m_data.lastFrame);
                out.writeU64("startNs", static_cast<u64>(std::max<std::int64_t>(0, m_data.t0)));
                out.writeU64("endNs", static_cast<u64>(std::max<std::int64_t>(0, m_data.t1)));
                out.writeU64("cpuEndNs", static_cast<u64>(std::max<std::int64_t>(0, m_data.cpuEnd)));
                out.writeU64("cpuFrames", m_data.cpu.size());
                out.writeU64("gpuFrames", m_data.gpu.size());
                u64 passes = 0, passNs = 0;
                for (const CaptureGpuFrame& frame : m_data.gpu)
                {
                    passes += frame.passes.size();
                    for (const CaptureGpuPass& pass : frame.passes)
                    {
                        passNs += pass.durationNs;
                    }
                }
                out.writeU64("gpuPasses", passes);
                out.writeU64("gpuPassNs", passNs);
                out.writeBool("gpuTruncated", m_data.gpuTruncated);
                out.endObject();
                out.beginObject("drawn");
                out.writeU64("primitives", m_canvas.primitives());
                out.writeU64("passRects", m_hits.size());
                out.writeString("hoverLabel", m_hoverLabel);
                out.writeF32("hoverWidth", m_hoverWidth);
                const u64 rects = std::min<u64>(m_hits.size(), STATE_RECTS);
                out.beginArray("rects", static_cast<u32>(rects));
                for (u64 i = 0; i < rects; ++i)
                {
                    out.beginObject({});
                    out.writeU64("gpuFrame", m_hits[i].frame);
                    out.writeU32("gpuPass", m_hits[i].pass);
                    out.writeF32("x0", m_hits[i].x0);
                    out.writeF32("x1", m_hits[i].x1);
                    out.endObject();
                }
                out.endArray();
                out.writeU64("redraws", m_redraws);
                out.writeU64("canvasWidth", static_cast<u64>(m_drawnSize.x));
                out.writeU64("canvasHeight", static_cast<u64>(m_drawnSize.y));
                out.writeBool("aligned", m_data.calibrated);
                out.endObject();
                out.beginObject("selectedPass");
                out.writeBool("selected", m_selection.hasGpuPass);
                out.writeU64("gpuFrame", m_selection.gpuFrame);
                out.writeU32("gpuPass", m_selection.gpuPass);
                if (const Selected pass = selectedPass(); pass.found)
                {
                    out.writeString("name", pass.name);
                    out.writeU64("startNs", static_cast<u64>(std::max<std::int64_t>(0, pass.startNs)));
                    out.writeU64("durationNs", pass.durationNs);
                }
                out.endObject();
                const u64 listed = std::min<u64>(m_data.gpu.size(), STATE_FRAMES);
                out.beginArray("frames", static_cast<u32>(listed));
                for (u64 i = 0; i < listed; ++i)
                {
                    const CaptureGpuFrame& frame = m_data.gpu[i];
                    out.beginObject({});
                    out.writeU64("gpuFrame", frame.index);
                    out.writeU64("startNs", static_cast<u64>(std::max<std::int64_t>(0, frame.startNs)));
                    out.writeU32("durationNs", frame.durationNs);
                    out.writeU32("outsideGraphNs", frame.outsideGraphNs);
                    out.writeU64("passes", frame.passes.size());
                    out.endObject();
                }
                out.endArray();
            }

            void registerCommands() override {}

        private:
            struct Selected
            {
                bool found = false;
                std::string name;
                std::int64_t startNs = 0;
                u32 durationNs = 0;
            };

            // The window a request asks for: the selected frames, else the capture's first ones
            [[nodiscard]] std::string keyOf() const
            {
                return std::to_string(m_capture) + ":" + std::to_string(m_selection.hasFrames) + ":" +
                    std::to_string(m_selection.firstFrame) + ":" + std::to_string(m_selection.lastFrame);
            }

            void refresh()
            {
                if (!m_host.data || m_capture == 0)
                {
                    m_dirty = true;
                    return;
                }
                if (const std::string key = keyOf(); key == m_key)
                {
                    return;
                }
                else
                {
                    m_key = key;
                }
                m_loading = true;
                m_value = m_generation->fetch_add(1) + 1;
                CaptureStore* store = m_host.store;
                const CaptureId capture = m_capture;
                const bool hasFrames = m_selection.hasFrames;
                const u64 selFirst = m_selection.firstFrame, selLast = m_selection.lastFrame;
                m_host.data->requestTask(
                    static_cast<u32>(ProfilerViewId::Gpu), m_generation, m_value,
                    [store, capture, hasFrames, selFirst, selLast](const CaptureCancelFn& cancelled,
                                                                   std::string&) -> std::shared_ptr<void>
                    {
                        auto window = std::make_shared<Window>();
                        window->capture = capture;
                        window->hasFrames = hasFrames;
                        CaptureInfo info;
                        if (!store->info(capture, info) || info.frames == 0)
                        {
                            window->error = info.frames == 0 ? "the capture has no frames" : "the capture is gone";
                            return window;
                        }
                        window->calibrated = info.gpuCalibrated;
                        u64 first = hasFrames ? selFirst : 0;
                        u64 last = hasFrames ? selLast : std::min<u64>(info.frames, DEFAULT_FRAMES) - 1;
                        first = std::min(first, info.frames - 1);
                        last = std::clamp(last, first, info.frames - 1);
                        window->requestedLast = last;
                        if (last - first + 1 > MAX_FRAMES)
                        {
                            last = first + MAX_FRAMES - 1;
                        }
                        window->firstFrame = first;
                        window->lastFrame = last;
                        CaptureRange range;
                        range.firstFrame = static_cast<std::int64_t>(first);
                        range.lastFrame = static_cast<std::int64_t>(last);
                        range.limit = MAX_FRAMES;
                        CaptureSlice<CaptureFrame> frames;
                        CaptureFrameStats stats;
                        window->error = profilerStoreCall(cancelled,
                                                          [&]
                                                          {
                                                              frames.rows.clear();
                                                              return store->frames(capture, range, frames, stats);
                                                          });
                        if (!window->error.empty() || frames.rows.empty())
                        {
                            return window;
                        }
                        window->cpu = std::move(frames.rows);
                        window->t0 = window->cpu.front().startNs;
                        window->cpuEnd = std::max(window->cpu.back().endNs, window->t0 + 1);
                        window->t1 = window->cpuEnd;
                        if (window->calibrated)
                        {
                            CaptureRange gpu;
                            gpu.fromNs = window->t0;
                            gpu.toNs = window->cpuEnd;
                            gpu.limit = MAX_GPU_ROWS;
                            CaptureSlice<CaptureGpuFrame> slice;
                            window->error = profilerStoreCall(cancelled,
                                                              [&]
                                                              {
                                                                  slice.rows.clear();
                                                                  slice.truncated = false;
                                                                  return store->gpuFrames(capture, gpu, slice);
                                                              });
                            window->gpu = std::move(slice.rows);
                            window->gpuTruncated = slice.truncated;
                            // GPU frames run after the CPU frames that submitted them: the axis reaches their ends
                            for (const CaptureGpuFrame& frame : window->gpu)
                            {
                                window->t1 =
                                    std::max(window->t1, frame.startNs + static_cast<std::int64_t>(frame.durationNs));
                            }
                        }
                        return window;
                    });
            }

            [[nodiscard]] Selected selectedPass() const
            {
                Selected found;
                if (!m_selection.hasGpuPass)
                {
                    return found;
                }
                for (const CaptureGpuFrame& frame : m_data.gpu)
                {
                    if (frame.index == m_selection.gpuFrame && m_selection.gpuPass < frame.passes.size())
                    {
                        const CaptureGpuPass& pass = frame.passes[m_selection.gpuPass];
                        found = {true, pass.name, frame.startNs + pass.startOffsetNs, pass.durationNs};
                        break;
                    }
                }
                return found;
            }

            float mapX(const std::int64_t ns, const float width) const
            {
                const double span = static_cast<double>(std::max<std::int64_t>(1, m_data.t1 - m_data.t0));
                return AXIS_LEFT + static_cast<float>(static_cast<double>(ns - m_data.t0) / span) * width;
            }

            void redraw(const Vec2 size)
            {
                m_dirty = false;
                ++m_redraws;
                m_drawnSize = size;
                m_hits.clear();
                m_hoverWidth = 0.0f;
                std::string summary;
                if (m_capture == 0)
                {
                    summary = m_host.text("status.noCapture");
                }
                else if (!m_data.error.empty())
                {
                    summary = m_host.text("status.error", {{"error", m_data.error}});
                }
                else if (m_loading || m_data.cpu.empty())
                {
                    summary = m_host.text("gpu.loading");
                }
                else
                {
                    summary = m_host.text("gpu.summary",
                                          {{"first", m_host.whole(m_data.firstFrame)},
                                           {"last", m_host.whole(m_data.lastFrame)},
                                           {"cpu", m_host.ms(static_cast<double>(m_data.cpuEnd - m_data.t0))}});
                    if (!m_data.hasFrames)
                    {
                        summary += " " + m_host.text("gpu.summary.first");
                    }
                    if (m_data.requestedLast > m_data.lastFrame)
                    {
                        summary += " " + m_host.text("gpu.summary.cut", {{"frames", m_host.whole(MAX_FRAMES)}});
                    }
                    if (m_data.calibrated)
                    {
                        u64 passes = 0;
                        for (const CaptureGpuFrame& frame : m_data.gpu)
                        {
                            passes += frame.passes.size();
                        }
                        summary += m_host.text(
                            "gpu.summary.gpu",
                            {{"frames", m_host.whole(m_data.gpu.size())}, {"passes", m_host.whole(passes)}});
                        if (m_data.gpuTruncated)
                        {
                            summary += " " + m_host.text("gpu.summary.truncated");
                        }
                    }
                    else
                    {
                        summary += m_host.text("gpu.summary.uncalibrated");
                    }
                }
                show(m_summary, summary);
                updateDetail();
                if (size.x <= AXIS_LEFT + AXIS_RIGHT + 8.0f || size.y <= 8.0f)
                {
                    m_canvas.clear();
                    return;
                }
                const float width = size.x - AXIS_LEFT - AXIS_RIGHT;
                m_canvas.update(
                    [&](UiCanvasPainter& painter)
                    {
                        if (m_data.cpu.empty())
                        {
                            return;
                        }
                        const UiScope& scope = *m_host.scope;
                        const u32 text = canvasColor(scope, "canvas.text");
                        const u32 grid = canvasColor(scope, "canvas.grid");
                        painter.text(Vec2{2.0f, CPU_Y}, Vec2{AXIS_LEFT - 4.0f, CPU_H}, m_host.text("gpu.lane.cpu"),
                                     text, UiCanvasAlign::Right, 10.0f);
                        painter.text(Vec2{2.0f, GPU_Y}, Vec2{AXIS_LEFT - 4.0f, GPU_H}, m_host.text("gpu.lane.gpu"),
                                     text, UiCanvasAlign::Right, 10.0f);
                        painter.text(Vec2{2.0f, PASS_Y}, Vec2{AXIS_LEFT - 4.0f, 14.0f}, m_host.text("gpu.lane.passes"),
                                     text, UiCanvasAlign::Right, 10.0f);
                        const double spanMs = static_cast<double>(m_data.t1 - m_data.t0) * 1e-6;
                        const UiChartTicks ticks = uiChartTicks(0.0, spanMs, 6);
                        for (u32 i = 0; i < ticks.count; ++i)
                        {
                            const double ms = ticks.at(i);
                            const float x = AXIS_LEFT + static_cast<float>(ms / spanMs) * width;
                            if (x < AXIS_LEFT || x > AXIS_LEFT + width)
                            {
                                continue;
                            }
                            painter.rect(Vec2{x, CPU_Y - 2.0f}, Vec2{1.0f, PASS_Y + PASS_H - CPU_Y + 4.0f}, grid);
                            painter.text(Vec2{x + 2.0f, AXIS_Y}, Vec2{60.0f, 16.0f},
                                         uiChartFormat(*m_host.locale, ms, ticks.step) + " ms", text,
                                         UiCanvasAlign::Left, 10.0f);
                        }
                        for (const CaptureFrame& frame : m_data.cpu)
                        {
                            const float x0 = mapX(frame.startNs, width), x1 = mapX(frame.endNs, width);
                            const bool picked = m_selection.hasFrames && frame.index >= m_selection.firstFrame &&
                                frame.index <= m_selection.lastFrame;
                            painter.rect(Vec2{x0, CPU_Y}, Vec2{std::max(1.0f, x1 - x0 - 1.0f), CPU_H},
                                         picked ? canvasColor(scope, "canvas.gpu.bar.picked")
                                                : canvasColor(scope, "canvas.gpu.bar"));
                        }
                        if (!m_data.calibrated)
                        {
                            return;
                        }
                        u32 labels = 0;
                        for (const CaptureGpuFrame& frame : m_data.gpu)
                        {
                            const float f0 = mapX(frame.startNs, width),
                                        f1 = mapX(frame.startNs + frame.durationNs, width);
                            if (f1 < AXIS_LEFT || f0 > AXIS_LEFT + width)
                            {
                                continue;
                            }
                            painter.rect(Vec2{f0, GPU_Y}, Vec2{std::max(1.0f, f1 - f0), GPU_H},
                                         withAlpha(canvasColor(scope, "canvas.series.b"), 190));
                            if (f1 - f0 < 1.5f)
                            {
                                continue; // narrower than a pixel and a half: the frame bar is the lane
                            }
                            for (u32 p = 0; p < frame.passes.size(); ++p)
                            {
                                const CaptureGpuPass& pass = frame.passes[p];
                                const std::int64_t start = frame.startNs + pass.startOffsetNs;
                                const float x0 = mapX(start, width), x1 = mapX(start + pass.durationNs, width);
                                if (x1 < AXIS_LEFT || x0 > AXIS_LEFT + width)
                                {
                                    continue;
                                }
                                const float w = std::max(1.0f, x1 - x0);
                                painter.rect(Vec2{x0, PASS_Y}, Vec2{w, PASS_H}, profilerNameColor(pass.name));
                                m_hits.push_back({x0, x0 + w, frame.index, p});
                                if (w >= 44.0f && labels < MAX_LABELS)
                                {
                                    ++labels;
                                    painter.text(
                                        Vec2{x0 + 2.0f, PASS_Y}, Vec2{w - 3.0f, PASS_H},
                                        profilerSafeText(pass.name, std::max<u32>(4, static_cast<u32>(w / 6.5f))),
                                        canvasColor(scope, "canvas.on-data"), UiCanvasAlign::Left, 10.0f);
                                }
                            }
                        }
                        if (m_selection.hasGpuPass)
                        {
                            for (const Hit& hit : m_hits)
                            {
                                if (hit.frame == m_selection.gpuFrame && hit.pass == m_selection.gpuPass)
                                {
                                    const u32 border = canvasColor(scope, "canvas.outline");
                                    const float w = hit.x1 - hit.x0;
                                    painter.rect(Vec2{hit.x0, PASS_Y}, Vec2{w, 2.0f}, border);
                                    painter.rect(Vec2{hit.x0, PASS_Y + PASS_H - 2.0f}, Vec2{w, 2.0f}, border);
                                    painter.rect(Vec2{hit.x0, PASS_Y}, Vec2{2.0f, PASS_H}, border);
                                    painter.rect(Vec2{hit.x1 - 2.0f, PASS_Y}, Vec2{2.0f, PASS_H}, border);
                                    break;
                                }
                            }
                        }
                        // the pass under the pointer names itself: a coloured bar says nothing until it is clicked
                        if (!m_hoverLabel.empty())
                        {
                            for (const Hit& hit : m_hits)
                            {
                                if (m_hover && hit.frame == m_hover->first && hit.pass == m_hover->second)
                                {
                                    // the element's text size (text scale included), measured as drawn
                                    const UiTextMeasurer* measurer = m_host.scope->hierarchy().textMeasurer();
                                    if (!measurer)
                                    {
                                        break;
                                    }
                                    const Vec2 text = measurer->measureText(m_canvas.entity(), m_hoverLabel);
                                    const Vec2 label{text.x + 2.0f * HOVER_PAD, text.y + HOVER_PAD};
                                    m_hoverWidth = label.x;
                                    const float x =
                                        std::clamp(hit.x0, AXIS_LEFT, std::max(AXIS_LEFT, size.x - label.x));
                                    const float below = PASS_Y + PASS_H + HOVER_PAD * 0.5f;
                                    const float y = size.y >= below + label.y ? below : PASS_Y;
                                    painter.rect(Vec2{x, y}, label, canvasColor(scope, "canvas.tip.fill"));
                                    painter.text(Vec2{x + HOVER_PAD, y}, Vec2{label.x - HOVER_PAD, label.y},
                                                 m_hoverLabel, canvasColor(scope, "canvas.tip.text"));
                                    break;
                                }
                            }
                        }
                    });
            }

            // The pass before or after the selected one over the shown GPU frames (the first or last without a
            // selection)
            void stepPass(const int direction)
            {
                std::vector<std::pair<u64, u32>> passes;
                std::optional<sizet> current;
                for (const CaptureGpuFrame& frame : m_data.gpu)
                {
                    for (u32 p = 0; p < frame.passes.size(); ++p)
                    {
                        if (m_selection.hasGpuPass && frame.index == m_selection.gpuFrame && p == m_selection.gpuPass)
                        {
                            current = passes.size();
                        }
                        passes.emplace_back(frame.index, p);
                    }
                }
                if (passes.empty())
                {
                    return;
                }
                const sizet next = current ? std::clamp<std::int64_t>(static_cast<std::int64_t>(*current) + direction,
                                                                      0, static_cast<std::int64_t>(passes.size()) - 1)
                                           : (direction < 0 ? passes.size() - 1 : 0);
                m_host.send(
                    {{"command", "profilerSelect"},
                     {"args",
                      {{"capture", m_capture}, {"gpuFrame", passes[next].first}, {"gpuPass", passes[next].second}}}});
            }

            [[nodiscard]] const Hit* hitAt(const float x, const float y) const
            {
                if (y < PASS_Y || y > PASS_Y + PASS_H)
                {
                    return nullptr;
                }
                const Hit* best = nullptr;
                for (const Hit& hit : m_hits)
                {
                    if (x >= hit.x0 && x <= hit.x1)
                    {
                        best = &hit; // the last one drawn is on top
                    }
                }
                return best;
            }

            void pointerEvent(const UiCanvasPointer& pointer)
            {
                switch (pointer.kind)
                {
                case UiCanvasPointerKind::Move:
                case UiCanvasPointerKind::Enter:
                    {
                        const Hit* hit = hitAt(pointer.local.x, pointer.local.y);
                        const std::optional<std::pair<u64, u32>> now =
                            hit ? std::optional(std::pair(hit->frame, hit->pass)) : std::nullopt;
                        if (now != m_hover)
                        {
                            m_hover = now;
                            updateDetail();
                            m_dirty = true;
                        }
                        break;
                    }
                case UiCanvasPointerKind::Leave:
                    if (m_hover)
                    {
                        m_hover.reset();
                        updateDetail();
                        m_dirty = true;
                    }
                    break;
                case UiCanvasPointerKind::Click:
                    if (const Hit* hit = hitAt(pointer.local.x, pointer.local.y))
                    {
                        m_host.send(
                            {{"command", "profilerSelect"},
                             {"args", {{"capture", m_capture}, {"gpuFrame", hit->frame}, {"gpuPass", hit->pass}}}});
                    }
                    break;
                default:
                    break;
                }
            }

            [[nodiscard]] std::string describe(const u64 frameIndex, const u32 passIndex) const
            {
                for (const CaptureGpuFrame& frame : m_data.gpu)
                {
                    if (frame.index == frameIndex && passIndex < frame.passes.size())
                    {
                        const CaptureGpuPass& pass = frame.passes[passIndex];
                        return m_host.text("gpu.describe",
                                           {{"name", profilerSafeText(pass.name, 64)},
                                            {"frame", m_host.whole(frameIndex)},
                                            {"pass", m_host.whole(passIndex)},
                                            {"offset", m_host.ms(static_cast<double>(pass.startOffsetNs))},
                                            {"duration", m_host.ms(static_cast<double>(pass.durationNs))}});
                    }
                }
                return {};
            }

            // "Opaque  0.30 ms" for the pass under the pointer
            [[nodiscard]] std::string hoverText() const
            {
                if (!m_hover)
                {
                    return {};
                }
                for (const CaptureGpuFrame& frame : m_data.gpu)
                {
                    if (frame.index == m_hover->first && m_hover->second < frame.passes.size())
                    {
                        const CaptureGpuPass& pass = frame.passes[m_hover->second];
                        return m_host.text("gpu.hover",
                                           {{"name", profilerSafeText(pass.name, 40)},
                                            {"duration", m_host.ms(static_cast<double>(pass.durationNs))}});
                    }
                }
                return {};
            }

            void updateDetail()
            {
                m_hoverLabel = hoverText();
                std::string text;
                if (m_hover)
                {
                    text = describe(m_hover->first, m_hover->second);
                }
                if (text.empty() && m_selection.hasGpuPass)
                {
                    text = describe(m_selection.gpuFrame, m_selection.gpuPass);
                    text = text.empty() ? m_host.text("gpu.outside",
                                                      {{"pass", m_host.whole(m_selection.gpuPass)},
                                                       {"frame", m_host.whole(m_selection.gpuFrame)}})
                                        : m_host.text("selected", {{"text", text}});
                }
                if (text.empty())
                {
                    text = m_data.calibrated ? m_host.text("gpu.hint") : std::string();
                }
                show(m_detail, text);
            }

            ProfilerViewHost m_host;
            Panel m_root;
            TextInput m_summary, m_detail;
            std::array<Button, 2> m_steps;
            Canvas m_canvas;
            std::shared_ptr<std::atomic<u64>> m_generation;
            u64 m_value = 0;
            Window m_data;
            ProfilerSelectionComponent m_selection;
            std::vector<Hit> m_hits;
            std::optional<std::pair<u64, u32>> m_hover;
            std::string m_hoverLabel; // the label drawn at the hovered pass
            float m_hoverWidth = 0.0f; // its box as drawn
            std::string m_key;
            std::string m_localeUsed;
            Vec2 m_drawnSize{};
            CaptureId m_capture = 0;
            u64 m_infoFrames = 0;
            bool m_active = false;
            bool m_loading = false;
            bool m_dirty = true;
            ThemeObserver m_theme; // after m_dirty, which its callback sets
            u64 m_redraws = 0;
        };
    } // namespace

    std::unique_ptr<ProfilerView> makeGpuView(const ProfilerViewHost& host) { return std::make_unique<GpuView>(host); }
} // namespace spite
