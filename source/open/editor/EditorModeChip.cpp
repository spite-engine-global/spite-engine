#include "EditorModeChip.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

#include "EditorContext.hpp"
#include "EditorPanelUi.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiTextMeasurer.hpp"
#include "uiview/UiStyleSheet.hpp"
#include "uiview/UiViewContext.hpp"

namespace spite
{
    namespace
    {
        constexpr std::array<std::string_view, 3> MODES{"edit", "playing", "paused"};
    }

    EditorModeChip::EditorModeChip(Engine& engine, UiScope& scope, const UiRectDesc& desc, const float padding) :
        m_engine(&engine), m_scope(&scope), m_styles(&editorUi(engine).uiView().stylesheet()), m_padding(padding)
    {
        if (engine.services().contains<Localization>())
        {
            m_localization = &engine.services().get<Localization>();
        }
        m_label = scope.createLabel("editor.bar.mode", "", desc);
        panelui::setTip(m_label);
    }

    void EditorModeChip::fit()
    {
        const UiTextMeasurer* measurer = m_scope->hierarchy().textMeasurer();
        const float textScale = m_engine->uiDisplay().textScale;
        const u64 sheet = m_styles->generation();
        const u64 revision = m_localization ? m_localization->revision() : 0;
        if (!measurer || !m_localization ||
            (textScale == m_fitTextScale && sheet == m_fitSheet && revision == m_fitRevision))
        {
            return;
        }
        m_fitTextScale = textScale;
        m_fitSheet = sheet;
        m_fitRevision = revision;
        float widest = 0.0f;
        std::string scratch;
        for (const std::string_view mode : MODES)
        {
            const std::string_view text =
                m_localization->resolve(InternedString("editor.bar.mode." + std::string(mode)), {}, scratch);
            widest = std::max(widest, measurer->measureText(m_label.entity(), text).x);
        }
        UiLayoutItemComponent item = m_scope->world().getConstComponent<UiLayoutItemComponent>(m_label.entity());
        item.minSize.x = std::ceil(widest) + 2.0f * m_padding;
        m_label.setLayoutItem(item);
    }

    void EditorModeChip::update(const EditorModeFrame::Mode mode)
    {
        const int now = static_cast<int>(mode);
        if (now != m_mode)
        {
            m_mode = now;
            const std::string name(MODES[static_cast<std::size_t>(now)]);
            m_label.setStyleClass(InternedString("toolbar.mode." + name + " statusbar.item." + name));
            m_label.setLocalizedText(InternedString("editor.bar.mode." + name));
            m_label.setAccessibleNameKey(InternedString("editor.bar.mode." + name + ".name"));
        }
        fit(); // after the class: the text is measured in the face it is drawn in
    }
} // namespace spite
