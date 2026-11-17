#include "EditorLog.hpp"
#include <spite/platform/LocalTime.hpp>
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorDialogs.hpp"
#include "EditorFeedback.hpp"
#include "EditorPanelList.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <format>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/Machine.hpp"
#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "control/LogHistory.hpp"
#include "engine/CaptureStore.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "serialization/FormatWalk.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiScope.hpp"
#include "ui/UiTextMeasurer.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;
        using namespace panelui;

        constexpr std::string_view LOG_VIEW = "editor.log";
        constexpr std::string_view LOG_TOOLBAR = "log.toolbar";
        constexpr std::string_view LOG_FILTER = "log.filter";
        constexpr std::string_view LOG_ACTIONS = "log.actions";
        constexpr std::string_view LOG_BAR = "log.bar";
        constexpr std::string_view LOG_SUMMARY = "log.summary";
        constexpr std::string_view LOG_ROWS = "log.rows";
        constexpr std::string_view LOG_DETAIL = "log.detail";

        // the level toggles in rank order: debug 0, info and success 1, warning 2, error 3
        constexpr std::array<std::string_view, 4> LEVEL_NAMES{"debug", "info", "warning", "error"};
        constexpr u8 DEFAULT_LEVELS = 0b1110; // the quiet default: info and above
        constexpr u32 MAX_CATEGORIES = 128; // the popup lists at most this many
        constexpr u32 MAX_CATEGORY_LENGTH = 40;
        constexpr float ACTION_WIDTH = 64.0f;
        constexpr float DETAIL_HEIGHT = 84.0f; // at most
        constexpr float MIN_DETAIL_HEIGHT = 24.0f; // below this the detail does not show
        constexpr float MIN_TABLE_ROWS = 2.0f; // the detail leaves the table at least this many rows
        constexpr float TIME_WIDTH = 112.0f;
        constexpr float LEVEL_WIDTH = 84.0f;
        constexpr float CATEGORY_WIDTH = 190.0f;
        constexpr float MESSAGE_WIDTH = 400.0f;
        constexpr std::string_view LOGS_ROOT = "logs"; // where logSave path= and the Save dialog write
        constexpr float CATEGORY_BUTTON_MAX = 130.0f; // a long category name clips
        constexpr float MAX_MESSAGE_WIDTH = 4096.0f;
        constexpr u32 RETURN_KEY = 40; // HID
        constexpr u32 SPACE_KEY = 44;

        u8 rankOf(const std::string_view level)
        {
            switch (level.empty() ? ' ' : level.front())
            {
            case 'd':
                return 0;
            case 'w':
                return 2;
            case 'e':
                return 3;
            default:
                return 1;
            }
        }

        std::string_view fileName(const std::string_view file)
        {
            const sizet slash = file.find_last_of("/\\");
            return slash == std::string_view::npos ? file : file.substr(slash + 1);
        }

        std::string_view fileStem(const std::string_view file)
        {
            const std::string_view name = fileName(file);
            const sizet dot = name.find_last_of('.');
            return dot == std::string_view::npos || dot == 0 ? name : name.substr(0, dot);
        }

        bool isNameChar(const char c)
        {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        }

        // "[Tag] text": the tag, when it is a short name
        std::string_view tagOf(const std::string_view message)
        {
            if (message.empty() || message.front() != '[')
            {
                return {};
            }
            const sizet close = message.find(']', 1);
            if (close == std::string_view::npos || close == 1 || close > MAX_CATEGORY_LENGTH)
            {
                return {};
            }
            const std::string_view tag = message.substr(1, close - 1);
            return std::ranges::all_of(tag, [](const char c) { return isNameChar(c) || c == '.'; })
                ? tag
                : std::string_view{};
        }

        // a row's category: its "[Tag]" prefix, else its source file
        std::string_view categoryOf(const LogEntryRow& row)
        {
            const std::string_view tag = tagOf(row.message);
            return tag.empty() ? fileStem(row.file) : tag;
        }

        // the message without the "[Tag] " the category column shows
        std::string_view bodyOf(const std::string_view message)
        {
            const std::string_view tag = tagOf(message);
            if (tag.empty())
            {
                return message;
            }
            const std::string_view body = message.substr(tag.size() + 2);
            return body.starts_with(' ') ? body.substr(1) : body;
        }

        std::string formatTime(const u64 timeNs)
        {
            const auto seconds = static_cast<std::time_t>(timeNs / 1'000'000'000ull);
            std::tm local{};
            spite::platform::toLocalTime(seconds, local);
            return std::format("{:02}:{:02}:{:02}.{:03}", local.tm_hour, local.tm_min, local.tm_sec,
                               (timeNs / 1'000'000ull) % 1000ull);
        }

        // a row as the copy and the saved file write it
        std::string formatRow(const LogEntryRow& row)
        {
            return std::format("{} {:<7} {}  ({}:{})", formatTime(row.timeNs), row.level, row.message,
                               fileName(row.file), row.line);
        }

        // What the table shows of a row (the level is localized when the row is read).
        struct LogCells
        {
            std::string time;
            std::string level;
            std::string category;
            std::string message;
            std::string levelClass; // the level cell's class suffix (<rowClass>.text.warn|error), not a column
            SPITE_OBSERVED_SCHEMA(LogCells, time, level, category, message, levelClass)
        };

        // a warning's and an error's level reads in its colour
        UiTableColumn column(const std::string_view field, const float width)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString("editor-log.column." + std::string(field)),
                    .sortable = false,
                    .classField = field == "level" ? InternedString("levelClass") : InternedString{}};
        }

        void localized(const TextWidget widget, const std::string_view key)
        {
            TextWidget text = widget;
            text.setLocalizedText(InternedString(key));
        }
    } // namespace

    // -- log ----------------------------------------------------------------------------

    struct EditorLog::State
    {
        State(Engine& engine, EditorFeedback& feedback, EditorDialogs& dialogs) :
            engine(&engine), control(&engine.control()), feedback(&feedback), dialogs(&dialogs),
            scope(editorUi(engine).ui()),
            history(engine.services().contains<LogHistory>() ? &engine.services().get<LogHistory>() : nullptr),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr)
        {
        }

        Engine* engine;
        ControlServer* control;
        EditorFeedback* feedback;
        EditorDialogs* dialogs;
        UiScope scope;
        DensityFollower density{scope};
        LogHistory* history;
        const Localization* localization; // not owned: the engine's service
        Panel root;
        std::array<Checkbox, LEVEL_NAMES.size()> levelBoxes;
        Button categoryButton;
        PopupList categoryList;
        Checkbox followBox;
        std::array<Button, 3> actions; // clear, save, copy
        // Held rows per level as chips (info, warning, error) that switch the level.
        std::array<Button, 3> tagChips;
        std::array<u32, 4> rankCounts{}; // every held row since Clear by rank (not only the shown ones)
        std::deque<u8> ranks; // the rank of each counted row, from index `ranksFrom`
        u64 ranksFrom = 0;
        std::array<u32, 3> tagCounts{}; // what the chips show
        u8 tagMask = 0xff; // the levels they show (none yet)
        ScrollView detailView;
        Label detailText;
        std::unique_ptr<FilteredTable> list;
        std::unique_ptr<UiDocumentView> view;

        // The model: log indices (ascending) of the rows the level and category admit, and of those the search also
        // matches. A key of the table is the index as an entity id (a session logs far fewer than 2^32 rows).
        std::deque<u64> admitted;
        std::deque<u64> matches;
        std::set<std::string, std::less<>> categories; // seen so far, at most MAX_CATEGORIES
        std::vector<std::string> categoryNames; // the popup's items after "all", in order
        u64 popped = 0; // shown rows that rolled off the front since the list last took the model
        u64 seen = 0; // log rows read so far
        u64 floor = 0; // Clear: rows before it are not shown
        u8 levels = DEFAULT_LEVELS;
        std::string category; // empty: every category
        std::string text; // the search
        bool follow = true;
        bool dirty = true; // the list lags the model
        bool categoriesDirty = true; // the popup's items lag `categories`
        bool detailDirty = true;
        std::vector<std::string> pending;
        u64 rebuilds = 0;
        u64 seenLocale = 0;
        float lastY = 0.0f;
        Vec2 lastSize{};
        float detailWidth = -1.0f;
        float detailHeight = -1.0f; // the measured height of the text
        float detailRoom = -1.0f; // the height the view has been given
        bool detailWanted = false; // a row is selected
        float fittedWidth = -1.0f;
        float widestSeen = 0.0f; // the longest message's width, once measured
        std::string longest; // the longest message the table read
        bool longestDirty = false;
        std::array<std::string, 5> levelTexts; // debug, info, success, warning, error: localized
        LogCells cells; // the row being read

        [[nodiscard]] std::string message(const std::string_view key) const
        {
            if (!localization)
            {
                return std::string(key);
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(key), scratch));
        }

        void loadLevelTexts()
        {
            constexpr std::array<std::string_view, 5> keys{"debug", "info", "success", "warning", "error"};
            for (u32 i = 0; i < keys.size(); ++i)
            {
                levelTexts[i] = message("editor-log.cell." + std::string(keys[i]));
            }
            seenLocale = localization ? localization->revision() : 0;
        }

        [[nodiscard]] const std::string& levelText(const std::string_view level) const
        {
            switch (level.empty() ? ' ' : level.front())
            {
            case 'd':
                return levelTexts[0];
            case 's':
                return levelTexts[2];
            case 'w':
                return levelTexts[3];
            case 'e':
                return levelTexts[4];
            default:
                return levelTexts[1];
            }
        }

        static Entity keyOf(const u64 index) { return Entity(static_cast<u32>(index), 1u); }

        // the index a key of a shown row names, when it is still shown
        [[nodiscard]] std::optional<u64> shownIndex(const Entity key) const
        {
            const auto it = std::ranges::lower_bound(matches, static_cast<u64>(key.index()));
            return it != matches.end() && *it == key.index() ? std::optional<u64>(*it) : std::nullopt;
        }

        // -- the model --

        void learn(const std::string_view name)
        {
            if (categories.size() < MAX_CATEGORIES && !categories.contains(name))
            {
                categories.emplace(name);
                categoriesDirty = true;
            }
        }

        [[nodiscard]] bool admits(const u8 rank, const std::string_view name) const
        {
            return ((levels >> rank) & 1) != 0 && (category.empty() || name == category);
        }

        // The rows from..to-1 that are held go through the filters. The history's lock is held only while they are
        // copied (the category, and the message of a row the levels admit while a search is typed): learning categories
        // and searching run outside it, so logging threads are not held up by a rebuild.
        void match(const u64 from, const u64 to)
        {
            struct Candidate
            {
                u64 index;
                std::string category;
                std::string message;
                bool admitted;
            };
            std::vector<Candidate> candidates;
            history->read(from, to - from,
                          [&](const u64 index, const LogEntryRow& row)
                          {
                              const std::string_view name = categoryOf(row);
                              const u8 rank = rankOf(row.level);
                              if (!ranks.empty() && index != ranksFrom + ranks.size())
                              {
                                  ranks.clear(); // rows rolled off between two reads: the counted ones are stale
                                  rankCounts = {};
                              }
                              if (ranks.empty())
                              {
                                  ranksFrom = index;
                              }
                              ranks.push_back(rank);
                              ++rankCounts[rank];
                              const bool pass = admits(rank, name);
                              candidates.push_back({index, std::string(name),
                                                    pass && !text.empty() ? row.message : std::string{}, pass});
                          });
            for (const Candidate& row : candidates)
            {
                learn(row.category);
                if (!row.admitted)
                {
                    continue;
                }
                admitted.push_back(row.index);
                if (text.empty() || containsIgnoringCase(row.message, text) || containsIgnoringCase(row.category, text))
                {
                    matches.push_back(row.index);
                }
            }
        }

        // Follows the log: new rows that pass the filters, rows that rolled off.
        void sync()
        {
            const u64 recorded = history ? history->recorded() : 0;
            if (recorded == seen)
            {
                return;
            }
            SPITE_ZONE("EditorLog.sync");
            match(std::max(seen, floor), recorded);
            seen = recorded;
            const u64 oldest = history->oldest();
            for (; !ranks.empty() && ranksFrom < oldest; ++ranksFrom)
            {
                --rankCounts[ranks.front()];
                ranks.pop_front();
            }
            while (!admitted.empty() && admitted.front() < oldest)
            {
                admitted.pop_front();
            }
            while (!matches.empty() && matches.front() < oldest)
            {
                matches.pop_front();
                ++popped;
            }
            dirty = true;
        }

        // A filter changed: the held log is read again, once.
        void rebuild()
        {
            admitted.clear();
            matches.clear();
            ranks.clear();
            rankCounts = {};
            popped = 0;
            if (history)
            {
                seen = history->recorded();
                match(std::max(history->oldest(), floor), seen);
            }
            ++rebuilds;
            dirty = true;
        }

        // The chips follow the counts and the levels: a text, a look and the selected state, each written only when it
        // differs (a log row of another level, or none, writes nothing).
        void syncTag()
        {
            const std::array<u32, 3> counts{rankCounts[1], rankCounts[2], rankCounts[3]};
            const u8 mask = (levels >> 1) & 0b111;
            if (counts == tagCounts && mask == tagMask)
            {
                return;
            }
            tagCounts = counts;
            tagMask = mask;
            constexpr std::array<std::string_view, 3> LOOKS{"info", "warn", "error"};
            for (u32 i = 0; i < tagChips.size(); ++i)
            {
                const bool on = ((mask >> i) & 1) != 0;
                tagChips[i].setLocalizedArg("count", static_cast<std::int64_t>(counts[i]));
                tagChips[i].setStyleClass(InternedString(
                    "log.tag.chip." +
                    (counts[i] == 0 ? std::string("zero") : std::string(LOOKS[i]) + (on ? "" : ".off"))));
                tagChips[i].setAccessibleSelected(on);
            }
        }

        void setLevels(const u8 mask)
        {
            levels = mask & 0b1111;
            for (u32 i = 0; i < levelBoxes.size(); ++i)
            {
                const bool on = ((levels >> i) & 1) != 0;
                if (levelBoxes[i].checked() != on)
                {
                    levelBoxes[i].setChecked(on);
                }
            }
            rebuild();
            syncTag();
        }

        void setCategory(std::string name)
        {
            category = std::move(name);
            if (category.empty())
            {
                localized(categoryButton, "editor-log.category.none");
            }
            else
            {
                localized(categoryButton, "editor-log.category");
                categoryButton.setLocalizedArgText("name", category);
            }
            rebuild();
        }

        void setText(const std::string_view typed)
        {
            text = typed;
            rebuild();
        }

        void setFollow(const bool value)
        {
            follow = value;
            if (followBox.checked() != value)
            {
                followBox.setChecked(value);
            }
            if (value)
            {
                list->table().scrollToEnd();
                lastY = list->table().scrollY();
            }
        }

        // Puts the model into the list; follows the end while following.
        void present()
        {
            if (!dirty)
            {
                return;
            }
            SPITE_ZONE("EditorLog.present");
            dirty = false;
            list->setShown(static_cast<u32>(matches.size()), static_cast<u32>(admitted.size()));
            TableView table = list->table();
            if (follow)
            {
                table.scrollToEnd();
            }
            else if (popped != 0)
            {
                // the viewed rows stay
                table.setScrollY(table.scrollY() - static_cast<float>(popped) * table.rowHeight());
            }
            popped = 0;
            lastY = table.scrollY();
            detailDirty = true;
        }

        // Follow tracks the scroll position: a scroll away from the end stops it, a scroll back to the end resumes
        // it; a resize of the list keeps it at the end.
        void trackScroll()
        {
            TableView table = list->table();
            const Vec2 size = table.size();
            if (size != lastSize)
            {
                lastSize = size;
                if (follow)
                {
                    table.scrollToEnd();
                }
                lastY = table.scrollY();
                return;
            }
            const float y = table.scrollY();
            if (y == lastY)
            {
                return;
            }
            lastY = y;
            if (follow != table.atEnd())
            {
                follow = !follow;
                followBox.setChecked(follow);
            }
        }

        // -- the rows --

        void fill(const u32 item)
        {
            cells = {};
            if (item >= matches.size())
            {
                return; // the list lags a shrunk model until present()
            }
            history->read(matches[item], 1,
                          [this](u64, const LogEntryRow& row)
                          {
                              cells.time = formatTime(row.timeNs);
                              cells.level = levelText(row.level);
                              cells.levelClass = row.level.starts_with("warn") ? "warn"
                                  : row.level.starts_with("err")               ? "error"
                                                                               : "";
                              cells.category = std::string(categoryOf(row));
                              cells.message = std::string(bodyOf(row.message));
                          });
            if (cells.message.size() > longest.size())
            {
                longest = cells.message;
                longestDirty = true;
            }
        }

        // The message column grows to the longest message read and to the viewport; a longer row scrolls sideways.
        void fitMessageColumn()
        {
            TableView table = list->table();
            const float width = table.size().x;
            if (!longestDirty && width == fittedWidth)
            {
                return;
            }
            fittedWidth = width;
            const InternedString message("message");
            if (longestDirty)
            {
                longestDirty = false;
                const UiTextMeasurer* measurer = scope.hierarchy().textMeasurer();
                const auto parts = scope.hierarchy().childrenOf(table.headerCell(message).entity());
                if (measurer && !parts.empty())
                {
                    widestSeen =
                        std::min(measurer->measureText(parts[0], longest).x + 2.0f * density.density().xs + 2.0f,
                                 MAX_MESSAGE_WIDTH);
                }
            }
            const float others = table.columnWidth(InternedString("time")) +
                table.columnWidth(InternedString("level")) + table.columnWidth(InternedString("category"));
            const float wanted = std::max(widestSeen, width - others - THUMB);
            if (wanted > table.columnWidth(message) + 0.5f)
            {
                table.apply({.kind = UiTableOpKind::Resize, .column = message, .width = wanted});
            }
        }

        // the cursor row, as the detail shows it
        [[nodiscard]] std::string detailOf() const
        {
            std::string out;
            const TableView table = list->table();
            if (table.selection().empty() || !history)
            {
                return out;
            }
            const Entity key = table.cursor() != Entity::undefined() ? table.cursor() : table.selection().front();
            if (const std::optional<u64> index = shownIndex(key))
            {
                history->read(*index, 1,
                              [&out](u64, const LogEntryRow& row)
                              {
                                  out = std::format("{} {}  [{}]  {}:{}\n{}", formatTime(row.timeNs), row.level,
                                                    categoryOf(row), fileName(row.file), row.line, bodyOf(row.message));
                              });
            }
            return out;
        }

        // the detail of the selection, and Copy enabled while rows are selected
        void showDetail()
        {
            detailDirty = false;
            const bool selected = !list->table().selection().empty();
            if (actions[2].isInteractable() != selected)
            {
                actions[2].setInteractable(selected);
            }
            const std::string detail = detailOf();
            detailWanted = !detail.empty();
            if (detailWanted && detailText.text() != detail)
            {
                detailText.setText(detail);
                detailWidth = -1.0f;
            }
        }

        // The detail takes what the panel can spare above MIN_TABLE_ROWS of the table, at most DETAIL_HEIGHT; a panel
        // with less room shows none (the table keeps the room).
        void fitDetail()
        {
            const float room = resolvedSize(scope, root).y - 2.0f * density.density().bar - list->headerHeight() -
                MIN_TABLE_ROWS * list->rowHeight();
            const float height = std::min(room, DETAIL_HEIGHT);
            const bool show = detailWanted && height >= MIN_DETAIL_HEIGHT;
            if (detailView.isVisible() != show)
            {
                detailView.setVisible(show);
            }
            if (show && height != detailRoom)
            {
                detailRoom = height;
                detailView.setSize(Vec2{0.0f, height});
            }
        }

        // The detail wraps at the view's width; the view scrolls over the height it measured.
        void layoutDetail()
        {
            fitDetail();
            if (!detailView.isVisible())
            {
                return;
            }
            const float width = std::max(0.0f, resolvedSize(scope, detailView).x - THUMB - 2.0f * density.density().xs);
            const float height = resolvedSize(scope, detailText).y;
            if (width == detailWidth && height == detailHeight)
            {
                return;
            }
            if (width != detailWidth)
            {
                place(detailText, Vec2{density.density().xs, density.density().xs}, Vec2{width, detailText.size().y});
            }
            detailWidth = width;
            detailHeight = height;
            detailView.setContentSize(Vec2{width, height + 2.0f * density.density().xs});
        }

        // -- copy and save --

        // the rows' text, one line each, oldest first; rows that rolled off are skipped
        [[nodiscard]] std::string textOf(const std::span<const u64> indices, u32& count) const
        {
            std::string out;
            count = 0;
            for (const u64 index : indices)
            {
                history->read(index, 1,
                              [&](u64, const LogEntryRow& row)
                              {
                                  out += formatRow(row);
                                  out += '\n';
                                  ++count;
                              });
            }
            return out;
        }

        std::string copy(IFormatWriter& out)
        {
            std::vector<u64> indices;
            for (const Entity key : list->table().selection())
            {
                if (const std::optional<u64> index = shownIndex(key))
                {
                    indices.push_back(*index);
                }
            }
            std::ranges::sort(indices);
            u32 count = 0;
            const std::string rows = textOf(indices, count);
            if (count == 0)
            {
                return "no row selected (logSelect first=<row> count=<n>)";
            }
            UiContext& ui = editorUi(*engine).ui();
            ui.setClipboardText(rows);
            // the OS clipboard hands lines back with CR LF; a CR in a message is the message's
            std::string taken = ui.clipboardText();
            std::string expected = rows;
            std::erase(taken, '\r');
            std::erase(expected, '\r');
            if (taken != expected)
            {
                return "the clipboard did not take the text";
            }
            feedback->postToastKey(ToastLevel::Info, "editor-log.copied",
                                   std::vector<MessageArg>{{"count", static_cast<std::int64_t>(count)}});
            out.writeU32("rows", count);
            out.writeString("text", rows);
            return {};
        }

        // The Save button: a file dialog over the logs root (the file's name, folders, replacing a file); the
        // choice is what logSave path= writes, queued like every panel action.
        void askSave()
        {
            const auto now = std::chrono::floor<std::chrono::seconds>(
                std::chrono::current_zone()->to_local(std::chrono::system_clock::now()));
            std::string scratch;
            const std::string title = localization
                ? std::string(localization->resolve(InternedString("editor-log.save.title"), scratch))
                : std::string("editor-log.save.title");
            dialogs->pickFile({.mode = EditorPickMode::SaveFile,
                               .title = title,
                               .roots = {std::string(LOGS_ROOT)},
                               .name = std::format("log-{:%Y%m%d-%H%M%S}", now),
                               .extensions = "txt"},
                              [this](const std::optional<EditorPicked> picked)
                              {
                                  if (picked)
                                  {
                                      pending.push_back(
                                          Json{{"command", "logSave"}, {"args", {{"path", picked->path}}}}.dump());
                                  }
                              });
        }

        std::string save(const ControlArgs& args, IFormatWriter& out)
        {
            std::string name = args.string("name").value_or("");
            const std::optional<std::string> chosen = args.string("path");
            if (chosen && !name.empty())
            {
                return "name and path both name the file: give one";
            }
            if (!chosen)
            {
                if (name.empty())
                {
                    const auto now = std::chrono::floor<std::chrono::seconds>(
                        std::chrono::current_zone()->to_local(std::chrono::system_clock::now()));
                    name = std::format("log-{:%Y%m%d-%H%M%S}", now);
                }
                if (name.size() > 64 || !std::ranges::all_of(name, isNameChar))
                {
                    return "name takes up to 64 letters, digits, '-' and '_'";
                }
            }
            if (!engine->services().contains<CaptureStore>())
            {
                return "no artifacts root to save under";
            }
            std::filesystem::path file =
                engine->services().get<CaptureStore>().artifactsRoot() / machineFolderName() / "logs" / (name + ".txt");
            if (chosen)
            {
                if (std::string error = control->resolveFile(LOGS_ROOT, *chosen, file); !error.empty())
                {
                    return error;
                }
            }
            const std::vector<u64> shownRows(matches.begin(), matches.end());
            u32 count = 0;
            const std::string body = textOf(shownRows, count);
            std::string error;
            if (const Expected<void> made = ensureDirectory(file.parent_path()); !made)
            {
                error = made.error().message.c_str();
            }
            else if (const Expected<void> written = writeTextFileAtomically(file, body); !written)
            {
                error = written.error().message.c_str();
            }
            if (!error.empty())
            {
                feedback->postToastKey(ToastLevel::Error, "editor-log.saveFailed",
                                       std::vector<MessageArg>{{"error", std::string_view(error)}});
                return error;
            }
            const std::string path = utf8Path(file);
            feedback->postToastKey(
                ToastLevel::Info, "editor-log.saved",
                std::vector<MessageArg>{{"count", static_cast<std::int64_t>(count)}, {"path", std::string_view(path)}});
            out.writeString("path", path);
            out.writeU32("rows", count);
            return {};
        }

        // -- control --

        void registerCommands()
        {
            control->registerViewCommand(
                "logLevel", "editorLog",
                "sets which levels the log panel shows: level alone shows that level and every level above it (debug "
                "shows all), with shown it switches that one level on or off: the level toggles",
                {requiredArg("level", ArgKind::String, "debug, info, warning or error", LEVEL_NAMES),
                 optionalArg("shown", ArgKind::Boolean, "switch only this level", "this level and above")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const auto rank = static_cast<u32>(
                        std::ranges::find(LEVEL_NAMES, args.string("level").value_or("")) - LEVEL_NAMES.begin());
                    if (const std::optional<bool> shown = args.boolean("shown"))
                    {
                        setLevels(static_cast<u8>(*shown ? levels | (1u << rank) : levels & ~(1u << rank)));
                    }
                    else
                    {
                        setLevels(static_cast<u8>(0b1111u & ~((1u << rank) - 1u)));
                    }
                    return std::string{};
                });
            control->registerViewCommand(
                "logCategory", "editorLog",
                "shows the log panel's rows of one category only (a row's [Tag] prefix, else its source file); no "
                "category shows all: the Category popup",
                {optionalArg("category", ArgKind::String, "the category", "all")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    setCategory(args.string("category").value_or(""));
                    return std::string{};
                });
            control->registerViewCommand("logFollow", "editorLog",
                                         "switches the log panel's follow of the newest row: the Follow box; on shows "
                                         "the end now",
                                         {requiredArg("follow", ArgKind::Boolean, "follow the end")},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         {
                                             setFollow(args.boolean("follow").value_or(true));
                                             return std::string{};
                                         });
            control->registerViewCommand(
                "logClear", "editorLog",
                "hides the log panel's rows so far (the engine's log history and the status bar's counts stay): the "
                "Clear button; the answer is the rows hidden",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    out.writeU64("cleared", matches.size());
                    floor = history ? history->recorded() : 0;
                    rebuild();
                    return std::string{};
                });
            control->registerViewCommand(
                "logSelect", "editorLog",
                "selects shown rows of the log panel by view row; count 0 clears the selection",
                {requiredArg("first", ArgKind::Integer, "the first view row"),
                 optionalArg("count", ArgKind::Integer, "rows from first", "1")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const u64 first = args.integer("first").value_or(0);
                    const u64 count = args.integer("count").value_or(1);
                    std::vector<Entity> keys;
                    for (u64 row = first; row < first + count && row < matches.size(); ++row)
                    {
                        keys.push_back(keyOf(matches[row]));
                    }
                    list->table().setSelection(keys);
                    if (!keys.empty())
                    {
                        list->table().scrollTo(keys.back());
                    }
                    detailDirty = true;
                    return std::string{};
                });
            control->registerViewCommand("logCopy", "editorLog",
                                         "copies the selected rows of the log panel to the clipboard, one line each "
                                         "(time, level, message, file:line): the Copy button; the answer is the text",
                                         {}, [this](const ControlArgs&, IFormatWriter& out) { return copy(out); });
            control->registerViewCommand(
                "logSave", "editorLog",
                "writes the rows the log panel shows to <artifacts>/<machine>/logs/<name>.txt, or to path under the "
                "logs root, one line each: what the Save button's file dialog does; the answer is the path",
                {optionalArg("name", ArgKind::String, "the file name without extension: letters, digits, - and _",
                             "log-<local date and time>"),
                 optionalPath("path", LOGS_ROOT, "a file under the logs root, instead of name (both are refused)",
                              "<name>.txt under the logs root")},
                [this](const ControlArgs& args, IFormatWriter& out) { return save(args, out); });
            control->registerQuery(
                "logView", "editorLog",
                "the log panel's filters (levels, category, search), follow, row counts, the selected row's detail "
                "and the shown rows' cells (time, level, category, message)",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    u32 shownLevels = 0;
                    for (u32 i = 0; i < LEVEL_NAMES.size(); ++i)
                    {
                        shownLevels += (levels >> i) & 1;
                    }
                    out.beginArray("levels", shownLevels);
                    for (u32 i = 0; i < LEVEL_NAMES.size(); ++i)
                    {
                        if ((levels >> i) & 1)
                        {
                            out.writeString("", LEVEL_NAMES[i]);
                        }
                    }
                    out.endArray();
                    out.writeString("category", category);
                    out.beginArray("categories", static_cast<u32>(categories.size()));
                    for (const std::string& name : categories)
                    {
                        out.writeString("", name);
                    }
                    out.endArray();
                    out.writeBool("follow", follow);
                    out.writeU64("rows", matches.size());
                    out.writeU64("admitted", admitted.size());
                    out.writeU64("listRows", list->table().rowCount());
                    out.writeU64("recorded", history ? history->recorded() : 0);
                    out.writeU64("clearedBefore", floor);
                    out.writeBool("atEnd", list->table().atEnd());
                    out.writeBool("detailWanted", detailWanted);
                    out.writeBool("detailShown", detailView.isVisible());
                    out.writeString("detail", detailView.isVisible() ? detailText.text() : std::string_view{});
                    out.writeU64("rebuilds", rebuilds);
                    list->writeView(out);
                    return std::string{};
                });
        }
    };

    EditorLog::EditorLog(Engine& engine, EditorFeedback& feedback, EditorDialogs& dialogs) :
        m_state(std::make_unique<State>(engine, feedback, dialogs))
    {
        State& state = *m_state;
        if (engine.services().contains<CaptureStore>())
        {
            state.control->setRoot(LOGS_ROOT,
                                   engine.services().get<CaptureStore>().artifactsRoot() / machineFolderName() / "logs",
                                   "the log panel's saved logs: logSave");
        }
        UiScope& scope = state.scope;
        state.loadLevelTexts();
        state.root = scope.createPanel("editor.panel", columnDesc());

        for (u32 i = 0; i < state.levelBoxes.size(); ++i)
        {
            state.levelBoxes[i] =
                scope.createCheckbox("log.level", "", barDesc(Vec2{}), ((DEFAULT_LEVELS >> i) & 1) != 0);
            localized(state.levelBoxes[i].label(), "editor-log.level." + std::string(LEVEL_NAMES[i]));
            state.levelBoxes[i].onChange(
                [&state, i](const bool checked)
                {
                    state.pending.push_back(Json{{"command", "logLevel"},
                                                 {"args", {{"level", std::string(LEVEL_NAMES[i])}, {"shown", checked}}}}
                                                .dump());
                });
        }
        UiRectDesc categoryDesc = barDesc(Vec2{});
        categoryDesc.contentSize =
            UiContentSizeComponent{.fitWidth = true, .fitHeight = false, .padding = Vec2{10.0f, 0.0f}};
        categoryDesc.layoutItem = UiLayoutItemComponent{.shrink = 0.0f, .maxSize = Vec2{CATEGORY_BUTTON_MAX, 0.0f}};
        state.categoryButton =
            state.density.track(scope.createButton("log.category", "", categoryDesc), HEIGHT_CONTROL);
        localized(state.categoryButton, "editor-log.category.none");
        state.categoryList =
            scope.createPopupList("log.category.list", UiDropdownDesc{.maxRows = 12, .searchable = true});
        state.categoryList.setAccessibleNameKey(InternedString("editor-log.category.list"));
        state.categoryList.onChoose(
            [&state](const u32 index)
            {
                const std::string name =
                    index == 0 || index > state.categoryNames.size() ? std::string{} : state.categoryNames[index - 1];
                state.pending.push_back(Json{{"command", "logCategory"}, {"args", {{"category", name}}}}.dump());
            });
        const auto openCategories = [&state]
        {
            if (state.categoriesDirty)
            {
                state.categoriesDirty = false;
                state.categoryNames.assign(state.categories.begin(), state.categories.end());
                std::vector<UiChoiceItem> items;
                items.push_back({.key = InternedString("editor-log.category.all")});
                for (const std::string& name : state.categoryNames)
                {
                    items.push_back({.id = InternedString(name), .label = name});
                }
                state.categoryList.setItems(items);
            }
            const auto at = std::ranges::find(state.categoryNames, state.category);
            state.categoryList.setSelected(state.category.empty() || at == state.categoryNames.end()
                                               ? 0
                                               : static_cast<i32>(at - state.categoryNames.begin()) + 1);
            state.categoryList.open(state.categoryButton);
        };
        state.categoryButton.onPress(
            [openCategories](const u8 button, Vec2)
            {
                if (button == 0)
                {
                    openCategories();
                }
            });
        state.categoryButton.onKeyDown(
            [openCategories](const u32 scancode, u8, const bool repeat)
            {
                if ((scancode == RETURN_KEY || scancode == SPACE_KEY) && !repeat)
                {
                    openCategories();
                    return true;
                }
                return false;
            });
        {
            const Density now = Density::read(scope);
            constexpr std::array<std::string_view, 3> KEYS{"info", "warn", "err"};
            for (u32 i = 0; i < state.tagChips.size(); ++i)
            {
                UiRectDesc chipDesc = fittedText(now.row, now.sm);
                state.tagChips[i] = state.density.track(scope.createButton("log.tag.chip", "", chipDesc), HEIGHT_ROW);
                bindFitPadding(state.tagChips[i], SPACE_SM, SPACE_NONE);
                state.tagChips[i].setLocalizedText(InternedString("editor-log.tag." + std::string(KEYS[i])));
                state.tagChips[i].onClick(
                    [&state, i]
                    {
                        const bool on = ((state.levels >> (i + 1)) & 1) != 0;
                        state.pending.push_back(
                            Json{{"command", "logLevel"},
                                 {"args", {{"level", std::string(LEVEL_NAMES[i + 1])}, {"shown", !on}}}}
                                .dump());
                    });
            }
            state.syncTag();
        }
        state.followBox = scope.createCheckbox("log.follow", "", barDesc(Vec2{}), true);
        localized(state.followBox.label(), "editor-log.follow");
        state.followBox.onChange(
            [&state](const bool checked)
            { state.pending.push_back(Json{{"command", "logFollow"}, {"args", {{"follow", checked}}}}.dump()); });

        constexpr std::array<std::string_view, 3> ACTION_KEYS{"clear", "save", "copy"};
        constexpr std::array<std::string_view, 3> ACTION_COMMANDS{"logClear", "logSave", "logCopy"};
        for (u32 i = 0; i < state.actions.size(); ++i)
        {
            state.actions[i] = state.density.track(
                scope.createButton("log.action", "", barDesc(Vec2{ACTION_WIDTH, 0.0f})), HEIGHT_CONTROL);
            localized(state.actions[i], "editor-log." + std::string(ACTION_KEYS[i]));
            state.actions[i].onClick(
                [&state, command = ACTION_COMMANDS[i]]
                {
                    if (command == "logSave")
                    {
                        state.askSave();
                    }
                    else
                    {
                        state.pending.push_back(Json{{"command", std::string(command)}}.dump());
                    }
                });
        }
        state.actions[2].setInteractable(false); // Copy: until rows are selected

        state.detailView = scope.createScrollView("log.detail.view", barDesc(Vec2{0.0f, DETAIL_HEIGHT}));
        state.detailView.setVisible(false);
        UiRectDesc detailTextDesc = topLeft();
        detailTextDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        state.detailText = scope.createLabel("log.detail.text", "", detailTextDesc);
        state.detailText.setParent(state.detailView.content());
        state.detailText.setStyleClass(InternedString("log.detail"));

        FilteredTableDesc desc{
            .id = "log",
            .owner = "editorLog",
            .nameKey = InternedString("editor-log.table"),
            .schema = &LogCells::spiteSchema(),
            .columns = {column("time", TIME_WIDTH), column("level", LEVEL_WIDTH), column("category", CATEGORY_WIDTH),
                        column("message", MESSAGE_WIDTH)},
            .row = [&state](const u32 item) -> const void*
            {
                state.fill(item);
                return &state.cells;
            },
            .key = [&state](const u32 item)
            { return item < state.matches.size() ? State::keyOf(state.matches[item]) : Entity(0u, 1u); }};
        desc.refilter = [&state](const std::string_view typed) { state.setText(typed); };
        desc.shows = [&state](const Entity key) { return state.shownIndex(key).has_value(); };
        state.list = std::make_unique<FilteredTable>(scope, *state.control, std::move(desc));
        state.list->table().onSelect([&state](std::span<const Entity>) { state.detailDirty = true; });
        state.registerCommands();
        scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(LOG_FILTER), [&state](const Widget& slot) { state.list->fillFilter(slot); });
        contract.slot(InternedString(LOG_ACTIONS),
                      [&state](const Widget& slot)
                      {
                          for (Button& action : state.actions)
                          {
                              putIn(state.scope, slot, action);
                              action.setStyleClass(slotFields(state.scope, slot).childClass);
                          }
                      });
        contract.slot(InternedString(LOG_BAR),
                      [&state](const Widget& slot)
                      {
                          const InternedString base = slotFields(state.scope, slot).childClass;
                          for (Checkbox& box : state.levelBoxes)
                          {
                              putIn(state.scope, slot, box);
                              box.setStyleClass(base);
                          }
                          putIn(state.scope, slot, state.categoryButton);
                          state.categoryButton.setStyleClass(classPart(base, ".button"));
                          putIn(state.scope, slot, state.followBox);
                          state.followBox.setStyleClass(base);
                      });
        contract.slot(InternedString(LOG_SUMMARY),
                      [&state](const Widget& slot)
                      {
                          for (Button& chip : state.tagChips)
                              putIn(state.scope, slot, chip);
                      });
        contract.slot(InternedString(LOG_ROWS), [&state](const Widget& slot) { state.list->fillRows(slot); });
        contract.slot(InternedString(LOG_DETAIL),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.detailView);
                          const UiSlotComponent& fields = slotFields(state.scope, slot);
                          state.detailView.setScrollEnabled(fields.scroll);
                          state.detailText.setStyleClass(fields.childClass.isValid() ? fields.childClass
                                                                                     : InternedString("log.detail"));
                      });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                Panel toolbar = toolbarPanel(viewScope, LOG_TOOLBAR, root);
                filterSlotPanel(viewScope, LOG_FILTER, toolbar);
                const Density density = Density::read(viewScope);
                Panel actions =
                    viewScope.createPanel(InternedString(LOG_ACTIONS),
                                          barDesc(Vec2{3.0f * ACTION_WIDTH + 2.0f * density.xs, density.control},
                                                  stack(UiLayoutDirection::Row, density.xs)));
                bindGap(actions, SPACE_XS);
                actions.setParent(toolbar);
                wrapRowPanel(viewScope, LOG_SUMMARY, root);
                wrapRowPanel(viewScope, LOG_BAR, root);
                fillSlotPanel(viewScope, LOG_ROWS, root);
                UiRectDesc detail = topLeft();
                detail.layout = stack(UiLayoutDirection::Column, 0.0f, {}, UiLayoutAlign::Stretch);
                detail.layoutItem = fixedItem();
                detail.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                Panel detailPanel = viewScope.createPanel(InternedString(LOG_DETAIL), detail);
                detailPanel.setParent(root);
            });
        state.view = openView(engine, LOG_VIEW, "assets/ui/editor/log.json", std::move(contract), state.root);
        state.setFollow(true);
    }

    EditorLog::~EditorLog() { m_state->control->unregisterOwner("editorLog"); }

    Widget EditorLog::content() const { return m_state->root; }


    void EditorLog::update()
    {
        State& state = *m_state;
        if (!state.pending.empty())
        {
            dispatch(*state.control, state.pending, "log");
        }
        state.list->update();
        state.sync();
        state.syncTag();
        if (!shown(state.scope, state.root))
        {
            return;
        }
        if (state.localization && state.localization->revision() != state.seenLocale)
        {
            state.loadLevelTexts();
            state.dirty = true;
        }
        state.present();
        state.trackScroll();
        state.fitMessageColumn();
        if (state.detailDirty)
        {
            state.showDetail();
        }
        state.layoutDetail();
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "log",
             .category = "debug",
             .order = 70,
             .dock = {.target = "project"},
             .zone = "EditorLog",
             .make = [](EditorPanelContext& c)
             { return makeEditorPanel<EditorLog>(c.engine, c.feedback, c.dialogs); }}};
    } // namespace
} // namespace spite
