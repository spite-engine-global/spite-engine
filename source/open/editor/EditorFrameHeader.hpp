#pragma once

#include <memory>

#include "ui/UiWidgets.hpp"

namespace spite
{
    class Engine;

    // The Forge header's frame readout (Telemetry density): `editor.bar.frame`, the first item of the
    // `shell.toolbar.end` slot (before the agent chip). Quiet: the FRAME OK lamp, the frame time in warn over
    // editorSettings frameBudgetMs; full: the value always and a sparkline of the frame time beside it. It takes the
    // status bar's closed 4 Hz windows (EditorStatusBar::onFrameWindow) and its over-budget flag, so it holds no timer
    // and does no per-frame work: a sample is one text compare (quiet) plus one appended float (full), and the
    // sparkline is a Canvas with one series (`editor.bar.frame.spark`, `size.sparkline.*` tokens, hidden in quiet,
    // 64 samples = 16 s, starts again when full is switched on) repainted only when the theme, the colour state or its
    // laid-out size changes. The lamp text carries the class family of the status bar's frame item
    // (`statusbar.item[.button|.warn]`) and keeps the width of its widest text (FRAME OK or 000.0 ms) so the items
    // after it stay put; its accessible name is 'Frame time {ms} ms' like the status bar's, and the tooltip
    // (tip.editor.bar.frame) explains the lamp. State is readable through uiTree (the label, its class and name, the
    // canvas series count, appended, min and max).
    class EditorFrameHeader
    {
    public:
        EditorFrameHeader(Engine& engine, bool full, float frameBudgetMs);
        ~EditorFrameHeader();
        EditorFrameHeader(const EditorFrameHeader&) = delete;
        EditorFrameHeader& operator=(const EditorFrameHeader&) = delete;

        [[nodiscard]] Widget root() const;
        // A closed frame window: its mean (ms) and whether the status bar's frame item is over its budget.
        void sample(double ms, bool over);
        // The telemetry density, the budget and the over flag as editorSettings left them (in its reply).
        void settle(bool full, float frameBudgetMs, bool over);
        // Once per frame: costs integer compares; repaints the sparkline after a theme or layout change.
        void update();
        // Whether the readout is part of the bar (the shell sheds it where the row cannot hold the whole end group:
        // the status bar shows the same lamp); nothing is written while the state is the one shown.
        void setShown(bool shown);
        // Whether the sparkline is part of the readout in full telemetry (the shell sheds it before the
        // lamp and value, which the status bar shows too); its history keeps filling while it is hidden.
        void setSparklineShown(bool shown);
        // The width (UI pixels) the readout takes when shown in the current mode: its padding, the reserved text and,
        // in full telemetry with `sparkline`, the gap and the sparkline.
        [[nodiscard]] float width(bool sparkline = true);

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
