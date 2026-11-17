#include "EditorAgentPanel.hpp"
#include "EditorAgentLayer.hpp"
#include "EditorEntityLabel.hpp"
#include "EditorFeedback.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorPanelUi.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ecs/World.hpp"
#include "engine/Engine.hpp"
#include "engine/PlayWorld.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "serialization/FormatWalk.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiLayoutTokens.hpp"
#include "ui/UiScope.hpp"
#include "ui/UiTween.hpp"

namespace spite
{
    std::string agentActivityCaption(const ControlServer::AgentActivity& row, const Localization& messages)
    {
        std::string key = "editor-agent.feed.state." + row.state;
        if (row.active())
        {
            if (row.action == "buildProject")
                key = row.state == "waiting" ? "editor-agent.feed.building" : "editor-agent.feed.preparing";
            else if (row.action == "reloadModule")
                key = "editor-agent.feed.reload";
            else if (row.action == "step" && row.state == "waiting")
                key = "editor-agent.feed.stepping";
            else if (row.phase == "inspect")
                key = "editor-agent.feed.inspecting";
            else if (row.phase == "review")
                key = "editor-agent.feed.checking";
            else
                key = "editor-agent.feed.applying";
        }
        std::string scratch;
        return std::string(messages.resolve(InternedString(key), scratch));
    }

    sizet agentPendingReviews(const ControlServer& control)
    {
        return static_cast<sizet>(std::ranges::count_if(control.agentActivities(),
                                                        [&control](const ControlServer::AgentActivity& row)
                                                        {
                                                            return !row.active() &&
                                                                (row.state == "ready" ||
                                                                 control.agentActivityCanReview(row.id) ||
                                                                 control.agentActivityCanAcknowledge(row.id));
                                                        }));
    }

    AgentMotion AgentMotion::read(const UiLayoutTokenTable& tokens)
    {
        const AgentMotion base;
        const auto token = [&tokens](const std::string_view name, const float fallback)
        { return tokens.find(InternedString(name)).value_or(fallback); };
        return {token("motion.fast", base.fast), token("motion.normal", base.normal), token("motion.slow", base.slow),
                token("motion.loop", base.loop)};
    }

    void pulseAgentActivity(UiContext& ui, const Entity element, const float seconds, const float delay)
    {
        (void)ui.tweens().start({.element = element,
                                 .component = "UiVisual",
                                 .field = "opacity",
                                 .to = Vec4{1.0f, 0.0f, 0.0f, 0.0f},
                                 .from = Vec4{0.60f, 0.0f, 0.0f, 0.0f},
                                 .duration = seconds,
                                 .delay = delay});
        (void)ui.tweens().start({.element = element,
                                 .component = "UiVisual",
                                 .field = "translate",
                                 .to = Vec4{},
                                 .from = Vec4{0.0f, 3.0f, 0.0f, 0.0f},
                                 .duration = seconds,
                                 .delay = delay});
    }

    namespace
    {
        using namespace panelui;
        using Activity = ControlServer::AgentActivity;
        using Json = nlohmann::json;

        constexpr std::string_view OWNER = "editorAgentPanel";
        constexpr std::string_view AGENT_VIEW = "editor.agent";
        constexpr std::string_view LIST_SLOT = "agent.feed.list";
        constexpr sizet MAX_CARDS = 64; // the feed's capacity
        constexpr sizet MAX_CHANGE_ROWS = 12;
        constexpr sizet MAX_CHIPS = 6;
        constexpr u64 AGE_TICK_MS = 1000; // the age texts are checked this often, and rebuilt only when one changed
        constexpr u64 LIVE_WINDOW_MS = 2500; // a client request keeps the live indicator on this long
        constexpr u64 HOLD_MS = 700; // a new operation shows as Working at least this long
        constexpr sizet MIN_KEEP_ALL = 2; // Keep all appears from this many changes awaiting review
        constexpr sizet MAX_STAGGER = 8;
        constexpr u32 K_KEY = 14; // HID

        enum class Signal : u8
        {
            Working,
            Checking,
            Waiting,
            Review,
            Done,
            Kept,
            Failed,
            Reverted,
            Cancelled,
            None
        };

        enum class Group : u8
        {
            Now,
            Review,
            Earlier
        };

        constexpr std::array<std::string_view, 3> GROUP_NAMES{"now", "review", "earlier"};

        // What makes the panel live: an operation in progress or held, or a client's recent request (a command or a
        // read).
        enum class LiveKind : u8
        {
            None,
            Read,
            Write,
            Operation
        };

        constexpr std::array<std::string_view, 4> LIVE_NAMES{"none", "read", "write", "operation"};

        // The age text of a row changes when this changes: never a string built to find out.
        u64 ageKeyOf(const u64 seconds)
        {
            return seconds < 60   ? 0
                : seconds < 3600  ? 1000 + seconds / 60
                : seconds < 86400 ? 100000 + seconds / 3600
                                  : 10000000 + seconds / 86400;
        }

        std::string tipKeyOf(const std::string_view pillKey)
        {
            constexpr std::string_view STATE = "feed.state.";
            const sizet at = pillKey.find(STATE);
            return std::string(pillKey.substr(0, at)) + "feed.tip." + std::string(pillKey.substr(at + STATE.size()));
        }

        bool live(const Signal signal) { return signal <= Signal::Review; }

        Signal signalOf(const Activity& row, const bool review)
        {
            if (row.active())
                return row.state == "waiting"                         ? Signal::Waiting
                    : row.phase == "review" || row.phase == "inspect" ? Signal::Checking
                                                                      : Signal::Working;
            if (review)
                return Signal::Review;
            if (row.state == "failed")
                return Signal::Failed;
            if (row.state == "kept")
                return Signal::Kept;
            if (row.state == "reverted" || row.state == "undone")
                return Signal::Reverted;
            return row.state == "cancelled" ? Signal::Cancelled : Signal::Done;
        }

        Group groupOf(const Signal signal)
        {
            return signal <= Signal::Waiting                                                          ? Group::Now
                : signal == Signal::Review || signal == Signal::Failed || signal == Signal::Cancelled ? Group::Review
                                                                                                      : Group::Earlier;
        }

        std::string_view signalName(const Signal signal)
        {
            constexpr std::array<std::string_view, 9> NAMES{"working", "checking", "waiting",  "review",   "done",
                                                            "kept",    "failed",   "reverted", "cancelled"};
            return NAMES[static_cast<u8>(signal)];
        }

        std::string_view iconOf(const Signal signal)
        {
            switch (signal)
            {
            case Signal::Checking:
                return "magnifying-glass";
            case Signal::Waiting:
                return "hourglass";
            case Signal::Review:
                return "eye";
            case Signal::Failed:
                return "warning-circle";
            case Signal::Done:
            case Signal::Kept:
                return "check-circle";
            case Signal::Reverted:
                return "arrow-counter-clockwise";
            default:
                return "minus";
            }
        }

        // One displayed row: the feed's activity, what it looks like and the list group it sits in.
        struct Item
        {
            const Activity* row;
            Signal signal;
            Group group;
            bool read = false; // the transient line of a client's reads
        };

        struct Card
        {
            Panel root, iconSlot, dots, text;
            std::array<Panel, 3> dot;
            Button header;
            Label icon, title, meta, status, caret;
            u64 id = 0, startedMs = 0, ageKey = 0;
            Signal signal = Signal::None;
            bool moving = false;
            bool shown = false;
            std::string pillKey;
        };
    } // namespace

    struct EditorAgentPanel::State
    {
        State(Engine& engine, EditorFeedback& feedback, EditorAgentLayer& agent) :
            engine(&engine), control(&engine.control()), feedback(&feedback), agent(&agent),
            scope(editorUi(engine).ui())
        {
        }

        Engine* engine;
        ControlServer* control;
        EditorFeedback* feedback;
        EditorAgentLayer* agent;
        UiScope scope;
        AgentMotion motion;
        Density density;
        u32 observer = 0;
        Panel root, cards, details, changes, entities, actions, empty;
        Panel lamp, strip, fill;
        ScrollView scroll;
        Label presence, count, emptyIcon, emptyTitle, emptyHint, current, result, why, more, step;
        std::array<Label, 3> groupLabel;
        Button keep, revert, acknowledge, keepAll;
        std::vector<Card> pool;
        std::vector<Label> changeRows;
        std::vector<Button> chips;
        std::vector<u64> chipEntities;
        std::vector<Item> items;
        std::vector<Entity> order, desired;
        std::vector<std::string> requests;
        std::vector<std::pair<u64, u64>> holds; // operation id, when its minimum Working time ends
        std::vector<u64> stagger; // the operations a Keep all settles, newest first
        std::string action, world;
        Entity detailParent = Entity::undefined();
        std::unique_ptr<UiDocumentView> view;
        u64 selected = 0, detailId = 0;
        u64 seenRevision = ~0ull, seenGeneration = ~0ull, seenLocale = ~0ull, seenAge = ~0ull;
        u64 changeRevision = ~0ull, changeGeneration = ~0ull, changeLocale = ~0ull, changeId = 0;
        u64 elapsed = ~0ull, startedMs = 0;
        sizet pending = 0, reviewCount = 0, failed = 0, shownPending = 0;
        u64 binds = 0;
        bool dirty = true, wasShown = false, reducedMotion = false, bound = false, expandMotion = false;
        bool lampLive = false, changesShown = false, keepAllQueued = false, lampBreathing = false;
        // when the next thing the panel shows changes by itself: the live window of a client's request, the end of a
        // hold
        u64 trafficDueMs = 0, holdDueMs = 0, seenTraffic = 0;
        bool trafficWrite = false;
        u8 stripMode = 0; // 0 off, 1 sliding, 2 reduced motion (a still bar)
        float stripWidth = 0.0f;

        [[nodiscard]] std::string message(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            std::string scratch;
            return std::string(engine->localization().resolve(InternedString(key), args, scratch));
        }

        [[nodiscard]] UiTweens& tweens() const { return editorUi(*engine).ui().tweens(); }

        // -- construction --------------------------------------------------------------------------------------------

        Label label(const std::string_view id, const Widget& parent, const char* style)
        {
            auto desc = topLeft();
            desc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
            desc.layoutItem = fixedItem();
            Label result = scope.createLabel(InternedString(id), "", desc);
            result.setStyleClass(InternedString(style));
            result.setInteractable(false);
            result.setParent(parent);
            return result;
        }

        Button button(const std::string_view id, const Widget& parent)
        {
            auto desc = fitWidthDesc();
            desc.size.y = density.control;
            Button result = scope.createButton(InternedString(id), "", desc);
            result.setParent(parent);
            result.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SIZE_CONTROL));
            bindFitPadding(result, SPACE_XS, SPACE_NONE);
            return result;
        }

        // A square of a size token in a row: its basis is the width, its minimum the height.
        Panel square(const std::string_view id, const Widget& parent, const std::string_view token)
        {
            Panel result = scope.createPanel(InternedString(id), barDesc({}));
            result.setParent(parent);
            result.setInteractable(false);
            bindBasis(result, token);
            result.bindLayoutToken(UiLayoutField::MinHeight, InternedString(token));
            return result;
        }

        void makeCard()
        {
            const sizet index = pool.size();
            const std::string id = std::format("agent.feed.card.{}", index);
            Card card;
            auto rootDesc = columnDesc();
            rootDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
            card.root = scope.createPanel(InternedString(id), rootDesc);
            card.root.setParent(cards);

            auto header = barDesc({},
                                  stack(UiLayoutDirection::Row, density.xs,
                                        Vec4{density.xs, density.xs, density.xs, density.xs}, UiLayoutAlign::Center));
            header.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
            card.header = scope.createButton(InternedString(id + ".header"), "", header);
            card.header.setStyleClass(InternedString("agent.feed.card.header"));
            bindGap(card.header, SPACE_XS);
            bindPadding(card.header, SPACE_XS, SPACE_XS, SPACE_XS, SPACE_XS);
            card.header.setParent(card.root);

            // The icon and the working dots share one square slot: the slot's layout centres whichever is shown, so
            // no text alignment or padding decides where the glyph sits.
            auto slotLayout = stack(UiLayoutDirection::Row, 0.0f, {}, UiLayoutAlign::Center);
            slotLayout.justify = UiLayoutJustify::Center;
            card.iconSlot = scope.createPanel(InternedString(id + ".iconSlot"), barDesc({}, slotLayout));
            card.iconSlot.setInteractable(false);
            card.iconSlot.setParent(card.header);
            bindBasis(card.iconSlot, SIZE_ROW);
            card.iconSlot.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SIZE_ROW));

            auto iconDesc = barDesc({});
            iconDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
            card.icon = scope.createLabel(InternedString(id + ".icon"), "", iconDesc);
            card.icon.setInteractable(false);
            card.icon.setAccessibleRole(UiRole::Presentation);
            card.icon.setParent(card.iconSlot);

            auto dotsLayout = stack(UiLayoutDirection::Row, density.xxs, {}, UiLayoutAlign::Center);
            auto dotsDesc = barDesc({}, dotsLayout);
            dotsDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
            card.dots = scope.createPanel(InternedString(id + ".dots"), dotsDesc);
            card.dots.setInteractable(false);
            card.dots.setParent(card.iconSlot);
            bindGap(card.dots, SPACE_XXS);
            for (sizet i = 0; i < card.dot.size(); ++i)
            {
                card.dot[i] = square(std::format("{}.dot.{}", id, i), card.dots, SPACE_XS);
                card.dot[i].setStyleClass(InternedString("agent.chip.lamp.editing"));
            }
            card.dots.setVisible(false);

            auto info = columnDesc();
            info.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
            info.layoutItem = fillItem();
            card.text = scope.createPanel(InternedString(id + ".text"), info);
            card.text.setInteractable(false);
            card.text.setParent(card.header);
            card.title = label(id + ".title", card.text, "agent.feed.card.title");
            card.meta = label(id + ".meta", card.text, "agent.feed.meta");

            auto pill = fitWidthDesc();
            pill.size.y = density.row;
            card.status = scope.createLabel(InternedString(id + ".status"), "", pill);
            card.status.setParent(card.header);
            card.status.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SIZE_ROW));
            bindFitPadding(card.status, SPACE_XXS, SPACE_NONE);

            card.caret = scope.createLabel(InternedString(id + ".caret"), "", barDesc({}));
            card.caret.setStyleClass(InternedString("agent.feed.caret"));
            card.caret.setInteractable(false);
            card.caret.setParent(card.header);
            bindBasis(card.caret, SPACE_LG);
            card.caret.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SIZE_ROW));

            const auto toggle = [this, index]
            {
                const u64 target = pool[index].id;
                selected = selected == target ? 0 : target;
                expandMotion = selected != 0;
                dirty = true;
            };
            card.header.onClick(toggle);
            card.status.onClick(toggle); // hoverable for its tooltip, so it passes the click on
            card.root.onClick(toggle);
            // K keeps the expanded card's changes from any focus inside it; there is no key for Revert
            card.root.onKeyDown(
                [this, index](const u32 scancode, const u8 modifiers, const bool repeat)
                {
                    if (scancode != K_KEY || modifiers != 0 || repeat || !selected || pool[index].id != selected ||
                        !control->agentActivityCanReview(selected))
                        return false;
                    queueReview("keep");
                    return true;
                });
            card.root.setVisible(false);
            pool.push_back(std::move(card));
            scope.commit();
        }

        void buildDetails()
        {
            auto detail = columnDesc(density.xxs, Vec4{density.md, density.xxs, density.md, density.xs});
            detail.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
            details = scope.createPanel("agent.feed.detail", detail);
            bindGap(details, SPACE_XXS);
            bindPadding(details, SPACE_MD, SPACE_XXS, SPACE_MD, SPACE_XS);
            details.setParent(cards);
            current = label("agent.feed.current", details, "agent.feed.event");
            bindFitPadding(current, SPACE_XS, SPACE_XXS);
            result = label("agent.feed.result", details, "modules.detail");
            why = label("agent.feed.why", details, "modules.detail");
            auto changeColumn = columnDesc(density.xxs);
            changeColumn.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
            changes = scope.createPanel("agent.feed.changes", changeColumn);
            bindGap(changes, SPACE_XXS);
            changes.setParent(details);
            more = label("agent.feed.changes.more", changes, "agent.feed.meta");
            entities = wrapRowPanel(scope, "agent.feed.entities", details);
            step = label("agent.feed.step", details, "agent.feed.meta");
            actions = wrapRowPanel(scope, "agent.feed.actions", details);
            keep = button("agent.feed.keep", actions);
            keep.setLocalizedText(InternedString("editor-agent.feed.keep"));
            revert = button("agent.feed.revert", actions);
            revert.setLocalizedText(InternedString("editor-agent.feed.revert"));
            revert.setStyleClass(InternedString("dialog.button.destructive"));
            acknowledge = button("agent.feed.acknowledge", actions);
            acknowledge.setLocalizedText(InternedString("editor-agent.feed.acknowledge"));
            acknowledge.onClick([this] { queueReview("acknowledge"); });
            keep.onClick([this] { queueReview("keep"); });
            revert.onClick([this] { queueReview("revert"); });
            details.onClick(
                [this]
                {
                    selected = 0;
                    dirty = true;
                });
            details.setVisible(false);
        }

        void build(Engine& host)
        {
            density = Density::read(scope);
            motion = AgentMotion::read(scope.layoutTokens());
            observer = scope.observeLayoutTokens(
                [this]
                {
                    density = Density::read(scope);
                    motion = AgentMotion::read(scope.layoutTokens());
                    dirty = true;
                });
            root = scope.createPanel("editor.panel", columnDesc());

            scroll = scope.createScrollView("agent.feed.scroll", topLeft());
            scroll.setLayoutItem(fillItem());
            scroll.setParent(root);
            auto content = columnDesc(density.xxs, Vec4{density.xs, density.xxs, density.xs, density.xxs});
            content.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
            cards = scope.createPanel("agent.feed.cards", content);
            bindGap(cards, SPACE_XXS);
            bindPadding(cards, SPACE_XS, SPACE_XXS, SPACE_XS, SPACE_XXS);
            cards.setParent(scroll.content());
            auto emptyDesc = columnDesc(density.xs);
            emptyDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
            empty = scope.createPanel("agent.feed.empty", emptyDesc);
            bindGap(empty, SPACE_XS);
            bindPadding(empty, SPACE_MD, SPACE_XL, SPACE_MD, SPACE_XL);
            empty.setParent(cards);
            empty.setInteractable(false);
            emptyIcon = label("agent.feed.empty.icon", empty, "agent.feed.empty.icon");
            emptyIcon.setIcon("robot");
            emptyIcon.setAccessibleRole(UiRole::Presentation);
            emptyTitle = label("agent.feed.empty.title", empty, "agent.feed.empty.title");
            emptyTitle.setLocalizedText(InternedString("editor-agent.feed.emptyCards"));
            emptyHint = label("agent.feed.empty.hint", empty, "agent.feed.empty.hint");
            emptyHint.setLocalizedText(InternedString("editor-agent.feed.emptyHint"));
            for (sizet i = 0; i < groupLabel.size(); ++i)
            {
                groupLabel[i] = label(std::format("agent.feed.group.{}", GROUP_NAMES[i]), cards, "agent.feed.group");
                groupLabel[i].setLocalizedText(
                    InternedString("editor-agent.feed.group." + std::string(GROUP_NAMES[i])));
                groupLabel[i].setVisible(false);
            }
            buildDetails();
            registerQuery();
            scope.commit();

            UiViewContract contract;
            contract.require(InternedString("agent.feed.lamp"), UiWidgetKind::Panel,
                             [this](const Widget& widget)
                             {
                                 lamp = widget.as<Panel>();
                                 lampLive = false; // a new lamp starts in the document's class
                                 dirty = true;
                             });
            contract.require(InternedString("agent.feed.presence"), UiWidgetKind::Label,
                             [this](const Widget& widget)
                             {
                                 presence = widget.as<Label>();
                                 dirty = true;
                             });
            contract.require(InternedString("agent.feed.count"), UiWidgetKind::Label,
                             [this](const Widget& widget)
                             {
                                 count = widget.as<Label>();
                                 dirty = true;
                             });
            contract.require(InternedString("agent.feed.keepAll"), UiWidgetKind::Button,
                             [this](const Widget& widget)
                             {
                                 keepAll = widget.as<Button>();
                                 keepAll.onClick([this] { keepAllQueued = true; });
                                 dirty = true;
                             });
            contract.require(InternedString("agent.feed.progress"), UiWidgetKind::Panel,
                             [this](const Widget& widget)
                             {
                                 strip = widget.as<Panel>();
                                 stripMode = 0;
                                 dirty = true;
                             });
            contract.require(InternedString("agent.feed.progress.fill"), UiWidgetKind::Panel,
                             [this](const Widget& widget)
                             {
                                 fill = widget.as<Panel>();
                                 stripMode = 0;
                                 dirty = true;
                             });
            contract.slot(InternedString(LIST_SLOT), [this](const Widget& slot) { putIn(scope, slot, scroll); });
            contract.fallback(
                [this](UiScope& viewScope, const Widget& viewRoot)
                {
                    columnRoot(viewRoot);
                    Widget(viewRoot).setStyleClass(InternedString("agent.dashboard"));
                    Panel header =
                        toolbarPanel(viewScope, "agent.feed.header", viewRoot, SPACE_SM, UiLayoutAlign::Center);
                    header.setStyleClass(InternedString("agent.feed.header"));
                    bindPadding(header, SPACE_MD, SPACE_XXS, SPACE_XS, SPACE_XXS);
                    Panel lampPanel = viewScope.createPanel("agent.feed.lamp", barDesc({}));
                    lampPanel.setParent(header);
                    lampPanel.setInteractable(false);
                    lampPanel.setStyleClass(InternedString("agent.bar.lamp"));
                    bindBasis(lampPanel, SPACE_SM);
                    lampPanel.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SPACE_SM));
                    auto presenceDesc = topLeft();
                    presenceDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                    presenceDesc.layoutItem = fillItem();
                    Label presenceLabel = viewScope.createLabel("agent.feed.presence", "", presenceDesc);
                    presenceLabel.setParent(header);
                    presenceLabel.setInteractable(false);
                    presenceLabel.setStyleClass(InternedString("agent.feed.meta"));
                    auto countDesc = barDesc({});
                    countDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
                    Label countLabel = viewScope.createLabel("agent.feed.count", "", countDesc);
                    countLabel.setParent(header);
                    countLabel.setInteractable(false);
                    countLabel.setStyleClass(InternedString("agent.feed.count"));
                    bindFitPadding(countLabel, SPACE_SM, SPACE_HAIRLINE);
                    auto keepDesc = fitWidthDesc();
                    keepDesc.size.y = density.control;
                    Button keepButton = viewScope.createButton("agent.feed.keepAll", "", keepDesc);
                    keepButton.setParent(header);
                    keepButton.setVisible(false);
                    keepButton.setLocalizedText(InternedString("editor-agent.feed.keepAll"));
                    keepButton.setTooltip(InternedString("editor-agent.feed.keepAllTip"));
                    keepButton.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SIZE_CONTROL));
                    bindFitPadding(keepButton, SPACE_SM, SPACE_NONE);
                    Panel stripPanel = viewScope.createPanel("agent.feed.progress", barDesc(Vec2{0.0f, density.xxs}));
                    stripPanel.setParent(viewRoot);
                    stripPanel.setVisible(false);
                    stripPanel.setInteractable(false);
                    viewScope.world().getComponent<UiStateComponent>(stripPanel.entity()).clipsChildren = true;
                    bindBasis(stripPanel, SPACE_XXS);
                    Panel fillPanel =
                        viewScope.createPanel("agent.feed.progress.fill", barDesc(Vec2{0.0f, density.xxs}));
                    fillPanel.setParent(stripPanel);
                    fillPanel.setInteractable(false);
                    fillPanel.setStyleClass(InternedString("agent.feed.progress.fill"));
                    fillSlotPanel(viewScope, LIST_SLOT, viewRoot);
                });
            view = openView(host, AGENT_VIEW, "assets/ui/editor/agent.json", std::move(contract), root);
        }

        // -- the feed as rows ----------------------------------------------------------------------------------------

        [[nodiscard]] bool holdActive(const u64 id, const u64 now) const
        {
            return std::ranges::any_of(holds,
                                       [id, now](const auto& hold) { return hold.first == id && hold.second > now; });
        }

        // The newest first, an inspected read left out (but for the latest while a client is reading), stably grouped
        // Now, Needs review, Earlier. An operation within its minimum Working time shows as Working.
        void collect(const u64 now)
        {
            items.clear();
            const auto traffic = control->agentTraffic();
            bool readLive = traffic.count && !traffic.command && now < traffic.atMs + LIVE_WINDOW_MS;
            const auto activities = control->agentActivities();
            for (auto at = activities.rbegin(); at != activities.rend() && items.size() < MAX_CARDS; ++at)
            {
                if (at->state == "inspected")
                {
                    if (readLive)
                        items.push_back({&*at, Signal::Checking, Group::Now, true});
                    readLive = false;
                    continue;
                }
                Signal signal =
                    signalOf(*at, !at->active() && (at->state == "ready" || control->agentActivityCanReview(at->id)));
                if (!live(signal) && holdActive(at->id, now))
                    signal = Signal::Working;
                items.push_back({&*at, signal, groupOf(signal)});
            }
            std::ranges::stable_sort(items, {}, &Item::group);
            reviewCount = std::ranges::count(items, Signal::Review, &Item::signal);
            pending =
                reviewCount +
                std::ranges::count_if(
                    items, [this](const Item& item)
                    { return item.signal != Signal::Review && control->agentActivityCanAcknowledge(item.row->id); });
            failed = std::ranges::count(items, Signal::Failed, &Item::signal);
        }

        // A row the panel has not shown yet starts its minimum Working time now: an agent command finishes within one
        // frame, so without it the row would never be seen working.
        void startHolds(const u64 now)
        {
            std::erase_if(holds, [now](const auto& hold) { return hold.second <= now; });
            for (const Activity& row : control->agentActivities())
                if (row.state != "inspected" && std::ranges::find(pool, row.id, &Card::id) == pool.end() &&
                    std::ranges::find(holds, row.id, &std::pair<u64, u64>::first) == holds.end())
                    holds.emplace_back(row.id, now + HOLD_MS);
            holdDueMs = holds.empty() ? 0 : std::ranges::min(holds, {}, &std::pair<u64, u64>::second).second;
        }

        [[nodiscard]] LiveKind liveOf(const u64 now, const Activity* focus) const
        {
            if ((focus && focus->active()) || (holdDueMs && now < holdDueMs))
                return LiveKind::Operation;
            const auto traffic = control->agentTraffic();
            if (traffic.count && now < traffic.atMs + LIVE_WINDOW_MS)
                return traffic.command ? LiveKind::Write : LiveKind::Read;
            return LiveKind::None;
        }

        [[nodiscard]] std::string liveCaption(const LiveKind kind, const Activity* focus) const
        {
            if (kind == LiveKind::Operation && focus && focus->active())
                return agentActivityCaption(*focus, engine->localization());
            return message(kind == LiveKind::Read ? "editor-agent.feed.inspecting" : "editor-agent.feed.applying");
        }

        [[nodiscard]] std::string titleOf(const Item& item) const
        {
            return item.read ? message("editor-agent.feed.inspecting") : item.row->title;
        }

        [[nodiscard]] static u64 ageSeconds(const u64 startedMs, const u64 now)
        {
            return now > startedMs ? (now - startedMs) / 1000 : 0;
        }

        [[nodiscard]] std::string ageText(const Activity& row, const u64 now) const
        {
            const u64 seconds = ageSeconds(row.startedMs, now);
            std::string age;
            if (seconds < 60)
            {
                age = message("editor-agent.feed.age.now");
            }
            else
            {
                const bool minutes = seconds < 3600, hours = seconds < 86400;
                const MessageArg args[]{{"count",
                                         static_cast<std::int64_t>(minutes     ? seconds / 60
                                                                       : hours ? seconds / 3600
                                                                               : seconds / 86400)}};
                age = message(minutes     ? "editor-agent.feed.age.minutes"
                                  : hours ? "editor-agent.feed.age.hours"
                                          : "editor-agent.feed.age.days",
                              args);
            }
            return age;
        }

        [[nodiscard]] std::string metaText(const Activity& row, const u64 now) const
        {
            const std::string age = ageText(row, now);
            const std::string kind = message("editor-agent.feed.kind." + row.kind);
            const MessageArg args[]{{"age", std::string_view(age)}, {"kind", std::string_view(kind)}};
            return message("editor-agent.feed.meta", args);
        }

        [[nodiscard]] std::string pillKeyOf(const Item& item) const
        {
            return live(item.signal) && item.signal != Signal::Review ? "editor-agent.feed.state.working"
                : item.signal == Signal::Review                       ? "editor-agent.feed.state.ready"
                                                                      : "editor-agent.feed.state." + item.row->state;
        }

        static void setText(Label& label, const std::string_view text)
        {
            if (label.text() != text)
                label.setText(text);
        }

        // -- binding -------------------------------------------------------------------------------------------------

        void bind()
        {
            SPITE_ZONE("EditorAgentPanel.bind");
            ++binds;
            const bool animate = wasShown && bound;
            const u64 now = control->agentActivityClockMs();
            if (animate)
                startHolds(now);
            collect(now);
            const Activity* focus = control->agentActivityFocus();
            const u64 focusId = focus && focus->active() ? focus->id : 0;
            bindHeader(focus, animate, now);

            // each operation keeps its card: release the cards whose operation left, then give the new ones a free card
            std::vector<sizet> slotOf(items.size(), ~sizet{0});
            for (sizet i = 0; i < pool.size(); ++i)
            {
                Card& card = pool[i];
                if (!card.id)
                    continue;
                const auto at =
                    std::ranges::find_if(items, [&card](const Item& item) { return item.row->id == card.id; });
                if (at != items.end())
                {
                    slotOf[static_cast<sizet>(at - items.begin())] = i;
                    continue;
                }
                resetMotion(card);
                card.id = 0;
                card.signal = Signal::None;
                card.shown = false;
                card.root.setVisible(false);
            }
            std::vector<bool> fresh(items.size(), false);
            for (sizet i = 0; i < items.size(); ++i)
            {
                if (slotOf[i] != ~sizet{0})
                    continue;
                auto free = std::ranges::find(pool, u64{0}, &Card::id);
                if (free == pool.end())
                {
                    makeCard();
                    free = pool.end() - 1;
                }
                free->id = items[i].row->id;
                slotOf[i] = static_cast<sizet>(free - pool.begin());
                fresh[i] = true;
            }

            if (selected && std::ranges::none_of(items, [this](const Item& item) { return item.row->id == selected; }))
                selected = 0;
            desired.clear();
            Card* expanded = nullptr;
            const Activity* expandedRow = nullptr;
            std::array<sizet, 3> inGroup{};
            for (sizet i = 0; i < items.size(); ++i)
            {
                const Item& item = items[i];
                Card& card = pool[slotOf[i]];
                if (inGroup[static_cast<u8>(item.group)]++ == 0)
                    desired.push_back(groupLabel[static_cast<u8>(item.group)].entity());
                desired.push_back(card.root.entity());
                const bool open = item.row->id == selected;
                if (open)
                {
                    expanded = &card;
                    expandedRow = item.row;
                }
                const bool loops = item.row->id == focusId || item.read ||
                    (item.signal == Signal::Working && !item.row->active() && holdActive(item.row->id, now));
                bindCard(card, item, open, loops, fresh[i], animate, now);
            }
            for (sizet i = 0; i < groupLabel.size(); ++i)
                if (groupLabel[i].isVisible() != (inGroup[i] != 0))
                    groupLabel[i].setVisible(inGroup[i] != 0);
            if (empty.isVisible() != items.empty())
                empty.setVisible(items.empty());
            if (desired != order)
            {
                scope.hierarchy().setChildOrder(cards.entity(), desired);
                order = desired;
            }
            bindDetails(expandedRow, expanded);
            seenRevision = control->agentActivityRevision();
            seenGeneration = control->stateGeneration();
            seenLocale = engine->localization().revision();
            seenAge = now / AGE_TICK_MS;
            reducedMotion = tweens().reducedMotion();
            stagger.clear();
            dirty = false;
            bound = true;
        }

        void bindHeader(const Activity* focus, const bool animate, const u64 now)
        {
            const LiveKind kind = liveOf(now, focus);
            const auto traffic = control->agentTraffic();
            trafficDueMs = traffic.count && now < traffic.atMs + LIVE_WINDOW_MS ? traffic.atMs + LIVE_WINDOW_MS : 0;
            trafficWrite = traffic.command;
            std::string status = kind != LiveKind::None ? liveCaption(kind, focus) : std::string{};
            if (failed)
            {
                if (!status.empty())
                    status += " · ";
                const MessageArg args[]{{"count", static_cast<std::int64_t>(failed)}};
                status += message("editor-agent.feed.failedCount", args);
            }
            if (status.empty())
                status = message(pending ? "editor-agent.feed.awaiting" : "editor-agent.feed.idle");
            setText(presence, status);
            const bool lit = kind != LiveKind::None || pending;
            if (lit != lampLive)
            {
                lampLive = lit;
                lamp.setStyleClass(InternedString(lit ? "agent.chip.lamp.editing" : "agent.bar.lamp"));
            }
            bindLiveMotion(kind != LiveKind::None);
            count.setVisible(pending != 0);
            if (pending)
            {
                setText(count, std::to_string(pending));
                const MessageArg args[]{{"count", static_cast<std::int64_t>(pending)}};
                count.setAccessibleName(message("editor-agent.feed.pending", args));
            }
            const bool offerKeepAll = reviewCount >= MIN_KEEP_ALL;
            const bool keepAllChanged = keepAll.isVisible() != offerKeepAll;
            if (keepAllChanged)
                keepAll.setVisible(offerKeepAll);
            if (offerKeepAll && (keepAllChanged || pending != shownPending))
            {
                const MessageArg args[]{{"count", static_cast<std::int64_t>(reviewCount)}};
                keepAll.setAccessibleName(message("editor-agent.feed.keepAllName", args));
            }
            if (animate && pending > shownPending && !tweens().reducedMotion())
            {
                const auto bump = [&](const std::string_view field, const Vec4 from, const Vec4 to, const UiEase ease)
                {
                    (void)tweens().start({.element = count.entity(),
                                          .component = "UiVisual",
                                          .field = field,
                                          .to = to,
                                          .from = from,
                                          .duration = motion.normal,
                                          .ease = ease});
                };
                bump("scale", Vec4{1.3f, 1.3f, 0, 0}, Vec4{1, 1, 0, 0}, UiEase::OutBack);
                bump("opacity", Vec4{0.5f, 0, 0, 0}, Vec4{1, 0, 0, 0}, UiEase::OutCubic);
            }
            shownPending = pending;
        }

        // The lamp breathes and the strip under the header slides while the panel is live; both are off, with no
        // tween running, when it is not. Reduced motion keeps a still lamp and a still full bar.
        void bindLiveMotion(const bool on)
        {
            const bool still = tweens().reducedMotion();
            const bool breathe = on && !still;
            if (breathe != lampBreathing)
            {
                lampBreathing = breathe;
                if (breathe)
                    (void)tweens().start({.element = lamp.entity(),
                                          .component = "UiVisual",
                                          .field = "opacity",
                                          .to = Vec4{1, 0, 0, 0},
                                          .from = Vec4{0.3f, 0, 0, 0},
                                          .duration = motion.loop,
                                          .ease = UiEase::InOutQuad,
                                          .repeat = 0,
                                          .yoyo = true});
                else
                    resetVisual(lamp.entity());
            }
            if (strip.isVisible() != on)
                strip.setVisible(on);
            const u8 mode = !on ? u8{0} : still ? u8{2} : u8{1};
            if (mode != stripMode)
            {
                stripMode = mode;
                stripWidth = 0.0f;
                resetVisual(fill.entity());
            }
        }

        // The strip's highlight, once the strip has a width: a bar of a third of it travelling across, looping
        void fitStrip()
        {
            const float width = resolvedSize(scope, strip).x;
            if (width <= 0.0f || width == stripWidth)
                return;
            stripWidth = width;
            resetVisual(fill.entity());
            if (stripMode == 2)
            {
                fill.setSize({width, density.xxs});
                return;
            }
            const float bar = std::max(width * 0.3f, density.row);
            fill.setSize({bar, density.xxs});
            (void)tweens().start({.element = fill.entity(),
                                  .component = "UiVisual",
                                  .field = "translate",
                                  .to = Vec4{width, 0, 0, 0},
                                  .from = Vec4{-bar, 0, 0, 0},
                                  .duration = motion.loop * 2.0f,
                                  .ease = UiEase::Linear,
                                  .repeat = 0});
        }

        void bindCard(Card& card, const Item& item, const bool open, const bool loops, const bool fresh,
                      const bool animate, const u64 now)
        {
            const Activity& row = *item.row;
            const Signal signal = item.signal;
            if (fresh)
                resetMotion(card);
            if (!card.shown)
            {
                card.shown = true;
                card.root.setVisible(true);
            }
            if (signal != card.signal)
            {
                const bool isLive = live(signal);
                card.root.setStyleClass(InternedString(isLive                         ? "agent.feed.card.live"
                                                           : signal == Signal::Failed ? "agent.feed.card.failed"
                                                                                      : "agent.feed.card"));
                card.icon.setStyleClass(InternedString(signal == Signal::Failed ? "agent.feed.icon.failed"
                                                           : isLive             ? "agent.feed.icon.live"
                                                                                : "agent.feed.icon"));
                card.status.setStyleClass(InternedString(isLive ? "agent.feed.card.status.live"
                                                             : signal == Signal::Failed
                                                             ? "agent.feed.card.status.failed"
                                                             : signal == Signal::Done ? "agent.feed.card.status.done"
                                                                                      : "agent.feed.card.status"));
                card.title.setStyleClass(InternedString(signal == Signal::Reverted || signal == Signal::Cancelled
                                                            ? "agent.feed.card.title.muted"
                                                            : "agent.feed.card.title"));
                card.icon.setVisible(signal != Signal::Working);
                card.dots.setVisible(signal == Signal::Working);
                card.icon.setIcon(iconOf(signal));
            }
            const std::string title = titleOf(item);
            setText(card.title, title);
            setText(card.meta, metaText(row, now));
            card.startedMs = row.startedMs;
            card.ageKey = ageKeyOf(ageSeconds(row.startedMs, now));
            const std::string pillKey = pillKeyOf(item);
            if (pillKey != card.pillKey)
            {
                card.pillKey = pillKey;
                card.status.setLocalizedText(InternedString(pillKey));
                card.status.setTooltip(InternedString(tipKeyOf(pillKey)));
            }
            card.caret.setIcon(open ? "caret-down" : "caret-right");
            card.header.setAccessibleName(title + " · " + message(pillKey));
            card.header.setAccessibleExpanded(open ? UiExpandState::Expanded : UiExpandState::Collapsed);

            const bool moving = loops && !tweens().reducedMotion();
            if (fresh || card.moving != moving || signal != card.signal)
            {
                const bool still = !tweens().reducedMotion();
                if (!fresh)
                    resetMotion(card);
                card.signal = signal;
                card.moving = moving;
                if (fresh && animate && still)
                    animateEntry(card);
                if (moving)
                    animateWorking(card, signal);
                else if (!fresh && animate && still)
                    animateChange(card, signal, staggerDelay(card.id));
            }
        }

        void bindDetails(const Activity* row, Card* card)
        {
            details.setVisible(row != nullptr);
            startedMs = row && row->active() ? row->startedMs : 0;
            if (!row)
            {
                detailId = 0;
                resetVisual(details.entity());
                return;
            }
            if (detailParent != card->root.entity())
            {
                details.setParent(card->root);
                detailParent = card->root.entity();
            }
            if (detailId != row->id)
            {
                detailId = row->id;
                if (expandMotion && !tweens().reducedMotion())
                    animateExpand();
            }
            expandMotion = false;

            current.setVisible(row->active());
            elapsed = ~0ull;
            if (row->active())
                action = agentActivityCaption(*row, engine->localization());
            bindChanges(*row);
            const bool showResult = !row->evidence.empty() && (row->reported || !changesShown);
            result.setVisible(showResult);
            if (showResult)
                setText(result, row->evidence);
            why.setVisible(!showResult && !row->why.empty());
            if (!row->why.empty())
                setText(why, row->why);
            const bool review = control->agentActivityCanReview(row->id);
            const bool canAcknowledge = control->agentActivityCanAcknowledge(row->id);
            actions.setVisible(review || canAcknowledge);
            keep.setVisible(review);
            revert.setVisible(review);
            acknowledge.setVisible(canAcknowledge);
            step.setVisible(review);
            if (review)
            {
                const MessageArg args[]{{"step", static_cast<std::int64_t>(row->step)}};
                const std::string line = message("editor-agent.feed.undoStep", args);
                setText(step, line);
                keep.setAccessibleName(message("editor-agent.feed.keep") + " · " + line);
                revert.setAccessibleName(message("editor-agent.feed.revert") + " · " + line);
            }
        }

        [[nodiscard]] const World* worldNamed(const std::string_view name) const
        {
            if (name == "play")
                return engine->playWorld() ? &engine->playWorld()->world() : nullptr;
            return name == "scene" ? &engine->world() : nullptr;
        }

        // What the operation's undo step changed, as rows and entity chips; read when the card, the feed, the history
        // or the locale moved, not per frame.
        void bindChanges(const Activity& row)
        {
            const u64 revision = control->agentActivityRevision();
            const u64 generation = control->stateGeneration();
            const u64 locale = engine->localization().revision();
            if (changeId == row.id && changeRevision == revision && changeGeneration == generation &&
                changeLocale == locale)
                return;
            changeId = row.id;
            changeRevision = revision;
            changeGeneration = generation;
            changeLocale = locale;
            const std::vector<ControlServer::AgentChange> found =
                row.step ? control->agentActivityChanges(row.id) : std::vector<ControlServer::AgentChange>{};
            changesShown = !found.empty();
            const World* shownWorld = worldNamed(row.world);
            const bool frameable = row.world == engine->gameWorldName();
            std::string buffer;
            const auto who = [&](const u64 entity)
            {
                if (shownWorld && shownWorld->isEntityAlive(Entity(entity)))
                    return std::string(entityLabel(buffer, *shownWorld, Entity(entity)));
                return std::format("#{}", entity);
            };
            for (sizet i = 0; i < std::min(found.size(), MAX_CHANGE_ROWS); ++i)
            {
                const ControlServer::AgentChange& change = found[i];
                std::string text;
                if (change.set && (!change.before.empty() || !change.after.empty()))
                {
                    const std::string before = change.before.empty() ? "…" : change.before;
                    const std::string after = change.after.empty() ? "…" : change.after;
                    const MessageArg args[]{{"field", std::string_view(change.field)},
                                            {"before", std::string_view(before)},
                                            {"after", std::string_view(after)}};
                    text = message("editor-agent.feed.change.values", args);
                }
                else if (change.set)
                {
                    const MessageArg args[]{{"field", std::string_view(change.field)}};
                    text = message("editor-agent.feed.change.changed", args);
                }
                else
                {
                    text = change.field;
                }
                if (change.entity)
                    text = who(change.entity) + " · " + text;
                if (changeRows.size() <= i)
                {
                    changeRows.push_back(label(std::format("agent.feed.change.{}", i), changes, "agent.feed.change"));
                    scope.hierarchy().reorderChild(changeRows.back().entity(), static_cast<u32>(i));
                }
                changeRows[i].setVisible(true);
                setText(changeRows[i], text);
            }
            for (sizet i = std::min(found.size(), MAX_CHANGE_ROWS); i < changeRows.size(); ++i)
                changeRows[i].setVisible(false);
            more.setVisible(found.size() > MAX_CHANGE_ROWS);
            if (found.size() > MAX_CHANGE_ROWS)
            {
                const MessageArg args[]{{"count", static_cast<std::int64_t>(found.size() - MAX_CHANGE_ROWS)}};
                setText(more, message("editor-agent.feed.change.more", args));
            }

            chipEntities.clear();
            for (const ControlServer::AgentChange& change : found)
                if (frameable && change.entity && chipEntities.size() < MAX_CHIPS && shownWorld &&
                    shownWorld->isEntityAlive(Entity(change.entity)) &&
                    std::ranges::find(chipEntities, change.entity) == chipEntities.end())
                    chipEntities.push_back(change.entity);
            world = row.world;
            for (sizet i = 0; i < chipEntities.size(); ++i)
            {
                if (chips.size() <= i)
                {
                    chips.push_back(button(std::format("agent.feed.entity.{}", i), entities));
                    chips.back().onClick([this, i] { queueFrame(chipEntities[i]); });
                }
                const std::string name = who(chipEntities[i]);
                chips[i].setVisible(true);
                chips[i].setText(name);
                const MessageArg args[]{{"entity", std::string_view(name)}};
                chips[i].setAccessibleName(message("editor-agent.feed.entity.frame", args));
            }
            for (sizet i = chipEntities.size(); i < chips.size(); ++i)
                chips[i].setVisible(false);
            entities.setVisible(!chipEntities.empty());
        }

        // Once a second: an integer per card, a string only where the age moved to another text
        void refreshAges(const u64 now)
        {
            seenAge = now / AGE_TICK_MS;
            const auto activities = control->agentActivities();
            for (Card& card : pool)
            {
                if (!card.id)
                    continue;
                const u64 key = ageKeyOf(ageSeconds(card.startedMs, now));
                if (key == card.ageKey)
                    continue;
                card.ageKey = key;
                if (const auto at = std::ranges::find(activities, card.id, &Activity::id); at != activities.end())
                    setText(card.meta, metaText(*at, now));
            }
        }

        void updateElapsed(const u64 now)
        {
            if (!startedMs)
                return;
            const u64 seconds = (now - startedMs) / 1000;
            if (seconds != elapsed)
            {
                elapsed = seconds;
                current.setText(std::format("{} · {}s", action, seconds));
            }
        }

        // -- motion --------------------------------------------------------------------------------------------------

        void resetVisual(const Entity element)
        {
            tweens().cancelElement(element);
            if (scope.world().hasComponent<UiVisualComponent>(element))
            {
                auto& visual = scope.world().getComponent<UiVisualComponent>(element);
                if (visual.opacity == 1 && visual.translate == Vec2{} && visual.scale == Vec2{1, 1})
                    return;
                visual.opacity = 1;
                visual.translate = {};
                visual.scale = {1, 1};
                scope.hierarchy().markVisualDirty(element);
            }
        }

        void resetMotion(Card& card)
        {
            resetVisual(card.root.entity());
            resetVisual(card.icon.entity());
            resetVisual(card.status.entity());
            for (const Panel& dot : card.dot)
                resetVisual(dot.entity());
            card.moving = false;
        }

        void resetAllMotion()
        {
            for (Card& card : pool)
                resetMotion(card);
            resetVisual(details.entity());
            resetVisual(count.entity());
            resetVisual(lamp.entity());
            resetVisual(fill.entity());
            lampBreathing = false;
            stripMode = 0;
            stripWidth = 0.0f;
        }

        UiTweenId tween(const Entity element, const std::string_view field, const Vec4 from, const Vec4 to,
                        const float seconds, const UiEase ease = UiEase::OutCubic, const float delay = 0.0f,
                        const UiTweenId after = NO_UI_TWEEN)
        {
            const auto started = tweens().start({.element = element,
                                                 .component = "UiVisual",
                                                 .field = field,
                                                 .to = to,
                                                 .from = from,
                                                 .duration = seconds,
                                                 .delay = delay,
                                                 .ease = ease,
                                                 .after = after});
            return started ? *started : NO_UI_TWEEN;
        }

        void animateWorking(Card& card, const Signal signal)
        {
            if (signal == Signal::Working)
            {
                const float lift = density.xxs;
                for (sizet i = 0; i < card.dot.size(); ++i)
                {
                    const Entity dot = card.dot[i].entity();
                    const float delay = static_cast<float>(i) * motion.slow * 0.25f;
                    for (const auto [field, from, to] :
                         {std::tuple{"translate", Vec4{0, lift, 0, 0}, Vec4{0, -lift, 0, 0}},
                          std::tuple{"opacity", Vec4{0.35f, 0, 0, 0}, Vec4{1, 0, 0, 0}}})
                        (void)tweens().start({.element = dot,
                                              .component = "UiVisual",
                                              .field = field,
                                              .to = to,
                                              .from = from,
                                              .duration = motion.slow,
                                              .delay = delay,
                                              .ease = UiEase::InOutQuad,
                                              .repeat = 0,
                                              .yoyo = true});
                }
                return;
            }
            const auto breathe = [&](const std::string_view field, const Vec4 from, const Vec4 to)
            {
                (void)tweens().start({.element = card.icon.entity(),
                                      .component = "UiVisual",
                                      .field = field,
                                      .to = to,
                                      .from = from,
                                      .duration = motion.loop,
                                      .ease = UiEase::InOutQuad,
                                      .repeat = 0,
                                      .yoyo = true});
            };
            breathe("opacity", Vec4{0.5f, 0, 0, 0}, Vec4{1, 0, 0, 0});
            if (signal == Signal::Checking)
                breathe("scale", Vec4{0.9f, 0.9f, 0, 0}, Vec4{1.08f, 1.08f, 0, 0});
        }

        void animateEntry(Card& card)
        {
            tween(card.root.entity(), "opacity", Vec4{0, 0, 0, 0}, Vec4{1, 0, 0, 0}, motion.normal);
            tween(card.root.entity(), "translate", Vec4{0, -density.md, 0, 0}, Vec4{}, motion.normal);
        }

        // A Keep all settles its cards one after another, newest first
        [[nodiscard]] float staggerDelay(const u64 id) const
        {
            const auto at = std::ranges::find(stagger, id);
            return at == stagger.end()
                ? 0.0f
                : static_cast<float>(std::min(static_cast<sizet>(at - stagger.begin()), MAX_STAGGER)) * motion.fast *
                    0.25f;
        }

        // One play per change: the pill crossfades, then the new signal's own beat
        void animateChange(Card& card, const Signal to, const float delay)
        {
            const float lift = density.md;
            tween(card.status.entity(), "opacity", Vec4{0.2f, 0, 0, 0}, Vec4{1, 0, 0, 0}, motion.fast, UiEase::OutCubic,
                  delay);
            switch (to)
            {
            case Signal::Failed:
                {
                    tween(card.icon.entity(), "opacity", Vec4{0.2f, 0, 0, 0}, Vec4{1, 0, 0, 0}, motion.normal);
                    const float shake = density.xs;
                    const float quarter = motion.normal * 0.25f;
                    const UiTweenId left =
                        tween(card.root.entity(), "translate", Vec4{}, Vec4{-shake, 0, 0, 0}, quarter);
                    const UiTweenId right = tween(card.root.entity(), "translate", Vec4{-shake, 0, 0, 0},
                                                  Vec4{shake, 0, 0, 0}, quarter * 2, UiEase::OutCubic, 0.0f, left);
                    tween(card.root.entity(), "translate", Vec4{shake, 0, 0, 0}, Vec4{}, quarter, UiEase::OutCubic,
                          0.0f, right);
                    break;
                }
            case Signal::Reverted:
                tween(card.root.entity(), "translate", Vec4{lift, 0, 0, 0}, Vec4{}, motion.normal);
                tween(card.root.entity(), "opacity", Vec4{0.45f, 0, 0, 0}, Vec4{1, 0, 0, 0}, motion.normal);
                break;
            case Signal::Done:
            case Signal::Kept:
            case Signal::Review:
                pulseAgentActivity(editorUi(*engine).ui(), card.icon.entity(), motion.fast, delay);
                tween(card.icon.entity(), "scale", Vec4{0.75f, 0.75f, 0, 0}, Vec4{1, 1, 0, 0}, motion.normal,
                      UiEase::OutBack, delay);
                break;
            default:
                break;
            }
        }

        void animateExpand()
        {
            tween(details.entity(), "opacity", Vec4{0, 0, 0, 0}, Vec4{1, 0, 0, 0}, motion.fast);
            tween(details.entity(), "translate", Vec4{0, -density.xs, 0, 0}, Vec4{}, motion.fast);
        }

        // -- requests ------------------------------------------------------------------------------------------------

        void queueReview(const std::string_view verb)
        {
            requests.push_back(
                Json{{"command", "reviewAgentActivity"}, {"args", {{"id", selected}, {"action", verb}}}}.dump());
        }

        void queueFrame(const u64 entity)
        {
            requests.push_back(
                Json{{"command", "select"}, {"args", {{"world", world}, {"ids", Json::array({entity})}}}}.dump());
            requests.push_back(Json{{"command", "editorCamera"}, {"args", {{"frame", "selection"}}}}.dump());
        }

        // False after one error toast: the rest of the batch is dropped
        bool run(const std::string& line)
        {
            const Json reply = Json::parse(control->handle(line), nullptr, false);
            if (reply.is_object() && !reply.value("ok", false))
            {
                feedback->postToast(ToastLevel::Error, reply.value("error", std::string{}));
                return false;
            }
            return true;
        }

        // Keeps every operation awaiting review, newest first, each through reviewAgentActivity: keeping a step marks
        // the older ones' fields reviewed too, so a row that settled by itself is skipped, not refused. The ids are
        // read now, at the frame's start, not when the button was pressed.
        bool keepEvery()
        {
            stagger.clear();
            const auto activities = control->agentActivities();
            for (auto at = activities.rbegin(); at != activities.rend(); ++at)
                if (control->agentActivityCanReview(at->id))
                    stagger.push_back(at->id);
            for (const u64 id : std::vector<u64>(stagger))
                if (control->agentActivityCanReview(id) &&
                    !run(Json{{"command", "reviewAgentActivity"}, {"args", {{"id", id}, {"action", "keep"}}}}.dump()))
                    return false;
            return true;
        }

        void flushRequests()
        {
            if (requests.empty() && !keepAllQueued)
                return;
            std::vector<std::string> queued;
            queued.swap(requests);
            for (const std::string& line : queued)
                if (!run(line))
                {
                    keepAllQueued = false;
                    return;
                }
            if (std::exchange(keepAllQueued, false))
                keepEvery();
        }

        // -- control -------------------------------------------------------------------------------------------------

        void registerQuery()
        {
            control->registerQuery(
                "agentPanelView", OWNER,
                "the Agent panel: its groups (now, review, earlier) with their row counts, the rows in the order shown "
                "(id, title, status as the pill says it, signal, group, kind and age), the expanded row (0: none), the "
                "counts of changes awaiting review and failed operations, keepAll (Keep all is offered) and live (the "
                "indicator under the header: active, kind none, read, write or operation, its caption, strip shown, "
                "and the operations held as Working); read from the feed, so a hidden panel answers too",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    const u64 now = control->agentActivityClockMs();
                    collect(now);
                    const Activity* focus = control->agentActivityFocus();
                    const LiveKind kind = liveOf(now, focus);
                    out.writeU64("expanded", selected);
                    out.writeBool("keepAll", reviewCount >= MIN_KEEP_ALL);
                    out.beginObject("live");
                    out.writeBool("active", kind != LiveKind::None);
                    out.writeString("kind", LIVE_NAMES[static_cast<u8>(kind)]);
                    out.writeString("caption", kind != LiveKind::None ? liveCaption(kind, focus) : std::string{});
                    out.writeBool("strip", strip.isValid() && strip.isVisible());
                    out.writeU64("held",
                                 std::ranges::count_if(holds, [now](const auto& hold) { return hold.second > now; }));
                    out.endObject();
                    out.writeU64("pending", pending);
                    out.writeU64("failed", failed);
                    out.writeU64("cards", pool.size());
                    out.writeU64("binds", binds);
                    out.beginArray("groups", static_cast<u32>(GROUP_NAMES.size()));
                    for (sizet i = 0; i < GROUP_NAMES.size(); ++i)
                    {
                        out.beginObject("");
                        out.writeString("name", GROUP_NAMES[i]);
                        out.writeU64("rows", std::ranges::count(items, static_cast<Group>(i), &Item::group));
                        out.endObject();
                    }
                    out.endArray();
                    out.beginArray("rows", static_cast<u32>(items.size()));
                    for (const Item& item : items)
                    {
                        out.beginObject("");
                        out.writeU64("id", item.row->id);
                        out.writeString("title", titleOf(item));
                        out.writeString("status", message(pillKeyOf(item)));
                        out.writeString("signal", signalName(item.signal));
                        out.writeString("group", GROUP_NAMES[static_cast<u8>(item.group)]);
                        out.writeString("kind", message("editor-agent.feed.kind." + item.row->kind));
                        out.writeString("time", ageText(*item.row, now));
                        out.endObject();
                    }
                    out.endArray();
                    return std::string{};
                });
        }
    };

    EditorAgentPanel::EditorAgentPanel(Engine& engine, EditorFeedback& feedback, EditorAgentLayer& agent) :
        m_state(std::make_unique<State>(engine, feedback, agent))
    {
        m_state->build(engine);
    }

    EditorAgentPanel::~EditorAgentPanel()
    {
        m_state->scope.unobserveLayoutTokens(m_state->observer);
        m_state->control->unregisterOwner(OWNER);
    }

    Widget EditorAgentPanel::content() const { return m_state->root; }

    void EditorAgentPanel::update()
    {
        State& state = *m_state;
        const bool visible = shown(state.scope, state.root);
        state.agent->setPanelShown(visible);
        if (!visible)
        {
            if (state.wasShown)
                state.resetAllMotion();
            state.wasShown = false;
            return;
        }
        state.flushRequests();
        const u64 now = state.control->agentActivityClockMs();
        // A client request moves the live state: rebind when it turns on or changes kind, only extend its window when
        // it goes on; rebind again when a window or a hold runs out.
        if (const auto traffic = state.control->agentTraffic(); traffic.count != state.seenTraffic)
        {
            state.seenTraffic = traffic.count;
            if (!state.trafficDueMs || traffic.command != state.trafficWrite)
                state.dirty = true;
            state.trafficDueMs = traffic.atMs + LIVE_WINDOW_MS;
        }
        if ((state.trafficDueMs && now >= state.trafficDueMs) || (state.holdDueMs && now >= state.holdDueMs))
            state.dirty = true;
        const bool reduced = state.tweens().reducedMotion();
        if (state.dirty || !state.wasShown || reduced != state.reducedMotion ||
            state.control->agentActivityRevision() != state.seenRevision ||
            state.control->stateGeneration() != state.seenGeneration ||
            state.engine->localization().revision() != state.seenLocale)
            state.bind();
        else if (now / AGE_TICK_MS != state.seenAge)
            state.refreshAges(now);
        state.wasShown = true;
        state.updateElapsed(now);
        if (state.stripMode)
            state.fitStrip();
        const float width = std::max(0.0f, resolvedSize(state.scope, state.scroll).x - state.density.md);
        if (state.cards.size().x != width)
            state.cards.setSize({width, state.cards.size().y});
        const Vec2 size{width, state.scope.hierarchy().layoutSizeOf(state.cards.entity()).y};
        if (state.scroll.contentSize() != size)
            state.scroll.setContentSize(size);
    }

    namespace
    {
        const EditorPanelRegistrar AGENT_REGISTRATION{
            {.name = "agent",
             .category = "tools",
             .order = 191,
             .dock = {.target = "project"},
             .zone = "EditorAgentDashboard",
             .make = [](EditorPanelContext& c)
             { return makeEditorPanel<EditorAgentPanel>(c.engine, c.feedback, c.agent); },
             .openByDefault = false}};
    } // namespace
} // namespace spite
