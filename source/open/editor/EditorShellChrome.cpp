#include "EditorShellChrome.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>
#include <utility>

#include "EditorContext.hpp"
#include "EditorPanelUi.hpp"
#include "base/Logging.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiTextMeasurer.hpp"
#include "uiview/UiStyleSheet.hpp"
#include "uiview/UiViewContext.hpp"

namespace spite
{
    namespace
    {
        constexpr std::array<std::string_view, 3> MODE_NAMES{"edit", "playing", "paused"};
        InternedString modeClass(const std::string_view family, const std::size_t mode)
        {
            return InternedString(std::string(family) + "." + std::string(MODE_NAMES[mode]));
        }
    } // namespace

    EditorModeFrame::EditorModeFrame(UiScope& scope, const Widget& image, const std::string_view name)
    {
        const std::string base = "editor.view." + std::string(name);
        UiRectDesc frameDesc{.anchor = {0.5f, 0.5f}, .pivot = {0.5f, 0.5f}};
        m_frame = scope.createPanel(InternedString(base + ".frame"), frameDesc);
        m_frame.setParent(image);
        m_frame.setStyleClass(modeClass("editor.view.frame", 0));
        m_frame.setVisible(false);
        m_frame.setInteractable(false);
    }

    void EditorModeFrame::setMode(const Mode mode)
    {
        if (mode == m_mode)
            return;
        m_mode = mode;
        m_frame.setStyleClass(modeClass("editor.view.frame", static_cast<std::size_t>(mode)));
        m_frame.setVisible(mode != Mode::Edit);
    }

    void EditorModeFrame::setSize(const Vec2 size)
    {
        if (m_frame.size() != size)
            m_frame.setSize(size);
    }

    EditorPlayCounter::EditorPlayCounter(Engine& engine, UiScope& scope, const UiRectDesc& desc, const float padding) :
        m_engine(&engine), m_scope(&scope), m_styles(&editorUi(engine).uiView().stylesheet()), m_padding(padding)
    {
        if (engine.services().contains<Localization>())
        {
            m_localization = &engine.services().get<Localization>();
        }
        m_label = scope.createLabel("editor.bar.playcount", "", desc);
        m_label.setStyleClass(InternedString("toolbar.playcount.idle"));
        m_label.setAccessibleNameKey(InternedString("editor.bar.frames.idle"));
        panelui::setTip(m_label);
    }

    // The label keeps the width of six of the widest digits, so the items after it stay put as the frame changes and
    // across Play and Paused. Measured again when the text scale, the stylesheet or the language changes.
    void EditorPlayCounter::fit()
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
        char widest = '0';
        float widestWidth = 0.0f;
        for (char digit = '0'; digit <= '9'; ++digit)
        {
            const float width = measurer->measureText(m_label.entity(), std::string_view(&digit, 1)).x;
            if (width > widestWidth)
            {
                widestWidth = width;
                widest = digit;
            }
        }
        std::string scratch;
        const std::string digits(6, widest);
        const MessageArg args[]{{"frame", std::string_view(digits)}};
        const std::string_view text = m_localization->resolve(InternedString("editor.bar.frames"), args, scratch);
        UiLayoutItemComponent item = m_scope->world().getConstComponent<UiLayoutItemComponent>(m_label.entity());
        item.minSize.x = std::ceil(measurer->measureText(m_label.entity(), text).x) + 2.0f * m_padding;
        m_label.setLayoutItem(item);
    }

    void EditorPlayCounter::update(const EditorModeFrame::Mode mode, const u64 frame, const float seconds)
    {
        using Mode = EditorModeFrame::Mode;
        fit();
        if (mode != m_mode)
        {
            const bool wasEdit = m_mode == Mode::Edit;
            m_mode = mode;
            m_frameShown = ~0ull;
            // editing shows no frame: the counter keeps its room (the items after it stay put) but draws nothing
            m_label.setStyleClass(InternedString(mode == Mode::Edit         ? "toolbar.playcount.idle"
                                                     : mode == Mode::Paused ? "statusbar.item.paused"
                                                                            : "statusbar.item.playing"));
            if (mode == Mode::Edit)
            {
                m_label.setText("");
                m_label.setAccessibleNameKey(InternedString("editor.bar.frames.idle"));
            }
            else if (wasEdit)
            {
                m_label.setLocalizedText(InternedString("editor.bar.frames"));
                m_label.setAccessibleNameKey(InternedString{}); // the name follows the frame
            }
        }
        if (mode == Mode::Edit)
        {
            return;
        }
        const u64 revision = m_localization ? m_localization->revision() : 0;
        if (revision != m_revision)
        {
            m_revision = revision;
            m_frameShown = ~0ull; // the language: the name again
        }
        m_since += seconds;
        if (frame == m_frameShown || (mode == Mode::Playing && m_frameShown != ~0ull && m_since < WRITE_SECONDS))
        {
            return;
        }
        m_frameShown = frame;
        m_since = 0.0f;
        const std::string digits = std::format("{:06}", frame);
        m_label.setLocalizedArgText("frame", digits);
        if (!m_localization)
        {
            return;
        }
        std::string scratch;
        const MessageArg args[]{{"frame", std::string_view(digits)}};
        m_label.setAccessibleName(m_localization->resolve(InternedString("editor.bar.frames.name"), args, scratch));
    }

    EditorTimeScale::EditorTimeScale(Engine& engine, UiScope& scope, const UiRectDesc& desc, const float padding) :
        m_engine(&engine), m_scope(&scope), m_styles(&editorUi(engine).uiView().stylesheet()), m_padding(padding),
        m_live(engine.configStore() != nullptr)
    {
        UiNumberDesc number;
        number.rect = desc;
        number.min = 0.0;
        number.max = 1000.0;
        number.step = 0.01;
        number.decimals = 2;
        m_field = scope.createNumberField("editor.bar.timescale", number);
        m_field.setAccessibleNameKey(InternedString("editor.timescale"));
        m_field.onNumber([this](const double value) { m_pending = value; });
        m_field.setInteractable(m_live);
    }

    void EditorTimeScale::fit()
    {
        const UiTextMeasurer* measurer = m_scope->hierarchy().textMeasurer();
        const float textScale = m_engine->uiDisplay().textScale;
        const u64 sheet = m_styles->generation();
        const Localization& localization = m_engine->localization();
        if (!measurer ||
            (textScale == m_fitTextScale && sheet == m_fitSheet && localization.revision() == m_fitRevision))
        {
            return;
        }
        m_fitTextScale = textScale;
        m_fitSheet = sheet;
        m_fitRevision = localization.revision();
        UiLayoutItemComponent item = m_scope->world().getConstComponent<UiLayoutItemComponent>(m_field.entity());
        item.minSize.x = std::ceil(measurer->measureText(m_field.entity(), "000.00").x) + 2.0f * m_padding;
        item.maxSize.x = item.minSize.x;
        m_field.setLayoutItem(item);
    }

    void EditorTimeScale::request(const double value)
    {
        const panelui::Json reply = panelui::request(
            m_engine->control(),
            panelui::Json{{"command", "setConfig"},
                          {"args", {{"section", "application.timing"}, {"path", "timeScale"}, {"value", value}}}});
        if (reply.value("ok", false))
        {
            return;
        }
        SWARNING_LOG("editor shell: time scale {} refused: {}", value, reply.value("error", std::string("refused")));
        m_shown = -1.0f; // the field returns to the scale in force
    }

    void EditorTimeScale::update()
    {
        fit();
        if (m_pending)
        {
            request(*std::exchange(m_pending, std::nullopt));
        }
        const float scale = m_engine->config().timing.timeScale;
        if (scale == m_shown)
        {
            return;
        }
        m_shown = scale;
        m_field.setNumber(scale);
        panelui::showSelected(m_field, scale != 1.0f);
    }

} // namespace spite
