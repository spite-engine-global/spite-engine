#pragma once

#include "EditorShellChrome.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class Engine;
    class Localization;
    class UiStyleSheet;

    // The Forge header's mode chip (Parity with board P): `editor.bar.mode`, the first item after
    // the transport buttons, showing the editor mode: Edit, Playing or Paused, as the play world is (`sceneInfo`
    // playing; uiTree reads the chip's text, name and class). Classes `toolbar.mode.edit|playing|paused` then the
    // status bar's lamp classes of the same mode (what a stylesheet without the chip's own classes shows). The
    // accessible name is the localized `editor.bar.mode.<mode>.name` ('Editor mode: playing'), so the state is read,
    // not only seen. Its width is that of the widest of the three texts, measured again when the text scale, the
    // stylesheet or the language changes, so Play and Pause move nothing. A quiescent bar compares one integer and
    // writes nothing.
    class EditorModeChip
    {
    public:
        EditorModeChip(Engine& engine, UiScope& scope, const UiRectDesc& desc, float padding);

        [[nodiscard]] Widget root() const { return m_label; }
        void setPadding(const float padding)
        {
            m_padding = padding;
            m_fitSheet = ~0ull;
        }
        // Once per frame: shows the mode when it changed.
        void update(EditorModeFrame::Mode mode);

    private:
        void fit();

        Engine* m_engine;
        UiScope* m_scope;
        const UiStyleSheet* m_styles;
        const Localization* m_localization = nullptr;
        Label m_label;
        float m_padding = 0.0f;
        int m_mode = -1; // none shown yet
        float m_fitTextScale = -1.0f;
        u64 m_fitSheet = ~0ull;
        u64 m_fitRevision = ~0ull;
    };
} // namespace spite
