#include "EditorFilePicker.hpp"
#include "EditorPanelList.hpp"
#include "EditorPanelUi.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>

#include "control/ControlServer.hpp"
#include "localization/Localization.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiScope.hpp"

namespace spite
{
    namespace
    {
        using namespace panelui;

        constexpr std::string_view TABLE = "editor-dialogs.";
        constexpr std::string_view SEPARATORS = "/\\";
        constexpr float FIELD_HEIGHT = 26.0f;
        constexpr float LABEL_WIDTH = 52.0f;
        constexpr float CRUMB_MAX_WIDTH = 160.0f;
        constexpr float BUTTON_PAD = 10.0f;
        constexpr u32 MAX_CRUMBS = 6;
        constexpr sizet MAX_RECENT = 8;
        constexpr u32 RETURN_KEY = 40;
        constexpr u32 SPACE_KEY = 44;
        constexpr std::array<std::string_view, 3> MODE_NAMES{"open", "save", "folder"}; // by EditorPickMode
        static_assert(MODE_NAMES.size() == static_cast<sizet>(EditorPickMode::Folder) + 1);

        // What the table shows of an entry (the strings are localized when it is built).
        struct PickRow
        {
            std::string name;
            std::string kind;
            std::string size;
            std::string modified;
            SPITE_OBSERVED_SCHEMA(PickRow, name, kind, size, modified)
        };

        UiTableColumn column(const std::string_view field, const float width, const bool activates = false)
        {
            return {.field = InternedString(field),
                    .width = width,
                    .titleKey = InternedString(std::string(TABLE) + "pick.column." + std::string(field)),
                    .activates = activates};
        }

        std::string message(const Localization* localization, const std::string_view key,
                            const std::initializer_list<MessageArg> args = {})
        {
            const std::string full = std::string(TABLE) + std::string(key);
            if (!localization)
            {
                return full;
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(full),
                                                     std::span<const MessageArg>(args.begin(), args.size()), scratch));
        }

        // The parts of a path between '/' or '\\', without the empty and "." ones.
        std::vector<std::string> splitPath(const std::string_view text)
        {
            std::vector<std::string> parts;
            for (sizet from = 0; from < text.size();)
            {
                sizet to = text.find_first_of(SEPARATORS, from);
                to = to == std::string_view::npos ? text.size() : to;
                if (const std::string_view part = text.substr(from, to - from); !part.empty() && part != ".")
                {
                    parts.emplace_back(part);
                }
                from = to + 1;
            }
            return parts;
        }

        std::string joinPath(const std::vector<std::string>& parts, const sizet from = 0, const sizet to = ~sizet{0})
        {
            std::string text;
            for (sizet i = from; i < std::min(parts.size(), to); ++i)
            {
                text += (text.empty() ? "" : "/") + parts[i];
            }
            return text;
        }

        std::string withRoot(const std::string_view root, const std::string_view relative)
        {
            return relative.empty() ? std::string(root) : std::string(root) + "/" + std::string(relative);
        }

        // The roots a request may choose under: its list narrowed to the roots that are set, else all of them.
        std::vector<std::string> allowedRoots(const ControlServer& control, const EditorPick& request)
        {
            std::vector<std::string> set = control.rootNames();
            if (request.roots.empty())
            {
                return set;
            }
            std::vector<std::string> allowed;
            for (const std::string& root : request.roots)
            {
                if (std::ranges::contains(set, root) && !std::ranges::contains(allowed, root))
                {
                    allowed.push_back(root);
                }
            }
            return allowed;
        }

        // The only extension of a comma list ("glb, .gltf" has two), without its dot; empty when it names none or
        // several.
        std::string onlyExtension(const std::string_view extensions)
        {
            std::string found;
            for (sizet from = 0; from <= extensions.size();)
            {
                sizet to = extensions.find(',', from);
                to = to == std::string_view::npos ? extensions.size() : to;
                std::string_view text = extensions.substr(from, to - from);
                from = to + 1;
                while (!text.empty() && text.front() == ' ')
                {
                    text.remove_prefix(1);
                }
                while (!text.empty() && text.back() == ' ')
                {
                    text.remove_suffix(1);
                }
                if (!text.empty() && text.front() == '.')
                {
                    text.remove_prefix(1);
                }
                if (text.empty())
                {
                    continue;
                }
                if (!found.empty())
                {
                    return {};
                }
                found = text;
            }
            return found;
        }

        bool hasExtension(const std::string_view name)
        {
            const sizet dot = name.rfind('.');
            return dot != std::string_view::npos && dot != 0 && dot + 1 < name.size();
        }

        // What a save name needs for the request's only extension: ".ext", "ext" after a trailing dot ("foo."), or
        // nothing when it has one or the request names none or several.
        std::string missingExtension(const std::string_view name, const std::string_view extensions)
        {
            if (hasExtension(name))
            {
                return {};
            }
            const std::string extension = onlyExtension(extensions);
            return extension.empty() ? extension : (name.ends_with('.') ? "" : ".") + extension;
        }

        // The one validation of a choice: "<root>/<path>" against what the request allows. On success `encoded` is
        // the choice in its one form; otherwise the reason (control's own refusal for a bad path).
        std::string resolveChoice(const ControlServer& control, const Localization* localization,
                                  const EditorPick& request, const std::string_view rooted, std::string& encoded)
        {
            const std::vector<std::string> parts = splitPath(rooted);
            if (parts.empty())
            {
                return message(localization, "pick.error.noPath");
            }
            const std::vector<std::string> allowed = allowedRoots(control, request);
            const std::string& root = parts[0];
            if (!std::ranges::contains(allowed, root))
            {
                std::string names;
                for (const std::string& name : allowed)
                {
                    names += (names.empty() ? "" : ", ") + name;
                }
                return message(localization, "pick.error.root", {{"root", root}, {"roots", names}});
            }
            std::string relative = joinPath(parts, 1);
            std::filesystem::path file;
            std::error_code error;
            if (request.mode == EditorPickMode::Folder)
            {
                if (std::string refusal = control.resolveDirectory(root, relative, file); !refusal.empty())
                {
                    return refusal;
                }
                // a root nothing was written to yet has no directory: choosing it is fine, its writers create it
                if (!std::filesystem::is_directory(file, error) &&
                    !(relative.empty() && !std::filesystem::exists(file, error)))
                {
                    return message(localization, "pick.error.notFolder", {{"path", withRoot(root, relative)}});
                }
                encoded = withRoot(root, relative);
                return {};
            }
            if (relative.empty())
            {
                return message(localization, "pick.error.noName");
            }
            if (request.mode == EditorPickMode::SaveFile)
            {
                relative += missingExtension(parts.back(), request.extensions);
            }
            if (std::string refusal = control.resolveFile(root, relative, file); !refusal.empty())
            {
                return refusal;
            }
            const std::string shown = withRoot(root, relative);
            if (request.mode == EditorPickMode::OpenFile)
            {
                if (!std::filesystem::is_regular_file(file, error))
                {
                    return message(localization, "pick.error.notFile", {{"path", shown}});
                }
                // the UTF-8 name in hand: path::string() throws for a name outside the ANSI code page
                if (!fileNameMatchesExtensions(parts.back(), request.extensions))
                {
                    return message(localization, "pick.error.extension",
                                   {{"path", shown}, {"extensions", request.extensions}});
                }
            }
            else
            {
                if (std::filesystem::is_directory(file, error))
                {
                    return message(localization, "pick.error.isFolder", {{"path", shown}});
                }
                // a file in the root's own directory needs no folder yet: its writer creates the root
                if (relative.find('/') != std::string::npos &&
                    !std::filesystem::is_directory(file.parent_path(), error))
                {
                    return message(localization, "pick.error.noParent", {{"path", shown}});
                }
            }
            encoded = shown;
            return {};
        }
    } // namespace

    struct FilePicker::Impl
    {
        Impl(UiScope& scope, ControlServer& control, const Localization* localization, const Widget& host,
             const Button& confirm, Hooks hooks) :
            scope(&scope), control(&control), localization(localization), hooks(std::move(hooks)), confirm(confirm),
            host(host)
        {
            build();
        }

        UiScope* scope;
        ControlServer* control;
        const Localization* localization;
        Hooks hooks;
        Button confirm;
        Widget host;

        Button up;
        Button places;
        Panel crumbBar;
        std::array<Button, MAX_CRUMBS> crumbs;
        std::array<u32, MAX_CRUMBS> crumbLevel{};
        PopupList placeList;
        Panel filterSlot;
        Panel rowsSlot;
        std::unique_ptr<FilteredTable> table;
        Label nameLabel;
        TextInput name;
        Panel nameRow;
        Label status;

        EditorPick request;
        std::vector<std::string> allowed;
        std::string root;
        std::string dir; // under root, '/' separated
        std::vector<DirectoryEntry> entries;
        bool truncated = false;
        std::vector<PickRow> rows;
        std::vector<std::string> recent; // "<root>[/<dir>]", newest first
        std::vector<std::pair<std::string, std::string>> placeTargets; // list item -> root, dir
        std::string statusText;
        std::optional<std::pair<std::string, std::string>> pendingGo;
        u64 seenLocale = 0;
        bool replaces = false;

        [[nodiscard]] std::string tr(const std::string_view key,
                                     const std::initializer_list<MessageArg> args = {}) const
        {
            return message(localization, key, args);
        }

        void build()
        {
            UiRectDesc desc = barDesc(Vec2{0.0f, FIELD_HEIGHT}, stack(UiLayoutDirection::Row, 4.0f));
            Panel location = scope->createPanel("filePicker.location", desc);
            location.setParent(host);

            const auto fitted = [](const float height)
            {
                UiRectDesc d = barDesc(Vec2{0.0f, height});
                d.contentSize =
                    UiContentSizeComponent{.fitWidth = true, .fitHeight = false, .padding = Vec2{BUTTON_PAD, 0.0f}};
                return d;
            };
            up = scope->createButton("filePicker.up", "", fitted(FIELD_HEIGHT));
            up.setLocalizedText(InternedString(std::string(TABLE) + "pick.up"));
            up.setParent(location);
            up.onClick([this] { goUp(); });
            places = scope->createButton("filePicker.places", "", fitted(FIELD_HEIGHT));
            places.setLocalizedText(InternedString(std::string(TABLE) + "pick.places"));
            places.setParent(location);
            placeList = scope->createPopupList("filePicker.places.list", UiDropdownDesc{.maxRows = 10});
            placeList.setAccessibleNameKey(InternedString(std::string(TABLE) + "pick.places"));
            placeList.onChoose(
                [this](const u32 index)
                {
                    if (index < placeTargets.size() && !placeTargets[index].first.empty())
                    {
                        pendingGo = placeTargets[index];
                    }
                });
            const auto openPlaces = [this]
            {
                rebuildPlaces();
                placeList.open(places);
            };
            places.onPress(
                [openPlaces](const u8 button, Vec2)
                {
                    if (button == 0)
                    {
                        openPlaces();
                    }
                });
            places.onKeyDown(
                [openPlaces](const u32 scancode, u8, const bool repeat)
                {
                    if ((scancode == RETURN_KEY || scancode == SPACE_KEY) && !repeat)
                    {
                        openPlaces();
                        return true;
                    }
                    return false;
                });

            UiRectDesc barDescription = barDesc(Vec2{0.0f, FIELD_HEIGHT}, stack(UiLayoutDirection::Row, 2.0f));
            barDescription.layoutItem = fillItem();
            crumbBar = scope->createPanel("filePicker.crumbs", barDescription);
            crumbBar.setParent(location);
            for (u32 i = 0; i < MAX_CRUMBS; ++i)
            {
                UiRectDesc crumb = fitted(FIELD_HEIGHT);
                crumb.layoutItem = UiLayoutItemComponent{.shrink = 0.0f, .maxSize = Vec2{CRUMB_MAX_WIDTH, 0.0f}};
                crumbs[i] = scope->createButton(InternedString("filePicker.crumb" + std::to_string(i)), "", crumb);
                crumbs[i].setParent(crumbBar);
                crumbs[i].setVisible(false);
                crumbs[i].onClick([this, i]
                                  { pendingGo = std::make_pair(root, joinPath(splitPath(dir), 0, crumbLevel[i])); });
            }

            // the table and its filter bar live in two slots as a panel's document view would give them
            const Density density = Density::read(*scope);
            UiRectDesc filterDesc = barDesc(Vec2{0.0f, density.control}, stack(UiLayoutDirection::Row, density.xs));
            filterSlot = scope->createPanel("filePicker.filter", filterDesc);
            filterSlot.setParent(host);
            scope->world().addComponent<UiSlotComponent>(filterSlot.entity(), UiSlotComponent{});
            rowsSlot = scope->createPanel(
                "filePicker.rows",
                barDesc(Vec2{0.0f, 240.0f}, stack(UiLayoutDirection::Column, 0.0f, {}, UiLayoutAlign::Stretch)));
            rowsSlot.setParent(host);
            scope->world().addComponent<UiSlotComponent>(rowsSlot.entity(), UiSlotComponent{});

            table = std::make_unique<FilteredTable>(
                *scope, *control,
                FilteredTableDesc{.id = "filePicker",
                                  .owner = "editorDialogs",
                                  .nameKey = InternedString(std::string(TABLE) + "pick.table"),
                                  .schema = &PickRow::spiteSchema(),
                                  .columns = {column("name", 280.0f, true), column("kind", 72.0f),
                                              column("size", 80.0f), column("modified", 124.0f)},
                                  .row = [this](const u32 item) -> const void* { return &rows[item]; },
                                  .key = [](const u32 item) { return Entity(item + 1, 1u); },
                                  .matches = [this](const u32 item, const std::string_view needle)
                                  { return containsIgnoringCase(rows[item].name, needle); },
                                  .activate = [this](const Entity key, InternedString) { activate(key); }});
            table->fillFilter(filterSlot);
            table->fillRows(rowsSlot);
            table->table().onSelect([this](const std::span<const Entity> selection) { selected(selection); });

            nameRow = scope->createPanel(
                "filePicker.nameRow",
                barDesc(Vec2{0.0f, FIELD_HEIGHT}, stack(UiLayoutDirection::Row, 6.0f, {}, UiLayoutAlign::Center)));
            nameRow.setParent(host);
            nameLabel = scope->createLabel("filePicker.nameLabel", "", barDesc(Vec2{LABEL_WIDTH, FIELD_HEIGHT}));
            nameLabel.setLocalizedText(InternedString(std::string(TABLE) + "pick.name"));
            nameLabel.setParent(nameRow);
            UiRectDesc nameDesc = barDesc(Vec2{0.0f, FIELD_HEIGHT});
            nameDesc.layoutItem = fillItem();
            name = scope->createTextInput("filePicker.name", nameDesc);
            name.setAccessibleNameKey(InternedString(std::string(TABLE) + "pick.name"));
            name.setParent(nameRow);
            name.onChange([this](std::string_view) { nameChanged(); });
            name.onSubmit([this](std::string_view) { hooks.confirm(); });
            name.onCancel([this] { hooks.cancel(); });

            UiRectDesc statusDesc = barDesc(Vec2{0.0f, 0.0f});
            UiContentSizeComponent fitHeight;
            fitHeight.fitWidth = false;
            statusDesc.contentSize = fitHeight;
            status = scope->createLabel("filePicker.status", "", statusDesc);
            status.setStyleClass(InternedString("dialog.body"));
            status.setParent(host);
            status.setVisible(false);
        }

        // -- location ---------------------------------------------------------------------------------------------

        void setStatus(const std::string& text)
        {
            if (text != statusText)
            {
                statusText = text;
                status.setText(text);
                status.setVisible(!text.empty());
            }
        }

        // Lists root/relative; the refusal, with the location unchanged.
        std::string list(const std::string& newRoot, const std::string& relative)
        {
            DirectoryListing listing;
            if (std::string error = control->listDirectory(withRoot(newRoot, relative), request.extensions,
                                                           request.mode == EditorPickMode::Folder, listing);
                !error.empty())
            {
                return error;
            }
            root = listing.root;
            dir = listing.path.size() > root.size() ? listing.path.substr(root.size() + 1) : std::string{};
            entries = std::move(listing.entries);
            truncated = listing.truncated;
            return {};
        }

        void go(const std::string& newRoot, const std::string& relative)
        {
            if (std::string error = list(newRoot, relative); !error.empty())
            {
                setStatus(error);
                return;
            }
            buildRows();
            showLocation();
            nameChanged();
        }

        void goUp()
        {
            if (!dir.empty())
            {
                std::vector<std::string> parts = splitPath(dir);
                pendingGo = std::make_pair(root, joinPath(parts, 0, parts.size() - 1));
            }
        }

        void buildRows()
        {
            rows.clear();
            rows.reserve(entries.size());
            for (const DirectoryEntry& entry : entries)
            {
                PickRow row;
                row.name = entry.name;
                row.kind = tr(entry.kind == DirectoryEntryKind::Directory  ? "pick.kind.folder"
                                  : entry.kind == DirectoryEntryKind::File ? "pick.kind.file"
                                                                           : "pick.kind.other");
                if (entry.kind == DirectoryEntryKind::File)
                {
                    row.size = std::to_string(entry.size);
                }
                if (entry.modified.size() >= 16)
                {
                    row.modified = entry.modified.substr(0, 10) + " " + entry.modified.substr(11, 5);
                }
                rows.push_back(std::move(row));
            }
            seenLocale = localization ? localization->revision() : 0;
            table->setItems(static_cast<u32>(rows.size()));
            table->table().setSelection(std::span<const Entity>{});
            table->table().setScrollY(0.0f);
        }

        void showLocation()
        {
            const std::vector<std::string> parts = splitPath(dir);
            struct Crumb
            {
                std::string text;
                u32 level;
            };
            std::vector<Crumb> list{{root, 0}};
            if (parts.size() + 1 <= MAX_CRUMBS)
            {
                for (u32 i = 0; i < parts.size(); ++i)
                {
                    list.push_back({"/ " + parts[i], i + 1});
                }
            }
            else
            {
                const u32 tail = MAX_CRUMBS - 2;
                const u32 hidden = static_cast<u32>(parts.size()) - tail;
                list.push_back({"/ ...", hidden});
                for (u32 i = 0; i < tail; ++i)
                {
                    list.push_back({"/ " + parts[hidden + i], hidden + i + 1});
                }
            }
            for (u32 i = 0; i < MAX_CRUMBS; ++i)
            {
                const bool used = i < list.size();
                if (used)
                {
                    crumbs[i].setText(list[i].text);
                    crumbs[i].setAccessibleName(i == 0 ? list[i].text : std::string_view(list[i].text).substr(2));
                    crumbs[i].setAccessibleSelected(i + 1 == list.size());
                    crumbLevel[i] = list[i].level;
                }
                crumbs[i].setVisible(used);
            }
            up.setInteractable(!dir.empty());
        }

        void rebuildPlaces()
        {
            std::vector<UiChoiceItem> items;
            placeTargets.clear();
            const auto header = [&](const std::string_view key)
            {
                items.push_back({.key = InternedString(std::string(TABLE) + std::string(key)), .header = true});
                placeTargets.emplace_back();
            };
            header("pick.places.roots");
            for (const std::string& name : allowed)
            {
                items.push_back({.id = InternedString("root." + name), .label = name});
                placeTargets.emplace_back(name, std::string{});
            }
            bool recentHeader = false;
            for (const std::string& place : recent)
            {
                const std::string root = place.substr(0, place.find('/'));
                if (!std::ranges::contains(allowed, root))
                {
                    continue;
                }
                if (!recentHeader)
                {
                    recentHeader = true;
                    header("pick.places.recent");
                }
                items.push_back({.id = InternedString("recent." + place), .label = place});
                placeTargets.emplace_back(root, place.size() > root.size() ? place.substr(root.size() + 1) : "");
            }
            placeList.setItems(items);
        }

        // -- the name and the confirm button ----------------------------------------------------------------------

        [[nodiscard]] bool exists(const std::string& fileName) const
        {
            std::filesystem::path file;
            if (!control->resolveFile(root, dir.empty() ? fileName : dir + "/" + fileName, file).empty())
            {
                return false;
            }
            std::error_code error;
            return std::filesystem::is_regular_file(file, error);
        }

        void nameChanged()
        {
            const std::string text(name.value());
            bool replace = false;
            if (request.mode == EditorPickMode::SaveFile && !text.empty())
            {
                const std::string target = text + missingExtension(text, request.extensions);
                replace = exists(target);
                setStatus(replace ? tr("pick.status.replace", {{"name", target}}) : std::string{});
            }
            else
            {
                setStatus({});
            }
            showConfirm(replace);
        }

        void showConfirm(const bool replace)
        {
            replaces = replace;
            const std::string label = replace   ? tr("pick.replace")
                : !request.confirmLabel.empty() ? request.confirmLabel
                                                : tr(request.mode == EditorPickMode::OpenFile       ? "pick.open"
                                                         : request.mode == EditorPickMode::SaveFile ? "pick.save"
                                                                                                    : "pick.folder");
            if (confirm.text() != label)
            {
                confirm.setText(label);
            }
            confirm.setStyleClass(replace ? InternedString("dialog.button.destructive") : InternedString{});
        }

        // -- the table --------------------------------------------------------------------------------------------

        void selected(const std::span<const Entity> selection)
        {
            if (selection.size() != 1 || request.mode == EditorPickMode::Folder)
            {
                return;
            }
            const u32 index = selection[0].index() - 1;
            if (index < entries.size() && entries[index].kind == DirectoryEntryKind::File &&
                name.value() != entries[index].name)
            {
                name.setText(entries[index].name);
                nameChanged();
            }
        }

        void activate(const Entity key)
        {
            const u32 index = key.index() - 1;
            if (index >= entries.size())
            {
                return;
            }
            const DirectoryEntry& entry = entries[index];
            if (entry.kind == DirectoryEntryKind::Directory)
            {
                pendingGo = std::make_pair(root, dir.empty() ? entry.name : dir + "/" + entry.name);
            }
            else if (entry.kind == DirectoryEntryKind::File && request.mode != EditorPickMode::Folder)
            {
                name.setText(entry.name);
                nameChanged();
                if (request.mode == EditorPickMode::OpenFile)
                {
                    hooks.confirm();
                }
            }
        }
    };

    FilePicker::FilePicker(UiScope& scope, ControlServer& control, const Localization* localization, const Widget& host,
                           const Button& confirm, Hooks hooks) :
        m_impl(std::make_unique<Impl>(scope, control, localization, host, confirm, std::move(hooks)))
    {
    }

    FilePicker::~FilePicker() = default;

    void FilePicker::open(const EditorPick& request, const float rowsHeight)
    {
        Impl& impl = *m_impl;
        impl.request = request;
        impl.allowed = allowedRoots(*impl.control, request);
        impl.pendingGo.reset();
        impl.statusText.clear();
        impl.status.setText("");
        impl.status.setVisible(false);
        const bool names = request.mode != EditorPickMode::Folder;
        impl.nameRow.setVisible(names);
        impl.name.setText(names ? request.name : std::string_view{});
        impl.rowsSlot.setSize(Vec2{impl.rowsSlot.size().x, rowsHeight});
        if (impl.table->filterText().empty() == false)
        {
            impl.control->handle(R"({"command":"filePickerFilter","args":{"text":""}})");
        }
        impl.rebuildPlaces();
        impl.places.setVisible(impl.allowed.size() > 1 || !impl.recent.empty());

        if (impl.allowed.empty())
        {
            impl.rows.clear();
            impl.entries.clear();
            impl.root.clear();
            impl.dir.clear();
            impl.table->setItems(0);
            impl.setStatus(impl.tr("pick.error.noRoots"));
            impl.showConfirm(false);
            return;
        }
        const std::string startRoot =
            std::ranges::contains(impl.allowed, request.root) ? request.root : impl.allowed[0];
        const std::string startDir = joinPath(splitPath(request.directory));
        // a start that no longer lists (the folder was removed) falls back to its root
        std::string error = impl.list(startRoot, startDir);
        if (!error.empty() && !startDir.empty())
        {
            error = impl.list(startRoot, {});
        }
        if (!error.empty())
        {
            impl.entries.clear();
            impl.root = startRoot;
            impl.dir.clear();
        }
        impl.buildRows();
        impl.showLocation();
        impl.nameChanged();
        if (!error.empty())
        {
            impl.setStatus(error);
        }
    }

    void FilePicker::update()
    {
        Impl& impl = *m_impl;
        if (impl.pendingGo)
        {
            const auto [root, dir] = *std::exchange(impl.pendingGo, std::nullopt);
            impl.go(root, dir);
        }
        impl.table->update();
        if (impl.localization && impl.localization->revision() != impl.seenLocale)
        {
            impl.buildRows();
            impl.rebuildPlaces();
            impl.nameChanged();
        }
    }

    std::string FilePicker::navigate(const std::string_view rooted)
    {
        Impl& impl = *m_impl;
        const std::vector<std::string> parts = splitPath(rooted);
        std::string error;
        if (parts.empty())
        {
            error = impl.tr("pick.error.noPath");
        }
        else if (!std::ranges::contains(impl.allowed, parts[0]))
        {
            std::string names;
            for (const std::string& name : impl.allowed)
            {
                names += (names.empty() ? "" : ", ") + name;
            }
            error = impl.tr("pick.error.root", {{"root", parts[0]}, {"roots", names}});
        }
        else
        {
            error = impl.list(parts[0], joinPath(parts, 1));
            if (error.empty())
            {
                impl.pendingGo.reset();
                impl.buildRows();
                impl.showLocation();
                impl.nameChanged();
                return {};
            }
        }
        impl.setStatus(error);
        return error;
    }

    std::string FilePicker::current(std::string& encoded)
    {
        Impl& impl = *m_impl;
        std::string rooted = withRoot(impl.root, impl.dir);
        if (impl.request.mode != EditorPickMode::Folder)
        {
            const std::string_view typed = impl.name.value();
            rooted += "/" + std::string(typed);
            if (typed.empty())
            {
                const std::string error = impl.tr("pick.error.noName");
                impl.setStatus(error);
                return error;
            }
        }
        std::string error = resolveChoice(*impl.control, impl.localization, impl.request, rooted, encoded);
        if (!error.empty())
        {
            impl.setStatus(error);
        }
        return error;
    }

    std::string FilePicker::choose(const std::string_view rooted, std::string& encoded)
    {
        Impl& impl = *m_impl;
        std::string error = resolveChoice(*impl.control, impl.localization, impl.request, rooted, encoded);
        if (!error.empty())
        {
            impl.setStatus(error);
        }
        return error;
    }

    std::string FilePicker::autoChoice(const EditorPick& request, std::string& encoded) const
    {
        const Impl& impl = *m_impl;
        const std::vector<std::string> allowed = allowedRoots(*impl.control, request);
        if (allowed.empty())
        {
            return impl.tr("pick.error.noRoots");
        }
        const std::string root = std::ranges::contains(allowed, request.root) ? request.root : allowed[0];
        std::string rooted = withRoot(root, joinPath(splitPath(request.directory)));
        if (request.mode != EditorPickMode::Folder)
        {
            rooted += "/" + request.name;
        }
        std::string error = resolveChoice(*impl.control, impl.localization, request, rooted, encoded);
        if (error.empty() && request.mode == EditorPickMode::SaveFile)
        {
            // no person approved a Replace: an existing file is never overwritten by the auto policy
            const std::vector<std::string> parts = splitPath(encoded);
            std::filesystem::path file;
            std::error_code code;
            if (impl.control->resolveFile(parts[0], joinPath(parts, 1), file).empty() &&
                std::filesystem::is_regular_file(file, code))
            {
                error = message(impl.localization, "pick.error.exists", {{"path", encoded}});
                encoded.clear();
            }
        }
        return error;
    }

    void FilePicker::remember(const std::string_view encoded)
    {
        Impl& impl = *m_impl;
        std::string place(encoded);
        if (impl.request.mode != EditorPickMode::Folder)
        {
            const sizet slash = place.rfind('/');
            place = slash == std::string::npos ? place : place.substr(0, slash);
        }
        std::erase(impl.recent, place);
        impl.recent.insert(impl.recent.begin(), std::move(place));
        if (impl.recent.size() > MAX_RECENT)
        {
            impl.recent.resize(MAX_RECENT);
        }
    }

    Entity FilePicker::initialFocus() const
    {
        const Impl& impl = *m_impl;
        return impl.request.mode == EditorPickMode::Folder ? impl.confirm.entity() : impl.name.entity();
    }

    void FilePicker::write(IFormatWriter& out) const
    {
        const Impl& impl = *m_impl;
        out.writeString("mode", MODE_NAMES[static_cast<u8>(impl.request.mode)]);
        out.beginArray("roots", static_cast<u32>(impl.allowed.size()));
        for (const std::string& root : impl.allowed)
        {
            out.writeString("", root);
        }
        out.endArray();
        out.writeString("root", impl.root);
        out.writeString("directory", impl.dir);
        out.writeString("location", withRoot(impl.root, impl.dir));
        out.writeString("name", impl.request.mode == EditorPickMode::Folder ? std::string_view{} : impl.name.value());
        out.writeString("extensions", impl.request.extensions);
        out.writeString("status", impl.statusText);
        out.writeBool("replaces", impl.replaces);
        out.writeString("confirmLabel", impl.confirm.text());
        out.writeBool("truncated", impl.truncated);
        out.beginArray("recent", static_cast<u32>(impl.recent.size()));
        for (const std::string& place : impl.recent)
        {
            out.writeString("", place);
        }
        out.endArray();
        out.beginObject("list");
        impl.table->writeView(out);
        out.endObject();
    }

    void FilePicker::writeRequest(const EditorPick& request, IFormatWriter& out)
    {
        out.writeString("mode", MODE_NAMES[static_cast<u8>(request.mode)]);
        out.beginArray("roots", static_cast<u32>(request.roots.size()));
        for (const std::string& root : request.roots)
        {
            out.writeString("", root);
        }
        out.endArray();
        out.writeString("root", request.root);
        out.writeString("directory", request.directory);
        out.writeString("name", request.name);
        out.writeString("extensions", request.extensions);
    }
} // namespace spite
