#include "EditorModules.hpp"
#include "engine/Module.hpp"
#include "EditorPanelRegistry.hpp"

#include "EditorBuildState.hpp"
#include "EditorContext.hpp"
#include "EditorFeedback.hpp"
#include "EditorLatestJob.hpp"
#include "EditorPanelUi.hpp"
#include "EditorSettings.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <cwchar>
#include <cwctype>
#define NOMINMAX
#include <Windows.h>
#endif

#include <nlohmann/json.hpp>

#include "base/Logging.hpp"
#include "base/ServiceLocator.hpp"
#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "application/Shell.hpp"
#include "ecs/World.hpp"
#include "engine/Engine.hpp"
#include "engine/ModuleHost.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "serialization/FormatWalk.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiForm.hpp"
#include "ui/UiScope.hpp"
#include "ui/UiTextMeasurer.hpp"

namespace spite
{
    using namespace panelui;

    namespace
    {
        constexpr sizet MAX_ROWS = 512; // modules listed over every directory
        constexpr u32 MAX_FIELDS = 12; // parameter rows and argument rows
        constexpr sizet MAX_COMMANDS = 256;
        constexpr sizet MAX_LISTED_COMMANDS = 12; // command names the detail lists
        constexpr sizet MAX_FOLDERS = 1000;
        constexpr sizet MAX_BROWSE_ENTRIES = 20000;
        constexpr sizet MAX_CHOICES = 32;
        constexpr sizet MAX_NAME_BYTES = 64;
        constexpr sizet MAX_TEXT_BYTES = 4096; // a field's text
        constexpr sizet MAX_REPLY_BYTES = sizet{256} << 10; // a reply parsed for the output
        constexpr sizet MAX_OUTPUT_BYTES = sizet{16} << 10;
        constexpr sizet MAX_OUTPUT_LINES = 300;
        constexpr sizet MAX_ERROR_BYTES = 300;
        constexpr u32 MAX_RUNNING_JOBS = 4; // worker threads a stuck share may hold
        constexpr std::chrono::milliseconds SCAN_BUDGET{2000};
        constexpr std::chrono::milliseconds BROWSE_BUDGET{3000};
        constexpr float LABEL_WIDTH = 190.0f;
        constexpr float CONTROL_WIDTH = 260.0f;
        constexpr float MIN_LABEL_WIDTH = 40.0f;
        constexpr float MIN_CONTROL_WIDTH = 80.0f;
        constexpr float BUTTON_PADDING = 8.0f;

        // The panel as a UI document: the document owns the containers (the panel, the header bar, the tab
        // bar's and the views' slots); the code owns the slots' content, whose texts and selections it writes.
        constexpr std::string_view VIEW = "editor.modules";
        constexpr std::string_view HEADER_SLOT = "modules.header"; // the module dropdown, Refresh and the status
        constexpr std::string_view TABS_SLOT = "modules.tabBar"; // the tab bar
        constexpr std::string_view VIEWS_SLOT = "modules.views"; // the four views, one shown at a time

        enum View : u32
        {
            ViewModules,
            ViewBuild,
            ViewParameters,
            ViewCommands,
            ViewBrowse,
            VIEW_COUNT
        };
        constexpr std::array<std::string_view, VIEW_COUNT> VIEW_NAMES{"modules", "build", "parameters", "commands",
                                                                      "browse"};
        // The engine's own libraries: the panel offers no Reload for them. A rebuild of the first two is
        // used after the host restarts; the render pipeline reloads from the Rendering panel (reloadRenderPipeline).
        constexpr std::array<std::string_view, 3> ENGINE_OWNED{"SpiteEngine", "SpiteEditor", "SpiteRenderPipeline"};

        struct DirScan
        {
            std::string dir;
            std::string error;
            u32 skipped = 0;
            bool truncated = false;
        };

        struct ScannedModule
        {
            AvailableModule module;
            u32 dir = 0;
        };

        struct ScanOut
        {
            std::vector<DirScan> dirs;
            std::vector<ScannedModule> modules;
            std::string error; // a worker that threw
        };

        ScanOut scanDirs(const std::vector<std::filesystem::path>& dirs, const std::atomic<bool>& cancel)
        {
            ScanOut out;
            for (u32 index = 0; index < dirs.size() && !cancel.load(); ++index)
            {
                ModuleScan scan = ModuleHost::scan(dirs[index], SCAN_BUDGET, &cancel);
                std::string name;
                (void)tryPathToUtf8(dirs[index], name);
                out.dirs.push_back({name, std::move(scan.error), scan.skipped, scan.truncated});
                for (AvailableModule& module : scan.modules)
                {
                    if (out.modules.size() >= MAX_ROWS)
                    {
                        out.dirs.back().truncated = true;
                        break;
                    }
                    out.modules.push_back({std::move(module), index});
                }
            }
            return out;
        }

        struct BrowseOut
        {
            std::string path; // empty on the Windows drive list
            std::vector<std::string> folders;
            u32 libraries = 0;
            std::string error;
            bool truncated = false;
        };

        char lowerAscii(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

        bool hasLibraryExtension(const std::filesystem::path& file, const std::filesystem::path& extension)
        {
#if defined(_WIN32)
            return std::ranges::equal(file.extension().native(), extension.native(), {}, std::towlower);
#else
            return file.extension() == extension;
#endif
        }

        BrowseOut listFolders(const std::string& utf8, const std::atomic<bool>& cancel)
        {
            using Clock = std::chrono::steady_clock;
            const Clock::time_point deadline = Clock::now() + BROWSE_BUDGET;
            BrowseOut out;
            out.path = utf8;
#if defined(_WIN32)
            if (utf8.empty())
            {
                std::array<wchar_t, 512> buffer{};
                const DWORD size = GetLogicalDriveStringsW(static_cast<DWORD>(buffer.size() - 1), buffer.data());
                for (const wchar_t* at = buffer.data(); size > 0 && size < buffer.size() && *at;
                     at += std::wcslen(at) + 1)
                {
                    std::string drive;
                    if (tryPathToUtf8(std::filesystem::path(at), drive))
                    {
                        out.folders.push_back(std::move(drive));
                    }
                }
                return out;
            }
#else
            if (out.path.empty())
                out.path = "/";
#endif
            std::error_code error;
            const auto dir = pathFromUtf8(out.path);
            const auto libraryExtension = ModuleHost::resolvePath(dir / "module").extension();
            sizet examined = 0;
            for (std::filesystem::directory_iterator it(dir, error), end; !error && it != end; it.increment(error))
            {
                if (++examined > MAX_BROWSE_ENTRIES || cancel.load() || Clock::now() >= deadline)
                {
                    out.truncated = true;
                    break;
                }
                std::error_code entryError;
                const std::filesystem::file_type type = it->symlink_status(entryError).type();
                if (type == std::filesystem::file_type::regular && isPlainEntry(it->path(), false) &&
                    hasLibraryExtension(it->path(), libraryExtension))
                {
                    ++out.libraries;
                }
                if (type != std::filesystem::file_type::directory || !isPlainEntry(it->path(), true))
                {
                    continue; // files, links, junctions and placeholders are not entered
                }
                std::string name;
                if (!tryPathToUtf8(it->path().filename(), name))
                {
                    continue;
                }
                if (out.folders.size() >= MAX_FOLDERS)
                {
                    out.truncated = true;
                    break;
                }
                out.folders.push_back(std::move(name));
            }
            if (error)
            {
                out.error = error.message();
            }
            std::ranges::sort(out.folders, [](const std::string& a, const std::string& b)
                              { return std::ranges::lexicographical_compare(a, b, {}, lowerAscii, lowerAscii); });
            return out;
        }

        // -- typed fields -------------------------------------------------------------------------------------------

        struct FieldSpec
        {
            std::string name;
            std::string type; // number, integer, entity, boolean, enum, string, component, json
            std::string doc;
            std::string defaultText;
            std::string hint,
                placeholder; // an argument's second line and its empty field's text (localized by the panel)
            bool required = false;
            std::vector<std::string> choices;
            std::optional<double> min, max;
            bool operator==(const FieldSpec&) const = default;
        };

        struct ParamLink
        {
            std::string command, arg, query; // the control entries that apply and read the parameter
            std::string refusal; // why it is read-only although it names a command: the module does not own the entries
        };

        struct CommandInfo
        {
            std::string name, kind, doc;
            std::vector<FieldSpec> args;
            u32 ignoredArgs = 0;
        };

        std::optional<std::string> strOf(const Json& object, const char* key)
        {
            const auto it = object.find(key);
            return it != object.end() && it->is_string() ? std::optional(it->get<std::string>()) : std::nullopt;
        }

        std::optional<double> numOf(const Json& object, const char* key)
        {
            const auto it = object.find(key);
            if (it == object.end() || !it->is_number())
            {
                return std::nullopt;
            }
            const double value = it->get<double>();
            return std::isfinite(value) ? std::optional(value) : std::nullopt;
        }

        bool plainName(const std::string& name, const sizet limit)
        {
            return !name.empty() && name.size() <= limit &&
                std::ranges::none_of(name, [](const char c) { return static_cast<unsigned char>(c) < 0x20; });
        }

        std::string numberText(const double value) { return std::format("{}", value); }

        std::string cut(std::string text, const sizet limit)
        {
            if (text.size() > limit)
            {
                sizet at = limit;
                while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
                {
                    --at;
                }
                text.resize(at);
                text += "...";
            }
            return text;
        }

        // The manifest's parameters: the usable ones (a name, a known type, finite bounds, choices for an enum);
        // `ignored` counts the rest.
        std::vector<FieldSpec> parseParameters(const Json& manifest, std::vector<ParamLink>& links, u32& ignored)
        {
            std::vector<FieldSpec> specs;
            ignored = 0;
            const auto listed = manifest.is_object() ? manifest.find("parameters") : manifest.end();
            if (!manifest.is_object() || listed == manifest.end() || !listed->is_array())
            {
                return specs;
            }
            for (const Json& entry : *listed)
            {
                FieldSpec spec;
                spec.name = entry.is_object() ? strOf(entry, "name").value_or(std::string{}) : std::string{};
                spec.type = entry.is_object() ? strOf(entry, "type").value_or("number") : std::string{};
                const bool known = spec.type == "number" || spec.type == "integer" || spec.type == "boolean" ||
                    spec.type == "string" || spec.type == "enum";
                if (!known || !plainName(spec.name, MAX_NAME_BYTES) || specs.size() >= MAX_FIELDS ||
                    std::ranges::contains(specs, spec.name, &FieldSpec::name))
                {
                    ++ignored;
                    continue;
                }
                spec.required = true;
                spec.doc = cut(strOf(entry, "help").value_or(std::string{}), 200);
                spec.min = numOf(entry, "min");
                spec.max = numOf(entry, "max");
                if (spec.min && spec.max && *spec.min > *spec.max)
                {
                    ++ignored; // an inverted range is no range: it would abort std::clamp and no value could pass it
                    continue;
                }
                if (spec.type == "enum")
                {
                    const auto choices = entry.find("choices");
                    if (choices == entry.end() || !choices->is_array() || choices->empty() ||
                        choices->size() > MAX_CHOICES)
                    {
                        ++ignored;
                        continue;
                    }
                    for (const Json& choice : *choices)
                    {
                        if (choice.is_string() && plainName(choice.get<std::string>(), 128))
                        {
                            spec.choices.push_back(choice.get<std::string>());
                        }
                    }
                    if (spec.choices.empty())
                    {
                        ++ignored;
                        continue;
                    }
                }
                if (const auto value = entry.find("default"); value != entry.end())
                {
                    if (value->is_boolean())
                    {
                        spec.defaultText = value->get<bool>() ? "true" : "false";
                    }
                    else if (value->is_string())
                    {
                        spec.defaultText = cut(value->get<std::string>(), 256);
                    }
                    else if (const std::optional<double> number = numOf(entry, "default"))
                    {
                        spec.defaultText = numberText(*number);
                    }
                }
                ParamLink link;
                link.command = strOf(entry, "command").value_or(std::string{});
                link.arg = strOf(entry, "arg").value_or(spec.name);
                link.query = strOf(entry, "query").value_or(std::string{});
                links.push_back(std::move(link));
                specs.push_back(std::move(spec));
            }
            return specs;
        }

        // A describe arg entry as a field (the kinds ControlServer names them).
        FieldSpec argSpec(const Json& arg)
        {
            FieldSpec spec;
            spec.name = strOf(arg, "name").value_or(std::string{});
            const std::string kind = strOf(arg, "kind").value_or("string");
            spec.type = kind == "boolean" || kind == "number" || kind == "integer" || kind == "entity" ||
                    kind == "component" || kind == "json"
                ? kind
                : "string";
            const auto required = arg.find("required");
            spec.required = required != arg.end() && required->is_boolean() && required->get<bool>();
            spec.doc = cut(strOf(arg, "doc").value_or(std::string{}), 200);
            spec.defaultText = cut(strOf(arg, "default").value_or(std::string{}), 256);
            if (spec.type == "integer" || spec.type == "entity")
            {
                spec.min = 0.0; // the control protocol's integers are unsigned
            }
            if (const auto choices = arg.find("choices"); choices != arg.end() && choices->is_array())
            {
                for (const Json& choice : *choices)
                {
                    if (choice.is_string() && spec.choices.size() < MAX_CHOICES)
                    {
                        spec.choices.push_back(cut(choice.get<std::string>(), 128));
                    }
                }
                if (!spec.choices.empty() && spec.type == "string")
                {
                    spec.type = "enum";
                }
            }
            return spec;
        }

        std::string rangeHint(const FieldSpec& spec)
        {
            if (spec.type == "boolean" || spec.type == "enum" || (!spec.min && !spec.max))
            {
                return {};
            }
            return std::format(" [{} .. {}]", spec.min ? numberText(*spec.min) : "",
                               spec.max ? numberText(*spec.max) : "");
        }

        // The argument form's scroll content, laid out by hand: wrapped text blocks (the owner fills them; an empty
        // one is hidden) and the pooled rows between its head and tail blocks. A text's height is measured at the
        // view's width, so what is below it never waits a frame for the layout to size a label. (Text laid out by
        // the layout and read back to place a sibling is a frame late: the Details view stacks all of its content in
        // one column instead.)
        // The rows: a label, and per field the one control its type needs (a text input,
        // or a dropdown for a boolean and an enum). Widgets are created once, then shown or hidden. Arguments start
        // empty, which sends nothing: the default is the empty field's placeholder, and a hint line under the field
        // holds the argument's description. A dropdown's first item clears the field: a boolean or an enum has no empty
        // text, and an explicit false is not an unset argument.
        class FieldRows
        {
        public:
            FieldRows(UiScope& scope, const std::string_view prefix, ScrollView view) : m_view(view), m_scope(&scope)
            {
                const std::string id(prefix);
                for (u32 i = 0; i < MAX_FIELDS; ++i)
                {
                    Row& row = m_rows[i];
                    row.label = scope.createLabel(InternedString(id + ".label"), "", topLeft());
                    row.label.setParent(view.content());
                    row.input = scope.createTextInput(InternedString(id + ".input"), topLeft());
                    row.input.setParent(view.content());
                    row.input.onChange([this, i](const std::string_view text) { edited(i, std::string(text)); });
                    row.input.onSubmit([this, i](std::string_view) { submit(i); });
                    row.drop = scope.createDropdown(InternedString(id + ".choice"), UiDropdownDesc{.rect = topLeft()});
                    row.drop.setParent(view.content());
                    row.drop.onChange(
                        [this, i](const u32 index)
                        {
                            if (index <= m_specs[i].choices.size())
                            {
                                edited(i, index == 0 ? std::string{} : m_specs[i].choices[index - 1]);
                            }
                        });
                    row.input.setStyleClass(InternedString("modules.arg"));
                    UiRectDesc hintDesc = topLeft();
                    hintDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
                    row.hint = scope.createLabel(InternedString(id + ".hint"), "", hintDesc);
                    row.hint.setParent(view.content());
                    row.hint.setStyleClass(InternedString("modules.hint"));
                }
                setSpecs({});
                m_observer = scope.observeLayoutTokens([this] { m_width = -1.0f; });
            }

            ~FieldRows() { m_scope->unobserveLayoutTokens(m_observer); }
            FieldRows(const FieldRows&) = delete;
            FieldRows& operator=(const FieldRows&) = delete;

            std::function<void(u32 index)> onSubmit;
            std::string unsetLabel; // the first item of a dropdown: set before setSpecs, with the locale

            void setBlocks(std::vector<Label> head, std::vector<Label> tail)
            {
                m_head = std::move(head);
                m_tail = std::move(tail);
            }

            [[nodiscard]] u32 count() const { return static_cast<u32>(m_specs.size()); }
            [[nodiscard]] const FieldSpec& spec(const u32 i) const { return m_specs[i]; }
            [[nodiscard]] const std::string& value(const u32 i) const { return m_values[i]; }

            // Shows these fields; with `keep` a field of the same name and type keeps its text, the others start
            // empty.
            void setSpecs(std::vector<FieldSpec> specs, const bool keep = true)
            {
                std::vector<std::string> kept(specs.size());
                for (sizet i = 0; i < specs.size(); ++i)
                {
                    const auto old = keep
                        ? std::ranges::find_if(m_specs, [&](const FieldSpec& other)
                                               { return other.name == specs[i].name && other.type == specs[i].type; })
                        : m_specs.end();
                    if (old != m_specs.end())
                    {
                        kept[i] = m_values[static_cast<sizet>(old - m_specs.begin())];
                    }
                    if (specs[i].type == "boolean")
                    {
                        specs[i].choices = {"true", "false"};
                    }
                }
                m_specs = std::move(specs);
                m_values.assign(MAX_FIELDS, std::string{});
                for (u32 i = 0; i < MAX_FIELDS; ++i)
                {
                    Row& row = m_rows[i];
                    const bool used = i < m_specs.size();
                    const bool choice = used && isChoice(m_specs[i]);
                    row.label.setVisible(used);
                    row.hint.setVisible(used && !m_specs[i].hint.empty());
                    row.input.setVisible(used && !choice);
                    row.drop.setVisible(choice);
                    if (!used)
                    {
                        continue;
                    }
                    row.label.setText(m_specs[i].name + (m_specs[i].required ? "*" : ""));
                    row.hint.setText(m_specs[i].hint);
                    row.input.setPlaceholder(m_specs[i].placeholder);
                    row.input.setAccessibleName(m_specs[i].name);
                    row.drop.setAccessibleName(m_specs[i].name);
                    if (choice)
                    {
                        std::vector<UiChoiceItem> items{{.label = unsetLabel}};
                        for (const std::string& option : m_specs[i].choices)
                        {
                            items.push_back({.label = option});
                        }
                        row.drop.setItems(items);
                    }
                    setValue(i, kept[i]);
                }
                m_width = -1.0f;
                relayout();
            }

            void setValue(const u32 i, const std::string& text)
            {
                if (i >= m_specs.size())
                {
                    return;
                }
                m_values[i] = text;
                Row& row = m_rows[i];
                if (isChoice(m_specs[i]))
                {
                    const auto at = std::ranges::find(m_specs[i].choices, text);
                    row.drop.setSelected(
                        at == m_specs[i].choices.end() ? 0 : static_cast<i32>(at - m_specs[i].choices.begin()) + 1);
                }
                else if (row.input.value() != text)
                {
                    row.input.setText(text);
                }
            }

            // Places the rows at the scroll view's resolved width and sizes the content. The head, the tail and the
            // hints are wrapped texts: their heights are measured from the text at this width, so the rows below them
            // never wait a frame for the layout to size a label.
            void relayout()
            {
                // an inactive tab resolves to width 0: placing the rows there would show them narrow for a frame
                // when the tab is next shown
                if (!m_scope->hierarchy().isVisibleOf(m_view.entity()))
                {
                    return;
                }
                const float width = std::max(0.0f, resolvedSize(*m_scope, m_view).x - THUMB);
                // runs every frame of the active view: compares in place, copies only when a text changed
                const u32 heads = static_cast<u32>(m_head.size());
                const u32 tails = static_cast<u32>(m_tail.size());
                const u32 count = heads + tails + static_cast<u32>(m_specs.size());
                const auto textAt = [&](const u32 k) -> std::string_view
                {
                    if (k < heads)
                    {
                        return m_head[k].text();
                    }
                    return k < heads + tails ? m_tail[k - heads].text()
                                             : std::string_view(m_specs[k - heads - tails].hint);
                };
                bool unchanged = width == m_width && m_placed.size() == count;
                for (u32 k = 0; unchanged && k < count; ++k)
                {
                    unchanged = textAt(k) == m_placed[k];
                }
                if (unchanged)
                {
                    return;
                }
                m_width = width;
                m_placed.resize(count);
                for (u32 k = 0; k < count; ++k)
                {
                    m_placed[k].assign(textAt(k));
                }
                // a field row is a control and a hairline above and below; the hint under an argument wraps whole
                const Density density = Density::read(*m_scope);
                const float pad = density.xs;
                const float gap = density.xs;
                const float field = density.control + 2.0f * density.hairline;
                const float inner = std::max(0.0f, width - 2.0f * pad);
                const float labelWidth =
                    std::min(LABEL_WIDTH, std::max(MIN_LABEL_WIDTH, width - MIN_CONTROL_WIDTH - 3.0f * pad));
                const float control =
                    std::min(CONTROL_WIDTH, std::max(MIN_CONTROL_WIDTH, width - labelWidth - 3.0f * pad));
                const UiTextMeasurer* measurer = m_scope->hierarchy().textMeasurer();
                const auto heightOf = [&](const Widget& label, const std::string& text)
                {
                    return text.empty() || !measurer ? 0.0f
                                                     : measurer->measureTextBlock(label.entity(), text, inner).size.y;
                };
                float y = pad;
                const auto stackBlocks = [&](std::vector<Label>& blocks, const u32 first)
                {
                    for (u32 i = 0; i < blocks.size(); ++i)
                    {
                        const float height = heightOf(blocks[i], m_placed[first + i]);
                        if (blocks[i].isVisible() != (height > 0.0f))
                        {
                            blocks[i].setVisible(height > 0.0f);
                        }
                        if (height > 0.0f)
                        {
                            place(blocks[i], Vec2{pad, y}, Vec2{inner, height});
                            y += height + gap;
                        }
                    }
                };
                stackBlocks(m_head, 0);
                for (u32 i = 0; i < m_specs.size(); ++i)
                {
                    Row& row = m_rows[i];
                    place(row.label, Vec2{pad, y}, Vec2{labelWidth, field});
                    const Vec2 at{pad + labelWidth + gap, y};
                    place(row.input, at, Vec2{control, field});
                    place(row.drop, at, Vec2{control, field});
                    y += field + gap;
                    if (const float hint = heightOf(row.hint, m_placed[heads + tails + i]); hint > 0.0f)
                    {
                        place(row.hint, Vec2{pad, y}, Vec2{inner, hint});
                        y += hint + gap;
                    }
                }
                stackBlocks(m_tail, static_cast<u32>(m_head.size()));
                m_view.setContentSize(Vec2{width, y});
            }

        private:
            struct Row
            {
                Label label;
                TextInput input;
                Dropdown drop;
                Label hint;
            };

            static bool isChoice(const FieldSpec& spec) { return spec.type == "boolean" || spec.type == "enum"; }

            void edited(const u32 i, std::string text) { m_values[i] = std::move(text); }

            void submit(const u32 i)
            {
                if (onSubmit && i < m_specs.size())
                {
                    onSubmit(i);
                }
            }

            ScrollView m_view;
            UiScope* m_scope;
            std::array<Row, MAX_FIELDS> m_rows;
            std::vector<FieldSpec> m_specs;
            std::vector<std::string> m_values;
            std::vector<Label> m_head, m_tail;
            std::vector<std::string> m_placed; // the blocks' and hints' texts as last placed
            float m_width = -1.0f;
            u32 m_observer = 0;
        };

        // a button as wide as its label
        UiRectDesc fitButtonDesc()
        {
            UiRectDesc desc = barDesc(Vec2{});
            desc.contentSize =
                UiContentSizeComponent{.fitWidth = true, .fitHeight = false, .padding = Vec2{BUTTON_PADDING, 0.0f}};
            return desc;
        }

        std::string joinedPath(const std::string& dir, const std::string& name)
        {
            if (dir.empty())
                return name;
            std::string joined;
            (void)tryPathToUtf8(pathFromUtf8(dir) / pathFromUtf8(name), joined);
            return joined;
        }

        // Resolve a module list entry with the same rules as the loader.
        std::string entryFile(const std::string& entry)
        {
            std::string resolved;
            (void)tryPathToUtf8(
                ModuleHost::resolvePath(std::filesystem::path(std::u8string(entry.begin(), entry.end())))
                    .lexically_normal(),
                resolved);
            return resolved;
        }

        bool sameModuleFile(const std::string& entry, const std::string& path)
        {
            return sameModuleSearchPath(entryFile(entry), path);
        }
    } // namespace

    // -- the panel --------------------------------------------------------------------------------------------------

    struct EditorModules::State
    {
        struct Row
        {
            std::string name, path, dir, displayName, description, manifest, manifestError;
            bool loaded = false;
            bool duplicate = false; // another listed file has the same name
            bool fixture = false; // the manifest calls it a test fixture: hidden unless loaded, selected or asked for
            bool persisted = false; // the module list of the user config names the file
            bool missing = false; // only the module list names it: in no scanned directory and not loaded
            u64 apiStamp = 0;
            u32 dirIndex = 0;
        };

        enum class Kind : u8
        {
            Load,
            Unload,
            Reload,
            ReloadBuilt,
            Param,
            Run,
            Persist,
            Unpersist,
            Build,
            CancelBuild,
            Panel // one of the panel's own commands: only a refusal is shown
        };

        // What the Build view's Build button does for the selected module's project
        struct BuildAction
        {
            bool cancel = false; // the editor's own build of it runs: the button is Cancel
            bool enabled = false;
            const char* tip = nullptr;
        };

        struct Action
        {
            Json request;
            Kind kind;
            std::string label;
            std::string originPath;
        };

        State(Engine& engine, const EditorSettingsService& settings, EditorFeedback& feedback,
              EditorBuildState& build) :
            engine(&engine), control(&engine.control()), host(&engine.modules()), settings(&settings),
            feedback(&feedback), build(&build), scope(editorUi(engine).ui())
        {
            if (engine.services().contains<Localization>())
            {
                localization = &engine.services().get<Localization>();
                shownRevision = localization->revision();
            }
        }

        Engine* engine;
        ControlServer* control;
        ModuleHost* host;
        const EditorSettingsService* settings;
        EditorFeedback* feedback;
        EditorBuildState* build; // the model's: the rows' build fields read it
        const Localization* localization = nullptr;
        UiScope scope;
        u64 shownRevision = 0;
        Density density = Density::read(scope); // the heights and spaces the code builds the form with
        u32 densityObserver = 0;

        // widgets
        Panel root;
        Panel workspace, browserPane, detailPane;
        TextInput moduleSearch;
        Button backButton, foldersButton, loadButton;
        Button copyPathButton;
        Button revealButton;
        Label selectionTitle, actionHint;
        Label listEmpty;
        ScrollView overviewView;
        Widget overviewColumn;
        std::string filter;
        bool narrow = false, detailVisible = false;
        float workspaceWidth = -1.0f;
        std::string workspaceSelection;
        u64 workspaceBuild = ~0ull, workspaceLoaded = ~0ull, workspaceLocale = ~0ull;
        u64 workspaceScans = ~0ull;
        Panel runBar;
        Panel browseBar;
        Panel actionBar;
        Panel reloadHost; // holds Reload and its tooltip: a disabled button takes no pointer, the host does
        Button reloadButton;
        const char* reloadTip = nullptr;
        Panel buildBar;
        Panel reloadBuiltHost; // as reloadHost, for Reload built modules
        Button reloadBuiltButton;
        const char* reloadBuiltTip = nullptr;
        Panel buildHost; // as reloadHost, for Build / Cancel
        Button buildButton;
        Button buildPanelAction;
        BuildAction shownBuild; // what the button shows, so an unchanged state writes nothing
        std::string shownBuildProject; // the project a shown Cancel is for; empty while the button is Build
        TextInput buildStatus;
        ScrollView buildView;
        Widget buildColumn; // the Build view's content: the empty state, the selected module's build, other projects
        Label buildNote, buildTitle, buildFacts, buildDiagnostics, buildCommand, buildOthersTitle, buildOthers;
        u64 buildSeen = ~0ull; // the build state's displayRevision the texts were written for
        Checkbox persistBox, fixtureBox;
        Dropdown moduleChoice;
        Dropdown commandChoice;
        Button refreshButton;
        TextInput status;
        TabBar tabs;
        std::array<Panel, VIEW_COUNT> views;
        Label detailTitle, detailText, detailMeta, detailCommands; // the parameters view's head: the descriptor
        std::unique_ptr<Table> moduleTable;
        std::unique_ptr<Table> folderTable;
        ScrollView paramView;
        ScrollView argView;
        Label outputText; // the run's result, wrapped, under the argument form
        Widget paramColumn; // the Details view's content: the descriptor's texts, then the parameters' form
        std::unique_ptr<PropertyForm> paramForm; // null while the module declares no usable parameter
        std::vector<FieldSpec> formSpecs; // what the form was built from, and for which module file
        std::string formFor;
        bool formStale = false; // the density changed: build the form again at the next sync
        std::unique_ptr<FieldRows> args;
        Label paramNote;
        Label commandLine;
        Button runButton;
        Label runStatus;
        TextInput browsePathInput;
        TextInput browseStatus;
        Button upButton;
        Button addButton;

        // data
        View view = ViewModules;
        std::vector<std::filesystem::path> scanRoots;
        std::vector<DirScan> dirs;
        std::vector<ScannedModule> scanned;
        std::vector<Row> rows;
        std::vector<Row> shownRows; // the list's rows: rows minus the hidden fixtures
        std::vector<std::string> persistedEntries; // the module list of the user config, as the host runs with it
        bool showFixtures = false;
        std::string selected, selectedPath;
        std::string detailFor; // the module the detail below was read for
        bool detailLoaded = false;
        bool detailStale = true;
        Json manifest;
        std::string manifestError;
        std::vector<FieldSpec> paramSpecs;
        std::vector<ParamLink> paramLinks;
        u32 ignoredParams = 0;
        std::string entriesFor; // the moduleCommands reply cached for this module, until the stamp or scan count moves
        u64 entriesStamp = ~0ull, entriesScans = ~0ull;
        Json entries;
        std::vector<CommandInfo> commands;
        i32 selectedCommand = -1;
        std::vector<std::string> output;
        std::string message; // the last action's outcome, shown beside the counts
        std::string browsePath;
        std::vector<std::string> folders;
        std::string browseError;
        bool browseTruncated = false;
        u32 browseLibraries = 0;
        std::vector<Action> pending;
        u64 loadedStamp = ~0ull;
        u64 seenCommand = ~0ull;
        u64 seenPaths = ~0ull;
        bool scanning = false;
        bool rowsDirty = true;
        bool rescan = false;
        bool browsing = false;
        bool rebrowse = false;
        bool wasShown = false;
        u64 scans = 0, listings = 0, refreshes = 0, actions = 0, entryReads = 0;

        EditorLatestJob<ScanOut> scanJob{MAX_RUNNING_JOBS};
        EditorLatestJob<BrowseOut> browseJob{MAX_RUNNING_JOBS};
        std::unique_ptr<UiDocumentView> doc; // last: destroyed first

        // -- document --

        std::string moduleStatus(const Row& row) const
        {
            if (row.missing)
                return msg("missingNote");
            if (const auto record = build->record(buildStem(row)))
            {
                if (isBuilding(*record))
                    return msg("status.building");
                if (record->state == "failed")
                    return msg("status.failed");
                if (record->needsReload)
                    return msg(engineOwned(buildStem(row)) ? "status.restart" : "status.reload");
                if (row.loaded && record->state == "ok")
                    return msg("status.current");
            }
            return msg(row.loaded ? "status.loaded" : "status.unloaded");
        }

        void followWorkspace()
        {
            const float width = resolvedSize(scope, root).x;
            const bool compact = width < (narrow ? 736.0f : 704.0f);
            if (workspaceWidth < 0.0f || compact != narrow)
            {
                narrow = compact;
                workspace.setLayout(stack(narrow ? UiLayoutDirection::Column : UiLayoutDirection::Row, density.md, {},
                                          UiLayoutAlign::Stretch));
                browserPane.setLayoutItem(narrow ? fillItem() : basisItem(280.0f));
            }
            workspaceWidth = width;
            browserPane.setVisible(!narrow || !detailVisible);
            detailPane.setVisible(!narrow || detailVisible);
            backButton.setVisible((narrow && detailVisible) || view == ViewBrowse);
            const View content = view == ViewBuild ? ViewModules : view;
            for (u32 i = 0; i < VIEW_COUNT; ++i)
                if (i != ViewBuild)
                    views[i].setVisible(i == content);
            tabs.setVisible(view != ViewBrowse);
            selectionTitle.setVisible(view != ViewBrowse);
            actionBar.setVisible(view != ViewBrowse);
            actionHint.setVisible(view != ViewBrowse && !selectedPath.empty());
            const u64 built = build->displayRevision();
            const u64 locale = localization ? localization->revision() : 0;
            if (workspaceSelection == selectedPath && workspaceBuild == built && workspaceLoaded == loadedStamp &&
                workspaceLocale == locale && workspaceScans == scans)
                return;
            workspaceSelection = selectedPath;
            workspaceBuild = built;
            workspaceLoaded = loadedStamp;
            workspaceLocale = locale;
            workspaceScans = scans;
            const Row* selected = selectedRow();
            setText(selectionTitle,
                    selected ? label(*selected) + " · " + moduleStatus(*selected) : msg("noneSelected"));
            loadButton.setInteractable(selected != nullptr);
            copyPathButton.setInteractable(selected != nullptr);
            revealButton.setVisible(selected != nullptr);
            revealButton.setInteractable(selected && !selected->missing);
            loadButton.setLocalizedText(InternedString(selected && selected->missing      ? "editor.modules.unpersist"
                                                           : selected && selected->loaded ? "editor.modules.unload"
                                                                                          : "editor.modules.load"));
            const std::string stem = selected ? buildStem(*selected) : std::string{};
            setText(actionHint,
                    !selected                   ? msg("noneSelected")
                        : engineOwned(stem)     ? msg(ownedHint(stem))
                        : !selected->loaded     ? msg("loadFirst")
                        : stem.empty()          ? msg("build.external")
                        : buildAction().enabled ? msg("actions.manualReload")
                                                : msg("build.state.building"));
        }

        // The header's widgets go in its slot and take its child class.
        void fillHeader(const Widget& slot)
        {
            const InternedString childClass = slotFields(scope, slot).childClass;
            for (Widget widget : {static_cast<Widget>(backButton), static_cast<Widget>(refreshButton),
                                  static_cast<Widget>(foldersButton), static_cast<Widget>(status)})
            {
                putIn(scope, slot, widget);
                widget.setStyleClass(childClass);
            }
        }

        void fillTabs(const Widget& slot) { tabs.setStyleClass(slotFields(scope, slot).childClass); }

        // The views go in their slot; its scroll field is the lists' and its child class the tables' rows'.
        void fillViews(const Widget& slot)
        {
            putIn(scope, slot, workspace);
            const UiSlotComponent& fields = slotFields(scope, slot);
            for (Table* table : {moduleTable.get(), folderTable.get()})
            {
                table->list.setScrollEnabled(fields.scroll);
                table->setRowClass(fields.childClass);
            }
            for (ScrollView form : {paramView, argView, buildView})
            {
                form.setScrollEnabled(fields.scroll);
            }
        }

        // The heights the code gives what it builds: a control, a row of text, and a bar of buttons (a control with a
        // hairline above and below). The tables and the field rows follow the density themselves.
        void applyDensity()
        {
            const auto height = [](Widget widget, const float value)
            {
                if (widget.size().y != value)
                {
                    widget.setSize(Vec2{widget.size().x, value});
                }
            };
            for (const Widget widget :
                 {static_cast<Widget>(moduleChoice), static_cast<Widget>(refreshButton), static_cast<Widget>(runButton),
                  static_cast<Widget>(upButton), static_cast<Widget>(addButton), static_cast<Widget>(reloadButton),
                  static_cast<Widget>(commandChoice), static_cast<Widget>(browsePathInput),
                  static_cast<Widget>(reloadBuiltButton), static_cast<Widget>(buildButton),
                  static_cast<Widget>(buildPanelAction)})
            {
                height(widget, density.control);
            }
            for (Widget widget : {static_cast<Widget>(moduleSearch), static_cast<Widget>(backButton),
                                  static_cast<Widget>(foldersButton), static_cast<Widget>(loadButton),
                                  static_cast<Widget>(copyPathButton), static_cast<Widget>(revealButton)})
                height(widget, density.control);
            if (copyResultButton.isValid())
                height(copyResultButton, density.control);
            if (resetDefaultsButton.isValid())
                height(resetDefaultsButton, density.control);
            height(selectionTitle, density.row);
            height(actionHint, density.row * 2);
            workspaceWidth = -1.0f;
            for (const Widget widget :
                 {static_cast<Widget>(status), static_cast<Widget>(browseStatus), static_cast<Widget>(buildStatus)})
            {
                height(widget, density.row);
            }
            for (const Widget bar :
                 {static_cast<Widget>(runBar), static_cast<Widget>(browseBar), static_cast<Widget>(buildBar)})
            {
                height(bar, density.control + 2.0f * density.hairline);
            }
        }

        // A density change: the heights above, and the output text's inset.
        void followDensity()
        {
            const Density now = Density::read(scope);
            if (now == density)
            {
                return;
            }
            density = now;
            applyDensity();
            formSpecs.clear(); // the form's rows are as high as the density says: build it again, not the detail
            formStale = true;
        }

        // -- text --

        std::string msg(const std::string_view key, const std::vector<MessageArg>& args = {}) const
        {
            const std::string full = "editor.modules." + std::string(key);
            if (!localization)
            {
                return full;
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(full), args, scratch));
        }

        static const char* stateKey(const Row& row)
        {
            return row.missing ? "state.missing" : row.loaded ? "state.loaded" : "state.unloaded";
        }

        std::string label(const Row& row) const
        {
            std::string text =
                row.displayName.empty() || row.displayName == row.name ? row.name : row.name + " - " + row.displayName;
            return row.duplicate ? text + " (" + msg("duplicate") + ")" : text;
        }

        // the selected file: by path (two files may share a name), by name only when selected without a path
        const Row* selectedRow() const
        {
            const auto it = selectedPath.empty()
                ? std::ranges::find(rows, selected, &Row::name)
                : std::ranges::find_if(rows,
                                       [&](const Row& row) { return sameModuleSearchPath(row.path, selectedPath); });
            return it == rows.end() ? nullptr : &*it;
        }

        // -- scanning ---

        void startScan()
        {
            scanRoots.clear();
            scanRoots.push_back(host->searchDirectory());
            std::string first;
            (void)tryPathToUtf8(scanRoots.front(), first);
            for (const std::string& path : settings->moduleSearchPaths())
            {
                if (!sameModuleSearchPath(path, first))
                {
                    scanRoots.emplace_back(std::u8string(path.begin(), path.end()));
                }
            }
            seenPaths = settings->moduleSearchPathRevision();
            rescan = !scanJob.start([roots = scanRoots](const std::atomic<bool>& cancel)
                                    { return scanDirs(roots, cancel); });
            scanning = !rescan;
        }

        void startBrowse()
        {
            rebrowse = !browseJob.start([path = browsePath](const std::atomic<bool>& cancel)
                                        { return listFolders(path, cancel); });
            browsing = !rebrowse;
        }

        void pollJobs()
        {
            if (std::optional<ScanOut> done = scanJob.poll())
            {
                dirs = std::move(done->dirs);
                scanned = std::move(done->modules);
                if (!done->error.empty())
                {
                    dirs.push_back({"scan", std::move(done->error), 0, false});
                }
                scanning = false;
                ++scans;
                rebuildRows();
                if (const std::string issue = scanIssue(); !issue.empty())
                {
                    feedback->postToast(ToastLevel::Warn, issue);
                }
            }
            if (std::optional<BrowseOut> done = browseJob.poll())
            {
                browsePath = done->path;
                folders = std::move(done->folders);
                browseError = std::move(done->error);
                browseTruncated = done->truncated;
                browseLibraries = done->libraries;
                browsing = false;
                ++listings;
                showBrowse();
            }
            if (rescan || settings->moduleSearchPathRevision() != seenPaths)
            {
                startScan();
            }
            if (rebrowse)
            {
                startBrowse();
            }
        }

        static u64 stampOf(const ModuleHost& host)
        {
            u64 stamp = host.generation();
            for (const ModuleHost::LoadedModule& module : host.modules())
            {
                stamp = stamp * 1099511628211ull ^ reinterpret_cast<std::uintptr_t>(module.descriptor);
            }
            return stamp;
        }

        void rebuildRows()
        {
            SPITE_ZONE("EditorModules.rebuildRows");
            std::vector<AvailableModule> found;
            found.reserve(scanned.size());
            for (const ScannedModule& item : scanned)
            {
                found.push_back(item.module);
            }
            host->markLoaded(found);
            std::vector<Row> next;
            for (sizet i = 0; i < found.size(); ++i)
            {
                Row row;
                row.name = found[i].name;
                (void)tryPathToUtf8(found[i].path, row.path);
                row.dirIndex = scanned[i].dir;
                row.dir = scanned[i].dir < dirs.size() ? dirs[scanned[i].dir].dir : std::string{};
                row.manifest = found[i].manifest;
                row.manifestError = found[i].manifestError;
                row.loaded = found[i].loaded;
                row.apiStamp = found[i].apiStamp;
                fillTitles(row);
                next.push_back(std::move(row));
            }
            // loaded from a directory nobody lists
            for (const ModuleHost::LoadedModule& module : host->modules())
            {
                if (next.size() < MAX_ROWS &&
                    std::ranges::none_of(next, [&](const Row& row) { return row.name == module.name; }))
                {
                    Row row;
                    row.name = module.name;
                    (void)tryPathToUtf8(module.source, row.path);
                    row.dir = row.path.substr(0, row.path.find_last_of("\\/"));
                    row.loaded = true;
                    row.apiStamp = module.descriptor->apiStamp;
                    row.dirIndex = ~0u;
                    next.push_back(std::move(row));
                }
            }
            persistedEntries = readPersisted();
            // named by the module list only (once a scan has answered): the user can see it and drop it
            for (const std::string& entry : persistedEntries)
            {
                if (scans > 0 && next.size() < MAX_ROWS &&
                    std::ranges::none_of(next, [&](const Row& row) { return sameModuleFile(entry, row.path); }))
                {
                    Row row;
                    row.path = entryFile(entry);
                    row.name = utf8Path(pathFromUtf8(row.path).stem());
                    row.dir = row.path.substr(0, row.path.find_last_of("\\/"));
                    row.missing = true;
                    row.dirIndex = ~0u;
                    next.push_back(std::move(row));
                }
            }
            for (Row& row : next)
            {
                row.duplicate = std::ranges::count(next, row.name, &Row::name) > 1;
                row.persisted = std::ranges::any_of(persistedEntries, [&](const std::string& entry)
                                                    { return sameModuleFile(entry, row.path); });
            }
            rows = std::move(next);
            rowsDirty = true;
            loadedStamp = stampOf(*host);
            detailStale = true;
            // loading renames a row (the file name becomes the module's own): the selection follows the file
            if (!selected.empty())
            {
                const Row* now = selectedRow();
                selected = now ? now->name : std::string{};
                if (!now)
                {
                    selectedPath.clear();
                    output.clear();
                    showOutput();
                }
            }
        }

        // The module list the next start loads: the config store's staged list (a startup section: the running config
        // holds the old one until a restart), else the list an engine without an application started with.
        std::vector<std::string> readPersisted()
        {
            const Json reply =
                request(*control, Json{{"query", "config"}, {"args", {{"section", "application.modules"}}}});
            if (reply.value("ok", false) && reply.contains("result") && reply["result"].contains("sections") &&
                reply["result"]["sections"].is_array() && !reply["result"]["sections"].empty())
            {
                const Json& values = reply["result"]["sections"][0].value("values", Json::object());
                std::vector<std::string> entries;
                for (const Json& entry : values.value("modules", Json::array()))
                {
                    if (entry.is_string() && entries.size() < MAX_ROWS)
                    {
                        entries.push_back(entry.get<std::string>());
                    }
                }
                return entries;
            }
            return engine->config().modules;
        }

        static void fillTitles(Row& row)
        {
            const Json parsed = row.manifest.empty() ? Json() : Json::parse(row.manifest, nullptr, false);
            if (parsed.is_object())
            {
                row.displayName = cut(strOf(parsed, "displayName").value_or(std::string{}), 80);
                row.description = cut(strOf(parsed, "description").value_or(std::string{}), 300);
                const auto flag = parsed.find("fixture");
                row.fixture = flag != parsed.end() && flag->is_boolean() && flag->get<bool>();
            }
        }

        // -- selection and detail --

        void select(const Row& row)
        {
            selected = row.name;
            selectedPath = row.path;
            detailVisible = true;
            if (view == ViewBrowse)
            {
                view = ViewModules;
                tabs.setActive(0);
            }
            detailStale = true;
            selectedCommand = -1;
            rowsDirty = true; // a selected fixture joins the list
        }

        void refreshDetail()
        {
            detailStale = false;
            ++refreshes;
            detailFor = selectedPath;
            const std::string keepCommand =
                selectedCommand >= 0 ? commands[static_cast<sizet>(selectedCommand)].name : std::string{};
            manifest = Json();
            manifestError.clear();
            paramSpecs.clear();
            paramLinks.clear();
            commands.clear();
            ignoredParams = 0;
            const Row* row = selectedRow();
            detailLoaded = row && row->loaded;
            if (row)
            {
                manifestError = row->manifestError;
                if (!row->manifest.empty())
                {
                    manifest = Json::parse(row->manifest, nullptr, false);
                }
                if (detailLoaded)
                {
                    readEntries(*row);
                }
                else
                {
                    entriesFor.clear(); // an unload ends the cached entries, though a reload may map the same address
                }
            }
            paramSpecs = parseParameters(manifest, paramLinks, ignoredParams);
            checkLinks();
            showParamForm();
            const auto kept = std::ranges::find(commands, keepCommand, &CommandInfo::name);
            selectedCommand =
                keepCommand.empty() || kept == commands.end() ? -1 : static_cast<i32>(kept - commands.begin());
            showCommand();
            refreshParamValues();
            showDetail();
        }

        // A manifest is the module's DLL's sidecar, i.e. untrusted: a parameter is applied only through a command the
        // selected module registered itself (kind command, taking the named argument), and read only through a query
        // it registered, so a hostile manifest cannot make the panel send loadModule, shutdown or any engine entry.
        void checkLinks()
        {
            for (u32 i = 0; i < paramLinks.size(); ++i)
            {
                ParamLink& link = paramLinks[i];
                link.refusal.clear();
                if (!detailLoaded || link.command.empty())
                {
                    continue;
                }
                const auto command = std::ranges::find(commands, link.command, &CommandInfo::name);
                const auto query = std::ranges::find(commands, link.query, &CommandInfo::name);
                if (command == commands.end() || command->kind != "command" ||
                    !std::ranges::contains(command->args, link.arg, &FieldSpec::name))
                {
                    link.refusal = msg(
                        "error.linkCommand",
                        {{"name", std::string_view(paramSpecs[i].name)}, {"entry", std::string_view(link.command)}});
                }
                else if (!link.query.empty() && (query == commands.end() || query->kind != "query"))
                {
                    link.refusal =
                        msg("error.linkQuery",
                            {{"name", std::string_view(paramSpecs[i].name)}, {"entry", std::string_view(link.query)}});
                }
            }
        }

        // the loaded module's registered entries and manifest (moduleCommands, which scans the executable's
        // directory): read again only when a load, an unload or a scan changed what it answers. The entries are the
        // ones filed under the module's own owner key; a loaded runtime module is never an engine-owned name, so
        // the engine's commands are not among them.
        void readEntries(const Row& row)
        {
            if (entriesFor != row.name || entriesStamp != loadedStamp || entriesScans != scans)
            {
                entriesFor = row.name;
                entriesStamp = loadedStamp;
                entriesScans = scans;
                ++entryReads;
                const Json reply = request(*control, Json{{"query", "moduleCommands"}, {"args", {{"name", row.name}}}});
                entries = reply.value("ok", false) && reply.contains("result") ? reply["result"] : Json();
            }
            if (!entries.is_object())
            {
                return;
            }
            const Json& result = entries;
            if (const auto it = result.find("manifest"); it != result.end() && it->is_object())
            {
                manifest = *it;
            }
            if (const std::optional<std::string> error = strOf(result, "manifestError"))
            {
                manifestError = *error;
            }
            const auto entries = result.find("entries");
            if (entries == result.end() || !entries->is_array())
            {
                return;
            }
            for (const Json& entry : *entries)
            {
                if (commands.size() >= MAX_COMMANDS)
                {
                    break;
                }
                CommandInfo info;
                info.name = strOf(entry, "name").value_or(std::string{});
                info.kind = strOf(entry, "kind").value_or("query");
                info.doc = cut(strOf(entry, "doc").value_or(std::string{}), 200);
                if (info.name.empty())
                {
                    continue;
                }
                const auto described = entry.find("args");
                if (described != entry.end() && described->is_array())
                {
                    for (const Json& arg : *described)
                    {
                        if (info.args.size() >= MAX_FIELDS)
                        {
                            ++info.ignoredArgs;
                        }
                        else if (arg.is_object())
                        {
                            info.args.push_back(argSpec(arg));
                        }
                    }
                }
                commands.push_back(std::move(info));
            }
        }

        // -- the parameter form --

        // the form's default, which Reset restores: the manifest's, else the type's zero
        static UiFormValue formDefault(const FieldSpec& spec)
        {
            const std::string& text = spec.defaultText;
            if (spec.type == "boolean")
            {
                return UiFormValue::ofBool(text == "true");
            }
            if (spec.type == "enum")
            {
                const auto at = std::ranges::find(spec.choices, text);
                return UiFormValue::ofNumber(at == spec.choices.end() ? 0.0
                                                                      : static_cast<double>(at - spec.choices.begin()));
            }
            if (spec.type == "string")
            {
                return UiFormValue::ofText(text);
            }
            double number = 0.0;
            (void)std::from_chars(text.data(), text.data() + text.size(), number);
            return UiFormValue::ofNumber(std::clamp(number, spec.min.value_or(-1.0e300), spec.max.value_or(1.0e300)));
        }

        // a value the module answered, or a checked field value, as the form shows it; none for a shape the type lacks
        static std::optional<UiFormValue> toFormValue(const FieldSpec& spec, const Json& value)
        {
            if (spec.type == "boolean")
            {
                return value.is_boolean() ? std::optional(UiFormValue::ofBool(value.get<bool>())) : std::nullopt;
            }
            if (spec.type == "enum")
            {
                const auto at =
                    value.is_string() ? std::ranges::find(spec.choices, value.get<std::string>()) : spec.choices.end();
                return at == spec.choices.end()
                    ? std::nullopt
                    : std::optional(UiFormValue::ofNumber(static_cast<double>(at - spec.choices.begin())));
            }
            if (spec.type == "string")
            {
                return UiFormValue::ofText(value.is_string() ? value.get<std::string>() : value.dump());
            }
            if (value.is_number())
            {
                return UiFormValue::ofNumber(value.get<double>());
            }
            double number = 0.0;
            const std::string text = value.is_string() ? value.get<std::string>() : std::string{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
            return !text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
                ? std::optional(UiFormValue::ofNumber(number))
                : std::nullopt;
        }

        // the text checkField and modulesState read a form value as
        static std::string valueText(const FieldSpec& spec, const UiFormValue& value)
        {
            if (spec.type == "boolean")
            {
                return value.number[0] != 0.0 ? "true" : "false";
            }
            if (spec.type == "enum")
            {
                const double at = value.number[0];
                return at >= 0.0 && at < static_cast<double>(spec.choices.size()) ? spec.choices[static_cast<sizet>(at)]
                                                                                  : std::string{};
            }
            return spec.type == "string" ? value.text : numberText(value.number[0]);
        }

        static UiFormField formField(const FieldSpec& spec)
        {
            UiFormField field;
            field.id = InternedString(spec.name);
            field.label = spec.name + rangeHint(spec);
            field.doc = spec.doc;
            field.defaultValue = formDefault(spec);
            if (spec.type == "boolean")
            {
                field.kind = UiFormKind::Bool;
            }
            else if (spec.type == "enum")
            {
                field.kind = UiFormKind::Enum;
                for (const std::string& choice : spec.choices)
                {
                    field.choices.push_back({.label = choice});
                }
            }
            else if (spec.type == "string")
            {
                field.kind = UiFormKind::String;
            }
            else
            {
                field.kind = UiFormKind::Number;
                field.min = spec.min.value_or(field.min);
                field.max = spec.max.value_or(field.max);
                field.decimals = spec.type == "integer" ? 0 : 3;
                const double span = spec.min && spec.max ? *spec.max - *spec.min : 0.0;
                field.step = spec.type == "integer" ? std::max(1.0, std::round(span / 200.0))
                    : span > 0.0                    ? span / 200.0
                                                    : 0.1;
            }
            return field;
        }

        // The form follows the parameters the manifest declares: built again when they differ from what it shows,
        // else only its read-only state (a parameter applies through a command of the loaded module) is written.
        void showParamForm()
        {
            ensureCommandActions();
            resetDefaultsButton.setInteractable(
                detailLoaded &&
                std::ranges::any_of(paramLinks, [](const ParamLink& link)
                                    { return !link.command.empty() && link.refusal.empty(); }));
            formStale = false;
            if (paramSpecs != formSpecs || formFor != selectedPath)
            {
                const bool had = paramForm != nullptr;
                paramForm.reset();
                if (had)
                {
                    scope.commit(); // the old rows die now, so the new form never shows two of each
                }
                formSpecs = paramSpecs;
                formFor = selectedPath; // another module with the same parameters gets its own form, values and errors
                if (paramSpecs.empty())
                {
                    return;
                }
                UiFormDesc desc;
                desc.rect = topLeft();
                desc.rowHeight = density.control + 2.0f * density.hairline;
                desc.gap = density.xs;
                for (const FieldSpec& spec : paramSpecs)
                {
                    desc.fields.push_back(formField(spec));
                }
                paramForm = std::make_unique<PropertyForm>(scope, InternedString("modules.param"), std::move(desc));
                paramForm->root().setParent(paramColumn);
                paramForm->onEdit([this](const UiFormEdit& edit) { paramEdited(edit); });
            }
            for (u32 i = 0; paramForm && i < paramSpecs.size(); ++i)
            {
                paramForm->setReadOnly(InternedString(paramSpecs[i].name),
                                       !detailLoaded || paramLinks[i].command.empty() ||
                                           !paramLinks[i].refusal.empty());
            }
        }

        // current values of the parameters that name a query; a row being edited or showing an error is left alone
        void refreshParamValues()
        {
            if (!detailLoaded || !paramForm)
            {
                return;
            }
            for (u32 i = 0; i < paramSpecs.size(); ++i)
            {
                const InternedString id{paramSpecs[i].name};
                if (paramLinks[i].query.empty() || !paramLinks[i].refusal.empty() || paramForm->isEditing(id) ||
                    !paramForm->state(id).error.empty())
                {
                    continue;
                }
                const Json reply = request(*control, Json{{"query", paramLinks[i].query}});
                if (!reply.value("ok", false) || !reply.contains("result") || !reply["result"].is_object())
                {
                    continue;
                }
                const Json& result = reply["result"];
                const auto value = result.find(paramLinks[i].arg);
                if (value == result.end())
                {
                    continue;
                }
                if (const std::optional<UiFormValue> next = toFormValue(paramSpecs[i], *value);
                    next && paramForm->value(id) != *next)
                {
                    paramForm->setValue(id, *next);
                }
            }
        }

        void selectCommand(const i32 index)
        {
            const i32 next = index >= 0 && index < static_cast<i32>(commands.size()) ? index : -1;
            if (next == selectedCommand)
            {
                return; // choosing the shown command again keeps its typed text and result
            }
            selectedCommand = next;
            output.clear();
            showOutput();
            setText(runStatus, std::string{});
            showCommand(true);
        }

        // The command's argument fields with their texts: the description with what the module declares about the
        // argument (an ArgDesc has no structured range: the description carries it, e.g. "(2..160)"), and the empty
        // field's placeholder, the default.
        std::vector<FieldSpec> argFields(const CommandInfo& info) const
        {
            std::vector<FieldSpec> specs = info.args;
            for (FieldSpec& spec : specs)
            {
                const bool choice = spec.type == "boolean" || spec.type == "enum";
                spec.hint = spec.doc;
                const auto add = [&](const std::string& text) { spec.hint += (spec.hint.empty() ? "" : " ") + text; };
                if (spec.required)
                {
                    add(msg("hint.required"));
                }
                if (choice && !spec.defaultText.empty())
                {
                    add(msg("hint.default", {{"value", std::string_view(spec.defaultText)}}));
                }
                spec.placeholder = !spec.defaultText.empty() ? spec.defaultText
                    : spec.required                          ? msg("placeholder.required")
                                                             : msg("placeholder.optional");
            }
            return specs;
        }

        Button copyResultButton, resetDefaultsButton;
        bool commandActionsReady = false;

        void ensureCommandActions()
        {
            if (commandActionsReady)
            {
                return;
            }
            commandActionsReady = true;
            control->registerViewCommand("moduleCopyResult", "editorModules", "copies the displayed command result", {},
                                         [this](const ControlArgs&, IFormatWriter& out)
                                         {
                                             std::string text;
                                             for (sizet i = 0; i < output.size(); ++i)
                                             {
                                                 text += (i == 0 ? "" : "\n") + output[i];
                                             }
                                             if (text.empty())
                                             {
                                                 return std::string("moduleCopyResult: no command result is displayed");
                                             }
                                             editorUi(*engine).ui().setClipboardText(text);
                                             out.writeString("text", text);
                                             return std::string{};
                                         });
            control->registerViewCommand(
                "moduleResetDefaults", "editorModules", "restores valid manifest defaults on writable parameters", {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    sync();
                    if (!detailLoaded || !paramForm || !manifest.is_object() || !manifest.contains("parameters") ||
                        !manifest["parameters"].is_array())
                    {
                        return std::string("moduleResetDefaults: no writable parameters");
                    }
                    u32 applied = 0;
                    for (u32 i = 0; i < paramSpecs.size(); ++i)
                    {
                        const FieldSpec& spec = paramSpecs[i];
                        const ParamLink& link = paramLinks[i];
                        if (link.command.empty() || !link.refusal.empty() ||
                            paramForm->state(InternedString(spec.name)).readOnly)
                        {
                            continue;
                        }
                        const auto& entries = manifest["parameters"];
                        const auto entry = std::ranges::find_if(
                            entries, [&](const Json& item)
                            { return item.is_object() && strOf(item, "name").value_or("") == spec.name; });
                        if (entry == entries.end() || !entry->contains("default"))
                        {
                            continue;
                        }
                        const Json& value = (*entry)["default"];
                        if ((spec.type == "boolean" && !value.is_boolean()) ||
                            ((spec.type == "number" || spec.type == "integer") && !value.is_number()) ||
                            ((spec.type == "string" || spec.type == "enum") && !value.is_string()) ||
                            (value.is_string() && value.get<std::string>() != spec.defaultText))
                        {
                            continue;
                        }
                        Json checked;
                        if (!checkField(spec, spec.defaultText, checked).empty() || !toFormValue(spec, checked))
                        {
                            continue;
                        }
                        if (fillParam(spec.name, spec.defaultText).empty())
                        {
                            ++applied;
                        }
                    }
                    out.writeU32("applied", applied);
                    return std::string{};
                });
            copyResultButton = scope.createButton("modules.copyResult", "", fitButtonDesc());
            copyResultButton.setParent(runBar);
            copyResultButton.setSize(Vec2{copyResultButton.size().x, density.control});
            copyResultButton.setLocalizedText(InternedString("editor.modules.copyResult"));
            copyResultButton.setInteractable(!output.empty());
            copyResultButton.onClick([this] { queuePlain("moduleCopyResult", Json::object()); });
            resetDefaultsButton = scope.createButton("modules.resetDefaults", "", fitButtonDesc());
            resetDefaultsButton.setParent(paramColumn);
            resetDefaultsButton.setSize(Vec2{resetDefaultsButton.size().x, density.control});
            resetDefaultsButton.setLocalizedText(InternedString("editor.modules.resetDefaults"));
            resetDefaultsButton.onClick([this] { queuePlain("moduleResetDefaults", Json::object()); });
        }

        // `fresh`: another command was selected, so the fields start empty; otherwise typed text stays.
        void showCommand(const bool fresh = false)
        {
            ensureCommandActions();
            std::vector<UiChoiceItem> items;
            for (const CommandInfo& info : commands)
            {
                items.push_back({.label = info.name});
            }
            commandChoice.setItems(items);
            commandChoice.setSelected(selectedCommand);
            args->unsetLabel = msg("unset");
            if (selectedCommand < 0)
            {
                args->setSpecs({});
                setText(commandLine, msg(commands.empty() ? "noCommands" : "noCommand"));
                runButton.setInteractable(false);
                return;
            }
            const CommandInfo& info = commands[static_cast<sizet>(selectedCommand)];
            args->setSpecs(argFields(info), !fresh);
            setText(commandLine,
                    std::format("{} {} - {}{}", info.kind, info.name, info.doc,
                                info.ignoredArgs ? " " + msg("moreArgs", {{"count", std::int64_t{info.ignoredArgs}}})
                                                 : ""));
            runButton.setInteractable(true);
        }

        // -- showing --

        void showStatus()
        {
            const std::int64_t loaded = std::ranges::count(rows, true, &Row::loaded);
            std::string text = message.empty() ? scanIssue() : message;
            if (text.empty())
            {
                const std::int64_t hidden = std::ranges::count_if(rows, [&](const Row& row) { return !isListed(row); });
                text =
                    msg("status", {{"modules", static_cast<std::int64_t>(rows.size()) - hidden}, {"loaded", loaded}});
                if (hidden > 0)
                {
                    text += " " + msg("hidden", {{"count", hidden}});
                }
                if (scanning)
                {
                    text += " " + msg("scanning");
                }
            }
            show(status, text);
        }

        void setText(Label& label, const std::string& text)
        {
            if (label.text() != text)
            {
                label.setText(text);
            }
        }

        // a Details text: hidden while empty, so the column leaves no gap for it
        void setBlock(Label& label, const std::string& text)
        {
            setText(label, text);
            if (label.isVisible() != !text.empty())
            {
                label.setVisible(!text.empty());
            }
        }

        // The Details column is as wide as the scroll view less its bar; the layout stacks and sizes everything in it,
        // and the scroll extent follows its resolved height.
        void relayoutColumn(ScrollView view, Widget column)
        {
            if (!scope.hierarchy().isVisibleOf(view.entity()))
            {
                return; // an inactive tab resolves to width 0: the column would be narrow for a frame when shown
            }
            const float width = std::max(0.0f, resolvedSize(scope, view).x - THUMB);
            place(column, Vec2{}, Vec2{width, 0.0f});
            view.setContentSize(Vec2{width, resolvedSize(scope, column).y});
        }

        // -- build: a view of the editor model's build state --

        static bool engineOwned(const std::string_view stem)
        {
            return std::ranges::any_of(ENGINE_OWNED, [&](const std::string_view name)
                                       { return std::ranges::equal(name, stem, {}, lowerAscii, lowerAscii); });
        }

        static const char* ownedHint(const std::string_view stem)
        {
            return std::ranges::equal(stem, std::string_view("SpiteRenderPipeline"), {}, lowerAscii, lowerAscii)
                ? "build.pipeline"
                : "build.restart";
        }

        // the configuration of the executable's directory (bin/<platform>/<configuration>), which spite build defaults
        // to
        std::string configuration() const { return utf8Path(host->searchDirectory().filename()); }

        std::string ageText(const BuildRecord& record) const
        {
            static constexpr std::array<const char*, 4> KEYS{"build.age.now", "build.age.minutes", "build.age.hours",
                                                             "build.age.days"};
            if (!record.ageSec)
            {
                return {};
            }
            const BuildAge age = buildAgeOf(*record.ageSec);
            return msg(KEYS[static_cast<sizet>(age.unit)], {{"count", std::int64_t{age.count}}});
        }

        bool isBuilding(const BuildRecord& record) const { return record.state == "building" && !record.abandoned; }

        // "failed, 3 min ago": the state the build is in; empty without a record
        std::string stateText(const BuildRecord& record) const
        {
            const char* key = record.state == "building"
                ? (record.abandoned ? "build.state.abandoned" : "build.state.building")
                : record.state == "failed" ? "build.state.failed"
                : record.state == "ok"     ? (record.needsReload ? "build.state.reload" : "build.state.ok")
                                           : nullptr;
            if (!key)
            {
                return {};
            }
            const std::string age = isBuilding(record) ? std::string{} : ageText(record);
            return age.empty() ? msg(key) : msg(key) + ", " + age;
        }

        std::optional<BuildRecord> recordOf(const Row& row) const { return build->record(buildStem(row)); }

        // the table's Build cell: the state in one short word and the age in one unit ("reload 58m"), so it keeps its
        // column in a row the path takes room from; the Build view spells both out
        std::string buildCell(const Row& row) const
        {
            static constexpr std::array<const char*, 4> AGES{"build.cell.now", "build.cell.minutes", "build.cell.hours",
                                                             "build.cell.days"};
            const std::optional<BuildRecord> record = recordOf(row);
            if (!record || (record->state != "ok" && record->state != "failed" && record->state != "building"))
            {
                return {};
            }
            const char* key = record->state == "building"
                ? (record->abandoned ? "build.cell.abandoned" : "build.cell.building")
                : record->state == "failed" ? "build.cell.failed"
                : record->needsReload       ? "build.cell.reload"
                                            : "build.cell.ok";
            if (!record->ageSec || isBuilding(*record))
            {
                return msg(key);
            }
            const BuildAge age = buildAgeOf(*record->ageSec);
            return msg(key) + " " + msg(AGES[static_cast<sizet>(age.unit)], {{"count", std::int64_t{age.count}}});
        }

        // Reload: the selected module, loaded, not being built and not the engine's own DLL
        void showReload()
        {
            const Row* row = selectedRow();
            const std::string stem = row ? buildStem(*row) : std::string{};
            const std::optional<BuildRecord> record = row ? build->record(stem) : std::nullopt;
            const bool building = record && isBuilding(*record);
            const bool owned = engineOwned(stem);
            reloadButton.setInteractable(row && row->loaded && !building && !owned);
            showSelected(reloadButton, row && row->loaded && record && record->needsReload && !building && !owned);
            if (const char* tip = !row ? "editor.tip.modules.reload.none"
                    : !row->loaded     ? "editor.tip.modules.reload.unloaded"
                    : owned            ? "editor.tip.modules.reload.engine"
                    : building         ? "editor.tip.modules.reload.building"
                                       : "editor.tip.modules.reload";
                tip != reloadTip)
            {
                reloadTip = tip;
                reloadHost.setTooltip(InternedString(tip));
            }
            const u32 reloadable = build->reloadableCount();
            const bool busy = build->building();
            reloadBuiltButton.setInteractable(reloadable > 0 && !busy);
            if (const char* tip = busy ? "editor.tip.modules.reloadBuilt.building"
                    : reloadable == 0  ? "editor.tip.modules.reloadBuilt.none"
                                       : "editor.tip.modules.reloadBuilt";
                tip != reloadBuiltTip)
            {
                reloadBuiltTip = tip;
                reloadBuiltHost.setTooltip(InternedString(tip));
            }
            show(buildStatus,
                 !build->hasProjects() ? msg("build.statusEmpty")
                     : busy            ? msg("build.statusBuilding", {{"count", std::int64_t{reloadable}}})
                                       : msg("build.status", {{"count", std::int64_t{reloadable}}}));
            showBuildButton();
        }

        // Build or Cancel for the selected module's project: Cancel while the build the editor started for it runs
        // (until its record shows it finished), else Build, disabled without a project of the executable's directory
        // or while a build of it runs that the editor did not start. Engine DLLs are never rows (ModuleHost::scan
        // leaves out what the process maps), so they get no button; the commands refuse them anyway.
        BuildAction buildAction() const
        {
            const Row* row = selectedRow();
            const std::string stem = row ? buildStem(*row) : std::string{};
            if (stem.empty())
            {
                return {false, false, row ? "editor.tip.modules.build.external" : "editor.tip.modules.build.none"};
            }
            const std::optional<BuildRecord> record = build->record(stem);
            const std::optional<u32> started = build->startedBuild(stem);
            const bool finished = started && record && record->pid == *started && record->state != "building";
            if (started && !finished)
            {
                return {true, true, "editor.tip.modules.cancelBuild"};
            }
            if (record && isBuilding(*record))
            {
                return {false, false, "editor.tip.modules.build.building"};
            }
            return {false, true, "editor.tip.modules.build"};
        }

        // writes only what changed: a quiescent panel calls this on every command and build revision
        void showBuildButton()
        {
            const BuildAction now = buildAction();
            if (now.cancel != shownBuild.cancel || !shownBuild.tip)
            {
                buildButton.setLocalizedText(
                    InternedString(now.cancel ? "editor.modules.cancelBuild" : "editor.modules.buildProject"));
                buildPanelAction.setLocalizedText(
                    InternedString(now.cancel ? "editor.modules.cancelBuild" : "editor.modules.buildProject"));
                // the label is one word; the name says what it acts on
                buildButton.setAccessibleNameKey(InternedString(now.cancel ? "editor.modules.access.cancelBuild"
                                                                           : "editor.modules.access.buildProject"));
                buildPanelAction.setAccessibleNameKey(InternedString(
                    now.cancel ? "editor.modules.access.cancelBuild" : "editor.modules.access.buildProject"));
            }
            // while it is Cancel, update() asks every frame whether the build still runs, so a build that exits
            // without writing a record (killed, or queued and ended) turns the button back
            const Row* row = now.cancel ? selectedRow() : nullptr;
            shownBuildProject = row ? buildStem(*row) : std::string{};
            if (now.enabled != shownBuild.enabled || !shownBuild.tip)
            {
                buildButton.setInteractable(now.enabled);
                buildPanelAction.setInteractable(now.enabled);
            }
            if (now.tip != shownBuild.tip)
            {
                buildHost.setTooltip(InternedString(now.tip));
                buildPanelAction.setTooltip(InternedString(now.tip));
            }
            shownBuild = now;
        }

        void buildOrCancel()
        {
            const BuildAction action = buildAction();
            const Row* row = selectedRow();
            if (!action.enabled || !row)
            {
                return;
            }
            const std::string stem = buildStem(*row);
            queue(Json{{"command", action.cancel ? "cancelBuild" : "buildProject"}, {"args", {{"project", stem}}}},
                  action.cancel ? Kind::CancelBuild : Kind::Build, stem);
            if (!action.cancel)
            {
                queuePlain("buildPanelSelect", {{"project", stem}});
                queuePlain("dock", {{"kind", "show"}, {"panel", "build"}});
            }
        }

        std::unique_ptr<Table> diagnosticTable;
        std::vector<BuildDiagnostic> diagnosticRows;

        void ensureDiagnosticTable()
        {
            if (diagnosticTable)
            {
                return;
            }
            control->registerViewCommand(
                "moduleCopyDiagnostic", "editorModules", "copies a displayed build diagnostic to the clipboard",
                {requiredArg("row", ArgKind::Integer, "diagnostic row in the selected module's build")},
                [this](const ControlArgs& args, IFormatWriter& out)
                {
                    const u64 row = args.integer("row").value_or(~0ull);
                    if (row >= diagnosticRows.size())
                    {
                        return std::string("moduleCopyDiagnostic: diagnostic row is no longer available");
                    }
                    const std::string text = diagnosticText(diagnosticRows[static_cast<sizet>(row)]);
                    editorUi(*engine).ui().setClipboardText(text);
                    out.writeString("text", text);
                    return std::string{};
                });
            diagnosticTable = std::make_unique<Table>(
                scope,
                TableDesc{.id = "modules.diagnostics",
                          .columns = {85.0f, 200.0f},
                          .cells = 3,
                          .buttonWidth = 60.0f,
                          .read =
                              [this](const u32 row, const std::span<std::string> cells, std::string& button)
                          {
                              const BuildDiagnostic& item = diagnosticRows[row];
                              cells[0] = item.severity == "error" ? msg("build.error")
                                  : item.severity == "warning"    ? msg("build.warning")
                                                                  : item.severity;
                              cells[1] = item.file;
                              if (!item.file.empty() && item.line > 0)
                              {
                                  cells[1] += std::format("({})", item.line);
                              }
                              cells[2] = item.code.empty() ? item.message : item.code + ": " + item.message;
                              button = msg("build.copyDiagnostic");
                          },
                          .press = [this](const u32 row) { queuePlain("moduleCopyDiagnostic", {{"row", row}}); },
                          .buttonName = [this](const u32 row)
                          { return msg("build.copyDiagnosticName", {{"row", std::int64_t{row + 1}}}); },
                          .headers = {{InternedString("editor.modules.build.severity"), false},
                                      {InternedString("editor.modules.build.location"), false},
                                      {InternedString("editor.modules.build.message"), false}},
                          .lastMin = 120.0f});
            Widget widget = diagnosticTable->widget();
            widget.setParent(buildColumn);
            widget.setLayoutItem(basisItem(140.0f));
            scope.hierarchy().reorderChild(widget.entity(), 3);
        }

        // The Build view: the empty state, the selected module's build (state, hints, the first diagnostics, the
        // command that builds it) and the projects no row names. Written only when the build state or the
        // selection moved.
        void showBuild()
        {
            const Row* row = selectedRow();
            const std::string stem = row ? buildStem(*row) : std::string{};
            const std::optional<BuildRecord> record = row ? build->record(stem) : std::nullopt;
            const std::string cfg = configuration();
            setBlock(buildNote,
                     build->hasProjects() ? std::string{}
                                          : msg("build.empty", {{"configuration", std::string_view(cfg)}}));
            ensureDiagnosticTable();
            diagnosticRows = record ? record->diagnostics : std::vector<BuildDiagnostic>{};
            diagnosticTable->setRowCount(static_cast<u32>(diagnosticRows.size()));
            diagnosticTable->widget().setVisible(!diagnosticRows.empty());
            diagnosticTable->rebindVisible();
            std::string title, facts, diagnostics, command;
            if (!row)
            {
                facts = msg("build.noneSelected");
            }
            else
            {
                const std::string state = record ? stateText(*record) : std::string{};
                title = std::format("{} - {}", row->name, state.empty() ? msg("build.state.none") : state);
                const auto add = [&](const std::string& text) { facts += (facts.empty() ? "" : " ") + text; };
                if (stem.empty())
                {
                    add(msg("build.external"));
                }
                else
                {
                    command = msg("build.command",
                                  {{"project", std::string_view(stem)}, {"configuration", std::string_view(cfg)}});
                    if (!record || record->state == "none")
                    {
                        add(msg("build.noRecord"));
                    }
                }
                if (record && record->abandoned)
                {
                    add(msg("build.abandoned"));
                }
                if (record && record->untracked)
                {
                    add(msg("build.untracked"));
                }
                if (record && record->needsReload)
                {
                    add(msg("build.needsReload"));
                }
                if (engineOwned(stem))
                {
                    add(msg(ownedHint(stem)));
                }
                if (record)
                {
                    if (record->diagnosticsTotal > record->diagnostics.size())
                    {
                        diagnostics = msg("build.moreDiagnostics",
                                          {{"count",
                                            std::int64_t{record->diagnosticsTotal} -
                                                static_cast<std::int64_t>(record->diagnostics.size())}});
                    }
                    else if (record->state == "failed" && diagnosticRows.empty())
                    {
                        diagnostics = msg("build.noDiagnostics");
                    }
                }
            }
            setBlock(buildTitle, title);
            setBlock(buildFacts, facts);
            setBlock(buildDiagnostics, diagnostics);
            setBlock(buildCommand, command);
            std::vector<std::string> stems;
            stems.reserve(rows.size());
            for (const Row& item : rows)
            {
                stems.push_back(buildStem(item));
            }
            std::string others;
            // what needs a look first: failed, then building or abandoned, then rebuilt and not reloaded
            std::vector<BuildRecord> listed =
                build->others([&](const std::string_view project) { return std::ranges::contains(stems, project); });
            const auto rank = [](const BuildRecord& record)
            {
                return record.state == "failed" ? 0 : record.state == "building" ? 1 : record.needsReload ? 2 : 3;
            };
            std::ranges::stable_sort(listed, {}, rank);
            for (const BuildRecord& other : listed)
            {
                std::string line = other.project + ": " + stateText(other);
                if (!other.diagnostics.empty())
                {
                    line += " - " + cut(diagnosticText(other.diagnostics.front()), 160);
                }
                if (engineOwned(other.project))
                {
                    line += " (" + msg(ownedHint(other.project)) + ")";
                }
                others += (others.empty() ? "" : "\n") + line;
            }
            setBlock(buildOthersTitle, others.empty() ? std::string{} : msg("build.others"));
            setBlock(buildOthers, others);
        }

        // "file(12): error C2065: message"
        std::string diagnosticText(const BuildDiagnostic& item) const
        {
            std::string where = item.file;
            if (!where.empty() && item.line > 0)
            {
                where += std::format("({})", item.line);
            }
            return std::format("{}{}{}{}{}{}", where, where.empty() ? "" : ": ", item.severity,
                               item.code.empty() ? "" : " " + item.code, ": ", item.message);
        }

        void showDetail()
        {
            const Row* row = selectedRow();
            persistBox.setChecked(row && row->persisted);
            persistBox.setInteractable(row != nullptr);
            std::string note;
            if (!row)
            {
                note = msg("noneSelected");
            }
            else if (row->missing)
            {
                note = msg("missingNote");
            }
            else if (!detailLoaded)
            {
                note = msg("loadFirst");
            }
            else if (paramSpecs.empty())
            {
                note = msg("noParams");
            }
            if (ignoredParams)
            {
                note += " " + msg("ignoredParams", {{"count", std::int64_t{ignoredParams}}});
            }
            const auto refused =
                std::ranges::find_if(paramLinks, [](const ParamLink& link) { return !link.refusal.empty(); });
            if (refused != paramLinks.end())
            {
                note += " " +
                    msg("readOnlyParams",
                        {{"count",
                          std::int64_t{std::ranges::count_if(paramLinks, [](const ParamLink& link)
                                                             { return !link.refusal.empty(); })}},
                         {"reason", std::string_view(refused->refusal)}});
            }
            const std::string issue = row ? (manifestError.empty() ? scanIssue() : manifestError) : scanIssue();
            setBlock(paramNote, issue.empty() ? note : issue + " " + note);
            std::string names;
            for (sizet i = 0; i < commands.size() && i < MAX_LISTED_COMMANDS; ++i)
            {
                names += (i ? ", " : "") + commands[i].name;
            }
            setBlock(detailTitle, std::string{});
            setBlock(detailText, row ? row->description : std::string{});
            setBlock(detailMeta,
                     !row              ? std::string{}
                         : row->loaded ? msg("info.pathApi",
                                             {{"path", std::string_view(row->path)},
                                              {"api", std::string_view(std::format("{:016x}", row->apiStamp))}})
                                       : row->path);
            setBlock(detailCommands,
                     commands.empty() ? std::string{}
                                      : msg("detail.commands",
                                            {{"count", static_cast<std::int64_t>(commands.size())},
                                             {"names", std::string_view(names)}}));
            setText(runStatus, row && !detailLoaded ? msg("loadFirst") : std::string{});
            showReload();
            showBuild();
            // dropdown
            std::vector<UiChoiceItem> items;
            for (const Row& item : shownRows)
            {
                items.push_back({.label = label(item)});
            }
            moduleChoice.setItems(items);
            const auto at = row ? std::ranges::find_if(shownRows, [&](const Row& item)
                                                       { return sameModuleSearchPath(item.path, row->path); })
                                : shownRows.end();
            moduleChoice.setSelected(at == shownRows.end() ? -1 : static_cast<i32>(at - shownRows.begin()));
        }

        std::string scanIssue() const
        {
            for (const DirScan& scan : dirs)
            {
                if (!scan.error.empty())
                {
                    return msg("scanFailed",
                               {{"dir", std::string_view(scan.dir)}, {"error", std::string_view(scan.error)}});
                }
                if (scan.truncated)
                {
                    return msg("scanTruncated", {{"dir", std::string_view(scan.dir)}});
                }
            }
            return {};
        }

        // a fixture is listed when loaded, selected or asked for
        bool isListed(const Row& row) const
        {
            return !row.fixture || showFixtures || row.loaded || (!selected.empty() && row.name == selected);
        }

        void showRows()
        {
            SPITE_ZONE("EditorModules.showRows");
            shownRows.clear();
            std::string query = filter;
            std::ranges::transform(query, query.begin(), lowerAscii);
            for (const Row& row : rows)
            {
                std::string searchable = row.name + " " + row.path;
                std::ranges::transform(searchable, searchable.begin(), lowerAscii);
                if (isListed(row) && (query.empty() || searchable.find(query) != std::string::npos))
                {
                    shownRows.push_back(row);
                }
            }
            moduleTable->setRowCount(static_cast<u32>(shownRows.size()));
            listEmpty.setVisible(shownRows.empty() && !scanning);
            listEmpty.setLocalizedText(filter.empty() ? "editor.modules.empty" : "editor.modules.noMatches");
            moduleTable->rebindVisible();
            rowsDirty = false;
            showStatus();
            showDetail();
        }

        void showOutput()
        {
            if (copyResultButton.isValid())
                copyResultButton.setInteractable(!output.empty());
            std::string text;
            for (const std::string& line : output)
            {
                text += line;
                text += '\n';
            }
            if (!text.empty())
            {
                text.pop_back();
            }
            if (outputText.text() != text)
            {
                outputText.setText(text);
            }
        }

        void showBrowse()
        {
            folderTable->setRowCount(static_cast<u32>(folders.size()));
            folderTable->rebindVisible();
            if (!browsePathInput.isValid() || scope.router().focused() != browsePathInput.entity())
            {
                show(browsePathInput, browsePath);
            }
            if (!browseError.empty())
                show(browseStatus, msg("browseFailed", {{"error", std::string_view(browseError)}}));
            else if (browseTruncated)
                show(browseStatus, msg("browseTruncated"));
            else
            {
#if defined(_WIN32)
                show(browseStatus,
                     browsePath.empty() ? msg("browseDrives")
                                        : msg("browseDlls", {{"count", std::int64_t{browseLibraries}}}));
#else
                show(browseStatus, msg("browseLibraries", {{"count", std::int64_t{browseLibraries}}}));
#endif
            }
        }

        void relocalize()
        {
            moduleSearch.setPlaceholder(msg("search"));
            moduleTable->rebindVisible();
            folderTable->rebindVisible();
            showStatus();
            showDetail();
            showCommand();
            showBrowse();
        }

        // -- actions --

        void queue(Json request, const Kind kind, std::string label)
        {
            message.clear();
            pending.push_back({std::move(request), kind, std::move(label), kind == Kind::Param ? selectedPath : ""});
        }

        void queuePlain(const char* command, Json args)
        {
            queue(Json{{"command", command}, {"args", std::move(args)}}, Kind::Panel, command);
        }

        void loadOrUnload(const u32 rowIndex)
        {
            if (rowIndex >= shownRows.size())
            {
                return;
            }
            loadOrUnload(shownRows[rowIndex]);
        }

        void loadOrUnload(const Row& row)
        {
            if (row.missing)
            {
                queue(Json{{"command", "unpersistModule"}, {"args", {{"path", row.path}}}}, Kind::Unpersist, row.name);
            }
            else if (row.loaded)
            {
                queue(Json{{"command", "unloadModule"}, {"args", {{"name", row.name}, {"persist", false}}}},
                      Kind::Unload, row.name);
            }
            else
            {
                queue(Json{{"command", "loadModule"}, {"args", {{"path", row.path}, {"persist", false}}}}, Kind::Load,
                      row.name);
            }
        }

        void reloadSelected()
        {
            if (const Row* row = selectedRow(); row && row->loaded)
            {
                queue(Json{{"command", "reloadModule"}, {"args", {{"name", row->name}}}}, Kind::Reload, row->name);
            }
        }

        // Reload built modules: the model's moduleReloadBuilt, which reloads every module whose DLL was rebuilt
        void reloadBuilt()
        {
            if (build->reloadableCount() > 0 && !build->building())
            {
                queue(Json{{"command", "moduleReloadBuilt"}, {"args", Json::object()}}, Kind::ReloadBuilt,
                      "moduleReloadBuilt");
            }
        }

        // Checks a field's text against its spec; the error, empty when valid. An omitted optional leaves `value` null.
        std::string checkField(const FieldSpec& spec, const std::string& text, Json& value) const
        {
            const std::vector<MessageArg> named{{"name", std::string_view(spec.name)}};
            value = Json();
            if (text.size() > MAX_TEXT_BYTES)
            {
                return msg("error.long", named);
            }
            if (text.empty() ||
                (spec.required &&
                 std::ranges::all_of(text, [](const char c) { return std::isspace(static_cast<unsigned char>(c)); })))
            {
                return spec.required ? msg("error.required", named) : std::string{};
            }
            const auto range = [&](const double number) -> std::string
            {
                if ((spec.min && number < *spec.min) || (spec.max && number > *spec.max))
                {
                    return msg("error.range",
                               {{"name", std::string_view(spec.name)},
                                {"min", std::string_view(spec.min ? numberText(*spec.min) : "")},
                                {"max", std::string_view(spec.max ? numberText(*spec.max) : "")}});
                }
                return {};
            };
            const char* first = text.data();
            const char* last = first + text.size();
            if (spec.type == "number")
            {
                double number = 0.0;
                const auto parsed = std::from_chars(first, last, number);
                if (parsed.ec != std::errc{} || parsed.ptr != last || !std::isfinite(number))
                {
                    return msg("error.number", named);
                }
                if (std::string error = range(number); !error.empty())
                {
                    return error;
                }
                value = number;
            }
            else if (spec.type == "integer" || spec.type == "entity")
            {
                std::int64_t number = 0;
                const auto parsed = std::from_chars(first, last, number);
                if (parsed.ec != std::errc{} || parsed.ptr != last || (spec.type == "entity" && number < 0))
                {
                    return msg("error.integer", named);
                }
                if (std::string error = range(static_cast<double>(number)); !error.empty())
                {
                    return error;
                }
                value = number;
            }
            else if (spec.type == "boolean")
            {
                if (text != "true" && text != "false")
                {
                    return msg("error.boolean", named);
                }
                value = text == "true";
            }
            else if (spec.type == "enum")
            {
                if (!std::ranges::contains(spec.choices, text))
                {
                    return msg("error.choice", named);
                }
                value = text;
            }
            else if (spec.type == "json")
            {
                value = Json::parse(text, nullptr, false);
                if (value.is_discarded())
                {
                    value = Json();
                    return msg("error.json", named);
                }
            }
            else
            {
                value = text;
            }
            return {};
        }

        // An edit the form accepted: the loaded module's own command applies it, a refusal shows under the row.
        void paramEdited(const UiFormEdit& edit)
        {
            const auto at = std::ranges::find(paramSpecs, edit.field.toView(), &FieldSpec::name);
            if (at == paramSpecs.end() || !detailLoaded)
            {
                return;
            }
            const u32 index = static_cast<u32>(at - paramSpecs.begin());
            const ParamLink& link = paramLinks[index];
            if (link.command.empty() || !link.refusal.empty())
            {
                return;
            }
            Json value;
            if (const std::string error = checkField(*at, valueText(*at, edit.value), value); !error.empty())
            {
                paramForm->setError(edit.field, error);
                message = error;
                showStatus();
                return;
            }
            // a drag reports an edit per move: only the last one of a field not yet sent is kept
            std::erase_if(pending,
                          [&](const Action& item)
                          {
                              return item.kind == Kind::Param && item.label == at->name &&
                                  sameModuleSearchPath(item.originPath, selectedPath);
                          });
            queue(Json{{"command", link.command}, {"args", {{link.arg, value}}}}, Kind::Param, at->name);
        }

        // moduleField on a parameter: as if the text had been typed and entered
        std::string fillParam(const std::string& name, const std::string& text)
        {
            const auto at = std::ranges::find(paramSpecs, name, &FieldSpec::name);
            if (at == paramSpecs.end() || !paramForm)
            {
                return "moduleField: no field named " + name;
            }
            const InternedString id{at->name};
            const std::vector<MessageArg> named{{"name", std::string_view(name)}};
            if (paramForm->state(id).readOnly)
            {
                return "moduleField: " + msg("error.readOnly", named);
            }
            Json value;
            std::string error = checkField(*at, text, value);
            const std::optional<UiFormValue> next = error.empty() ? toFormValue(*at, value) : std::nullopt;
            if (!next && error.empty())
            {
                error = msg("error.required", named);
            }
            if (!error.empty())
            {
                paramForm->setError(id, error);
                return "moduleField: " + error;
            }
            return paramForm->commit(id, *next) ? std::string{} : "moduleField: " + name + " was refused";
        }

        void runSelected()
        {
            if (selectedCommand < 0 || !detailLoaded)
            {
                return;
            }
            const CommandInfo& info = commands[static_cast<sizet>(selectedCommand)];
            Json argsObject = Json::object();
            for (u32 i = 0; i < info.args.size(); ++i)
            {
                Json value;
                if (const std::string error = checkField(info.args[i], args->value(i), value); !error.empty())
                {
                    setText(runStatus, error);
                    return;
                }
                if (!value.is_null())
                {
                    argsObject[info.args[i].name] = std::move(value);
                }
            }
            queue(Json{{info.kind == "command" ? "command" : "query", info.name}, {"args", std::move(argsObject)}},
                  Kind::Run, info.name);
        }

        // What a reply shows: a result as formatted JSON (a string as it is), a failure as its error text; capped in
        // bytes and lines. The view wraps the lines.
        void setOutput(const std::string& reply)
        {
            output.clear();
            std::string text;
            const Json parsed = reply.size() > MAX_REPLY_BYTES ? Json() : Json::parse(reply, nullptr, false);
            const auto plain = [](const Json& value)
            { return value.is_string() ? value.get<std::string>() : value.dump(2); };
            if (reply.size() > MAX_REPLY_BYTES)
            {
                text = reply.substr(0, MAX_OUTPUT_BYTES);
            }
            else if (!parsed.is_object())
            {
                text = reply;
            }
            else if (parsed.value("ok", false) && parsed.contains("result"))
            {
                text = plain(parsed["result"]);
            }
            else if (parsed.contains("error"))
            {
                text = plain(parsed["error"]);
            }
            else
            {
                text = parsed.dump(2);
            }
            bool truncated = reply.size() > MAX_REPLY_BYTES || text.size() > MAX_OUTPUT_BYTES;
            if (text.size() > MAX_OUTPUT_BYTES)
            {
                text.resize(MAX_OUTPUT_BYTES);
            }
            for (sizet at = 0; at < text.size();)
            {
                if (output.size() >= MAX_OUTPUT_LINES)
                {
                    truncated = true;
                    break;
                }
                const sizet end = std::min(text.find('\n', at), text.size());
                output.push_back(text.substr(at, end - at));
                at = end + 1;
            }
            if (truncated)
            {
                output.push_back(msg("outputTruncated"));
            }
            showOutput();
        }

        // Decision: a load or unload swaps a module image (waitIdle; components, UI reactions and handlers other
        // panels and the frame still hold), so it runs where a client's request does, in the control pump at the
        // start of the next frame, not inside this one; the other actions run here.
        void dispatchPending()
        {
            SPITE_ZONE("EditorModules.dispatchPending");
            std::vector<Action> todo;
            todo.swap(pending);
            for (Action& action : todo)
            {
                ++actions;
                if (action.kind == Kind::Load || action.kind == Kind::Unload || action.kind == Kind::Reload ||
                    action.kind == Kind::ReloadBuilt)
                {
                    std::string line = action.request.dump();
                    control->runAtFrameStart("editorModules", std::move(line),
                                             [this, action = std::move(action)](const std::string& reply)
                                             { settle(action, reply); });
                    continue;
                }
                std::string reply;
                {
                    SPITE_ZONE("EditorModules.executeCommand");
                    reply = control->handle(action.request.dump());
                }
                {
                    SPITE_ZONE("EditorModules.settleReply");
                    settle(action, reply);
                }
            }
        }

        void settle(const Action& action, const std::string& reply)
        {
            const Json parsed = Json::parse(reply, nullptr, false);
            const bool ok = parsed.is_object() && parsed.value("ok", false);
            const std::string error = ok
                ? std::string{}
                : cut(parsed.is_object() && parsed.contains("error") && parsed["error"].is_string()
                          ? parsed["error"].get<std::string>()
                          : std::string(reply),
                      MAX_ERROR_BYTES);
            const std::vector<MessageArg> named{{"name", std::string_view(action.label)},
                                                {"error", std::string_view(error)}};
            bool refused = false; // the command ran and stopped at a module that refused
            switch (action.kind)
            {
            case Kind::Load:
                message = msg(ok ? "done.load" : "failed", named);
                break;
            case Kind::Unload:
                message = msg(ok ? "done.unload" : "failed", named);
                break;
            case Kind::Persist:
                message = msg(ok ? "done.persist" : "failed", named);
                loadedStamp = ~0ull;
                break;
            case Kind::Unpersist:
                {
                    // removed 0: the list no longer named it (edited since the rows were read)
                    const Json result = ok && parsed.contains("result") ? parsed["result"] : Json::object();
                    message = msg(!ok                                  ? "failed"
                                      : result.value("removed", 0) > 0 ? "done.unpersist"
                                                                       : "done.unpersistNone",
                                  named);
                    loadedStamp = ~0ull; // the list may have changed: the rows are read again
                    break;
                }
            case Kind::Reload:
                {
                    const Json result = ok && parsed.contains("result") ? parsed["result"] : Json::object();
                    message =
                        msg(ok ? "done.reload" : "failed",
                            {{"name", std::string_view(action.label)},
                             {"error", std::string_view(error)},
                             {"ms", std::round(result.value("ms", 0.0) * 10.0) / 10.0},
                             {"restored", std::int64_t{result.value("restored", 0)}},
                             {"warnings",
                              static_cast<std::int64_t>(result.contains("warnings") ? result["warnings"].size() : 0)}});
                    break;
                }
            case Kind::ReloadBuilt:
                {
                    // the command answers ok with the module it stopped at in `failed`
                    const Json result = ok && parsed.contains("result") ? parsed["result"] : Json::object();
                    const Json stopped = result.value("failed", Json());
                    std::string names;
                    for (const Json& name : result.value("reloaded", Json::array()))
                    {
                        names += (names.empty() ? "" : ", ") + name.get<std::string>();
                    }
                    refused = stopped.is_object();
                    message = refused
                        ? msg(names.empty() ? "done.reloadBuiltStopped" : "done.reloadBuiltStoppedAfter",
                              {{"name", std::string_view(stopped.value("name", std::string{}))},
                               {"error",
                                std::string_view(cut(stopped.value("message", std::string{}), MAX_ERROR_BYTES))},
                               {"names", std::string_view(names)}})
                        : names.empty() ? msg("done.reloadBuiltNone")
                                        : msg("done.reloadBuilt", {{"names", std::string_view(names)}});
                    if (!ok)
                    {
                        message = msg("failed", named);
                    }
                    break;
                }
            case Kind::Build:
                {
                    // started: the build state's toast says when it finishes; a refusal says why it did not start
                    const Json started =
                        ok && parsed.contains("result") ? parsed["result"].value("started", Json()) : Json();
                    // the pid as text: a number argument would be shown with digit grouping
                    const std::string pid = std::to_string(started.is_object() ? started.value("pid", 0u) : 0u);
                    const std::string script = started.is_object() ? started.value("script", std::string{}) : "";
                    // a script SPITE_BUILD_SCRIPT named in place of Build.ps1 is said, never silent
                    const bool overridden = started.is_object() && started.value("scriptOverride", false);
                    message = !ok ? msg("failed", named)
                                  : msg(overridden ? "done.buildScript" : "done.build",
                                        {{"name", std::string_view(action.label)},
                                         {"pid", std::string_view(pid)},
                                         {"script", std::string_view(script)}});
                    showBuildButton();
                    break;
                }
            case Kind::CancelBuild:
                {
                    const Json cancelled =
                        ok && parsed.contains("result") ? parsed["result"].value("cancelled", Json()) : Json();
                    message = !ok ? msg("failed", named)
                        : cancelled.is_object() && cancelled.value("ended", false)
                        ? msg("done.cancelBuild", named)
                        : msg("done.cancelBuildPending", named);
                    build->refreshNow(); // the record now names a dead pid: abandoned, so Build is offered again
                    showBuildButton();
                    break;
                }
            case Kind::Param:
                message = msg(ok ? "done.apply" : "failed", named);
                if (!detailStale && detailFor == selectedPath && sameModuleSearchPath(action.originPath, selectedPath))
                {
                    if (paramForm && paramForm->has(InternedString(action.label)))
                    {
                        paramForm->setError(InternedString(action.label), error);
                    }
                    if (ok)
                    {
                        refreshParamValues();
                    }
                }
                break;
            case Kind::Run:
                setOutput(reply);
                setText(runStatus, msg(ok ? "done.run" : "failed", named));
                break;
            case Kind::Panel:
                if (!ok)
                {
                    message = msg("failed", named);
                }
                break;
            }
            if (!ok)
            {
                feedback->postToast(ToastLevel::Error, msg("failed", named));
            }
            else if (refused)
            {
                feedback->postToast(ToastLevel::Error, message);
            }
            else if (action.kind == Kind::Load || action.kind == Kind::Unload || action.kind == Kind::Persist ||
                     action.kind == Kind::Unpersist || action.kind == Kind::Reload || action.kind == Kind::ReloadBuilt)
            {
                feedback->postToast(ToastLevel::Info, message);
            }
            showStatus();
        }

        // -- query --

        // what a command or query must see: the rows and the detail as the host is now, not as the last frame left them
        void sync()
        {
            if (loadedStamp != stampOf(*host))
            {
                rebuildRows();
            }
            if (detailStale || detailFor != selectedPath)
            {
                refreshDetail();
            }
            else if (formStale)
            {
                showParamForm();
            }
        }

        // the project a row builds as: its DLL's name when the DLL sits beside the executable (the only directory
        // Build.ps1 records), else none
        std::string buildStem(const Row& row) const
        {
            std::string hostDir;
            (void)tryPathToUtf8(host->searchDirectory(), hostDir);
            return sameModuleSearchPath(row.dir, hostDir) ? utf8Path(pathFromUtf8(row.path).stem()) : std::string{};
        }

        void writeState(IFormatWriter& out)
        {
            sync();
            build->refreshNow();
            out.writeString("view", VIEW_NAMES[view]);
            out.writeString("selected", selected);
            out.writeString("selectedPath", selectedPath);
            out.writeString("filter", filter);
            out.writeBool("compact", narrow);
            out.writeBool("detailVisible", !narrow || detailVisible);
            out.writeBool("activityExpanded", shown(scope, views[ViewBuild]));
            out.writeBool("scanning", scanning);
            out.writeBool("browsing", browsing);
            out.writeBool("showFixtures", showFixtures);
            out.writeBool("persist", selectedRow() && selectedRow()->persisted);
            out.beginArray("persisted", static_cast<u32>(persistedEntries.size()));
            for (const std::string& entry : persistedEntries)
            {
                out.writeString("", entry);
            }
            out.endArray();
            out.writeString("message", message);
            out.beginArray("directories", static_cast<u32>(dirs.size()));
            for (const DirScan& dir : dirs)
            {
                out.beginObject("");
                out.writeString("dir", dir.dir);
                out.writeString("error", dir.error);
                out.writeU32("skipped", dir.skipped);
                out.writeBool("truncated", dir.truncated);
                out.endObject();
            }
            out.endArray();
            out.beginArray("modules", static_cast<u32>(rows.size()));
            for (const Row& row : rows)
            {
                out.beginObject("");
                out.writeString("name", row.name);
                out.writeString("displayName", row.displayName);
                out.writeString("description", row.description);
                out.writeString("path", row.path);
                out.writeString("dir", row.dir);
                out.writeBool("loaded", row.loaded);
                out.writeBool("duplicate", row.duplicate);
                out.writeBool("fixture", row.fixture);
                out.writeBool("persisted", row.persisted);
                out.writeBool("missing", row.missing);
                out.writeBool("listed", isListed(row));
                out.writeString("apiStamp", row.apiStamp == 0 ? std::string{} : std::format("{:016x}", row.apiStamp));
                out.writeString("manifestError", row.manifestError);
                build->writeModule(out, buildStem(row));
                out.endObject();
            }
            out.endArray();
            build->writeOthers(
                out, [this](const std::string_view stem)
                { return std::ranges::any_of(rows, [&](const Row& row) { return buildStem(row) == stem; }); });
            out.writeU32("reloadable", build->reloadableCount());
            out.writeBool("reloadBuiltEnabled", build->reloadableCount() > 0 && !build->building());
            const BuildAction action = buildAction();
            out.beginObject("buildAction");
            out.writeString("kind", action.cancel ? "cancelBuild" : "buildProject");
            out.writeBool("enabled", action.enabled);
            const Row* selectedModule = selectedRow();
            out.writeString("project", selectedModule ? buildStem(*selectedModule) : std::string{});
            const std::optional<u32> pid =
                selectedModule ? build->startedBuild(buildStem(*selectedModule)) : std::nullopt;
            if (pid)
            {
                out.writeU32("pid", *pid);
            }
            else
            {
                out.writeNull("pid");
            }
            out.endObject();
            out.beginArray("searchPaths", static_cast<u32>(settings->moduleSearchPaths().size()));
            for (const std::string& path : settings->moduleSearchPaths())
            {
                out.writeString("", path);
            }
            out.endArray();
            out.beginObject("browse");
            out.writeString("path", browsePath);
            out.writeString("error", browseError);
            out.writeBool("truncated", browseTruncated);
            out.writeU32("folderCount", static_cast<u32>(folders.size()));
            out.writeU32("dllCount", browseLibraries);
            out.beginArray("folders", static_cast<u32>(std::min<sizet>(folders.size(), 50)));
            for (sizet i = 0; i < std::min<sizet>(folders.size(), 50); ++i)
            {
                out.writeString("", folders[i]);
            }
            out.endArray();
            out.endObject();
            out.writeString("manifestError", manifestError);
            out.writeU32("ignoredParameters", ignoredParams);
            out.beginArray("parameters", static_cast<u32>(paramSpecs.size()));
            for (u32 i = 0; i < paramSpecs.size(); ++i)
            {
                out.beginObject("");
                out.writeString("name", paramSpecs[i].name);
                out.writeString("type", paramSpecs[i].type);
                const InternedString id{paramSpecs[i].name};
                out.writeString("value", paramForm ? valueText(paramSpecs[i], paramForm->value(id)) : std::string{});
                out.writeString("error", paramForm ? paramForm->state(id).error : std::string{});
                out.writeString("command", paramLinks[i].command);
                out.writeString("arg", paramLinks[i].arg);
                out.writeString("query", paramLinks[i].query);
                out.writeString("readOnlyReason", paramLinks[i].refusal);
                out.writeBool("required", paramSpecs[i].required);
                if (paramSpecs[i].min)
                {
                    out.writeF64("min", *paramSpecs[i].min);
                }
                if (paramSpecs[i].max)
                {
                    out.writeF64("max", *paramSpecs[i].max);
                }
                out.endObject();
            }
            out.endArray();
            out.beginArray("commands", static_cast<u32>(commands.size()));
            for (const CommandInfo& info : commands)
            {
                out.beginObject("");
                out.writeString("name", info.name);
                out.writeString("kind", info.kind);
                out.writeU32("args", static_cast<u32>(info.args.size()));
                out.endObject();
            }
            out.endArray();
            out.writeString("selectedCommand",
                            selectedCommand >= 0 ? commands[static_cast<sizet>(selectedCommand)].name : std::string{});
            out.beginArray("fields", args->count());
            for (u32 i = 0; i < args->count(); ++i)
            {
                out.beginObject("");
                out.writeString("name", args->spec(i).name);
                out.writeString("type", args->spec(i).type);
                out.writeBool("required", args->spec(i).required);
                out.writeString("value", args->value(i));
                out.writeString("default", args->spec(i).defaultText);
                out.writeString("placeholder", args->spec(i).placeholder);
                out.writeString("hint", args->spec(i).hint);
                out.endObject();
            }
            out.endArray();
            out.beginArray("output", static_cast<u32>(output.size()));
            for (const std::string& line : output)
            {
                out.writeString("", line);
            }
            out.endArray();
            out.beginObject("moduleList");
            writeTableStats(out, *moduleTable);
            out.endObject();
            out.writeU64("scans", scans);
            out.writeU64("listings", listings);
            out.writeU64("detailReads", refreshes);
            out.writeU64("entryReads", entryReads);
            out.writeU64("actions", actions);
            out.writeU64("moduleBinds", moduleTable->binds);
        }

        // -- commands --

        void registerCommands()
        {
            control->registerViewCommand("moduleReveal", "editorModules",
                                         "Reveals the selected DLL in the file manager", {},
                                         [this](const ControlArgs&, IFormatWriter&)
                                         {
                                             const auto* row = selectedRow();
                                             if (!row || row->missing)
                                                 return std::string("no existing module selected");
                                             return revealInFileManager(pathFromUtf8(row->path))
                                                 ? std::string{}
                                                 : std::string("could not open file manager");
                                         });
            control->registerViewCommand("moduleCopyPath", "editorModules", "Copies the selected module's file path",
                                         {},
                                         [this](const ControlArgs&, IFormatWriter& out)
                                         {
                                             const auto* row = selectedRow();
                                             if (!row)
                                                 return std::string("no module selected");
                                             editorUi(*engine).ui().setClipboardText(row->path);
                                             out.writeString("path", row->path);
                                             return std::string{};
                                         });
            control->registerViewCommand("moduleFilter", "editorModules", "Filters the module list by name or path",
                                         {optionalArg("text", ArgKind::String, "search text")},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         {
                                             filter = args.string("text").value_or("");
                                             if (moduleSearch.value() != filter)
                                                 moduleSearch.setText(filter);
                                             rowsDirty = true;
                                             return std::string{};
                                         });
            control->registerViewCommand("moduleBrowser", "editorModules", "Returns to the module list", {},
                                         [this](const ControlArgs&, IFormatWriter&)
                                         {
                                             detailVisible = false;
                                             view = ViewModules;
                                             tabs.setActive(0);
                                             return std::string{};
                                         });
            control->registerViewCommand(
                "moduleActivity", "editorModules", "Shows or hides the separate Build panel",
                {requiredArg("open", ArgKind::Boolean, "whether the Build panel is open")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    queuePlain("dock",
                               {{"kind", args.boolean("open").value_or(false) ? "show" : "close"}, {"panel", "build"}});
                    return std::string{};
                });
            control->registerViewCommand(
                "modulesView", "editorModules", "shows one of the module browser's views",
                {requiredArg(
                     "view", ArgKind::String,
                     "modules, build (the build status and Reload built modules), parameters (the module's detail), "
                     "commands or browse",
                     VIEW_NAMES),
                 optionalArg("detail", ArgKind::Boolean, "show Overview instead of the compact module list", "false")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const std::string name = args.string("view").value_or("");
                    const auto it = std::ranges::find(VIEW_NAMES, name);
                    if (it - VIEW_NAMES.begin() == ViewBuild)
                    {
                        if (const auto* row = selectedRow())
                            queuePlain("buildPanelSelect", {{"project", buildStem(*row)}});
                        queuePlain("dock", {{"kind", "show"}, {"panel", "build"}});
                        return std::string{};
                    }
                    view = static_cast<View>(it - VIEW_NAMES.begin());
                    detailVisible = view != ViewModules || args.boolean("detail").value_or(false);
                    tabs.setActive(view == ViewParameters ? 1 : view == ViewCommands ? 2 : 0);
                    return std::string{};
                });
            control->registerViewCommand("moduleSelect", "editorModules",
                                         "selects a listed module, as modulesState lists it; two files may share a "
                                         "name, the path tells them apart",
                                         {optionalArg("name", ArgKind::String, "a module name"),
                                          optionalPath("path", HOST_ROOT, "the module file, as modulesState lists it")},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         {
                                             const std::string name = args.string("name").value_or("");
                                             const std::string path = args.string("path").value_or("");
                                             if (name.empty() && path.empty())
                                             {
                                                 return std::string("moduleSelect needs a name or a path");
                                             }
                                             if (!selectByNameOrPath(name, path))
                                             {
                                                 std::string known;
                                                 for (sizet i = 0; i < rows.size() && i < 10; ++i)
                                                 {
                                                     known += (known.empty() ? "" : ", ") + rows[i].name;
                                                 }
                                                 return std::format("moduleSelect: {} is not listed; listed: {}",
                                                                    path.empty() ? name : path,
                                                                    known.empty() ? "none" : known);
                                             }
                                             return std::string{};
                                         });
            control->registerViewCommand(
                "moduleOptions", "editorModules",
                "sets the module list's options: fixtures and the selected module's startup persistence",
                {optionalArg("fixtures", ArgKind::Boolean, "list the test fixtures"),
                 optionalArg("persist", ArgKind::Boolean, "load the selected module on startup")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    showFixtures = args.boolean("fixtures").value_or(showFixtures);
                    if (const auto value = args.boolean("persist"))
                    {
                        const Row* row = selectedRow();
                        if (!row)
                        {
                            return std::string("moduleOptions persist needs a selected module");
                        }
                        if (*value != row->persisted)
                        {
                            queue(Json{{"command", *value ? "persistModule" : "unpersistModule"},
                                       {"args", {{"path", row->path}}}},
                                  *value ? Kind::Persist : Kind::Unpersist, row->name);
                        }
                    }
                    rowsDirty = true;
                    fixtureBox.setChecked(showFixtures);
                    return std::string{};
                });
            control->registerViewCommand(
                "moduleRefresh", "editorModules",
                "scans the module directories again (the lists are read once, then on request) and reads the build "
                "status files again",
                {},
                [this](const ControlArgs&, IFormatWriter&)
                {
                    rescan = true;
                    build->refreshNow();
                    return std::string{};
                });
            control->registerViewCommand(
                "moduleBrowse", "editorModules",
                "lists the sub-directories of an absolute directory in the browse view (links are skipped); no path: "
                "the filesystem roots",
                {optionalPath("path", HOST_ROOT, "an absolute directory", "the filesystem roots")},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    const std::string text = args.string("path").value_or("");
                    std::string error;
                    const std::string path = text.empty() ? std::string{} : normalizeModuleSearchPath(text, error);
                    if (!text.empty() && path.empty())
                    {
                        return "moduleBrowse: " + error;
                    }
                    browsePath = path;
                    startBrowse();
                    return std::string{};
                });
            control->registerViewCommand("moduleCommandSelect", "editorModules",
                                         "selects a command or query of the selected module",
                                         {requiredArg("name", ArgKind::String, "a name modulesState.commands lists")},
                                         [this](const ControlArgs& args, IFormatWriter&)
                                         {
                                             sync();
                                             const std::string name = args.string("name").value_or("");
                                             const auto it = std::ranges::find(commands, name, &CommandInfo::name);
                                             if (it == commands.end() && !selected.empty() && !detailLoaded)
                                             {
                                                 return std::format("moduleCommandSelect: {} is not loaded: load it "
                                                                    "first (loadModule), "
                                                                    "then choose {}",
                                                                    selected, name);
                                             }
                                             if (it == commands.end())
                                             {
                                                 return std::format("moduleCommandSelect: {} is not a command of {}",
                                                                    name, selected.empty() ? "(no module)" : selected);
                                             }
                                             selectCommand(static_cast<i32>(it - commands.begin()));
                                             return std::string{};
                                         });
            control->registerViewCommand(
                "moduleField", "editorModules",
                "enters a value in a parameter row of the Details view (applied through the module's command, as if "
                "typed and confirmed) or types into one of the selected command's arguments",
                {requiredArg("name", ArgKind::String, "a parameter or argument name"),
                 requiredArg("value", ArgKind::String, "the field's text"),
                 optionalArg("scope", ArgKind::String, "parameter or argument", "parameter", FIELD_SCOPES)},
                [this](const ControlArgs& args, IFormatWriter&)
                {
                    sync();
                    const std::string name = args.string("name").value_or("");
                    const std::string text = args.string("value").value_or("");
                    if (args.string("scope").value_or("parameter") != "argument")
                    {
                        return fillParam(name, text);
                    }
                    for (u32 i = 0; i < this->args->count(); ++i)
                    {
                        if (this->args->spec(i).name == name)
                        {
                            this->args->setValue(i, text);
                            return std::string{};
                        }
                    }
                    return "moduleField: no field named " + name;
                });
            control->registerQuery(
                "modulesState", "editorModules",
                "the module browser (views modules, build, parameters, commands, browse): modules with loaded state, "
                "scan problems, search paths, the "
                "browse listing (path, folders, dllCount: the native library files in the directory), the "
                "selected module's parameters, commands, argument fields and output, and counters. A module row "
                "with missing true names a file only the user config's module list names (in no scanned directory, "
                "not loaded; its button is Remove, the unpersistModule command). Each module row "
                "also "
                "carries its project's last build: buildState (ok, failed, building or none), "
                "needsReload (the DLL was rebuilt since it loaded), untracked (built by something other than "
                "Build.ps1), abandoned (a build that died), ageSec (since it finished, null without a "
                "record) and buildDiagnostics (the first three: file, line, severity, code, message) of "
                "buildDiagnosticsTotal; otherBuilds lists the projects no row names (project, buildState, "
                "abandoned, ageSec, buildDiagnostics); reloadable counts the loaded modules moduleReloadBuilt would "
                "reload and reloadBuiltEnabled says whether the panel's Reload built modules button is enabled "
                "(something to reload, no build running); buildAction is the Build view's Build button for the "
                "selected module: kind (buildProject, or cancelBuild while the build the editor started for it "
                "runs), enabled (false without a project beside the executable or while a build of it runs that the "
                "editor did not start), project and pid (the editor's running build, else null)",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    writeState(out);
                    return std::string{};
                });
        }

        static constexpr std::array<std::string_view, 2> FIELD_SCOPES{"parameter", "argument"};

        bool selectByNameOrPath(const std::string& name, const std::string& path)
        {
            if (loadedStamp != stampOf(*host))
            {
                rebuildRows();
            }
            const auto it = std::ranges::find_if(rows,
                                                 [&](const Row& row)
                                                 {
                                                     return (name.empty() || row.name == name) &&
                                                         (path.empty() || sameModuleSearchPath(row.path, path));
                                                 });
            if (it == rows.end())
            {
                return false;
            }
            select(*it);
            return true;
        }
    };

    namespace
    {
        // the localized key of a text widget
        void localized(TextWidget widget, const char* key) { widget.setLocalizedText(InternedString(key)); }
    } // namespace

    EditorModules::EditorModules(Engine& engine, const EditorSettingsService& settings, EditorFeedback& feedback,
                                 EditorBuildState& build) :
        m_state(std::make_unique<State>(engine, settings, feedback, build))
    {
        State& state = *m_state;
        UiScope& scope = state.scope;
        const auto viewDesc = [&state]
        {
            UiRectDesc desc = columnDesc(state.density.xs);
            desc.layoutItem = fillItem();
            return desc;
        };
        const auto grow = [](auto& widget) { widget.setLayoutItem(fillItem()); };
        // the heights of the controls, lines and bars below are applyDensity's
        const auto line = [&](const char* id, const Widget& parent)
        {
            TextInput text = readOnlyText(scope, InternedString(id), barDesc(Vec2{}));
            text.setParent(parent);
            return text;
        };

        state.root = scope.createPanel("editor.panel", columnDesc());
        UiDropdownDesc dropdownDesc;
        dropdownDesc.rect = barDesc(Vec2{240.0f, 0.0f});
        dropdownDesc.searchable = true;
        state.moduleChoice = scope.createDropdown("modules.choice", dropdownDesc);
        state.moduleChoice.setVisible(false);
        state.moduleChoice.setAccessibleNameKey("editor.modules.access.module");
        state.moduleChoice.setPlaceholderKey(InternedString("editor.modules.choice.none"));
        state.moduleChoice.onChange(
            [&state](const u32 index)
            {
                if (index < state.shownRows.size())
                {
                    state.queuePlain("moduleSelect",
                                     {{"name", state.shownRows[index].name}, {"path", state.shownRows[index].path}});
                }
            });
        state.refreshButton = scope.createButton("modules.refresh", "", fitButtonDesc());
        localized(state.refreshButton, "editor.modules.refresh");
        state.refreshButton.onClick([&state] { state.queuePlain("moduleRefresh", Json::object()); });
        state.status = readOnlyText(scope, "modules.status", topLeft());
        state.status.setAccessibleNameKey("editor.modules.access.status");
        state.status.setLayoutItem(fillItem());

        state.tabs = scope.createTabBar("modules.tabs");
        state.tabs.setLayoutItem(fixedItem());

        // the modules view: the list and the actions on the selected module
        state.workspace = scope.createPanel(
            "modules.workspace",
            barDesc(Vec2{}, stack(UiLayoutDirection::Row, state.density.md, {}, UiLayoutAlign::Stretch)));
        state.workspace.setLayoutItem(fillItem());
        state.browserPane = scope.createPanel("modules.browser", columnDesc(state.density.xs));
        state.browserPane.setParent(state.workspace);
        state.detailPane = scope.createPanel("modules.selected", columnDesc(state.density.xs));
        state.detailPane.setLayoutItem(fillItem());
        state.detailPane.setParent(state.workspace);
        state.moduleSearch = scope.createTextInput("modules.search", barDesc(Vec2{0, state.density.control}));
        bindInputPadding(state.moduleSearch);
        state.moduleSearch.setLayoutItem(fixedItem());
        state.moduleSearch.setParent(state.browserPane);
        state.moduleSearch.setAccessibleNameKey("editor.modules.search");
        state.moduleSearch.setPlaceholder(state.msg("search"));
        state.moduleSearch.onChange([&state](std::string_view text)
                                    { state.queuePlain("moduleFilter", {{"text", std::string(text)}}); });
        state.listEmpty = scope.createLabel("modules.empty", "", barDesc(Vec2{0, state.density.row * 2}));
        state.listEmpty.setParent(state.browserPane);
        state.listEmpty.setVisible(false);
        state.backButton = scope.createButton("modules.back", "", fitButtonDesc());
        localized(state.backButton, "editor.modules.back");
        state.backButton.onClick([&state] { state.queuePlain("moduleBrowser", Json::object()); });
        state.foldersButton = scope.createButton("modules.manageFolders", "", fitButtonDesc());
        localized(state.foldersButton, "editor.modules.manageFolders");
        state.foldersButton.onClick([&state] { state.queuePlain("modulesView", {{"view", "browse"}}); });
        state.selectionTitle = scope.createLabel("modules.selection.title", "", barDesc(Vec2{0, state.density.row}));
        state.selectionTitle.setStyleClass("modules.detail.title");
        state.selectionTitle.setParent(state.detailPane);
        state.views[ViewModules] = scope.createPanel("modules.view.modules", viewDesc());
        state.moduleTable = std::make_unique<Table>(
            scope,
            TableDesc{.id = "modules",
                      .columns = {140.0f},
                      .cells = 2,
                      .read =
                          [&state](const u32 row, const std::span<std::string> cells, std::string& button)
                      {
                          const State::Row& item = state.shownRows[row];
                          cells[0] = state.label(item);
                          cells[1] = state.moduleStatus(item);
                      },
                      .select =
                          [&state](const u32 row)
                      {
                          state.queuePlain("moduleSelect",
                                           {{"name", state.shownRows[row].name}, {"path", state.shownRows[row].path}});
                      },
                      .cellNameKeys = {"editor.modules.access.cell.name", "editor.modules.access.cell.state"},
                      .lastMin = 110.0f,
                      .ellipsizeLast = true,
                      .isSelected = [&state](u32 row) { return state.shownRows[row].path == state.selectedPath; }});
        state.moduleTable->list.setParent(state.browserPane);
        grow(state.moduleTable->list);
        UiRectDesc actionDesc = barDesc(Vec2{}, stack(UiLayoutDirection::Row, state.density.xs));
        actionDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true}; // as tall as its tallest
        state.actionBar = scope.createPanel("modules.actionBar", actionDesc);
        bindGap(state.actionBar, SPACE_XS);
        state.actionBar.setParent(state.detailPane);
        auto actionsLayout = stack(UiLayoutDirection::Row, state.density.xs);
        actionsLayout.wrap = true;
        state.actionBar.setLayout(actionsLayout);
        state.loadButton = scope.createButton("modules.loadSelected", "", fitButtonDesc());
        state.loadButton.setParent(state.actionBar);
        state.loadButton.onClick(
            [&state]
            {
                if (const auto* row = state.selectedRow())
                    state.loadOrUnload(*row);
            });
        state.copyPathButton = scope.createButton("modules.copyPath", "", fitButtonDesc());
        state.copyPathButton.setParent(state.actionBar);
        localized(state.copyPathButton, "editor.modules.copyPath");
        state.copyPathButton.onClick([&state] { state.queuePlain("moduleCopyPath", Json::object()); });
        UiRectDesc reloadHostDesc = barDesc(Vec2{}, stack(UiLayoutDirection::Row, 0.0f));
        reloadHostDesc.contentSize = UiContentSizeComponent{.fitWidth = true, .fitHeight = true};
        state.reloadHost = scope.createPanel("modules.reloadHost", reloadHostDesc);
        state.reloadHost.setParent(state.actionBar);
        state.reloadButton = scope.createButton("modules.reload", "", fitButtonDesc());
        state.reloadButton.setParent(state.reloadHost);
        localized(state.reloadButton, "editor.modules.reload");
        state.reloadButton.onClick([&state] { state.reloadSelected(); });
        const auto option = [&](Checkbox& box, const char* id, const char* key, const char* argument)
        {
            box = scope.createCheckbox(id, "", barDesc(Vec2{}));
            box.setParent(state.actionBar);
            localized(box.label(), key);
            setTip(box);
            box.onChange([&state, argument](const bool on) { state.queuePlain("moduleOptions", {{argument, on}}); });
        };
        option(state.persistBox, "modules.persist", "editor.modules.persist", "persist");
        option(state.fixtureBox, "modules.fixtures", "editor.modules.fixtures", "fixtures");

        // the parameters view: the descriptor, then the parameters, in one scrolling form
        state.views[ViewParameters] = scope.createPanel("modules.view.parameters", viewDesc());
        UiRectDesc scrollDesc = topLeft();
        scrollDesc.layoutItem = fillItem();
        UiRectDesc blockDesc = topLeft();
        blockDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
        // a wrapped text of a form's head or tail
        const auto block = [&](const char* id, const Widget& parent, const char* styleClass)
        {
            Label text = scope.createLabel(InternedString(id), "", blockDesc);
            text.setParent(parent);
            text.setStyleClass(InternedString(styleClass));
            text.setVisible(false);
            return text;
        };
        // a scrolling column of wrapped texts, as wide as its scroll view less the bar
        const auto textColumn = [&](const char* viewId, const char* id, ScrollView& view, const Widget& parent)
        {
            view = scope.createScrollView(InternedString(viewId), scrollDesc);
            view.setParent(parent);
            UiRectDesc contentDesc = topLeft();
            contentDesc.layout = stack(UiLayoutDirection::Column, 0.0f, Vec4{}, UiLayoutAlign::Stretch);
            contentDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = true};
            Panel column = scope.createPanel(InternedString(id), contentDesc);
            column.setParent(view.content());
            bindGap(column, SPACE_XS);
            bindPadding(column, SPACE_XS, SPACE_XS, SPACE_XS, SPACE_XS);
            return column;
        };
        state.overviewColumn =
            textColumn("modules.overview", "modules.overview.column", state.overviewView, state.views[ViewModules]);
        state.paramColumn =
            textColumn("modules.params", "modules.detail.column", state.paramView, state.views[ViewParameters]);
        const Widget paramContent = state.paramColumn;
        state.detailTitle = block("modules.detail.title", state.overviewColumn, "modules.detail.title");
        state.detailText = block("modules.detail.text", state.overviewColumn, "modules.detail");
        state.detailMeta = block("modules.detail.meta", state.overviewColumn, "modules.detail");
        state.revealButton = scope.createButton("modules.reveal", "", fitButtonDesc());
        localized(state.revealButton, "editor.modules.reveal");
        state.revealButton.setParent(state.overviewColumn);
        state.revealButton.onClick([&state] { state.queuePlain("moduleReveal", Json::object()); });
        state.detailCommands = block("modules.detail.commands", state.overviewColumn, "modules.detail");
        state.paramNote = block("modules.paramNote", paramContent, "modules.detail");

        // the build view: Reload built modules and the build status over a column with the selected module's build
        state.views[ViewBuild] = scope.createPanel("modules.view.build", viewDesc());
        state.buildBar =
            scope.createPanel("modules.buildBar", barDesc(Vec2{}, stack(UiLayoutDirection::Row, state.density.xs)));
        bindGap(state.buildBar, SPACE_XS);
        state.buildBar.setParent(state.views[ViewBuild]);
        state.buildPanelAction = scope.createButton("modules.buildPanelAction", "", fitButtonDesc());
        state.buildPanelAction.setParent(state.buildBar);
        state.buildPanelAction.onClick([&state] { state.buildOrCancel(); });
        state.buildHost = scope.createPanel("modules.buildProjectHost", reloadHostDesc);
        state.buildHost.setParent(state.actionBar);
        state.buildButton = scope.createButton("modules.buildProject", "", fitButtonDesc());
        state.buildButton.setParent(state.buildHost);
        state.buildButton.onClick([&state] { state.buildOrCancel(); });
        state.showBuildButton();
        state.reloadBuiltHost = scope.createPanel("modules.reloadBuiltHost", reloadHostDesc);
        state.reloadBuiltHost.setParent(state.buildBar);
        state.reloadBuiltButton = scope.createButton("modules.reloadBuilt", "", fitButtonDesc());
        state.reloadBuiltButton.setParent(state.reloadBuiltHost);
        localized(state.reloadBuiltButton, "editor.modules.reloadBuilt");
        state.reloadBuiltButton.onClick([&state] { state.reloadBuilt(); });
        state.buildStatus = line("modules.buildStatus", state.buildBar);
        state.buildStatus.setAccessibleNameKey("editor.modules.access.buildStatus");
        state.buildStatus.setLayoutItem(fillItem());
        state.buildColumn =
            textColumn("modules.build", "modules.build.column", state.buildView, state.views[ViewBuild]);
        state.buildNote = block("modules.build.note", state.buildColumn, "modules.detail");
        state.buildTitle = block("modules.build.title", state.buildColumn, "modules.detail.title");
        state.buildFacts = block("modules.build.facts", state.buildColumn, "modules.detail");
        state.buildDiagnostics = block("modules.build.diagnostics", state.buildColumn, "modules.output");
        state.buildCommand = block("modules.build.command", state.buildColumn, "modules.output");
        state.buildOthersTitle = block("modules.build.othersTitle", state.buildColumn, "modules.detail.title");
        state.buildOthers = block("modules.build.others", state.buildColumn, "modules.detail");

        // the commands view: a command and Run in one bar, then one scrolling form: the command, its arguments, the
        // run's status and its result
        state.views[ViewCommands] = scope.createPanel("modules.view.commands", viewDesc());
        state.runBar =
            scope.createPanel("modules.runBar", barDesc(Vec2{}, stack(UiLayoutDirection::Row, state.density.xs)));
        bindGap(state.runBar, SPACE_XS);
        state.runBar.setParent(state.views[ViewCommands]);
        state.commandChoice =
            scope.createDropdown("modules.commandChoice", UiDropdownDesc{.rect = barDesc(Vec2{}), .searchable = true});
        state.commandChoice.setParent(state.runBar);
        state.commandChoice.setLayoutItem(fillItem());
        state.commandChoice.setAccessibleNameKey("editor.modules.access.command");
        state.commandChoice.onChange(
            [&state](const u32 index)
            {
                if (index < state.commands.size())
                {
                    state.queuePlain("moduleCommandSelect", {{"name", state.commands[index].name}});
                }
            });
        state.runButton = scope.createButton("modules.run", "", fitButtonDesc());
        state.runButton.setParent(state.runBar);
        localized(state.runButton, "editor.modules.run");
        state.runButton.onClick([&state] { state.runSelected(); });
        state.argView = scope.createScrollView("modules.args", scrollDesc);
        state.argView.setParent(state.views[ViewCommands]);
        const Widget argContent = state.argView.content();
        state.commandLine = block("modules.commandLine", argContent, "modules.detail");
        state.runStatus = block("modules.runStatus", argContent, "modules.detail");
        state.outputText = block("modules.outputText", argContent, "modules.output");
        state.args = std::make_unique<FieldRows>(scope, "modules.arg", state.argView);
        state.args->setBlocks({state.commandLine}, {state.runStatus, state.outputText});
        state.args->onSubmit = [&state](u32) { state.runSelected(); };

        // the browse view
        state.views[ViewBrowse] = scope.createPanel("modules.view.browse", viewDesc());
        state.browseBar =
            scope.createPanel("modules.browseBar", barDesc(Vec2{}, stack(UiLayoutDirection::Row, state.density.xs)));
        bindGap(state.browseBar, SPACE_XS);
        state.browseBar.setParent(state.views[ViewBrowse]);
        state.upButton = scope.createButton("modules.up", "", fitButtonDesc());
        state.upButton.setParent(state.browseBar);
        localized(state.upButton, "editor.modules.up");
        state.upButton.onClick(
            [&state]
            {
                const auto current = pathFromUtf8(state.browsePath);
#if !defined(_WIN32)
                if (current == current.root_path())
                    return;
#endif
                std::string parent;
                if (!state.browsePath.empty() && current.has_parent_path() && current.parent_path() != current)
                {
                    (void)tryPathToUtf8(current.parent_path(), parent);
                }
                state.queuePlain("moduleBrowse", parent.empty() ? Json::object() : Json{{"path", parent}});
            });
        state.browsePathInput = scope.createTextInput("modules.browsePath", topLeft());
        state.browsePathInput.setParent(state.browseBar);
        state.browsePathInput.setLayoutItem(fillItem());
        state.browsePathInput.setAccessibleNameKey("editor.modules.access.path");
        state.browsePathInput.onSubmit([&state](const std::string_view text)
                                       { state.queuePlain("moduleBrowse", {{"path", std::string(text)}}); });
        state.addButton = scope.createButton("modules.add", "", fitButtonDesc());
        state.addButton.setParent(state.browseBar);
        localized(state.addButton, "editor.modules.add");
        state.addButton.onClick([&state] { state.queuePlain("moduleSearchPathAdd", {{"path", state.browsePath}}); });
        state.browseStatus = line("modules.browseStatus", state.views[ViewBrowse]);
        state.browseStatus.setAccessibleNameKey("editor.modules.access.browseStatus");
        state.folderTable = std::make_unique<Table>(
            scope,
            TableDesc{
                .id = "modules.folders",
                .cells = 1,
                .buttonWidth = 70.0f,
                .read =
                    [&state](const u32 row, const std::span<std::string> cells, std::string& button)
                {
                    cells[0] = state.folders[row];
                    button = state.msg("open");
                },
                .press = [&state](const u32 row)
                { state.queuePlain("moduleBrowse", {{"path", joinedPath(state.browsePath, state.folders[row])}}); }});
        state.folderTable->list.setParent(state.views[ViewBrowse]);
        grow(state.folderTable->list);

        state.fixtureBox.setParent(state.views[ViewBrowse]);
        state.actionHint = scope.createLabel("modules.actionHint", "", barDesc(Vec2{0, state.density.row * 2}));
        state.actionHint.setParent(state.detailPane);
        state.tabs.setParent(state.detailPane);
        constexpr std::array<View, 3> TAB_VIEWS{ViewModules, ViewParameters, ViewCommands};
        constexpr std::array<const char*, 3> TAB_KEYS{"editor.modules.overview", "editor.modules.parameters",
                                                      "editor.modules.tab.commands"};
        for (u32 i = 0; i < TAB_VIEWS.size(); ++i)
        {
            Tab tab = state.tabs.addTab(InternedString(std::format("modules.tab.{}", VIEW_NAMES[TAB_VIEWS[i]])), "");
            tab.label().setLocalizedText(TAB_KEYS[i]);
        }
        state.tabs.setActive(0);
        state.tabs.onChange(
            [&state](u32 index)
            {
                constexpr std::array<View, 3> views{ViewModules, ViewParameters, ViewCommands};
                if (index < views.size())
                    state.queuePlain("modulesView", {{"view", VIEW_NAMES[views[index]]}, {"detail", true}});
            });
        for (u32 i : {u32(ViewModules), u32(ViewParameters), u32(ViewCommands), u32(ViewBrowse)})
            state.views[i].setParent(state.detailPane);
        state.views[ViewBuild].setParent(state.root);
        state.views[ViewBuild].setVisible(false);
        scope.hierarchy().reorderChild(state.buildHost.entity(), 1);
        scope.hierarchy().reorderChild(state.reloadHost.entity(), 2);

        for (const Panel& view : state.views)
        {
            bindGap(view, SPACE_XS);
        }
        state.applyDensity();
        state.densityObserver = scope.observeLayoutTokens([&state] { state.followDensity(); });
        state.showCommand();
        state.registerCommands();
        state.scope.commit();

        UiViewContract contract;
        contract.slot(InternedString(HEADER_SLOT), [&state](const Widget& slot) { state.fillHeader(slot); });
        contract.slot(InternedString(TABS_SLOT), [&state](const Widget& slot) { state.fillTabs(slot); });
        contract.slot(InternedString(VIEWS_SLOT), [&state](const Widget& slot) { state.fillViews(slot); });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                const Density density = Density::read(viewScope);
                columnRoot(root, density.xs, Vec4{density.xs, density.xs, density.xs, density.xs});
                bindGap(root, SPACE_XS);
                bindPadding(root, SPACE_XS, SPACE_XS, SPACE_XS, SPACE_XS);
                const auto bar = [&](const std::string_view id, const float height)
                {
                    Panel slot = viewScope.createPanel(
                        InternedString(id), barDesc(Vec2{0.0f, height}, stack(UiLayoutDirection::Row, density.xs)));
                    bindGap(slot, SPACE_XS);
                    slot.setParent(root);
                    return slot;
                };
                Panel header = bar(HEADER_SLOT, density.bar);
                auto headerLayout = stack(UiLayoutDirection::Row, density.xs);
                headerLayout.wrap = true;
                header.setLayout(headerLayout);
                viewScope.world().addComponent<UiContentSizeComponent>(
                    header.entity(), UiContentSizeComponent{.fitWidth = false, .fitHeight = true});
                bar(TABS_SLOT, 0.0f).setVisible(false);
                Panel views = viewScope.createPanel(InternedString(VIEWS_SLOT), columnDesc(density.xs));
                bindGap(views, SPACE_XS);
                views.setLayoutItem(fillItem());
                views.setParent(root);
            });
        state.doc = openView(engine, VIEW, "assets/ui/editor/modules.json", std::move(contract), state.root);
        state.startScan();
        (void)tryPathToUtf8(state.scanRoots.front(), state.browsePath);
        state.startBrowse();
    }

    EditorModules::~EditorModules()
    {
        m_state->control->unregisterOwner("editorModules");
        m_state->scope.unobserveLayoutTokens(m_state->densityObserver);
    }

    Widget EditorModules::content() const { return m_state->root; }

    void EditorModules::update()
    {
        State& state = *m_state;
        if (!state.pending.empty())
        {
            state.dispatchPending();
        }
        {
            SPITE_ZONE("EditorModules.pollJobs");
            state.pollJobs();
            state.feedback->reportJobs("modules",
                                       (state.scanJob.busy() ? 1u : 0u) + (state.browseJob.busy() ? 1u : 0u));
        }
        const bool visible = shown(state.scope, state.root);
        state.build->update(visible);
        if (!visible)
        {
            state.wasShown = false;
            return;
        }
        state.moduleTable->followText();
        state.folderTable->followText();
        state.followWorkspace();
        state.relayoutColumn(state.overviewView, state.overviewColumn);
        state.relayoutColumn(state.paramView, state.paramColumn);
        state.relayoutColumn(state.buildView, state.buildColumn);
        state.args->relayout();
        bool refreshed = !state.wasShown;
        if (state.localization && state.localization->revision() != state.shownRevision)
        {
            state.shownRevision = state.localization->revision();
            state.relocalize();
        }
        if (const u64 stamp = State::stampOf(*state.host); stamp != state.loadedStamp)
        {
            state.rebuildRows();
            refreshed = true;
        }
        if (refreshed || state.rowsDirty)
        {
            state.showRows();
        }
        if (!state.wasShown ||
            (!state.shownBuildProject.empty() && !state.build->startedBuild(state.shownBuildProject)))
        {
            // shown again (a build started or ended meanwhile), or the Cancel's build exited: one handle wait a frame
            state.showBuildButton();
        }
        if (const u64 built = state.build->displayRevision(); built != state.buildSeen)
        {
            state.buildSeen = built;
            state.moduleTable->rebindVisible();
            state.showReload();
            state.showBuild();
        }
        state.sync();
        if (const u64 command = state.control->commandGeneration(); command != state.seenCommand)
        {
            state.seenCommand = command;
            state.showBuildButton(); // a buildProject or cancelBuild another client sent
            if (state.view == ViewParameters && state.wasShown)
            {
                state.refreshParamValues();
            }
        }
        state.wasShown = true;
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{{.name = "modules",
                                                       .category = "project",
                                                       .order = 120,
                                                       .dock = {.target = "project"},
                                                       .zone = "EditorModules",
                                                       .make = [](EditorPanelContext& c)
                                                       {
                                                           return makeEditorPanel<EditorModules>(
                                                               c.engine, c.settings, c.feedback,
                                                               EditorModelAccess::build(c.model));
                                                       }}};
    } // namespace
} // namespace spite
