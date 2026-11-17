#include "EditorProjectSettings.hpp"
#include "EditorContext.hpp"
#include "EditorDialogs.hpp"
#include "EditorEntityLabel.hpp"
#include "EditorFeedback.hpp"
#include "EditorPanelList.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorPanelUi.hpp"
#include "ProjectSettingsModel.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "serialization/FormatWalk.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiForm.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        using namespace projectsettings;
        using namespace panelui;

        constexpr std::string_view VIEW = "editor.projectSettings";
        constexpr std::string_view HEADER_SLOT = "projectSettings.header";
        constexpr std::string_view ACTIONS_SLOT = "projectSettings.actions";
        constexpr std::string_view ROWS_SLOT = "projectSettings.rows";
        constexpr std::string_view STATUS_SLOT = "projectSettings.status";
        constexpr std::string_view TOOLBAR = "projectSettings.toolbar";
        constexpr std::string_view FILTER_SLOT = "projectSettings.filter";
        constexpr std::string_view RESULTS_SLOT = "projectSettings.results";
        constexpr std::string_view CHANGES = "changes"; // the dropdown's last item: the diff view
        constexpr std::string_view GENERAL = "general"; // the group id of a section's own fields
        constexpr std::string_view OWNER = "editorProjectSettings";
        constexpr std::string_view TABLE = "editor-projectSettings.";
        constexpr float BUTTON_PADDING = 8.0f;
        constexpr float PAGE_CHOICE_WIDTH = 180.0f;
        constexpr u32 COMMAND_REFRESH_FRAMES = 20; // a busy session reads the config this often at most
        constexpr float PAGE_COLUMN_WIDTH = 90.0f; // the cross-page table's least column widths
        constexpr float FIELD_COLUMN_WIDTH = 220.0f;
        constexpr float VALUE_COLUMN_WIDTH = 110.0f;

        UiTableColumn column(const std::string_view field, const float width, const bool activates = false)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString("editor-projectSettings.column." + std::string(field)),
                    .activates = activates,
                    .sortable = false};
        }

        std::string groupKey(const std::string_view section, const std::string_view group)
        {
            return std::string(section) + "|" + std::string(group);
        }

        UiRectDesc fitButtonDesc()
        {
            UiRectDesc desc = barDesc(Vec2{});
            desc.contentSize =
                UiContentSizeComponent{.fitWidth = true, .fitHeight = false, .padding = Vec2{BUTTON_PADDING, 0.0f}};
            return desc;
        }

        // A line of text as wide as its container and as tall as its text.
        UiRectDesc textLineDesc()
        {
            UiRectDesc desc = barDesc(Vec2{});
            desc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
            return desc;
        }
    } // namespace

    struct EditorProjectSettings::State
    {
        // One property form of a page: the section's own fields, or the fields of one nested struct.
        struct GroupForm
        {
            std::string path;
            Panel bar; // the heading's row: the fold button and the title
            Button toggle;
            Label heading;
            Label doc;
            bool shownFolded = false; // what the widgets show
            std::unique_ptr<PropertyForm> form;
            std::vector<sizet> rowAt; // the section's row of each field, in order
            std::vector<InternedString> ids; // the field ids, likewise
            std::vector<Label> sources; // the source badge of each field
            std::vector<std::string> shownSource;
        };

        struct Page
        {
            std::string id;
            std::string stem;
            std::string signature;
            std::string language; // the locale the headings and names were written for
            Panel box;
            Label doc;
            Label note;
            Label values;
            std::vector<GroupForm> groups;
        };

        State(Engine& engine, EditorDialogs& dialogs, EditorFeedback& feedback) :
            engine(&engine), control(&engine.control()), dialogs(&dialogs), feedback(&feedback),
            localization(&engine.localization()), scope(editorUi(engine).ui())
        {
        }

        Engine* engine;
        ControlServer* control;
        EditorDialogs* dialogs;
        EditorFeedback* feedback;
        const Localization* localization;
        UiScope scope;
        DensityFollower density{scope};
        Panel root;
        Dropdown pageChoice;
        Button save;
        Button resetPage;
        Label status;
        ScrollView scroller;
        Panel pageHost; // the pages, as tall as the shown one
        std::vector<std::unique_ptr<Page>> pages;
        std::vector<std::string> pageIds; // the dropdown's items: the pages, then CHANGES
        std::vector<std::function<void()>> pending;
        std::string selected;
        Section current; // the shown page's section as last read
        InternedString childClass; // the rows slot's: restyles the page's text
        InternedString barClass; // the actions slot's: restyles the buttons and the status
        std::unique_ptr<FilteredTable> table; // the search box and the cross-page table
        std::vector<ListEntry> entries; // the table's items: every field of every page, or the changed ones
        Widget rowsSlot; // holds the pages
        Widget resultsSlot; // holds the table; shown in place of the pages while a search or the diff lists fields
        std::set<std::string> folded; // groupKey of the groups folded in this panel (not persisted)
        bool changes = false; // the diff view is the page
        bool listing = false; // the table is shown
        std::string entriesKey; // the locale and mode the table's items were built for
        u64 seenCommand = 0;
        u64 seenLocale = ~0ull;
        u32 sinceRefresh = 0;
        bool dirty = true; // a refresh is due now: this panel's own command ran, or it was hidden
        bool stale = false; // another client's command ran: a refresh is due within COMMAND_REFRESH_FRAMES
        u64 refreshes = 0;
        bool unavailable = false;
        bool unsynced = false; // a refused edit left its typed text in a row
        std::unique_ptr<UiDocumentView> doc; // last: destroyed first

        [[nodiscard]] std::string message(const std::string_view key, const std::span<const MessageArg> args = {}) const
        {
            std::string scratch;
            return std::string(
                localization->resolve(InternedString(std::string(TABLE) + std::string(key)), args, scratch));
        }

        [[nodiscard]] std::string pageTitle(const std::string_view id) const
        {
            return projectsettings::pageTitle(*localization, id);
        }

        // The panel's own text for a schema description (the schema keeps the English the control protocol shows);
        // a section a game module registers has none and shows the schema's.
        [[nodiscard]] std::string schemaText(const std::string& key, const std::string& schema) const
        {
            return localization->contains(InternedString(std::string(TABLE) + key)) ? message(key) : schema;
        }

        // Runs a command now; the error ("" when accepted) and its result.
        std::string run(const std::string_view command, const Json& args, Json* result = nullptr)
        {
            const u64 before = control->commandGeneration();
            const Json reply = request(*control, Json{{"command", command}, {"args", args}});
            if (seenCommand == before) // the panel's own command: the refresh it asks for is not a stale one
            {
                seenCommand = control->commandGeneration();
            }
            if (reply.value("ok", false))
            {
                if (result && reply.contains("result"))
                {
                    *result = reply["result"];
                }
                return {};
            }
            return reply.contains("error") && reply["error"].is_string() ? reply["error"].get<std::string>()
                                                                         : std::string("refused");
        }

        void toast(const ToastLevel level, const std::string_view key, const std::span<const MessageArg> args = {})
        {
            feedback->postToastKey(level, std::string(TABLE) + std::string(key), args);
        }

        bool read(const std::string_view id, std::vector<Section>& out) const
        {
            return readSections(*control, id, out);
        }

        [[nodiscard]] Page* pageOf(const std::string_view id) const
        {
            const auto it = std::ranges::find(pages, id, [](const std::unique_ptr<Page>& page) { return page->id; });
            return it == pages.end() ? nullptr : it->get();
        }

        [[nodiscard]] PropertyForm* formOf(const std::string_view section, const std::string_view group) const
        {
            if (Page* page = pageOf(section))
            {
                const auto it = std::ranges::find(page->groups, group, &GroupForm::path);
                return it == page->groups.end() ? nullptr : it->form.get();
            }
            return nullptr;
        }

        [[nodiscard]] bool anyEditing() const
        {
            const Page* page = pageOf(selected);
            return page &&
                std::ranges::any_of(page->groups,
                                    [](const GroupForm& group)
                                    {
                                        return std::ranges::any_of(group.ids, [&](const InternedString id)
                                                                   { return group.form->isEditing(id); });
                                    });
        }

        // -- text styles: the slot's child class restyles what the code built --

        [[nodiscard]] InternedString part(const std::string_view suffix, const std::string_view shipped) const
        {
            return childClass.isValid() ? classPart(childClass, suffix)
                                        : (shipped.empty() ? InternedString{} : InternedString(shipped));
        }

        void restyle()
        {
            for (const std::unique_ptr<Page>& page : pages)
            {
                for (Label* label : {&page->doc, &page->note, &page->values})
                {
                    label->setStyleClass(part(".label", "settings.noteWrap"));
                }
                for (GroupForm& group : page->groups)
                {
                    if (group.heading.isValid())
                    {
                        group.heading.setStyleClass(part(".label", "settings.heading"));
                        group.toggle.setStyleClass(part(".toggle", ""));
                    }
                    if (group.doc.isValid())
                    {
                        group.doc.setStyleClass(part(".label", "settings.note"));
                    }
                    for (Label& source : group.sources)
                    {
                        source.setStyleClass(part(".label", "settings.source"));
                    }
                }
            }
            const auto bar = [this](const std::string_view suffix, const std::string_view shipped)
            {
                return barClass.isValid() ? classPart(barClass, suffix)
                                          : (shipped.empty() ? InternedString{} : InternedString(shipped));
            };
            for (Button* button : {&save, &resetPage})
            {
                button->setStyleClass(bar(".toggle", ""));
            }
            status.setStyleClass(bar(".label", "settings.note"));
        }

        void fillRows(const Widget& slot)
        {
            rowsSlot = slot;
            rowsSlot.setVisible(!listing);
            putIn(scope, slot, scroller);
            scroller.setScrollEnabled(slotFields(scope, slot).scroll);
            childClass = slotFields(scope, slot).childClass;
            restyle();
        }

        // The page is as tall as its sections; the scroll content is that or the viewport. Writes only on a change.
        void fitPage()
        {
            const Vec2 view = resolvedSize(scope, scroller);
            if (view.x <= 0.0f || view.y <= 0.0f)
            {
                return;
            }
            const float height = resolvedSize(scope, pageHost).y + 2.0f * density.density().xs;
            const Vec2 want{view.x, std::max(view.y, height)};
            if (scroller.contentSize() != want)
            {
                scroller.setContentSize(want);
            }
        }

        // -- building a page --

        Label textLine(const std::string& id, const std::string_view shipped, const Widget& parent)
        {
            Label label = scope.createLabel(InternedString(id), "", textLineDesc());
            label.setStyleClass(part(".label", shipped));
            label.setParent(parent);
            return label;
        }

        Page& ensurePage(const Section& section)
        {
            if (Page* page = pageOf(section.id))
            {
                return *page;
            }
            auto page = std::make_unique<Page>();
            page->id = section.id;
            page->stem = std::string(stemOf(section.id));
            UiRectDesc boxDesc =
                barDesc(Vec2{}, stack(UiLayoutDirection::Column, density.density().md, {}, UiLayoutAlign::Stretch));
            boxDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
            page->box = scope.createPanel(InternedString("ps." + page->stem), boxDesc);
            bindGap(page->box, SPACE_MD);
            page->box.setParent(pageHost);
            page->doc = textLine("ps." + page->stem + ".doc", "settings.noteWrap", page->box);
            page->note = textLine("ps." + page->stem + ".note", "settings.noteWrap", page->box);
            page->values = textLine("ps." + page->stem + ".values", "settings.noteWrap", page->box);
            pages.push_back(std::move(page));
            return *pages.back();
        }

        void destroyForms(Page& page)
        {
            for (GroupForm& group : page.groups)
            {
                group.form.reset();
                // the badges go with the form's rows
                for (const Widget widget :
                     {Widget(group.toggle), Widget(group.heading), Widget(group.doc), Widget(group.bar)})
                {
                    if (widget.isValid())
                    {
                        scope.destroy(widget);
                    }
                }
            }
            page.groups.clear();
            scope.commit(); // the old widgets die now, so a rebuilt page never shows two of each
        }

        [[nodiscard]] std::string groupTitle(const Page& page, const std::string& path, const bool onlyGroup) const
        {
            if (path.empty())
            {
                return onlyGroup ? std::string{} : message("general");
            }
            const std::string key = "group." + page.stem + "." + path;
            return localization->contains(InternedString(std::string(TABLE) + key)) ? message(key)
                                                                                    : identifierWords(path);
        }

        [[nodiscard]] std::string toggleName(const Page& page, const std::string& path, const bool folded) const
        {
            const std::string title = groupTitle(page, path, false);
            const MessageArg args[]{{"group", std::string_view(title)}};
            return message(folded ? "group.expand" : "group.collapse", args);
        }

        // The widgets of every group follow the panel's folded set; they are written when it differs.
        void applyFolds(Page& page)
        {
            for (GroupForm& group : page.groups)
            {
                const bool want = group.heading.isValid() && folded.contains(groupKey(page.id, group.path));
                if (group.heading.isValid() && group.shownFolded != want)
                {
                    group.shownFolded = want;
                    group.form->root().setVisible(!want);
                    if (group.doc.isValid())
                    {
                        group.doc.setVisible(!want);
                    }
                    group.toggle.setIcon(want ? "caret-right" : "caret-down");
                    group.toggle.setAccessibleName(toggleName(page, group.path, want));
                }
            }
        }

        void rebuild(Page& page, const Section& section)
        {
            destroyForms(page);
            page.signature = signatureOf(section);
            page.language = localization->locale();
            page.doc.setText(schemaText("doc." + page.stem, section.doc));
            if (section.listed())
            {
                page.note.setText(schemaText("reason." + page.stem, section.reason));
            }
            const std::vector<std::string> order = groupPaths(section);
            page.groups.reserve(order.size());
            for (const std::string& path : order)
            {
                GroupForm group;
                group.path = path;
                UiFormDesc desc;
                desc.rect = formRect();
                for (sizet i = 0; i < section.rows.size(); ++i)
                {
                    if (section.rows[i].group == path)
                    {
                        group.rowAt.push_back(i);
                        group.ids.emplace_back(section.rows[i].leaf);
                        desc.fields.push_back(fieldOf(section.rows[i]));
                    }
                }
                const std::string title = groupTitle(page, path, order.size() == 1);
                const std::string formId = "ps." + page.stem + "." + (path.empty() ? std::string("general") : path);
                if (!title.empty())
                {
                    UiRectDesc headDesc =
                        barDesc(Vec2{}, stack(UiLayoutDirection::Row, density.density().xs, {}, UiLayoutAlign::Center));
                    headDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                    group.bar = scope.createPanel(InternedString(formId + ".bar"), headDesc);
                    bindGap(group.bar, SPACE_XS);
                    group.bar.setParent(page.box);
                    group.toggle = scope.createButton(InternedString(formId + ".toggle"), "", barDesc(Vec2{}));
                    group.toggle.bindLayoutToken(UiLayoutField::MinWidth, InternedString(SIZE_CONTROL));
                    group.toggle.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SIZE_CONTROL));
                    group.toggle.setIcon("caret-down");
                    group.toggle.setAccessibleName(toggleName(page, path, false));
                    group.toggle.setParent(group.bar);
                    group.toggle.onClick(
                        [this, id = section.id, path]
                        {
                            pending.push_back(
                                [this, id, path]
                                {
                                    run("projectSettingsGroup",
                                        {{"page", id},
                                         {"group", path.empty() ? "general" : path},
                                         {"collapsed", !folded.contains(groupKey(id, path))}});
                                });
                        });
                    group.heading = textLine(formId + ".heading", "settings.heading", group.bar);
                    group.heading.setLayoutItem(fillItem());
                    group.heading.setText(title);
                    const auto doc =
                        std::ranges::find(section.groups, path, &std::pair<std::string, std::string>::first);
                    if (doc != section.groups.end() && !doc->second.empty())
                    {
                        group.doc = textLine(formId + ".doc", "settings.note", page.box);
                        group.doc.setText(schemaText("doc." + page.stem + "." + path, doc->second));
                    }
                }
                group.form = std::make_unique<PropertyForm>(scope, InternedString(formId), std::move(desc));
                group.form->root().setParent(page.box);
                group.form->onEdit([this, id = section.id, path](const UiFormEdit& edit) { edited(id, path, edit); });
                for (const sizet at : group.rowAt)
                {
                    // beside the Restart badge under the label: a row reads in two lines, not three
                    Label badge = textLine(formId + "." + section.rows[at].leaf + ".source", "settings.source",
                                           group.form->badges(InternedString(section.rows[at].leaf)));
                    UiLayoutItemComponent item;
                    item.shrink = 0.0f;
                    badge.setLayoutItem(item);
                    scope.world().getComponent<UiContentSizeComponent>(badge.entity()).fitWidth = true;
                    group.sources.push_back(badge);
                    group.shownSource.emplace_back();
                }
                page.groups.push_back(std::move(group));
            }
            restyle();
        }

        // -- edits: every one is a config command --

        void edited(const std::string& section, const std::string& group, const UiFormEdit& edit)
        {
            if (current.id != section)
            {
                return;
            }
            const std::string path =
                group.empty() ? std::string(edit.field.toView()) : group + "." + std::string(edit.field.toView());
            const auto row = std::ranges::find(current.rows, path, &Row::path);
            if (row == current.rows.end())
            {
                return;
            }
            const bool reset = edit.reset;
            const Json value = reset ? Json() : toJson(*row, edit.value);
            pending.push_back(
                [this, section, group, field = edit.field, path, reset, value]
                {
                    const std::string error = reset
                        ? run("resetConfig", {{"section", section}, {"path", path}})
                        : run("setConfig", {{"section", section}, {"path", path}, {"value", value}});
                    if (PropertyForm* form = formOf(section, group))
                    {
                        form->setError(field, error);
                    }
                    unsynced = unsynced || !error.empty();
                    dirty = true; // the rows follow what is staged, also after a refusal
                });
        }

        void follow(Page& page, const Section& section)
        {
            for (GroupForm& group : page.groups)
            {
                for (sizet i = 0; i < group.rowAt.size(); ++i)
                {
                    const Row& row = section.rows[group.rowAt[i]];
                    const InternedString id{row.leaf};
                    if (!group.form->isEditing(id))
                    {
                        if (const UiFormValue next = toForm(row, row.value); group.form->value(id) != next)
                        {
                            group.form->setValue(id, next);
                        }
                    }
                    const std::string key = row.unsaved() ? "unsaved" : row.source.empty() ? "default" : row.source;
                    if (group.shownSource[i] != key)
                    {
                        group.shownSource[i] = key;
                        group.sources[i].setLocalizedText(InternedString(std::string(TABLE) + "source." + key));
                    }
                }
            }
        }

        // The page's text lines that follow the section: whether the doc shows, why a section has no fields, its
        // values.
        void describePage(Page& page, const Section& section)
        {
            page.doc.setVisible(!section.doc.empty());
            const bool noFields = section.rows.empty();
            page.note.setVisible(noFields);
            if (!section.listed() && noFields)
            {
                page.note.setLocalizedText(InternedString(std::string(TABLE) + "note.noFields"));
            }
            const bool showValues = noFields && !section.listed() && !section.values.empty();
            page.values.setVisible(showValues);
            if (showValues)
            {
                page.values.setText(section.values.dump());
            }
        }

        [[nodiscard]] std::string_view statusKey(const Section& section) const
        {
            if (section.listed())
            {
                return "status.listed";
            }
            if (section.layer == "memory")
            {
                return "status.memory";
            }
            const bool restart = section.restartPending();
            return section.dirty ? (restart ? "status.restartUnsaved" : "status.unsaved")
                                 : (restart ? "status.restart" : "status.saved");
        }

        void chrome(const Section& section)
        {
            const std::string_view state =
                listing ? (changes ? "status.changes" : "status.search") : statusKey(section);
            status.setLocalizedText(InternedString(std::string(TABLE) + std::string(state)));
            status.setLocalizedArg("file", section.file);
            const bool canSave = !listing && !section.listed() && section.layer != "memory";
            if (save.isInteractable() != canSave)
            {
                save.setInteractable(canSave);
            }
            const bool canReset = !listing && section.anyModified();
            if (resetPage.isInteractable() != canReset)
            {
                resetPage.setInteractable(canReset);
            }
        }

        void showUnavailable()
        {
            unavailable = true;
            entriesKey.clear();
            setEntries({});
            status.setLocalizedText(InternedString(std::string(TABLE) + "status.unavailable"));
            for (Widget widget : {static_cast<Widget>(save), static_cast<Widget>(resetPage)})
            {
                widget.setInteractable(false);
            }
            for (const std::unique_ptr<Page>& page : pages)
            {
                page->box.setVisible(false);
            }
        }

        // Every page's section with its rows: the cross-page table needs all of them.
        void setEntries(std::vector<ListEntry> next)
        {
            if (next != entries)
            {
                entries = std::move(next);
                table->setItems(static_cast<u32>(entries.size()));
            }
        }

        // The table replaces the pages while a search text is set or the diff is the page.
        void showMode()
        {
            const bool want = changes || !table->filterText().empty();
            if (want == listing)
            {
                return;
            }
            listing = want;
            dirty = dirty || listing; // the table gets its items now
            rowsSlot.setVisible(!listing);
            resultsSlot.setVisible(listing);
            if (!unavailable && !current.id.empty())
            {
                chrome(current);
            }
        }

        void refresh()
        {
            SPITE_ZONE("EditorProjectSettings.refresh");
            ++refreshes;
            seenCommand = control->commandGeneration();
            seenLocale = localization->revision();
            dirty = stale = false;
            sinceRefresh = 0;
            std::vector<Section> list;
            if (!read({}, list))
            {
                showUnavailable();
                return;
            }
            unavailable = false;
            const std::vector<std::string> ids = orderedIds(list);
            std::vector<std::string> items = ids;
            items.emplace_back(CHANGES);
            if (items != pageIds)
            {
                pageIds = items;
                std::vector<UiChoiceItem> choices;
                for (const std::string& id : items)
                {
                    const std::string key = "page." + std::string(stemOf(id));
                    const InternedString full(std::string(TABLE) + key);
                    choices.push_back({.id = InternedString(id),
                                       .label = id,
                                       .key = localization->contains(full) ? full : InternedString{}});
                }
                pageChoice.setItems(choices);
            }
            if (ids.empty())
            {
                return;
            }
            if (!std::ranges::contains(ids, selected))
            {
                selected = ids.front();
            }
            pageChoice.setSelected(
                static_cast<i32>(std::ranges::find(items, changes ? std::string(CHANGES) : selected) - items.begin()));
            // every page is read only for the table (a search or the diff shows it, or its items are for another
            // locale or mode); the pages alone read the shown one
            const std::string language = localization->locale();
            const std::string key = language + (changes ? "|changes" : "|all");
            const bool wantEntries = listing || entriesKey != key;
            std::vector<Section> sections;
            if (wantEntries)
            {
                sections = readAll(*control, ids);
            }
            else if (std::vector<Section> one; read(selected, one) && !one.empty())
            {
                sections = std::move(one);
            }
            const auto at = std::ranges::find(sections, selected, &Section::id);
            if (at == sections.end())
            {
                return;
            }
            current = *at;
            Page& page = ensurePage(current);
            if (page.signature != signatureOf(current) || page.language != language)
            {
                rebuild(page, current);
            }
            for (const std::unique_ptr<Page>& other : pages)
            {
                other->box.setVisible(other.get() == &page);
            }
            describePage(page, current);
            follow(page, current);
            applyFolds(page);
            if (wantEntries)
            {
                setEntries(entriesOf(*localization, sections, changes));
                entriesKey = key;
            }
            showMode();
            chrome(current);
        }

        // -- the actions --

        void saveNow()
        {
            Json result;
            const std::string error = run("saveConfig", {{"section", current.id}}, &result);
            dirty = true;
            const std::string file = result.value("file", current.file);
            const MessageArg args[]{{"file", std::string_view(file)}};
            if (!error.empty())
            {
                const MessageArg refused[]{{"error", std::string_view(error)}};
                toast(ToastLevel::Error, "toast.error", refused);
                return;
            }
            toast(ToastLevel::Info, result.value("restartRequired", false) ? "toast.savedRestart" : "toast.saved",
                  args);
        }

        void resetNow()
        {
            const std::string title = pageTitle(current.id);
            const MessageArg args[]{{"page", std::string_view(title)}};
            dialogs->confirm({.title = message("reset.title", args),
                              .body = message("reset.body"),
                              .confirmLabel = message("reset.confirm")},
                             [this, id = current.id, title](const bool confirmed)
                             {
                                 if (!confirmed)
                                 {
                                     return;
                                 }
                                 const std::string error = run("resetConfig", {{"section", id}});
                                 const MessageArg done[]{{"page", std::string_view(title)}};
                                 const MessageArg refused[]{{"error", std::string_view(error)}};
                                 error.empty() ? toast(ToastLevel::Info, "toast.reset", done)
                                               : toast(ToastLevel::Error, "toast.error", refused);
                                 dirty = true;
                             });
        }

        // -- control entries --

        // A group has a heading, so it folds, unless it is the only group of its page and has no name.
        [[nodiscard]] static bool foldable(const Section& section, const std::string& path)
        {
            return !path.empty() || groupPaths(section).size() > 1;
        }

        void registerEntries()
        {
            control->registerViewCommand(
                "projectSettingsPage", OWNER,
                "sets the page the Project Settings panel shows: a config section id, or changes for the diff view",
                {requiredArg("page", ArgKind::String, "a config section id, as projectSettingsView's pages list them")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const std::string id = args.string("page").value_or("");
                    std::vector<Section> list;
                    if (!read({}, list))
                    {
                        return std::string("this run has no config store");
                    }
                    const std::vector<std::string> ids = orderedIds(list);
                    if (id != CHANGES && !std::ranges::contains(ids, id))
                    {
                        std::string known;
                        for (const std::string& each : ids)
                        {
                            known += each + ", ";
                        }
                        return "no page '" + id + "'; pages: " + known + std::string(CHANGES);
                    }
                    changes = id == CHANGES;
                    selected = changes ? selected : id;
                    if (!changes && !table->filterText().empty()) // a page replaces the results, as the dropdown does
                    {
                        request(*control, Json{{"command", "projectSettingsFilter"}, {"args", {{"text", ""}}}});
                    }
                    dirty = true;
                    return std::string{};
                });
            control->registerViewCommand(
                "projectSettingsGroup", OWNER,
                "folds or unfolds a group of fields on a Project Settings page; the state stays in the panel",
                {requiredArg("page", ArgKind::String, "a config section id, as projectSettingsView's pages list them"),
                 requiredArg("group", ArgKind::String,
                             "a group id of that page in projectSettingsView's groups (general: the section's own "
                             "fields)"),
                 optionalArg("collapsed", ArgKind::Boolean, "true folds the group, false unfolds it", "toggles it")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const std::string page = args.string("page").value_or("");
                    std::vector<Section> one;
                    if (!read(page, one) || one.empty())
                    {
                        return "no page '" + page + "'; projectSettingsView lists the pages";
                    }
                    const std::string id = args.string("group").value_or("");
                    const std::string path = id == GENERAL ? std::string{} : id;
                    const std::vector<std::string> paths = groupPaths(one.front());
                    if (!std::ranges::contains(paths, path) || !foldable(one.front(), path))
                    {
                        std::string known;
                        for (const std::string& each : paths)
                        {
                            known += (known.empty() ? "" : ", ") + (each.empty() ? std::string(GENERAL) : each);
                        }
                        return "page '" + page + "' has no group '" + id + "' that folds; groups: " + known;
                    }
                    const std::string key = groupKey(page, path);
                    if (args.boolean("collapsed").value_or(!folded.contains(key)))
                    {
                        folded.insert(key);
                    }
                    else
                    {
                        folded.erase(key);
                    }
                    dirty = true;
                    return std::string{};
                });
            control->registerQuery(
                "projectSettingsView", OWNER,
                "the Project Settings panel: its pages (a config section or file each), and for the shown page, or "
                "page=, the status, the groups (folded or not) and per field the value, default, saved value, source "
                "(default, project, user, memory), apply mode, whether it is modified from the default or unsaved, "
                "and the error under it; mode is page or changes (the diff view); while a search text is set or the "
                "diff is shown, results lists the fields across every page and filter the search box",
                {optionalArg("page", ArgKind::String, "a config section id", "the shown page")},
                [this](const ControlArgs& args, IFormatWriter& out)
                {
                    // from the config query, not the forms: a hidden panel builds none
                    std::vector<Section> list;
                    if (!read({}, list))
                    {
                        out.writeBool("available", false);
                        return std::string{};
                    }
                    out.writeBool("available", true);
                    out.writeU64("refreshes", refreshes);
                    const std::vector<std::string> ids = orderedIds(list);
                    std::string page = args.string("page").value_or("");
                    if (page.empty())
                    {
                        page = std::ranges::contains(ids, selected) || ids.empty() ? selected : ids.front();
                    }
                    out.writeString("page", page);
                    out.writeString("mode", changes ? CHANGES : table->filterText().empty() ? "page" : "search");
                    out.beginArray("pages", static_cast<u32>(ids.size()));
                    for (const std::string& id : ids)
                    {
                        const Section& section = *std::ranges::find(list, id, &Section::id);
                        out.beginObject("");
                        out.writeString("id", id);
                        out.writeString("title", pageTitle(id));
                        out.writeString("kind", section.kind);
                        out.writeString("layer", section.layer);
                        out.writeString("file", section.file);
                        out.writeBool("dirty", section.dirty);
                        out.writeBool("pending", section.pending);
                        out.endObject();
                    }
                    out.endArray();
                    std::vector<Section> one;
                    if (!read(page, one) || one.empty())
                    {
                        return "no page '" + page + "'; pages: " + (ids.empty() ? "none" : ids.front());
                    }
                    const Section& section = one.front();
                    out.writeString("status", statusKey(section));
                    out.writeBool("canSave", !section.listed() && section.layer != "memory");
                    const std::vector<std::string> paths = groupPaths(section);
                    out.beginArray("groups", static_cast<u32>(paths.size()));
                    for (const std::string& path : paths)
                    {
                        const bool fold = foldable(section, path);
                        out.beginObject("");
                        out.writeString("id", path.empty() ? std::string(GENERAL) : path);
                        out.writeBool("foldable", fold);
                        out.writeBool("collapsed", fold && folded.contains(groupKey(section.id, path)));
                        out.endObject();
                    }
                    out.endArray();
                    out.beginArray("rows", static_cast<u32>(section.rows.size()));
                    for (const Row& row : section.rows)
                    {
                        out.beginObject("");
                        out.writeString("path", row.path);
                        out.writeString("group", row.group);
                        out.writeString("kind", row.kind);
                        out.writeString("unit", row.unit);
                        writeJson(out, "value", row.value);
                        writeJson(out, "default", row.def);
                        writeJson(out, "saved", row.saved);
                        out.writeString("source",
                                        row.unsaved()            ? "unsaved"
                                            : row.source.empty() ? "default"
                                                                 : row.source);
                        out.writeString("apply", row.apply);
                        out.writeBool("restart", row.apply == "startup");
                        out.writeBool("restartPending", row.restartPending());
                        out.writeBool("modified", row.modified());
                        out.writeBool("unsaved", row.unsaved());
                        out.writeBool("readOnly", row.readOnly);
                        std::string error;
                        if (const PropertyForm* form = formOf(section.id, row.group);
                            form && form->has(InternedString(row.leaf)))
                        {
                            error = form->state(InternedString(row.leaf)).error;
                        }
                        out.writeString("error", error);
                        out.endObject();
                    }
                    out.endArray();
                    const std::string& needle = table->filterText();
                    if (changes || !needle.empty())
                    {
                        const std::vector<ListEntry> found = findEntries(*control, *localization, ids, changes, needle);
                        out.beginArray("results", static_cast<u32>(found.size()));
                        for (const ListEntry& entry : found)
                        {
                            writeEntry(out, entry);
                        }
                        out.endArray();
                    }
                    table->writeView(out);
                    return std::string{};
                });
        }
    };

    EditorProjectSettings::EditorProjectSettings(Engine& engine, EditorDialogs& dialogs, EditorFeedback& feedback) :
        m_state(std::make_unique<State>(engine, dialogs, feedback))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;

        state.root = scope.createPanel("editor.panel", columnDesc());
        UiDropdownDesc choiceDesc;
        choiceDesc.rect = barDesc(Vec2{PAGE_CHOICE_WIDTH, 0.0f});
        state.pageChoice =
            state.density.track(scope.createDropdown("projectSettings.page", choiceDesc), HEIGHT_CONTROL);
        state.pageChoice.setAccessibleNameKey(InternedString("editor-projectSettings.page.access"));
        state.pageChoice.onChange(
            [&state](const u32 index)
            {
                if (index < state.pageIds.size())
                {
                    state.pending.push_back([&state, id = state.pageIds[index]]
                                            { state.run("projectSettingsPage", {{"page", id}}); });
                }
            });
        const auto button = [&](const char* id, const char* key)
        {
            Button out = state.density.track(scope.createButton(id, "", fitButtonDesc()), HEIGHT_BUTTON);
            out.setLocalizedText(InternedString(std::string(TABLE) + key));
            out.setTooltip(InternedString(std::string(TABLE) + key + ".tip"));
            return out;
        };
        state.save = button("projectSettings.save", "save");
        state.save.onClick([&state] { state.pending.push_back([&state] { state.saveNow(); }); });
        state.resetPage = button("projectSettings.reset", "reset");
        state.resetPage.onClick([&state] { state.pending.push_back([&state] { state.resetNow(); }); });
        state.status = scope.createLabel("projectSettings.statusText", "", barDesc(Vec2{}));
        state.status.setLayoutItem(fillItem());
        state.status.bindLayoutToken(UiLayoutField::MinHeight, InternedString(SIZE_ROW));
        state.status.setStyleClass(InternedString("settings.note"));

        UiRectDesc scrollDesc = topLeft();
        scrollDesc.layoutItem = fillItem();
        state.scroller = scope.createScrollView("projectSettings.scroll", scrollDesc);
        Panel content = state.scroller.content();
        state.density.apply(
            [content](const Density& d) mutable
            {
                content.setLayout(stack(UiLayoutDirection::Column, 0.0f, Vec4{d.xs, d.xs, d.xs + THUMB, d.xs},
                                        UiLayoutAlign::Stretch));
            });
        UiRectDesc hostDesc = barDesc(Vec2{}, stack(UiLayoutDirection::Column, 0.0f, {}, UiLayoutAlign::Stretch));
        hostDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        state.pageHost = scope.createPanel("projectSettings.pages", hostDesc);
        state.pageHost.setParent(content);
        state.table = std::make_unique<FilteredTable>(
            scope, *state.control,
            FilteredTableDesc{.id = "projectSettings",
                              .owner = std::string(OWNER),
                              .nameKey = InternedString("editor-projectSettings.table"),
                              .schema = &ListCells::spiteSchema(),
                              .columns = {column("page", PAGE_COLUMN_WIDTH), column("field", FIELD_COLUMN_WIDTH, true),
                                          column("value", VALUE_COLUMN_WIDTH),
                                          column("defaultValue", VALUE_COLUMN_WIDTH),
                                          column("saved", VALUE_COLUMN_WIDTH)},
                              .row = [&state](const u32 item) -> const void* { return &state.entries[item].cells; },
                              .key = [&state](const u32 item) { return Entity(state.entries[item].key, 1u); },
                              .matches = [&state](const u32 item, const std::string_view needle)
                              { return matches(state.entries[item], needle); },
                              .activate =
                                  [&state](const Entity key, const InternedString)
                              {
                                  const auto at = std::ranges::find(state.entries, key.index(), &ListEntry::key);
                                  if (at != state.entries.end())
                                  {
                                      state.pending.push_back([&state, id = at->section]
                                                              { state.run("projectSettingsPage", {{"page", id}}); });
                                  }
                              }});
        state.registerEntries();
        scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(HEADER_SLOT),
                      [&state](const Widget& slot) { putIn(state.scope, slot, state.pageChoice); });
        contract.slot(InternedString(FILTER_SLOT), [&state](const Widget& slot) { state.table->fillFilter(slot); });
        contract.slot(InternedString(RESULTS_SLOT),
                      [&state](const Widget& slot)
                      {
                          state.table->fillRows(slot);
                          state.resultsSlot = slot;
                          state.resultsSlot.setVisible(state.listing);
                      });
        contract.slot(InternedString(ACTIONS_SLOT),
                      [&state](const Widget& slot)
                      {
                          putIn(state.scope, slot, state.save);
                          putIn(state.scope, slot, state.resetPage);
                          state.barClass = slotFields(state.scope, slot).childClass;
                          state.restyle();
                      });
        contract.slot(InternedString(ROWS_SLOT), [&state](const Widget& slot) { state.fillRows(slot); });
        contract.slot(InternedString(STATUS_SLOT),
                      [&state](const Widget& slot) { putIn(state.scope, slot, state.status); });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                columnRoot(root);
                toolbarPanel(viewScope, HEADER_SLOT, root);
                Panel toolbar = toolbarPanel(viewScope, TOOLBAR, root);
                fitBarPanel(viewScope, ACTIONS_SLOT, toolbar);
                filterSlotPanel(viewScope, FILTER_SLOT, toolbar);
                fillSlotPanel(viewScope, ROWS_SLOT, root);
                fillSlotPanel(viewScope, RESULTS_SLOT, root);
                statusPanel(viewScope, STATUS_SLOT, root);
            });
        state.doc = openView(engine, VIEW, "assets/ui/editor/projectSettings.json", std::move(contract), state.root);
    }

    EditorProjectSettings::~EditorProjectSettings() { m_state->control->unregisterOwner(OWNER); }

    Widget EditorProjectSettings::content() const { return m_state->root; }

    void EditorProjectSettings::update()
    {
        State& state = *m_state;
        if (const u64 command = state.control->commandGeneration(); command != state.seenCommand)
        {
            state.seenCommand = command; // another client's command: read after at most COMMAND_REFRESH_FRAMES
            state.stale = true;
        }
        if (!state.pending.empty())
        {
            std::vector<std::function<void()>> actions;
            actions.swap(state.pending);
            for (const std::function<void()>& action : actions)
            {
                action();
            }
        }
        const u64 before = state.control->commandGeneration();
        state.table->update();
        if (state.seenCommand == before) // its own filter and sort commands change nothing a refresh reads
        {
            state.seenCommand = state.control->commandGeneration();
        }
        if (!shown(state.scope, state.root))
        {
            state.dirty = true;
            return;
        }
        state.showMode();
        state.fitPage();
        if (state.unsynced && !state.anyEditing())
        {
            state.unsynced = false;
            state.dirty = true;
        }
        ++state.sinceRefresh;
        if (state.dirty || state.localization->revision() != state.seenLocale ||
            (state.stale && state.sinceRefresh >= COMMAND_REFRESH_FRAMES))
        {
            state.refresh();
        }
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "projectSettings",
             .category = "tools",
             .order = 105,
             .dock = {.target = "project"},
             .zone = "EditorProjectSettings",
             .make = [](EditorPanelContext& c)
             { return makeEditorPanel<EditorProjectSettings>(c.engine, c.dialogs, c.feedback); }}};
    } // namespace
} // namespace spite
