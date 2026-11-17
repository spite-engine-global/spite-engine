#pragma once

#include <array>
#include <optional>
#include <string_view>

#include "base/MathTypes.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class Engine;
    class Localization;
    class UiStyleSheet;

    // A non-interactive border indicates Play or Pause around the Scene view.
    class EditorModeFrame
    {
    public:
        enum class Mode : u8
        {
            Edit,
            Playing,
            Paused
        };

        // Children of `image`, between its own content and the focus ring painted last.
        EditorModeFrame(UiScope& scope, const Widget& image, std::string_view name);

        // Edit hides the frame.
        void setMode(Mode mode);
        // The image's size in layout units: the frame covers it.
        void setSize(Vec2 size);

    private:
        Panel m_frame;
        Mode m_mode = Mode::Edit;
    };

    // The play frame counter at the end of the toolbar's transport group: `editor.bar.playcount`, a label
    // of the localized message editor.bar.frames with the play world's simulated frame as a six-digit zero-padded
    // `frame` (accessible name editor.bar.frames.name), in the status bar's lamp classes
    // (`statusbar.item.edit|playing|paused`). In Edit it shows nothing but keeps its room (the unlit lamp, accessible
    // name editor.bar.frames.idle), so pressing Play moves and wraps nothing. While playing it is written at most
    // every 0.25 s and only when the frame moved (a stream of frames is not a stream of relayouts); paused it follows
    // a step at once. Its minimum width is that of six of the widest digits, measured once per text scale, stylesheet
    // and language, so the items beside it stay put as the digits change and across Edit, Play and Paused.
    class EditorPlayCounter
    {
    public:
        static constexpr float WRITE_SECONDS = 0.25f;

        EditorPlayCounter(Engine& engine, UiScope& scope, const UiRectDesc& desc, float padding);

        [[nodiscard]] Widget root() const { return m_label; }
        // The text padding the density gives (space.xs): the reserved width is measured again.
        void setPadding(const float padding)
        {
            m_padding = padding;
            m_fitSheet = ~0ull;
        }
        // Once per frame: integer compares when nothing moved. `seconds` is the frame's duration.
        void update(EditorModeFrame::Mode mode, u64 frame, float seconds);

    private:
        void fit();

        Engine* m_engine;
        UiScope* m_scope;
        const UiStyleSheet* m_styles;
        const Localization* m_localization = nullptr;
        Label m_label;
        float m_padding = 0.0f;
        EditorModeFrame::Mode m_mode = EditorModeFrame::Mode::Edit;
        u64 m_frameShown = ~0ull;
        u64 m_revision = ~0ull;
        float m_since = 0.0f; // seconds since the last write
        // what the reserved width was measured for
        float m_fitTextScale = -1.0f;
        u64 m_fitSheet = ~0ull;
        u64 m_fitRevision = ~0ull;
    };

    // The simulation's speed at the end of the toolbar's transport group: `editor.bar.timescale`, a number
    // field of the scale in x (0 to 1000, two decimals; 0 freezes the simulation while the editor keeps running),
    // lit (`state.selected`) while the scale is not 1. It edits application.timing.timeScale through setConfig (the
    // Timing page's field, the undo stack, the 0 to 1000 check); the field's own range is that check's, so a refusal
    // (logged) shows the running scale again. The Window menu's Time Scale submenu lists PRESETS, 1x being the reset,
    // and calls set(). The field follows engine.config().timing.timeScale, the scale in force: a setConfig, an undo or
    // the Timing page moves it. Its width is that of "000.00", measured again when the text scale, stylesheet or
    // language changes (the bar at 1024 px holds one line with it), so the value moves nothing. A quiescent bar
    // compares one float.
    class EditorTimeScale
    {
    public:
        static constexpr std::array<double, 7> PRESETS{0.0, 0.1, 0.25, 0.5, 1.0, 2.0, 4.0};

        EditorTimeScale(Engine& engine, UiScope& scope, const UiRectDesc& desc, float padding);

        [[nodiscard]] Widget field() const { return m_field; }
        void setPadding(const float padding)
        {
            m_padding = padding;
            m_fitSheet = ~0ull;
        }
        // Asks for a scale; sent by the next update().
        void set(const double scale) { m_pending = scale; }
        // Once per frame: sends a waiting edit, then shows the scale in force.
        void update();

    private:
        void fit();
        void request(double value);

        Engine* m_engine;
        UiScope* m_scope;
        const UiStyleSheet* m_styles;
        NumberField m_field;
        float m_padding = 0.0f;
        bool m_live = false; // the run has a config store to edit
        std::optional<double> m_pending;
        float m_shown = -1.0f; // the scale the field shows; negative: none yet
        float m_fitTextScale = -1.0f;
        u64 m_fitSheet = ~0ull;
        u64 m_fitRevision = ~0ull;
    };

} // namespace spite
