#pragma once

#include <algorithm>
#include <memory>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "EditorContext.hpp"
#include "base/ContentRoot.hpp"
#include "base/Logging.hpp"
#include "control/ControlServer.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiScope.hpp"
#include "ui/UiTextMeasurer.hpp"

// What the editor's panels share: layout descriptors, the control-request helpers and the virtual text Table.
// One panel, one Editor<Panel>.cpp/.hpp pair (EditorConsole, EditorLog, EditorStats, EditorPasses,
// EditorSettingsPanel, ...): the class, its control entries and its document view over
// assets/ui/editor/<panel>.json, where the document owns the containers and the code the slots; the file ends with the
// panel's registrar (EditorPanelRegistry.hpp lists the steps to add one). Anything two
// panels use goes here, not into either panel's file. Each panel is a client of the control protocol: what it
// shows comes from control tables and queries, what it does is a control command queued between frames, and it
// registers a query under its own owner name so the CLI inspects it. A table with a sortable header, a filter bar and
// keyboard rows is FilteredTable (EditorPanelList.hpp); the virtual text Table here is the plain list, with a header
// row (TableDesc::headers) when its columns need titles.
namespace spite::panelui
{
    using Json = nlohmann::json;

    constexpr float MAX_COLUMN_SHARE = 0.6f; // of a table row, a label column's growth cap
    constexpr float THUMB = 10.0f; // kept clear for the scroll thumb
    inline constexpr u32 NO_ROW = static_cast<u32>(-1);

    // The spacing scale of the base sheet (assets/ui/engine.json). The shipped documents bind their gaps,
    // paddings and bar heights to these names; a fallback build binds the same ones, so it follows the density as the
    // document does (PanelDocumentTest.TheFallbackBuildIsTheShippedDocument holds them equal).
    inline constexpr std::string_view SPACE_NONE = "space.none";
    inline constexpr std::string_view SPACE_HAIRLINE = "space.hairline";
    inline constexpr std::string_view SPACE_XXS = "space.xxs";
    inline constexpr std::string_view SPACE_XS = "space.xs";
    inline constexpr std::string_view SPACE_SM = "space.sm";
    inline constexpr std::string_view SPACE_MD = "space.md";
    inline constexpr std::string_view SPACE_LG = "space.lg";
    inline constexpr std::string_view SPACE_XL = "space.xl";
    inline constexpr std::string_view SIZE_ROW = "size.row";
    inline constexpr std::string_view SIZE_CONTROL = "size.control";
    inline constexpr std::string_view SIZE_BAR = "size.bar";
    inline constexpr std::string_view SIZE_TAB = "size.tab";

    // The spaces and sizes a presenter that places its own children reads: the scope's layout token table
    // now, the comfortable values (the base sheet's) for a name the table lacks, as in a UI with no view. A presenter
    // reads it once and again from UiScope::observeLayoutTokens, never per row or frame (DensityFollower).
    struct Density
    {
        float hairline = 1.0f;
        float xxs = 2.0f;
        float xs = 4.0f;
        float sm = 6.0f;
        float md = 8.0f;
        float lg = 12.0f;
        float xl = 16.0f;
        float row = 26.0f;
        float control = 30.0f;
        float bar = 34.0f;
        float tab = 36.0f;
        bool operator==(const Density&) const = default;

        [[nodiscard]] static Density read(const UiScope& scope)
        {
            const UiLayoutTokenTable& tokens = scope.layoutTokens();
            const auto token = [&tokens](const std::string_view name, const float fallback)
            { return tokens.find(InternedString(name)).value_or(fallback); };
            const Density comfortable;
            return Density{.hairline = token(SPACE_HAIRLINE, comfortable.hairline),
                           .xxs = token(SPACE_XXS, comfortable.xxs),
                           .xs = token(SPACE_XS, comfortable.xs),
                           .sm = token(SPACE_SM, comfortable.sm),
                           .md = token(SPACE_MD, comfortable.md),
                           .lg = token(SPACE_LG, comfortable.lg),
                           .xl = token(SPACE_XL, comfortable.xl),
                           .row = token(SIZE_ROW, comfortable.row),
                           .control = token(SIZE_CONTROL, comfortable.control),
                           .bar = token(SIZE_BAR, comfortable.bar),
                           .tab = token(SIZE_TAB, comfortable.tab)};
        }
    };

    // A canvas painter's colour: the active theme's color var `name` (the base sheet's "vars" define each one, a theme
    // overrides it). A name no loaded sheet defines paints magenta, so a missing one shows
    // (PanelColorTokensTest holds every name the painters read in every shipped theme). Read once per redraw, not per
    // primitive; a painter redraws on a theme change from a ThemeObserver.
    inline constexpr u32 MISSING_CANVAS_COLOR = uiCanvasColor(255, 0, 255);
    [[nodiscard]] inline u32 canvasColor(const UiScope& scope, const std::string_view name)
    {
        return scope.colorTokens().find(InternedString(name)).value_or(MISSING_CANVAS_COLOR);
    }
    // `color` with its alpha scaled by `alpha`/255.
    [[nodiscard]] constexpr u32 withAlpha(const u32 color, const u8 alpha)
    {
        return (color & 0x00FFFFFFu) | (((color >> 24) * alpha / 255u) << 24);
    }

    // Runs `onChange` when the theme's tokens changed (a theme switch, a reloaded layer): a painter's way to redraw.
    class ThemeObserver
    {
    public:
        ThemeObserver(UiScope& scope, std::function<void()> onChange) :
            m_scope(&scope), m_id(scope.observeLayoutTokens(std::move(onChange)))
        {
        }
        ~ThemeObserver() { m_scope->unobserveLayoutTokens(m_id); }
        ThemeObserver(const ThemeObserver&) = delete;
        ThemeObserver& operator=(const ThemeObserver&) = delete;

    private:
        UiScope* m_scope;
        u32 m_id;
    };

    // A height the code gives what it builds: a size of the scale plus `count` of a space (a button in a bar is the
    // control and a hairline above and below it; a field row a text line and an xxs above and below).
    struct DensityHeight
    {
        float Density::* size;
        float Density::* space = &Density::hairline;
        float count = 0.0f;
        [[nodiscard]] float of(const Density& density) const { return density.*size + count * (density.*space); }
    };
    inline constexpr DensityHeight HEIGHT_ROW{&Density::row};
    inline constexpr DensityHeight HEIGHT_CONTROL{&Density::control};
    inline constexpr DensityHeight HEIGHT_BAR{&Density::bar};
    inline constexpr DensityHeight HEIGHT_BUTTON{&Density::control, &Density::hairline, 2.0f};
    inline constexpr DensityHeight HEIGHT_FIELD{&Density::row, &Density::xxs, 2.0f};

    // The heights and spaces a panel's code-built controls take, kept up to date: apply() runs a placement
    // now and again from UiScope::observeLayoutTokens, so a density change re-places them and a quiet frame reads no
    // token. Owned by the panel's state; the placements must not outlive their widgets.
    class DensityFollower
    {
    public:
        explicit DensityFollower(UiScope& scope) : m_scope(&scope), m_density(Density::read(scope))
        {
            m_observer = scope.observeLayoutTokens([this] { follow(); });
        }
        ~DensityFollower() { m_scope->unobserveLayoutTokens(m_observer); }
        DensityFollower(const DensityFollower&) = delete;
        DensityFollower& operator=(const DensityFollower&) = delete;

        [[nodiscard]] const Density& density() const { return m_density; }

        void apply(std::function<void(const Density&)> placement)
        {
            placement(m_density);
            m_placements.push_back(std::move(placement));
        }

        // The widget's height, its width kept; returns the widget.
        template <class W>
        W track(const W& widget, const DensityHeight height)
        {
            apply(
                [target = static_cast<Widget>(widget), height](const Density& density) mutable
                {
                    if (const float value = height.of(density); target.size().y != value)
                    {
                        target.setSize(Vec2{target.size().x, value});
                    }
                });
            return widget;
        }

    private:
        void follow()
        {
            if (const Density now = Density::read(*m_scope); now != m_density)
            {
                m_density = now;
                for (const auto& placement : m_placements)
                {
                    placement(m_density);
                }
            }
        }

        UiScope* m_scope;
        Density m_density;
        u32 m_observer = 0;
        std::vector<std::function<void(const Density&)>> m_placements;
    };

    inline void bindGap(Widget widget, const std::string_view token)
    {
        widget.bindLayoutToken(UiLayoutField::Gap, InternedString(token));
    }

    inline void bindPadding(Widget widget, const std::string_view left, const std::string_view top,
                            const std::string_view right, const std::string_view bottom)
    {
        widget.bindLayoutToken(UiLayoutField::PaddingLeft, InternedString(left));
        widget.bindLayoutToken(UiLayoutField::PaddingTop, InternedString(top));
        widget.bindLayoutToken(UiLayoutField::PaddingRight, InternedString(right));
        widget.bindLayoutToken(UiLayoutField::PaddingBottom, InternedString(bottom));
    }

    inline void bindFitPadding(Widget widget, const std::string_view x, const std::string_view y)
    {
        widget.bindLayoutToken(UiLayoutField::FitPaddingX, InternedString(x));
        widget.bindLayoutToken(UiLayoutField::FitPaddingY, InternedString(y));
    }

    inline void bindInputPadding(Widget widget)
    {
        World& world = widget.scope()->world();
        if (!world.tryGetConstComponent<UiContentSizeComponent>(widget.entity()))
            world.addComponent<UiContentSizeComponent>(widget.entity(),
                                                       UiContentSizeComponent{.fitWidth = false, .fitHeight = false});
        widget.bindLayoutToken(UiLayoutField::FitPaddingX, InternedString(SPACE_XS));
    }

    inline void bindBasis(Widget widget, const std::string_view token)
    {
        widget.bindLayoutToken(UiLayoutField::Basis, InternedString(token));
    }

    // A toolbar row of the shipped documents: the scale's gap, left and right padding, half of it above and below,
    // and the bar's height.
    inline void bindToolbar(const Widget& widget, const std::string_view gap = SPACE_XS)
    {
        bindGap(widget, gap);
        bindPadding(widget, SPACE_XS, SPACE_XXS, SPACE_XS, SPACE_XXS);
        bindBasis(widget, SIZE_BAR);
    }

    // A status line of the shipped documents: a row high, padded at the sides only.
    inline void bindStatusRow(const Widget& widget)
    {
        bindGap(widget, SPACE_XS);
        bindPadding(widget, SPACE_XS, SPACE_NONE, SPACE_XS, SPACE_NONE);
        bindBasis(widget, SIZE_ROW);
    }

    // A toolbar's group the content's width fits (a bar of buttons): the gap, no padding around its fit.
    inline void bindFitBar(const Widget& widget)
    {
        bindGap(widget, SPACE_XS);
        bindFitPadding(widget, SPACE_NONE, SPACE_NONE);
    }

    inline UiRectDesc topLeft(const Vec2 offset = {}, const Vec2 size = {})
    {
        UiRectDesc desc;
        desc.offset = offset;
        desc.size = size;
        desc.pivot = Vec2{0.0f, 0.0f};
        return desc;
    }

    // A settings form's root: it stretches with its column up to a readable width, so a wide panel keeps each label
    // beside its control instead of a control as wide as the panel.
    inline constexpr float FORM_MAX_WIDTH = 720.0f;
    inline UiRectDesc formRect()
    {
        UiRectDesc desc = topLeft();
        UiLayoutItemComponent item;
        item.maxSize = Vec2{FORM_MAX_WIDTH, 0.0f};
        desc.layoutItem = item;
        return desc;
    }

    // A layout container's component: children in one line along `direction`.
    inline UiLayoutComponent stack(const UiLayoutDirection direction, const float gap, const Vec4 padding = {},
                                   const UiLayoutAlign align = UiLayoutAlign::Start)
    {
        UiLayoutComponent layout;
        layout.direction = direction;
        layout.gap = gap;
        layout.padding = padding;
        layout.align = align;
        return layout;
    }

    // An item that keeps its authored main size however little room there is.
    inline UiLayoutItemComponent fixedItem()
    {
        UiLayoutItemComponent item;
        item.shrink = 0.0f;
        return item;
    }

    // An item that takes the container's free main size.
    inline UiLayoutItemComponent fillItem()
    {
        UiLayoutItemComponent item;
        item.grow = 1.0f;
        item.basis = 0.0f;
        return item;
    }

    // An item of a fixed main size that never shrinks (a table that is its header while its note takes the rest).
    inline UiLayoutItemComponent basisItem(const float basis)
    {
        UiLayoutItemComponent item;
        item.shrink = 0.0f;
        item.basis = basis;
        return item;
    }

    // A panel body's empty state, one look in every panel: the hint wrapped to the body's width and
    // centred in what the content leaves (FilteredTable's note is the same label). Hidden until its owner shows it,
    // which puts it in the slot after the content it stands for and gives that content a header-only item.
    inline constexpr std::string_view EMPTY_NOTE_CLASS = "list.row.empty";

    inline Label emptyNote(UiScope& scope, const std::string_view id)
    {
        UiRectDesc desc = topLeft();
        desc.layoutItem = fillItem();
        Label note = scope.createLabel(InternedString(id), "", desc);
        note.setStyleClass(InternedString(EMPTY_NOTE_CLASS));
        note.setInteractable(false);
        note.setVisible(false);
        return note;
    }

    // A top-left rect of the given size laid out by its parent: a fixed item, or a container.
    inline UiRectDesc barDesc(const Vec2 size, const std::optional<UiLayoutComponent>& layout = std::nullopt)
    {
        UiRectDesc desc = topLeft({}, size);
        desc.layout = layout;
        desc.layoutItem = fixedItem();
        return desc;
    }

    // A button or label whose width is its text's (its height the caller's); bind its padding with bindFitPadding.
    inline UiRectDesc fitWidthDesc()
    {
        UiRectDesc desc = barDesc(Vec2{});
        desc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = false};
        return desc;
    }

    // A panel root whose children stack top to bottom at the panel's width.
    inline UiRectDesc columnDesc(const float gap = 0.0f, const Vec4 padding = {})
    {
        UiRectDesc desc = topLeft();
        desc.layout = stack(UiLayoutDirection::Column, gap, padding, UiLayoutAlign::Stretch);
        return desc;
    }

    inline void place(Widget& widget, const Vec2 offset, const Vec2 size)
    {
        if (widget.offset() != offset)
        {
            widget.setOffset(offset);
        }
        if (widget.size() != size)
        {
            widget.setSize(size);
        }
    }

    // Changing text without interning it: a read-only input's UiValue.
    inline TextInput readOnlyText(UiScope& scope, const InternedString id, const UiRectDesc& desc = topLeft())
    {
        TextInput text = scope.createTextInput(id, desc);
        text.setInteractable(false);
        return text;
    }

    // Whether the text changed (and was written).
    inline bool show(TextInput& text, const std::string_view value)
    {
        if (text.value() == value)
        {
            return false;
        }
        text.setText(value);
        return true;
    }

    inline bool shown(const UiScope& scope, const Widget& widget)
    {
        return scope.hierarchy().layout().isVisible(widget.entity()) && widget.size().x > 0.0f &&
            widget.size().y > 0.0f;
    }

    // A widget's size as the container laid it out, in layout units (a plain panel's own size is what was authored);
    // zero while it is hidden or before the first resolve.
    inline Vec2 resolvedSize(const UiScope& scope, const Widget& widget)
    {
        const UiHierarchyManager& hierarchy = scope.hierarchy();
        if (!hierarchy.isVisibleOf(widget.entity()))
        {
            return Vec2{};
        }
        return hierarchy.absoluteRectOf(widget.entity()).size / hierarchy.layoutScale();
    }

    // Runs a request; the reply parsed (an object with ok false when unparsable).
    inline Json request(ControlServer& control, const Json& line)
    {
        Json reply = Json::parse(control.handle(line.dump()), nullptr, false);
        return reply.is_object() ? reply : Json{{"ok", false}, {"error", "unparsable reply"}};
    }

    // Dispatches queued request lines; a refused one is logged.
    inline void dispatch(ControlServer& control, std::vector<std::string>& pending, const std::string_view who)
    {
        std::vector<std::string> requests;
        requests.swap(pending);
        for (const std::string& line : requests)
        {
            if (const Json reply = Json::parse(control.handle(line), nullptr, false);
                !reply.is_object() || !reply.value("ok", false))
            {
                SWARNING_LOG("{}: {} -> {}", who, line, reply.dump());
            }
        }
    }

    // Code-built content goes into its slot once, so a re-fill keeps the children's order.
    inline void putIn(UiScope& scope, const Widget& slot, Widget item)
    {
        if (scope.hierarchy().parentOf(item.entity()) != slot.entity())
        {
            item.setParent(slot);
        }
    }

    // The editor's tooltip convention: the widget's tooltip is the message "tip.<element id>" of the editor table
    // (assets/loc/<locale>/editor.json), with the shortcut (a literal such as "Ctrl+S") beside it. Shown after the
    // hover delay and on keyboard focus (Widget::setTooltip); a panel document names its own key with "tooltip".
    inline void setTip(Widget widget, const std::string_view shortcut = {})
    {
        const InternedString id = widget.scope()->world().getConstComponent<UiIdentityComponent>(widget.entity()).id;
        widget.setTooltip(InternedString("editor.tip." + std::string(id.toView())), shortcut);
    }

    // A slot's child class with a part suffix (`<class>.button`); none while the slot names no class.
    inline InternedString classPart(const InternedString base, const std::string_view suffix)
    {
        return base.isValid() ? InternedString(std::string(base.toView()) + std::string(suffix)) : InternedString{};
    }

    // The one look of a button that stays pressed in (a tab, a filter, a view mode, a lit toolbar toggle): the class
    // `state.selected` in front of its own class list (it wins what both name; every stylesheet gives it a fill and a
    // border) and the selected state uiTree reads. Nothing is written while the state is the one shown; the text never
    // changes. A slot that gives the button its class goes through setOwnClass, which keeps the state.
    inline constexpr std::string_view SELECTED_CLASS = "state.selected";

    // whether the first token of a class list is the selected class (not a longer name that starts with it)
    inline bool isLit(const std::string_view classes) { return classes.substr(0, classes.find(' ')) == SELECTED_CLASS; }

    inline void setOwnClass(Widget widget, const InternedString own)
    {
        const InternedString current =
            widget.scope()->world().getConstComponent<UiIdentityComponent>(widget.entity()).styleClass;
        widget.setStyleClass(
            isLit(current.toView())
                ? InternedString(std::string(SELECTED_CLASS) + (own.isValid() ? " " : "") + std::string(own.toView()))
                : own);
    }

    inline void showSelected(Widget widget, const bool on)
    {
        World& world = widget.scope()->world();
        const std::string_view current =
            world.getConstComponent<UiIdentityComponent>(widget.entity()).styleClass.toView();
        if (isLit(current) != on)
        {
            std::string_view own = on ? current : current.substr(SELECTED_CLASS.size());
            own.remove_prefix(!on && own.starts_with(' ') ? 1 : 0);
            widget.setStyleClass(InternedString(
                on ? std::string(SELECTED_CLASS) + (own.empty() ? "" : " ") + std::string(own) : std::string(own)));
        }
        const auto* accessible = world.tryGetConstComponent<UiAccessibleComponent>(widget.entity());
        if (!accessible || !accessible->hasSelected || accessible->selected != on)
        {
            widget.setAccessibleSelected(on);
        }
    }

    // The slot's fields: `scroll` and `childClass`.
    inline const UiSlotComponent& slotFields(UiScope& scope, const Widget& slot)
    {
        return scope.world().getConstComponent<UiSlotComponent>(slot.entity());
    }

    // Opens a panel's document view under its docked host; a refused or unreadable document leaves the fallback.
    inline std::unique_ptr<UiDocumentView> openView(Engine& engine, const std::string_view name,
                                                    const std::string_view document, UiViewContract contract,
                                                    const Widget& host)
    {
        auto view = std::make_unique<UiDocumentView>(editorUi(engine).ui(), std::string(name), std::move(contract),
                                                     host.entity());
        if (!view->load(resolveContentPath(document)))
        {
            SWARNING_LOG("{}: {} did not apply ({} problems): the default panel shows", name, document,
                         view->problems().size());
        }
        return view;
    }

    // The fallback's root: a column filling the host, the shipped documents' root.
    inline void columnRoot(const Widget& root, const float gap = 0.0f, const Vec4 padding = {})
    {
        Widget panel = root;
        panel.setPivot(Vec2{0.0f, 0.0f});
        panel.setLayout(stack(UiLayoutDirection::Column, gap, padding, UiLayoutAlign::Stretch));
        panel.setLayoutItem(fillItem());
    }

    // A slot panel that fills the column's free height and holds one list.
    inline Panel fillSlotPanel(UiScope& scope, const std::string_view id, const Widget& parent)
    {
        Panel slot = scope.createPanel(InternedString(id), columnDesc());
        slot.setLayoutItem(fillItem());
        slot.setParent(parent);
        return slot;
    }

    // The fallback builds' bars, as the shipped documents have them: the toolbar row (bar high, padded), the bar of
    // buttons the content's width fits (control high) and the status line (row high, padded at the sides). Their
    // tokens are bound, so they follow the density; the sizes written here are the density's at build time.
    inline Panel toolbarPanel(UiScope& scope, const std::string_view id, const Widget& parent,
                              const std::string_view gap = SPACE_XS, const UiLayoutAlign align = UiLayoutAlign::Stretch)
    {
        const Density density = Density::read(scope);
        Panel bar = scope.createPanel(InternedString(id),
                                      barDesc(Vec2{0.0f, density.bar},
                                              stack(UiLayoutDirection::Row, gap == SPACE_MD ? density.md : density.xs,
                                                    Vec4{density.xs, density.xxs, density.xs, density.xxs}, align)));
        bindToolbar(bar, gap);
        bar.setParent(parent);
        return bar;
    }

    inline Panel fitBarPanel(UiScope& scope, const std::string_view id, const Widget& parent)
    {
        const Density density = Density::read(scope);
        UiRectDesc desc = barDesc(Vec2{0.0f, density.control}, stack(UiLayoutDirection::Row, density.xs));
        desc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = false};
        Panel bar = scope.createPanel(InternedString(id), desc);
        bindFitBar(bar);
        bar.setParent(parent);
        return bar;
    }

    // A bar of the buttons its panel's width holds, then another line: as tall as its lines
    inline Panel wrapRowPanel(UiScope& scope, const std::string_view id, const Widget& parent)
    {
        const Density density = Density::read(scope);
        UiLayoutComponent line = stack(UiLayoutDirection::Row, density.xs,
                                       Vec4{density.xs, density.xxs, density.xs, density.xxs}, UiLayoutAlign::Stretch);
        line.wrap = true;
        UiRectDesc desc = barDesc(Vec2{}, line);
        desc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        Panel bar = scope.createPanel(InternedString(id), desc);
        bindGap(bar, SPACE_XS);
        bindPadding(bar, SPACE_XS, SPACE_XXS, SPACE_XS, SPACE_XXS);
        bar.setParent(parent);
        return bar;
    }

    // A toolbar whose buttons wrap onto another line where the panel is narrower than they are: the bar
    // of the slot is a wrapping line, and both are as tall as their lines. Returns the bar.
    inline Panel wrapBarPanel(UiScope& scope, const std::string_view toolbarId, const std::string_view barId,
                              const Widget& parent)
    {
        const Density density = Density::read(scope);
        UiRectDesc desc =
            barDesc(Vec2{},
                    stack(UiLayoutDirection::Column, 0.0f, Vec4{density.xs, density.xxs, density.xs, density.xxs},
                          UiLayoutAlign::Stretch));
        desc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        Panel toolbar = scope.createPanel(InternedString(toolbarId), desc);
        bindPadding(toolbar, SPACE_XS, SPACE_XXS, SPACE_XS, SPACE_XXS);
        toolbar.setParent(parent);
        UiLayoutComponent line = stack(UiLayoutDirection::Row, density.xs);
        line.wrap = true;
        UiRectDesc lineDesc = barDesc(Vec2{}, line);
        lineDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        Panel bar = scope.createPanel(InternedString(barId), lineDesc);
        bindGap(bar, SPACE_XS);
        bar.setParent(toolbar);
        return bar;
    }

    inline Panel statusPanel(UiScope& scope, const std::string_view id, const Widget& parent)
    {
        const Density density = Density::read(scope);
        Panel status = scope.createPanel(
            InternedString(id),
            barDesc(Vec2{0.0f, density.row},
                    stack(UiLayoutDirection::Row, density.xs, Vec4{density.xs, 0.0f, density.xs, 0.0f})));
        bindStatusRow(status);
        status.setParent(parent);
        return status;
    }

    // A container of code-built content: children in a line, the scale's xs between them (and around them when
    // `padded`), both bound so the density moves them. The size is the authored one (0: the layout's).
    inline Panel stackPanel(UiScope& scope, const std::string_view id, const Vec2 size,
                            const UiLayoutDirection direction, const UiLayoutAlign align = UiLayoutAlign::Start,
                            const bool padded = false)
    {
        const float xs = Density::read(scope).xs;
        Panel panel = scope.createPanel(
            InternedString(id), barDesc(size, stack(direction, xs, padded ? Vec4{xs, xs, xs, xs} : Vec4{}, align)));
        bindGap(panel, SPACE_XS);
        if (padded)
        {
            bindPadding(panel, SPACE_XS, SPACE_XS, SPACE_XS, SPACE_XS);
        }
        return panel;
    }

    // A virtual list of text rows: an optional button, then cells whose
    // widths are given (the last fills the row). Each cell clips its text to its
    // column; a label column (all but the last) grows to its widest shown text, up
    // to MAX_COLUMN_SHARE of the row, and is shown narrower only in a row without room for it. Rows are read through
    // `read` only for visible slots; a bind writes only the cells whose text
    // changed.
    struct TableDesc
    {
        std::string_view id;
        std::vector<float> columns; // all but the last cell's minimum widths
        u32 cells = 1;
        float buttonWidth = 0.0f; // > 0: a button before the cells, at least this wide and as wide as its widest label
        std::function<void(u32 row, std::span<std::string> cells, std::string& button)> read;
        std::function<void(u32 row)> press;
        // The accessible name of the row's button, when its text ("Show") does not say what it acts on
        std::function<std::string(u32 row)> buttonName;
        std::function<void(u32 row)> select; // a click on the row outside its button
        // A header row over the list: one entry per cell (the button's column has none). The title is a loc key (the
        // unit goes in it: "Self (ms)"); a sortable one is a button that calls `sort(cell)` and shows the sort caret
        // (setSort); `headerName(cell, title, order)` is its accessible name, order "none", "ascending" or
        // "descending".
        struct Header
        {
            InternedString titleKey;
            bool sortable = false;
        };
        std::vector<Header> headers;
        // The accessible name of each cell (a loc key) of a table without headers; a header's title names its cell.
        std::vector<InternedString> cellNameKeys;
        std::function<void(u32 cell)> sort;
        std::function<std::string(u32 cell, std::string_view title, std::string_view order)> headerName;
        // The least width the last cell keeps (its own title raises it): a label column grows to its text only while
        // the last cell has it, and in a row too narrow the label columns give it up down to their titles.
        float lastMin = 0.0f;
        // One bit per label cell whose text is never cut to make room for the last cell (a short word, unlike a name):
        // the column keeps the room of its widest shown text as its floor.
        u32 wholeCells = 0;
        bool ellipsizeLast = false; // the last cell ends its text with an ellipsis where the column is short (a path)
        std::function<bool(u32 row)> isSelected;
    };

    class Table
    {
    public:
        Table(UiScope& scope, TableDesc desc) :
            m_scope(&scope), m_desc(std::move(desc)), m_density(Density::read(scope))
        {
            m_columns = m_shown = m_desc.columns;
            m_buttonWidth = m_desc.buttonWidth;
            m_textScale = scope.hierarchy().textScale();
            m_floors.assign(m_columns.size(), 0.0f);
            m_lastMin = m_desc.lastMin;
            m_markWidth = m_density.lg + m_density.xs;
            UiVirtualListDesc listDesc;
            listDesc.rect = topLeft();
            listDesc.rowHeight = m_density.row;
            listDesc.createRow = [this](const u32 slot) -> Widget { return createSlot(slot); };
            listDesc.bindRow = [this](const u32 slot, const u32 row)
            {
                m_slots[slot].row = row;
                bind(slot);
            };
            list = scope.createVirtualList(InternedString(m_desc.id), std::move(listDesc));
            if (!m_desc.headers.empty())
            {
                buildHeader();
            }
            m_observer = scope.observeLayoutTokens([this] { follow(); });
        }

        ~Table() { m_scope->unobserveLayoutTokens(m_observer); }
        Table(const Table&) = delete;
        Table& operator=(const Table&) = delete;

        VirtualList list;
        u64 writes = 0; // cell and button texts rewritten
        u64 binds = 0; // slot binds that read a row

        // What a parent takes: the list, or the header over the list when the table has one.
        [[nodiscard]] Widget widget() const { return m_root.isValid() ? Widget(m_root) : Widget(list); }

        // A text scale change since the rows were fitted: the columns and the button fit the shown texts again. A
        // compare when nothing changed.
        void followText()
        {
            if (const float scale = m_scope->hierarchy().textScale(); scale != m_textScale)
            {
                m_textScale = scale;
                refit();
            }
        }

        // The column the rows are sorted by (-1: none) and its direction, shown as a caret on its header.
        void setSort(const i32 cell, const bool descending)
        {
            if (cell == m_sortCell && descending == m_sortDescending)
            {
                return;
            }
            m_sortCell = cell;
            m_sortDescending = descending;
            bindHeader();
        }
        [[nodiscard]] i32 sortCell() const { return m_sortCell; }
        [[nodiscard]] bool sortDescending() const { return m_sortDescending; }
        [[nodiscard]] u32 headerCount() const { return static_cast<u32>(m_heads.size()); }
        // the title as the locale has it now
        [[nodiscard]] std::string headerTitle(const u32 cell) const { return resolve(m_desc.headers[cell].titleKey); }
        [[nodiscard]] bool headerSortable(const u32 cell) const { return m_desc.headers[cell].sortable; }

        void setRowCount(const u32 count)
        {
            if (list.rowCount() != count)
            {
                list.setRowCount(count);
            }
        }

        // Re-reads the visible rows (the rows' data shifted) and the header's names (a locale change).
        void rebindVisible()
        {
            for (u32 slot = 0; slot < m_slots.size(); ++slot)
            {
                bind(slot);
            }
            if (!m_heads.empty())
            {
                bindHeader();
                if (fitHeaders())
                {
                    relayout();
                }
            }
        }

        void rebindRow(const u32 row)
        {
            for (u32 slot = 0; slot < m_slots.size(); ++slot)
            {
                if (m_slots[slot].row == row)
                {
                    bind(slot);
                }
            }
        }

        [[nodiscard]] bool atBottom() const
        {
            const float max = static_cast<float>(list.rowCount()) * list.rowHeight() - list.size().y;
            return list.scrollOffset().y >= max - 1.0f;
        }

        void scrollToBottom() { list.setScrollOffset(Vec2{0.0f, 1.0e9f}); }

        // The class of every row's root; its parts take `<class>.button`, `.cell` and `.text` (a slot's childClass).
        void setRowClass(const InternedString rowClass)
        {
            if (m_rowClass == rowClass)
            {
                return;
            }
            m_rowClass = rowClass;
            for (const Slot& slot : m_slots)
            {
                style(slot);
            }
        }

        // The width the last cell's text has in a row, as layout places it (the label columns as they are shown).
        [[nodiscard]] float lastCellWidth() const
        {
            float x = m_density.xs + (m_buttonWidth > 0.0f ? m_buttonWidth + m_density.xs : 0.0f);
            for (u32 cell = 0; cell + 1 < m_desc.cells; ++cell)
            {
                x += (cell < m_shown.size() ? m_shown[cell] : 0.0f) + m_density.xs;
            }
            return std::max(0.0f, rowWidth() - x - m_density.xs);
        }

        // The element whose style the last cell's text paints in: the list itself before it has a row.
        [[nodiscard]] Entity textElement() const
        {
            return m_slots.empty() ? list.entity() : m_slots.front().cells.back().entity();
        }

        [[nodiscard]] u32 firstRow() const { return static_cast<u32>(list.scrollOffset().y / list.rowHeight()); }
        [[nodiscard]] float rowHeight() const { return list.rowHeight(); }
        [[nodiscard]] u32 slotCount() const { return static_cast<u32>(m_slots.size()); }

        // The shown rows' cells, top to bottom.
        [[nodiscard]] std::vector<std::vector<std::string>> visibleCells() const
        {
            std::vector<const Slot*> slots;
            for (const Slot& slot : m_slots)
            {
                if (slot.row != NO_ROW && slot.row < list.rowCount())
                {
                    slots.push_back(&slot);
                }
            }
            std::ranges::sort(slots, {}, &Slot::row);
            std::vector<std::vector<std::string>> cells;
            for (const Slot* slot : slots)
            {
                cells.push_back(slot->shown);
            }
            return cells;
        }

    private:
        struct Slot
        {
            Panel root;
            Button button;
            std::vector<Panel> clips; // one per cell: clips its text to the column
            std::vector<TextInput> cells;
            std::vector<std::string> shown;
            std::string buttonShown;
            std::string buttonNameShown;
            u32 row = NO_ROW;
            float width = -1.0f;
        };

        struct Head
        {
            Panel cell;
            Button button; // a sortable column's header
            Label text; // a plain one's
            Label mark; // the sort caret
            std::string title;
            std::string name;
        };

        // the header row is a control high, as FilteredTable's
        [[nodiscard]] float headerHeight() const { return m_density.control; }

        void buildHeader()
        {
            const std::string id(m_desc.id);
            UiRectDesc rootDesc = topLeft();
            rootDesc.layout = stack(UiLayoutDirection::Column, 0.0f, {}, UiLayoutAlign::Stretch);
            m_root = m_scope->createPanel(InternedString(id + ".panel"), rootDesc);
            m_header = m_scope->createPanel(InternedString(id + ".header"), barDesc(Vec2{0.0f, headerHeight()}));
            m_header.setStyleClass(InternedString("list.row.header"));
            m_header.setAccessibleRole(UiRole::Row);
            m_header.setParent(m_root);
            if (m_desc.buttonWidth > 0.0f)
            {
                m_spacer = m_scope->createPanel(InternedString(id + ".header.spacer"),
                                                barDesc(Vec2{m_desc.buttonWidth, headerHeight()}));
                m_spacer.setInteractable(false);
                m_spacer.setParent(m_header);
            }
            for (u32 cell = 0; cell < m_desc.headers.size(); ++cell)
            {
                const TableDesc::Header& header = m_desc.headers[cell];
                Head& head = m_heads.emplace_back();
                const bool last = cell + 1 == m_desc.cells;
                UiRectDesc desc = barDesc(Vec2{last ? 0.0f : m_columns[cell], headerHeight()},
                                          stack(UiLayoutDirection::Row, 0.0f, {}, UiLayoutAlign::Stretch));
                if (last)
                {
                    desc.layoutItem = fillItem();
                }
                head.cell = m_scope->createPanel(InternedString(id + ".header.cell"), desc);
                head.cell.setAccessibleRole(UiRole::ColumnHeader);
                head.cell.setAccessibleNameKey(header.titleKey);
                m_scope->world().getComponent<UiStateComponent>(head.cell.entity()).clipsChildren = true;
                head.cell.setParent(m_header);
                UiRectDesc fill = topLeft();
                fill.layoutItem = fillItem();
                if (header.sortable)
                {
                    head.button = m_scope->createButton(InternedString(id + ".header.button"), "", fill);
                    head.button.setStyleClass(InternedString("list.row.header.button"));
                    head.button.setLocalizedText(header.titleKey);
                    head.button.setParent(head.cell);
                    head.button.onClick(
                        [this, cell]
                        {
                            if (m_desc.sort)
                            {
                                m_desc.sort(cell);
                            }
                        });
                    head.mark =
                        m_scope->createLabel(InternedString(id + ".header.mark"), "", barDesc(Vec2{m_markWidth, 0.0f}));
                    head.mark.setStyleClass(InternedString("list.row.header.mark"));
                    head.mark.setIcon("caret-down");
                    head.mark.setInteractable(false);
                    head.mark.setParent(head.cell);
                    head.mark.setVisible(false);
                }
                else
                {
                    head.text = m_scope->createLabel(InternedString(id + ".header.text"), "", fill);
                    head.text.setStyleClass(InternedString("list.row.header.text"));
                    head.text.setLocalizedText(header.titleKey);
                    head.text.setInteractable(false);
                    head.text.setParent(head.cell);
                }
            }
            list.setParent(m_root);
            list.setLayoutItem(fillItem());
            layoutHeader();
            bindHeader();
        }

        [[nodiscard]] std::string resolve(const InternedString key) const
        {
            const Localization* localization = m_scope->hierarchy().localization();
            if (!localization && m_scope->world().services().contains<Localization>())
            {
                localization = &m_scope->world().services().get<Localization>();
            }
            std::string scratch;
            // a key the table does not have yet (the panel is built before the editor's strings load) has no title
            return localization && localization->contains(key) ? std::string(localization->resolve(key, scratch))
                                                               : std::string();
        }

        // The header at the rows' geometry: the same inset, gaps and column widths, the scroll thumb's room at the end
        void layoutHeader()
        {
            const float height = headerHeight();
            m_header.setLayout(stack(UiLayoutDirection::Row, m_density.xs,
                                     Vec4{m_density.xs, 0.0f, THUMB + m_density.xs, 0.0f}, UiLayoutAlign::Start));
            if (m_header.size().y != height)
            {
                m_header.setSize(Vec2{0.0f, height});
            }
            if (m_spacer.isValid())
            {
                m_spacer.setSize(Vec2{m_buttonWidth, height});
            }
            for (u32 cell = 0; cell < m_heads.size(); ++cell)
            {
                if (cell + 1 < m_desc.cells)
                {
                    m_heads[cell].cell.setSize(Vec2{m_shown[cell], height});
                }
                else if (m_heads[cell].cell.size().y != height)
                {
                    m_heads[cell].cell.setSize(Vec2{0.0f, height});
                }
            }
        }

        void bindHeader()
        {
            for (u32 cell = 0; cell < m_heads.size(); ++cell)
            {
                Head& head = m_heads[cell];
                head.title = resolve(m_desc.headers[cell].titleKey);
                if (!m_desc.headers[cell].sortable)
                {
                    continue;
                }
                const bool sorted = static_cast<i32>(cell) == m_sortCell;
                if (head.mark.isVisible() != sorted)
                {
                    head.mark.setVisible(sorted);
                }
                if (const std::string_view icon = m_sortDescending ? "caret-down" : "caret-up";
                    sorted && head.mark.icon() != icon)
                {
                    head.mark.setIcon(icon);
                }
                if (m_desc.headerName && !head.title.empty())
                {
                    std::string name = m_desc.headerName(cell, head.title,
                                                         !sorted                ? "none"
                                                             : m_sortDescending ? "descending"
                                                                                : "ascending");
                    if (name != head.name)
                    {
                        head.name = std::move(name);
                        head.button.setAccessibleName(head.name);
                    }
                }
            }
        }

        // Widens the label columns to their headers' texts and caret (up to a share of the row), and raises the
        // last cell's minimum to its own title: a title is cut only where the row has no room. True when the titles
        // were measured: the columns, their floors and the last minimum may have moved, so the rows place again.
        bool fitHeaders()
        {
            const UiTextMeasurer* measurer = m_scope->hierarchy().textMeasurer();
            if (!measurer)
            {
                return false;
            }
            for (const Head& head : m_heads)
            {
                if (head.mark.isValid())
                {
                    // the caret's glyph as the theme sizes it, and a gap before it
                    m_markWidth = measurer->measureText(head.mark.entity(), "caret-down").x + m_density.xs;
                    break;
                }
            }
            const auto wanted = [&](const Head& head)
            {
                return measurer
                           ->measureText(head.button.isValid() ? head.button.entity() : head.text.entity(), head.title)
                           .x +
                    m_density.md + (head.mark.isValid() ? m_markWidth : 0.0f); // the button's rounding room
            };
            m_lastMin = std::max(m_desc.lastMin, m_lastContent);
            if (!m_heads.empty() && m_heads.size() == m_desc.cells && !m_heads.back().title.empty())
            {
                m_lastMin = std::max(m_lastMin, wanted(m_heads.back()));
            }
            for (u32 cell = 0; cell < m_heads.size() && cell < m_columns.size(); ++cell)
            {
                if (m_heads[cell].title.empty())
                {
                    continue;
                }
                m_floors[cell] = wanted(m_heads[cell]);
                // a title is the column's floor, so the others give way to it (shrinkColumns): only its share caps it
                m_columns[cell] = std::max(m_columns[cell], std::min(m_floors[cell], rowWidth() * MAX_COLUMN_SHARE));
            }
            for (Head& head : m_heads)
            {
                if (head.mark.isValid() && head.mark.size().x != m_markWidth)
                {
                    head.mark.setSize(Vec2{m_markWidth, 0.0f});
                }
            }
            return true;
        }

        void relayout()
        {
            m_shownWidth = rowWidth();
            shrinkColumns();
            for (Slot& slot : m_slots)
            {
                slot.width = -1.0f;
                layout(slot);
            }
            if (!m_heads.empty())
            {
                layoutHeader();
            }
        }

        void style(const Slot& slot)
        {
            const auto part = [this](const std::string_view suffix) { return classPart(m_rowClass, suffix); };
            Widget root = slot.root;
            root.setStyleClass(m_rowClass);
            if (slot.button.isValid())
            {
                Button button = slot.button;
                button.setStyleClass(part(".button"));
            }
            for (u32 cell = 0; cell < slot.cells.size(); ++cell)
            {
                Panel clip = slot.clips[cell];
                TextInput text = slot.cells[cell];
                clip.setStyleClass(part(".cell"));
                const bool ellipsis = m_desc.ellipsizeLast && cell + 1 == slot.cells.size();
                text.setStyleClass(ellipsis ? (m_rowClass.isValid() ? part(".text.ellipsis")
                                                                    : InternedString("list.row.text.ellipsis"))
                                            : part(".text"));
            }
        }

        Widget createSlot(const u32 index)
        {
            Slot& slot = m_slots.emplace_back();
            const std::string id(m_desc.id);
            slot.root = m_scope->createPanel(InternedString(id + ".row"), topLeft());
            if (m_desc.select)
            {
                slot.root.onClick(
                    [this, index]
                    {
                        if (m_slots[index].row < list.rowCount())
                        {
                            m_desc.select(m_slots[index].row);
                        }
                    });
            }
            if (m_desc.buttonWidth > 0.0f)
            {
                slot.button = m_scope->createButton(InternedString(id + ".row.button"), "", topLeft());
                slot.button.setParent(slot.root);
                slot.button.onClick(
                    [this, index]
                    {
                        if (m_slots[index].row < list.rowCount() && m_desc.press)
                        {
                            m_desc.press(m_slots[index].row);
                        }
                    });
            }
            for (u32 cell = 0; cell < m_desc.cells; ++cell)
            {
                Panel clip = m_scope->createPanel(InternedString(id + ".row.cell"), topLeft());
                clip.setParent(slot.root);
                clip.setInteractable(false);
                m_scope->world().getComponent<UiStateComponent>(clip.entity()).clipsChildren = true;
                slot.cells.push_back(readOnlyText(*m_scope, InternedString(id + ".row.text")));
                if (cell < m_desc.headers.size())
                {
                    slot.cells.back().setAccessibleNameKey(m_desc.headers[cell].titleKey);
                }
                else if (cell < m_desc.cellNameKeys.size())
                {
                    slot.cells.back().setAccessibleNameKey(m_desc.cellNameKeys[cell]);
                }
                slot.cells.back().setParent(clip);
                slot.clips.push_back(clip);
            }
            slot.shown.resize(m_desc.cells);
            style(slot);
            return slot.root;
        }

        float rowWidth() const { return std::max(0.0f, list.size().x - THUMB); }

        // What a row holds besides the label columns and the last cell: the insets, the gaps and the button
        [[nodiscard]] float fixedWidth() const
        {
            return m_density.xs * static_cast<float>(m_desc.cells + 1) +
                (m_buttonWidth > 0.0f ? m_buttonWidth + m_density.xs : 0.0f);
        }

        // The label columns as shown: a row too narrow for them and the last cell's minimum takes the difference from
        // what they have above their title floors, in proportion; the columns keep the widths they have grown to, so a
        // wider row gives them back.
        void shrinkColumns()
        {
            m_shown = m_columns;
            if (m_lastMin <= 0.0f || rowWidth() <= 0.0f)
            {
                return;
            }
            float total = 0.0f, slack = 0.0f;
            for (u32 cell = 0; cell < m_columns.size(); ++cell)
            {
                total += m_columns[cell];
                slack += m_columns[cell] - std::min(m_floors[cell], m_columns[cell]);
            }
            const float excess = total - (rowWidth() - fixedWidth() - m_lastMin);
            if (excess <= 0.0f || slack <= 0.0f)
            {
                return;
            }
            const float share = std::min(1.0f, excess / slack);
            for (u32 cell = 0; cell < m_columns.size(); ++cell)
            {
                m_shown[cell] -= (m_columns[cell] - std::min(m_floors[cell], m_columns[cell])) * share;
            }
        }

        // The widest a label column grows: a share of the row, and the other columns, the gaps and the last cell's
        // minimum stay in the row
        [[nodiscard]] float columnCap(const u32 cell) const
        {
            float cap = rowWidth() * MAX_COLUMN_SHARE;
            if (m_lastMin > 0.0f)
            {
                float others = fixedWidth() + m_lastMin;
                for (u32 other = 0; other < m_columns.size(); ++other)
                {
                    others += other == cell ? 0.0f : m_columns[other] + m_density.xs;
                }
                cap = std::min(cap, rowWidth() - others);
            }
            return cap;
        }

        // Widens a label column to the cell's text, and raises the last cell's minimum to its text (up to a share of
        // the row: the label columns give way to it); whether anything grew.
        bool fit(const Slot& slot, const u32 cell)
        {
            const UiTextMeasurer* measurer = m_scope->hierarchy().textMeasurer();
            if (!measurer)
            {
                return false;
            }
            if (cell >= m_columns.size())
            {
                const float need = std::min(measurer->measureText(slot.cells[cell].entity(), slot.shown[cell]).x + 2.0f,
                                            rowWidth() * MAX_COLUMN_SHARE);
                if (m_lastMin <= 0.0f || need <= m_lastMin)
                {
                    return false;
                }
                m_lastContent = m_lastMin = need;
                return true;
            }
            const float need = measurer->measureText(slot.cells[cell].entity(), slot.shown[cell]).x + 2.0f;
            const bool whole = (m_desc.wholeCells >> cell) & 1u;
            const bool floored = whole && need > m_floors[cell];
            if (floored)
            {
                m_floors[cell] = need;
            }
            const float wanted = whole ? need : std::min(need, columnCap(cell));
            if (wanted > m_columns[cell])
            {
                m_columns[cell] = wanted;
                return true;
            }
            return floored; // a higher floor changes how the columns give way
        }

        // Widens the buttons to the slot's label and the room the button's rounding takes; whether it grew.
        bool fitButton(const Slot& slot)
        {
            const UiTextMeasurer* measurer = m_scope->hierarchy().textMeasurer();
            if (!measurer)
            {
                return false;
            }
            const float wanted = measurer->measureText(slot.button.entity(), slot.buttonShown).x + m_density.md;
            if (wanted <= m_buttonWidth)
            {
                return false;
            }
            m_buttonWidth = wanted;
            return true;
        }

        // Text measures differently at another text scale: the columns and the button start from their minimums and
        // grow to the shown texts again.
        void refit()
        {
            m_columns = m_desc.columns;
            m_buttonWidth = m_desc.buttonWidth;
            m_lastContent = 0.0f;
            m_lastMin = m_desc.lastMin;
            m_floors.assign(m_floors.size(), 0.0f);
            m_headersFitted = false;
            for (Slot& slot : m_slots)
            {
                for (std::string& shown : slot.shown)
                {
                    shown.clear();
                }
                slot.buttonShown.clear();
                slot.width = -1.0f;
            }
            rebindVisible();
        }

        // places the slot's cells at the row's width and the columns' widths
        void layout(Slot& slot)
        {
            const float width = rowWidth();
            if (slot.width == width)
            {
                return;
            }
            slot.width = width;
            if (width != m_shownWidth)
            {
                m_shownWidth = width;
                shrinkColumns();
                if (!m_heads.empty())
                {
                    layoutHeader();
                }
            }
            const float inset = m_density.hairline;
            const float height = m_density.row - 2.0f * inset;
            float x = m_density.xs;
            if (slot.button.isValid())
            {
                place(slot.button, Vec2{x, inset}, Vec2{m_buttonWidth, height});
                x += m_buttonWidth + m_density.xs;
            }
            for (u32 cell = 0; cell < slot.cells.size(); ++cell)
            {
                const float cellWidth =
                    cell < m_shown.size() ? m_shown[cell] : std::max(0.0f, width - x - m_density.xs);
                place(slot.clips[cell], Vec2{x, inset}, Vec2{cellWidth, height});
                place(slot.cells[cell], Vec2{0.0f, 0.0f}, Vec2{cellWidth, height});
                x += cellWidth + m_density.xs;
            }
        }

        // A density or theme change: the rows take the new height and spaces; each pooled slot re-places at its next
        // bind, the visible ones in the re-pool here.
        void follow()
        {
            const Density now = Density::read(*m_scope);
            if (now == m_density)
            {
                return;
            }
            m_density = now;
            for (Slot& slot : m_slots)
            {
                slot.width = -1.0f;
            }
            list.setRowHeight(now.row);
            if (!m_heads.empty())
            {
                layoutHeader();
            }
        }

        void bind(const u32 index)
        {
            Slot& slot = m_slots[index];
            if (slot.row == NO_ROW || slot.row >= list.rowCount())
            {
                return;
            }
            if (const float scale = m_scope->hierarchy().textScale(); scale != m_textScale)
            {
                m_textScale = scale;
                refit(); // rebinds every slot, this one too
                return;
            }
            ++binds;
            if (!m_heads.empty() && !m_headersFitted)
            {
                // the first bind with the titles known: the columns take their widths before the rows place
                bindHeader();
                m_headersFitted = std::ranges::none_of(m_heads, [](const Head& head) { return head.title.empty(); }) &&
                    m_scope->hierarchy().textMeasurer() != nullptr && rowWidth() > 0.0f;
                if (fitHeaders())
                {
                    relayout();
                }
            }
            layout(slot);
            // read into scratch and copied into the shown texts: both keep their capacity, so a rebind allocates
            // only when a text outgrows it
            std::vector<std::string>& cells = m_cellScratch;
            cells.resize(slot.cells.size());
            for (std::string& cell : cells)
            {
                cell.clear();
            }
            std::string& button = m_buttonScratch;
            button.clear();
            m_desc.read(slot.row, cells, button);
            if (m_desc.isSelected)
                showSelected(slot.root, m_desc.isSelected(slot.row));
            bool grown = false;
            for (u32 cell = 0; cell < cells.size(); ++cell)
            {
                if (cells[cell] != slot.shown[cell])
                {
                    slot.shown[cell].assign(cells[cell]);
                    show(slot.cells[cell], slot.shown[cell]);
                    ++writes;
                    grown |= fit(slot, cell);
                }
            }
            if (slot.button.isValid() && button != slot.buttonShown)
            {
                // a two-state toggle's labels: a bounded set of interned strings
                slot.buttonShown.assign(button);
                slot.button.setText(slot.buttonShown);
                ++writes;
                grown |= fitButton(slot);
            }
            if (grown)
            {
                relayout();
            }
            if (slot.button.isValid() && m_desc.buttonName)
            {
                std::string name = m_desc.buttonName(slot.row);
                if (name != slot.buttonNameShown)
                {
                    slot.buttonNameShown = std::move(name);
                    slot.button.setAccessibleName(slot.buttonNameShown);
                }
            }
        }

        UiScope* m_scope;
        TableDesc m_desc;
        Density m_density;
        u32 m_observer = 0;
        InternedString m_rowClass{};
        std::vector<float> m_columns; // the label columns' widths they have grown to
        std::vector<float> m_shown; // and as the row shows them
        std::vector<float> m_floors; // the least each keeps: its title
        float m_shownWidth = -1.0f; // the row width m_shown was made for
        Panel m_root, m_header, m_spacer; // with a header: the column that holds it and the list
        std::vector<Head> m_heads;
        i32 m_sortCell = -1;
        bool m_sortDescending = false;
        bool m_headersFitted = false; // the columns have taken their titles' widths
        float m_buttonWidth = 0.0f; // the button's width: the desc's, or its widest label's when wider
        float m_textScale = 1.0f; // the text scale the columns and the button were fitted at
        float m_lastMin = 0.0f; // the last cell's least width: the desc's, or its title's or its text's when wider
        float m_lastContent = 0.0f; // the widest text the last cell has shown
        float m_markWidth = 0.0f; // the sort caret's room
        std::vector<Slot> m_slots;
        std::vector<std::string> m_cellScratch;
        std::string m_buttonScratch;
    };

    // A list's view fields: the first shown row, the pooled rows, the work done and the shown cells' texts.
    inline void writeBoundCells(IFormatWriter& out, const u32 firstRow, const u32 slots, const u64 binds,
                                const u64 writes, const std::vector<std::vector<std::string>>& cells)
    {
        out.writeU32("firstRow", firstRow);
        out.writeU32("slots", slots);
        out.writeU64("binds", binds);
        out.writeU64("writes", writes);
        out.beginArray("visible", static_cast<u32>(cells.size()));
        for (const std::vector<std::string>& row : cells)
        {
            out.beginArray("", static_cast<u32>(row.size()));
            for (const std::string& cell : row)
            {
                out.writeString("", cell);
            }
            out.endArray();
        }
        out.endArray();
    }

    inline void writeTableStats(IFormatWriter& out, const Table& table)
    {
        writeBoundCells(out, table.firstRow(), table.slotCount(), table.binds, table.writes, table.visibleCells());
        if (table.headerCount() > 0)
        {
            out.beginArray("header", table.headerCount());
            for (u32 cell = 0; cell < table.headerCount(); ++cell)
            {
                out.beginObject({});
                out.writeString("title", table.headerTitle(cell));
                out.writeBool("sortable", table.headerSortable(cell));
                out.writeString("sorted",
                                static_cast<i32>(cell) != table.sortCell() ? "none"
                                    : table.sortDescending()               ? "descending"
                                                                           : "ascending");
                out.endObject();
            }
            out.endArray();
        }
    }

    // A file path as a panel shows it: relative to the content root (the checkout) when it lies inside it, with
    // forward slashes; any other path as it is. The full path stays in the panel's query and its detail line.
    inline std::string displayPath(const std::string_view path)
    {
        std::error_code error;
        const std::filesystem::path full(std::u8string(path.begin(), path.end()));
        const std::filesystem::path relative = std::filesystem::relative(full, contentRoot(), error);
        if (error || relative.empty())
        {
            return std::string(path);
        }
        const std::u8string text = relative.generic_u8string();
        return text.starts_with(u8"..") ? std::string(path) : std::string(text.begin(), text.end());
    }

    // The list's slot fill: the list goes in, the slot's scroll and child class apply.
    inline void fillList(UiScope& scope, const Widget& slot, Table& table)
    {
        putIn(scope, slot, table.list);
        const UiSlotComponent& fields = slotFields(scope, slot);
        table.list.setScrollEnabled(fields.scroll);
        table.setRowClass(fields.childClass);
    }
} // namespace spite::panelui
