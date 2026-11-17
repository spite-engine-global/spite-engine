#include "EditorFrameHeader.hpp"
#include "EditorContext.hpp"
#include "EditorPanelUi.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include "ui/UiCanvas.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiScope.hpp"
#include "uiview/UiStyleSheet.hpp"
#include "uiview/UiViewContext.hpp"

namespace spite
{
    namespace
    {
        constexpr std::string_view SIZE_SPARK_WIDTH = "size.sparkline.width";
        constexpr std::string_view SIZE_SPARK_HEIGHT = "size.sparkline.height";
        constexpr std::string_view FRAME_OK = "editor-statusbar.frame.ok";
        constexpr std::string_view FRAME_VALUE = "editor-statusbar.frame";
        constexpr std::string_view FRAME_NAME = "editor-statusbar.frame.name";
        constexpr u32 SPARK_SAMPLES = 64; // 16 s of 4 Hz windows
        constexpr float SPARK_MAX_BUDGETS = 2.0f; // the sparkline's top edge, in budgets
        constexpr float SPARK_MAX_NO_BUDGET_MS = 33.3f;
    } // namespace

    struct EditorFrameHeader::State
    {
        explicit State(Engine& engine) :
            engine(&engine), scope(editorUi(engine).ui()), styles(&editorUi(engine).uiView().stylesheet()),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        UiScope scope;
        const UiStyleSheet* styles;
        const Localization* localization;
        Panel root;
        Label text;
        Canvas spark;
        UiSeriesId series;

        bool full = false;
        bool sparkShown = true; // the shell sheds the sparkline alone where the end group is a little too wide
        float budgetMs = 16.6f;
        bool over = false;
        bool known = false;
        enum class Shown : u8
        {
            Nothing,
            Ok,
            Value
        };
        Shown shown = Shown::Nothing; // the label's message
        std::string msShown, argShown, nameShown, scratch;
        InternedString textClass;
        u64 revision = ~0ull;
        // what the sparkline was last painted for
        bool paintedOver = false;
        bool painted = false;
        u64 paintedSheet = ~0ull;
        u64 seenLayout = ~0ull;
        Vec2 paintedSize{};
        // what the reserved width was measured for
        float fitTextScale = -1.0f;
        bool fitFull = false;
        float widest = 0.0f;
        bool yielded = false;
        u64 roomLayout = ~0ull;
        u64 fitSheet = ~0ull;
        u64 fitRevision = ~0ull;

        [[nodiscard]] UiSeriesRange range() const
        {
            const float top = budgetMs > 0.0f ? budgetMs * SPARK_MAX_BUDGETS : SPARK_MAX_NO_BUDGET_MS;
            return UiSeriesRange{.tail = true, .min = 0.0f, .max = top};
        }

        // The label follows the status bar's frame item: the value in full or over the budget, FRAME OK otherwise.
        void showText()
        {
            if (known && (full || over))
            {
                if (shown != Shown::Value)
                {
                    shown = Shown::Value;
                    argShown.clear();
                    text.setLocalizedText(InternedString(FRAME_VALUE));
                }
                if (argShown != msShown)
                {
                    argShown = msShown;
                    text.setLocalizedArgText("ms", msShown);
                }
            }
            else if (shown != Shown::Ok)
            {
                shown = Shown::Ok;
                text.setLocalizedText(InternedString(FRAME_OK));
            }
            const InternedString wanted(over       ? "statusbar.item.warn"
                                            : full ? "statusbar.item"
                                                   : "statusbar.item.button");
            if (wanted != textClass)
            {
                textClass = wanted;
                text.setStyleClass(wanted);
            }
        }

        // 'Frame time {ms} ms' (the status bar's name), rebuilt when the value text or the language changes
        void showName()
        {
            const u64 now = localization ? localization->revision() : 0;
            if (!known || (msShown == nameShown && now == revision))
            {
                return;
            }
            nameShown = msShown;
            revision = now;
            std::string name;
            if (localization)
            {
                const std::array<MessageArg, 1> args{MessageArg{"ms", std::string_view(msShown)}};
                name = localization->resolve(InternedString(FRAME_NAME), args, scratch);
            }
            else
            {
                name = std::string(FRAME_NAME);
            }
            text.setAccessibleName(name);
        }

        [[nodiscard]] std::string resolved(const std::string_view key, const std::span<const MessageArg> args)
        {
            return localization ? std::string(localization->resolve(InternedString(key), args, scratch))
                                : std::string(key);
        }

        // The label keeps the width of its widest text (FRAME OK in quiet, a three-digit reading), so the items after
        // the readout stay put when the text changes. Measured again when the mode, the text scale, the stylesheet or
        // the language changes.
        void fitWidth()
        {
            const UiTextMeasurer* measurer = scope.hierarchy().textMeasurer();
            const float textScale = engine->uiDisplay().textScale;
            const u64 sheet = styles->generation();
            const u64 now = localization ? localization->revision() : 0;
            if (!measurer || (full == fitFull && textScale == fitTextScale && sheet == fitSheet && now == fitRevision))
            {
                return;
            }
            fitFull = full;
            fitTextScale = textScale;
            fitSheet = sheet;
            fitRevision = now;
            const std::array<MessageArg, 1> args{MessageArg{"ms", std::string_view("000.0")}};
            widest = measurer->measureText(text.entity(), resolved(FRAME_VALUE, args)).x;
            if (!full)
            {
                widest = std::max(widest, measurer->measureText(text.entity(), resolved(FRAME_OK, {})).x);
            }
            widest = std::ceil(widest);
            reserve(yielded ? 0.0f : widest);
        }

        void reserve(const float width)
        {
            UiLayoutItemComponent item = scope.world().getConstComponent<UiLayoutItemComponent>(text.entity());
            if (item.minSize.x != width)
            {
                item.minSize.x = width;
                text.setLayoutItem(item);
            }
        }

        // A bar too narrow for the reserved width (the group overflows to its left, over the transport) gives the
        // reservation up until the group has room for it again.
        void checkRoom()
        {
            const UiHierarchyManager& hierarchy = scope.hierarchy();
            const u64 layout = hierarchy.layoutGeneration();
            const Entity group = hierarchy.parentOf(root.entity());
            if (layout == roomLayout || widest <= 0.0f || !group.isValid() || !hierarchy.isVisibleOf(root.entity()))
            {
                return;
            }
            roomLayout = layout;
            const float scale = hierarchy.layoutScale();
            const float slack =
                (hierarchy.absoluteRectOf(root.entity()).position.x - hierarchy.absoluteRectOf(group).position.x) /
                scale;
            if (!yielded && slack < -0.5f)
            {
                yielded = true;
                reserve(0.0f);
            }
            else if (yielded && slack >= widest - hierarchy.absoluteRectOf(text.entity()).size.x / scale)
            {
                yielded = false;
                reserve(widest);
            }
        }

        void paint()
        {
            const UiStyle style = styles->resolve(InternedString("editor.bar.frame.text"), UiWidgetKind::Label,
                                                  UiInteractionState::Normal, textClass);
            const Vec2 size = paintedSize;
            const UiSeriesId id = series;
            const u32 color = style.textColor;
            spark.update([=](UiCanvasPainter& painter) { painter.series(id, Vec2{}, size, color); });
        }
    };

    EditorFrameHeader::EditorFrameHeader(Engine& engine, const bool full, const float frameBudgetMs) :
        m_state(std::make_unique<State>(engine))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        using namespace panelui;
        // an item of the header like the agent chip: as wide as its content, as high as a toolbar control
        UiRectDesc rootDesc = barDesc(Vec2{}, stack(UiLayoutDirection::Row, 0.0f, {}, UiLayoutAlign::Center));
        rootDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
        state.root = scope.createPanel("editor.bar.frame", rootDesc);
        bindGap(state.root, SPACE_SM);
        bindPadding(state.root, SPACE_SM, SPACE_NONE, SPACE_SM, SPACE_NONE);
        state.root.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SIZE_CONTROL));
        state.root.setStyleClass(InternedString("toolbar.frame"));
        setTip(state.root);

        UiRectDesc textDesc = barDesc(Vec2{});
        textDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
        state.text = scope.createLabel("editor.bar.frame.text", "", textDesc);
        state.text.setParent(state.root);
        state.text.setInteractable(false);

        state.spark = scope.createCanvas("editor.bar.frame.spark", UiCanvasDesc{.rect = barDesc(Vec2{})});
        state.spark.setParent(state.root);
        state.spark.setInteractable(false);
        bindBasis(state.spark, SIZE_SPARK_WIDTH);
        state.spark.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SIZE_SPARK_HEIGHT));
        state.spark.setVisible(false);
        scope.hierarchy().observeSize(state.spark.entity()); // its laid-out size is what the series is drawn over
        state.series = state.spark.createSeries(SPARK_SAMPLES, state.range());
        state.full = !full; // settle() sees the change
        settle(full, frameBudgetMs, false);
        scope.commit();
    }

    EditorFrameHeader::~EditorFrameHeader() = default;

    Widget EditorFrameHeader::root() const { return m_state->root; }

    void EditorFrameHeader::sample(const double ms, const bool over)
    {
        State& state = *m_state;
        state.known = true;
        state.over = over;
        state.msShown = std::format("{:.1f}", ms);
        if (state.full)
        {
            const float value = static_cast<float>(ms);
            state.spark.appendSamples(state.series, std::span<const float>(&value, 1));
        }
        state.showText();
        state.showName();
    }

    void EditorFrameHeader::settle(const bool full, const float frameBudgetMs, const bool over)
    {
        State& state = *m_state;
        if (full != state.full)
        {
            state.full = full;
            state.spark.setVisible(full && state.sparkShown);
            if (full)
            {
                state.spark.clearSeries(state.series); // a new history, not the one before quiet
            }
        }
        if (frameBudgetMs != state.budgetMs)
        {
            state.budgetMs = frameBudgetMs;
            state.spark.setSeriesRange(state.series, state.range());
        }
        state.over = over;
        state.showText();
        state.showName();
        state.fitWidth();
    }

    void EditorFrameHeader::setShown(const bool shown)
    {
        if (m_state->root.isVisible() != shown)
        {
            m_state->root.setVisible(shown);
        }
    }

    void EditorFrameHeader::setSparklineShown(const bool shown)
    {
        State& state = *m_state;
        if (state.sparkShown != shown)
        {
            state.sparkShown = shown;
            state.spark.setVisible(state.full && shown);
        }
    }

    float EditorFrameHeader::width(const bool sparkline)
    {
        State& state = *m_state;
        state.fitWidth();
        const panelui::Density density = panelui::Density::read(state.scope);
        float width = 2.0f * density.sm + state.widest;
        if (state.full && sparkline)
        {
            width += density.sm + state.scope.layoutTokens().find(InternedString(SIZE_SPARK_WIDTH)).value_or(0.0f);
        }
        return width;
    }

    void EditorFrameHeader::update()
    {
        State& state = *m_state;
        state.showName(); // the language: one revision compare
        state.fitWidth();
        state.checkRoom();
        if (!state.full || !state.sparkShown)
        {
            return;
        }
        const u64 sheet = state.styles->generation();
        const u64 layout = state.scope.hierarchy().layoutGeneration();
        const bool restyle = !state.painted || state.over != state.paintedOver || sheet != state.paintedSheet;
        if (!restyle && layout == state.seenLayout)
        {
            return;
        }
        state.seenLayout = layout;
        const Vec2 size = state.spark.size();
        if (size.x <= 0.0f || size.y <= 0.0f || (!restyle && size == state.paintedSize))
        {
            return;
        }
        state.painted = true;
        state.paintedOver = state.over;
        state.paintedSheet = sheet;
        state.paintedSize = size;
        state.paint();
    }
} // namespace spite
