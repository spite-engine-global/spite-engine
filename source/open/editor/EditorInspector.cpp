#include "base/WritableLayout.hpp"
#include <spite/platform/LocalTime.hpp>
#include "EditorInspector.hpp"
#include "EditorAgentLayer.hpp"
#include "EditorDialogs.hpp"
#include "EditorAssetCatalog.hpp"
#include "EditorRenderedPreviews.hpp"
#include "editor/EditorPreviewJobs.hpp"
#include "EditorPanelRegistry.hpp"
#include "EditorContext.hpp"
#include "EditorEntityLabel.hpp"
#include "EditorSettings.hpp"
#include "EditorSystemCost.hpp"
#include "editor/EditorModel.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <ctime>
#include <format>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "asset/AssetHandle.hpp"
#include "animation/AnimationComponents.hpp"
#include "asset/AssetManifest.hpp"
#include "asset/LoadBakedFunctions.hpp"
#include "base/io/FileSystem.hpp"
#include "base/ContentRoot.hpp"
#include "assetruntime/AssetRuntime.hpp"
#include "base/Logging.hpp"
#include "base/MathFunctions.hpp"
#include "control/ControlServer.hpp"
#include "EditorPanelUi.hpp"
#include "ecs/ComponentMetadata.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "ecs/SystemDescriptor.hpp"
#include "engine/Engine.hpp"
#include "engine/PlayWorld.hpp"
#include "localization/Localization.hpp"
#include <spite/profiling/Zone.hpp>
#include "serialization/FormatWalk.hpp"
#include "serialization/JsonFormat.hpp"
#include "uiview/UiStyleSheet.hpp"
#include "uiview/UiViewContext.hpp"
#include "ui/UiColorPicker.hpp"
#include "ui/UiDocument.hpp"
#include "ui/UiDragDrop.hpp"
#include "ui/UiForm.hpp"
#include "ui/UiScope.hpp"
#include "ui/UiTextMeasurer.hpp"
#include "world/DisabledComponent.hpp"
#include "world/EntityName.hpp"

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;

        constexpr std::string_view OWNER = "editorInspector";
        constexpr float THUMB = 10.0f; // kept clear for the scroll thumb
        constexpr float BUTTON_WIDTH = 60.0f;
        constexpr float OPTION_WIDTH = 36.0f; // the on/off button of the Show engine components row
        constexpr float LABEL_SHARE = 0.4f;
        constexpr float LABEL_MIN = 40.0f; // a label column never narrows below this to make room for number fields
        constexpr float FIELD_PAD = 8.0f; // a number field's text inset, both sides
        constexpr float CHECK_BOX = 16.0f; // a checkbox's box (UiValueWidgets.cpp)
        constexpr float CHECK_GAP = 6.0f; // and the gap before its label
        constexpr std::string_view LANE_SAMPLE = "-0.000"; // the widest number text a lane is sized for
        constexpr std::string_view TABLE = "editor-inspector.";
        constexpr u32 LANES = 4;
        constexpr u32 NO_ROW = static_cast<u32>(-1);
        constexpr u32 MAX_COMPONENTS = 256;
        constexpr std::array<std::string_view, 2> ROTATION_MODES{"euler", "raw"};
        constexpr u32 MAX_ENTITY_CHOICES = 500; // entities the Pick list names; more end in a note
        constexpr u32 POPUP_ROWS = 14; // rows the Add and Pick lists show before they scroll
        constexpr std::string_view SWATCH_MIXED_ICON = "dots-three"; // a colour row whose entities differ
        constexpr std::string_view REMOVE_ICON = "x"; // a component header's Remove

        // The Add list's groups in order, and the name of each (the row id's suffix; the heading's text is the locale's
        // `group.<name>`); Game and Engine are the default offer, Ui and RuntimeDerived only with the engine toggle on
        // (ComponentCategory).
        constexpr std::array<ComponentCategory, 4> ADD_GROUPS{ComponentCategory::Game, ComponentCategory::Engine,
                                                              ComponentCategory::Ui, ComponentCategory::RuntimeDerived};
        constexpr std::string_view groupName(const ComponentCategory category)
        {
            switch (category)
            {
            case ComponentCategory::Game:
                return "game";
            case ComponentCategory::Engine:
                return "engine";
            case ComponentCategory::Ui:
                return "ui";
            case ComponentCategory::RuntimeDerived:
                return "runtimeDerived";
            }
            return {};
        }
        constexpr u32 groupRank(const ComponentCategory category)
        {
            return static_cast<u32>(std::ranges::find(ADD_GROUPS, category) - ADD_GROUPS.begin());
        }

        enum class RowKind : u8
        {
            Summary,
            Header,
            Group, // a nested struct, fixed array or Mat4: its rows follow, indented
            Value,
            Asset,
            Add,
            Option, // after Add: whether the Add list offers the engine's own components too
            Section, // the collapsed read-only Runtime group: its components' rows follow while it is open
            Agent, // the selected entity's unreviewed change by a control client, with Revert and Keep
            Note, // a line under it: a changed field and the value it had, or why Revert was refused
            Subtitle, // under the Summary row: systems and last write
            Change, // under a changed field's row: who changed it, when and the value it had, with Revert and Keep
        };
        constexpr std::array<std::string_view, 12> ROW_KIND_NAMES{"summary", "header", "group",    "value",
                                                                  "asset",   "add",    "option",   "section",
                                                                  "agent",   "note",   "subtitle", "change"};
        constexpr u32 CELLS = 2; // systems and last write
        constexpr float EDGE_WIDTH = 2.0f; // the agent edge of a changed field
        constexpr sizet MAX_NOTES = 3; // unclaimed changed entries the agent block lists before "and N more"
        // the stylesheet layer the panel owns while it exists, and its classes
        constexpr std::string_view STYLE_LAYER = "editor.inspector";
        constexpr const char* STYLE_FILE = "assets/ui/inspector.json"; // under the content root
        constexpr std::string_view FIELD_CLASS = "inspectorField";
        // the panel as a UI document: the document owns the panel's containers, the code the rows slot
        constexpr std::string_view VIEW = "inspector.panel";
        constexpr std::string_view ROWS_SLOT = "inspector.rows";
        constexpr std::string_view DOCUMENT = "assets/ui/editor/inspector.json";
        // the assets query's type names, which the project panel's drag payloads ("asset.<name>") use too
        constexpr std::pair<AssetType, std::string_view> ASSET_KINDS[] = {{AssetType::MESH, "model"},
                                                                          {AssetType::MATERIAL, "material"},
                                                                          {AssetType::TEXTURE, "texture"},
                                                                          {AssetType::SKELETON, "skeleton"}};

        AssetType expectedAssetType(const FieldDesc& field, AssetType fallback)
        {
            if (field.assetType.empty())
                return fallback;
            for (const auto& [type, kind] : ASSET_KINDS)
                if (field.assetType == to_string(type))
                    return type;
            return AssetType::UNKNOWN;
        }

        std::string_view assetIcon(AssetType type)
        {
            switch (type)
            {
            case AssetType::MESH:
                return "cube";
            case AssetType::MATERIAL:
                return "palette";
            case AssetType::TEXTURE:
                return "image";
            case AssetType::SKELETON:
                return "bone";
            default:
                return {};
            }
        }

        // The style classes of what the rows build: the legacy names while the slot names no child class, else
        // <childClass> on the row root and <childClass>.label|group|header|field on its parts.
        struct RowClasses
        {
            InternedString root{};
            InternedString label{"inspectorLabel"};
            InternedString group{"inspectorGroup"};
            // a component's section title: the Forge title first, else the editor themes' band
            InternedString header{"inspector.section inspectorHeader"};
            InternedString field{FIELD_CLASS};
            InternedString button{"inspectorButton"}; // a row's buttons cut their text at their rect (ellipsis)
            // the agent layer: the root of a changed row (the agent wash), its 2px edge and the agent block's text
            InternedString changed{"inspectorChanged"};
            InternedString edge{"inspectorEdge"};
            InternedString agent{"inspectorAgent"};
            // a field with an invalid entry (its border) and the note under it: the shared form looks, sized as the row
            InternedString fieldError{"inspectorField form.field.error"};
            InternedString error{"form.error inspectorError"};
            // the Forge header and strip: the same names under every row class base
            InternedString title{
                "inspector.title inspectorField"}; // the first class that sets a value wins: the field fills the rest
            InternedString subtitle{"inspector.subtitle"};
            // the component header's parts and the Add button, over the row's button class
            InternedString rule{"inspector.rule"};
            InternedString tag{"inspector.tag"};
            InternedString caret{"inspector.caret inspectorButton"};
            InternedString remove{"inspector.remove inspectorButton"};
            InternedString primary{"inspector.primary inspectorButton"};
            InternedString sectionRow{"inspector.sectionRow inspectorHeader"};
            // a number row's unit after its lanes, in the code style: the same name under every base
            InternedString unit{"inspector.unit inspectorUnit"};
            // a vector's lane tags: X Y Z in their axis colours, the rest (W, a colour's channels) plain
            std::array<InternedString, LANES> axis{
                InternedString("inspector.axis.x inspector.axis"), InternedString("inspector.axis.y inspector.axis"),
                InternedString("inspector.axis.z inspector.axis"), InternedString("inspector.axis")};

            RowClasses() = default;
            explicit RowClasses(const std::string_view base) :
                root(base), label(std::format("{}.label", base).c_str()), group(std::format("{}.group", base).c_str()),
                header(std::format("inspector.section {}.header", base).c_str()),
                field(std::format("{}.field", base).c_str()), button(std::format("{}.button", base).c_str()),
                changed(std::format("{}.changed", base).c_str()), edge(std::format("{}.edge", base).c_str()),
                agent(std::format("{}.agent", base).c_str()),
                fieldError(std::format("{}.field form.field.error", base).c_str()),
                error(std::format("form.error {}.error", base).c_str()),
                title(std::format("inspector.title {}.field", base).c_str()),
                caret(std::format("inspector.caret {}.button", base).c_str()),
                remove(std::format("inspector.remove {}.button", base).c_str()),
                primary(std::format("inspector.primary {}.button", base).c_str()),
                sectionRow(std::format("inspector.sectionRow {}.header", base).c_str())
            {
            }

            [[nodiscard]] InternedString labelOf(const RowKind kind) const
            {
                return kind == RowKind::Header                                                   ? header
                    : kind == RowKind::Group                                                     ? group
                    : kind == RowKind::Agent || kind == RowKind::Note || kind == RowKind::Change ? agent
                    : kind == RowKind::Subtitle                                                  ? subtitle
                                                                                                 : label;
            }
        };

        // One cell of the strip: its text, the tooltip with the detail, and whether the value is over a budget
        struct Cell
        {
            std::string value;
            std::string tip;
            bool warn = false;
            bool operator==(const Cell&) const = default;
        };

        struct Row
        {
            RowKind kind = RowKind::Value;
            ComponentID component = INVALID_COMPONENT_ID;
            u32 depth = 0;
            std::string label;
            std::string path; // Component.field[.field|[i]]..., what set takes
            FieldDesc field; // the value's desc; arrayCount 0 for an element
            u32 offset = 0; // in the component
            u32 lane = 0; // a Mat4 column
            bool editable = false; // an Agent row: Revert is offered
            u32 count = 0; // an Agent row: the entities its step touched, all of which Revert undoes
            bool changed = false; // a control client changed the field and nobody reviewed it (agent edge and wash)
            std::array<Cell, CELLS> cells; // the subtitle's values
            bool euler = false; // a Quat row shown as three angles in degrees (else the raw x y z w)
            bool color = false; // a Vec3 or Vec4 field flagged COLOR: a swatch before its lanes opens the picker
            u8 mixed = 0; // the selected entities differ: a bit per lane of a number row, bit 0 for any other row
            bool error = false; // a Note row: the message of the field above it
            bool toggle = false; // the Summary row: the enabled checkbox shows (something is selected)
            bool enabled = true; // the Summary row: the primary entity's own flag (no DisabledComponent)
            bool inactive = false; // the Summary row: enabled itself, but a parent is disabled (InactiveComponent)
            std::string shown; // the displayed value, compared on refresh
            std::string name; // an entity reference: its entity's name beside the id, for the well (not the value)
            std::string tag; // a Header row: the module tag at its end (the component's category)
        };

        // The header checkbox's model: the primary's own flag and whether it is inactive anyway;
        // mixed (bit 0 of Row::mixed) when the selected entities' flags differ.
        struct EnabledState
        {
            bool toggle = false;
            bool enabled = true;
            bool inactive = false;
            u8 mixed = 0;
            bool operator==(const EnabledState&) const = default;
        };

        // A component's rows, re-read when it changes.
        struct Shown
        {
            ComponentID component = INVALID_COMPONENT_ID;
            u32 first = 0;
            u32 end = 0;
        };

        struct InspectorStats
        {
            u64 rebuilds = 0; // the row model rebuilt: selection or components changed
            u64 refreshed = 0; // rows whose value changed
            u64 rebinds = 0; // pool slots rewritten
            u64 resyncs = 0; // every row re-read because a state-changing command ran
            SPITE_OBSERVED_SCHEMA(InspectorStats, rebuilds, refreshed, rebinds, resyncs)
        };

        enum class Editor : u8
        {
            None, // read-only: the value as text
            Check, // a bool: the checkbox
            Choice, // an enum with names: a button that opens the list of them
            Pick, // an entity: its name in a well and a Pick button that opens the list of entities
            Numbers,
            Text,
        };

        u32 laneCount(const FieldKind kind)
        {
            switch (kind)
            {
            case FieldKind::Vec2:
                return 2;
            case FieldKind::Vec3:
                return 3;
            case FieldKind::Vec4:
            case FieldKind::Quat:
            case FieldKind::Mat4:
                return 4;
            default:
                return 1;
            }
        }

        u32 floatCount(const FieldKind kind) { return kind == FieldKind::Mat4 ? 16 : laneCount(kind); }

        bool isFloats(const FieldKind kind)
        {
            return kind == FieldKind::Vec2 || kind == FieldKind::Vec3 || kind == FieldKind::Vec4 ||
                kind == FieldKind::Quat || kind == FieldKind::Mat4;
        }

        Editor editorOf(const Row& row)
        {
            if (row.kind == RowKind::Summary)
            {
                return row.editable ? Editor::Text : Editor::None; // the entity's name field
            }
            if (!row.editable || row.kind != RowKind::Value)
            {
                return Editor::None;
            }
            if (!row.field.enumNames.empty())
            {
                return Editor::Choice;
            }
            switch (row.field.kind)
            {
            case FieldKind::Bool:
                return Editor::Check;
            case FieldKind::Entity:
                return Editor::Pick;
            case FieldKind::U64:
            case FieldKind::String:
            case FieldKind::InternedString:
            case FieldKind::EntityList:
                return Editor::Text;
            default:
                return Editor::Numbers;
            }
        }

        // An editable float with a finite declared range: a slider before its number lane
        bool slidable(const Row& row)
        {
            return editorOf(row) == Editor::Numbers &&
                (row.field.kind == FieldKind::F32 || row.field.kind == FieldKind::F64) && hasFiniteRange(row.field);
        }

        // A colour channel for display: finite, in [0, 1]
        float unitOf(const float x) { return std::isfinite(x) ? std::clamp(x, 0.0f, 1.0f) : 0.0f; }

        // The lanes a number-field row shows: a Quat in Euler mode shows three angles.
        u32 valueLanes(const Row& row) { return row.euler ? 3 : laneCount(row.field.kind); }

        // The angles a person typed, kept with the quaternion they produced: while the stored quaternion still equals
        // it the row shows them, so a pitch beyond 90 (which has no ZYX form of its own) and the roll and yaw at
        // +-90 stay as entered and a drag across 90 is continuous.
        struct EulerMemo
        {
            static constexpr sizet CAPACITY = 64;
            struct Entry
            {
                std::array<f32, 4> quat;
                std::array<double, 3> angles;
            };
            std::vector<Entry> entries;

            [[nodiscard]] const Entry* find(const f32* p) const
            {
                const auto it = std::ranges::find_if(entries,
                                                     [p](const Entry& entry)
                                                     {
                                                         return std::abs(entry.quat[0] - p[0]) < 1e-6f &&
                                                             std::abs(entry.quat[1] - p[1]) < 1e-6f &&
                                                             std::abs(entry.quat[2] - p[2]) < 1e-6f &&
                                                             std::abs(entry.quat[3] - p[3]) < 1e-6f;
                                                     });
                return it == entries.end() ? nullptr : &*it;
            }

            void remember(const Quat& quat, const std::array<double, 3>& angles)
            {
                if (const Entry* known = find(&quat.x))
                {
                    entries.erase(entries.begin() + (known - entries.data()));
                }
                if (entries.size() == CAPACITY)
                {
                    entries.erase(entries.begin());
                }
                entries.push_back({{quat.x, quat.y, quat.z, quat.w}, angles});
            }
        };

        // The ZYX Euler angles of the quaternion at p, in degrees. At pitch +-90 the roll and yaw only matter as
        // their difference (sum at -90): the yaw is zero and the roll carries it, where the plain formula turns
        // rounding noise into angles that are another rotation.
        std::array<double, 3> eulerOf(const f32* p, const EulerMemo& memo)
        {
            if (const EulerMemo::Entry* entry = memo.find(p))
            {
                return entry->angles;
            }
            const Quat q = normalize(Quat(p[0], p[1], p[2], p[3]));
            const f32 sinPitch = 2.0f * (q.w * q.y - q.z * q.x);
            if (std::abs(sinPitch) < 0.9999995f)
            {
                const Vec3 radians = quatToEulerAnglesZyx(q);
                return {toDegrees(radians.x), toDegrees(radians.y), toDegrees(radians.z)};
            }
            const f32 m01 = 2.0f * (q.x * q.y - q.w * q.z);
            const f32 m11 = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
            const f32 roll = std::atan2(sinPitch > 0.0f ? m01 : -m01, m11);
            return {toDegrees(roll), sinPitch > 0.0f ? 90.0 : -90.0, 0.0};
        }

        double scalarOf(const FieldKind kind, const std::byte* p)
        {
            switch (kind)
            {
            case FieldKind::U8:
                return *reinterpret_cast<const u8*>(p);
            case FieldKind::U16:
                return *reinterpret_cast<const u16*>(p);
            case FieldKind::U32:
                return *reinterpret_cast<const u32*>(p);
            case FieldKind::I32:
                return *reinterpret_cast<const i32*>(p);
            case FieldKind::F32:
                return *reinterpret_cast<const f32*>(p);
            case FieldKind::F64:
                return *reinterpret_cast<const double*>(p);
            default:
                return 0.0;
            }
        }

        // Lane i of a number-field row over the component data, as stored.
        double rawLane(const Row& row, const std::byte* data, const u32 lane)
        {
            const std::byte* p = data + row.offset;
            if (isFloats(row.field.kind))
            {
                return reinterpret_cast<const f32*>(p)[row.lane * LANES + lane];
            }
            return scalarOf(row.field.kind, p);
        }

        // The lanes as the row shows them: a Quat in Euler mode as its three angles.
        std::array<double, LANES> laneValues(const Row& row, const std::byte* data, const EulerMemo& memo)
        {
            std::array<double, LANES> values{};
            if (row.euler)
            {
                const std::array<double, 3> angles = eulerOf(reinterpret_cast<const f32*>(data + row.offset), memo);
                std::ranges::copy(angles, values.begin());
                return values;
            }
            for (u32 lane = 0; lane < laneCount(row.field.kind); ++lane)
            {
                values[lane] = rawLane(row, data, lane);
            }
            return values;
        }

        AssetHandle assetAt(const Row& row, const std::byte* data)
        {
            if (row.field.kind == FieldKind::InternedString && row.field.assetType == "SKELETON")
                return {*reinterpret_cast<const InternedString*>(data + row.offset), AssetType::SKELETON, 0};
            return *reinterpret_cast<const AssetHandle*>(data + row.offset);
        }

        std::string display(const Row& row, const std::byte* data)
        {
            const std::byte* p = data + row.offset;
            if (row.kind == RowKind::Asset)
            {
                const AssetHandle& handle = assetAt(row, data);
                return handle.isValid() ? std::format("{} ({} #{})", handle.name.toView(), to_string(handle.assetType),
                                                      handle.subAssetIndex)
                                        : std::string("None");
            }
            switch (row.field.kind)
            {
            case FieldKind::Bool:
                return *reinterpret_cast<const bool*>(p) ? "true" : "false";
            case FieldKind::U64:
                return std::to_string(*reinterpret_cast<const u64*>(p));
            case FieldKind::String:
                return *reinterpret_cast<const std::string*>(p);
            case FieldKind::InternedString:
                return std::string(reinterpret_cast<const InternedString*>(p)->toView());
            case FieldKind::Entity:
                return std::to_string(reinterpret_cast<const Entity*>(p)->id());
            case FieldKind::EntityList:
                {
                    std::string text = "[";
                    for (const Entity entity : *reinterpret_cast<const std::vector<Entity>*>(p))
                    {
                        text += (text.size() > 1 ? ", " : "") + std::to_string(entity.id());
                    }
                    return text + "]";
                }
            default:
                {
                    std::string text;
                    for (u32 lane = 0; lane < laneCount(row.field.kind); ++lane)
                    {
                        const double value = rawLane(row, data, lane);
                        text += lane ? " " : "";
                        // floats print as floats: their shortest form, not a double's
                        if (row.field.kind == FieldKind::F64)
                        {
                            text += std::format("{}", value);
                        }
                        else if (row.field.kind == FieldKind::F32 || isFloats(row.field.kind))
                        {
                            text += std::format("{}", static_cast<f32>(value));
                        }
                        else
                        {
                            text += std::to_string(static_cast<std::int64_t>(value));
                        }
                    }
                    return text;
                }
            }
        }

        // The index into the field's enum names of the stored value `shown` (display()'s text), -1 for a value the
        // names do not list
        i32 enumIndex(const FieldDesc& field, const std::string_view shown)
        {
            std::int64_t value = 0;
            if (std::from_chars(shown.data(), shown.data() + shown.size(), value).ec != std::errc{})
            {
                return -1;
            }
            const auto it = std::ranges::find(field.enumNames, value, &EnumName::value);
            return it == field.enumNames.end() ? -1 : static_cast<i32>(it - field.enumNames.begin());
        }

        // The JSON value of a number-field edit for one entity: the lane
        // replaced in that entity's own vector or matrix.
        Json numberValue(const Row& row, const std::byte* data, const u32 lane, const double value, EulerMemo& memo)
        {
            if (row.euler)
            {
                // the lane replaced in this entity's own angles, then back to its quaternion; the angles are kept
                // against it so the row shows them
                std::array<double, 3> angles = eulerOf(reinterpret_cast<const f32*>(data + row.offset), memo);
                angles[lane] = value;
                const Quat out = eulerAnglesZyxToQuat(Vec3(toRadians(static_cast<f32>(angles[0])),
                                                           toRadians(static_cast<f32>(angles[1])),
                                                           toRadians(static_cast<f32>(angles[2]))));
                memo.remember(out, angles);
                return Json::array({out.x, out.y, out.z, out.w});
            }
            if (isFloats(row.field.kind))
            {
                const auto* floats = reinterpret_cast<const f32*>(data + row.offset);
                Json array = Json::array();
                for (u32 i = 0; i < floatCount(row.field.kind); ++i)
                {
                    array.push_back(static_cast<double>(floats[i]));
                }
                array[row.lane * LANES + lane] = value;
                return array;
            }
            switch (row.field.kind)
            {
            case FieldKind::F32:
            case FieldKind::F64:
                return value;
            case FieldKind::I32:
                return static_cast<std::int64_t>(std::llround(value));
            default:
                return static_cast<u64>(std::max(0.0, std::round(value)));
            }
        }

        // The JSON value a committed text means; null when it means none.
        Json textValue(const Row& row, const std::string_view text)
        {
            switch (row.field.kind)
            {
            case FieldKind::String:
            case FieldKind::InternedString:
                return std::string(text);
            case FieldKind::EntityList:
                {
                    Json list = Json::parse(text, nullptr, false);
                    return list.is_array() && std::ranges::all_of(list, &Json::is_number_unsigned) ? list : Json();
                }
            default:
                {
                    u64 id = 0;
                    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
                    return error == std::errc{} && end == text.data() + text.size() ? Json(id) : Json();
                }
            }
        }

        // The kind's bounds, narrowed to the field's declared range: a typed number clamps into what set accepts
        void numericRange(const FieldDesc& field, UiNumericComponent& out)
        {
            out.decimals = 0;
            out.step = 0.25;
            switch (field.kind)
            {
            case FieldKind::U8:
                out.min = 0.0;
                out.max = 255.0;
                break;
            case FieldKind::U16:
                out.min = 0.0;
                out.max = 65535.0;
                break;
            case FieldKind::U32:
                out.min = 0.0;
                out.max = 4294967295.0;
                break;
            case FieldKind::I32:
                out.min = -2147483648.0;
                out.max = 2147483647.0;
                break;
            default:
                out.decimals = 3;
                out.step = 0.1;
                out.min = -1.0e300;
                out.max = 1.0e300;
                break;
            }
            out.min = std::max(out.min, field.min);
            out.max = std::min(out.max, field.max);
        }

        // By name: each DLL holds its own copy of an inline schema, so the
        // address differs across modules.
        bool isAssetHandle(const StructSchema& schema)
        {
            return schema.typeName == AssetHandle::spiteSchema().typeName;
        }

        UiRectDesc topLeft()
        {
            UiRectDesc desc;
            desc.pivot = Vec2{0.0f, 0.0f};
            return desc;
        }

        void place(Widget& widget, const bool visible, const Vec2 offset = {}, const Vec2 size = {})
        {
            if (widget.isVisible() != visible)
            {
                widget.setVisible(visible);
            }
            if (!visible)
            {
                return;
            }
            if (widget.offset() != offset)
            {
                widget.setOffset(offset);
            }
            if (widget.size() != size)
            {
                widget.setSize(size);
            }
        }

        template <typename T>
        void setText(T& widget, const std::string_view text)
        {
            if (widget.text() != text)
            {
                widget.setText(text);
            }
        }
    } // namespace

    struct EditorInspector::State
    {
        State(Engine& engine, EditorModel& model) :
            engine(&engine), control(&engine.control()), scope(editorUi(engine).ui()),
            styles(&editorUi(engine).uiView().stylesheet()),
            localization(engine.services().contains<Localization>() ? &engine.services().get<Localization>() : nullptr),
            model(&model), worldName(model.selectionWorld()), selection(model.selection())
        {
        }

        struct Slot
        {
            Panel root;
            Label label;
            Label assetIcon;
            std::array<NumberField, LANES> numbers;
            TextInput text;
            Button button;
            Checkbox enabled; // the Summary row's enabled flag of the selected entities, and a bool row's box
            std::array<Label, CELLS> cells; // the subtitle's hover targets
            Button keep; // an Agent row's second button: Revert is `button`
            bool revertTipped = false; // Revert names the scope of its step in a tooltip
            Panel edge; // the agent edge of a changed row
            Panel swatch; // a colour row's colour, over `button` (the frame that opens the picker)
            Label tag; // a Header row's module tag
            Panel rule; // and the hairline between its title and the tag
            std::array<Label, LANES> axes; // a vector row's lane tags
            Slider slider; // a ranged float row's slider, before its number lane
            Label unit; // a number row's unit, after its lanes
            bool removeTipped = false; // a Header row's Remove carries its tooltip
            u32 row = NO_ROW;
            std::string key; // what the slot shows; equal: nothing to write
        };

        Engine* engine;
        ControlServer* control;
        UiScope scope;
        const UiStyleSheet* styles;
        const Localization* localization;
        EditorDialogs* dialogs = nullptr;
        u64 shownRevision = 0; // the localization revision the rows' texts were resolved at
        float shownTextScale = 1.0f; // the text scale the number lanes were sized for
        float lane = 0.0f; // measuredLane's last result, at laneScale and laneRevision
        float revertWidth = BUTTON_WIDTH; // the buttons fitted to their labels, measured with the lane
        float keepWidth = BUTTON_WIDTH;
        float axisGlyph = 0.0f; // a lane tag's letter, measured with the lane
        float pickWidth = BUTTON_WIDTH;
        float modeWidth = BUTTON_WIDTH; // the rotation mode button: the wider of its two labels
        float enabledWidth = BUTTON_WIDTH; // the enabled checkbox: box and the widest of its labels
        float optionWidth = OPTION_WIDTH; // the on/off button: its width, or the wider of its two labels
        float laneScale = 0.0f;
        u64 laneRevision = ~u64{0};
        Panel host; // the docked content: the document view's root goes in it
        VirtualList list;
        Label hint; // the body's note while nothing is selected: what to select and where
        bool hintShown = false; // the list is hidden and the hint takes the slot
        Tooltip labelTip; // a label's full text, which its column may ellipsize
        Tooltip revertTip; // what a Revert undoes when its step touched more than this entity
        Tooltip cellTip; // a strip cell's detail: the cost note, the matching systems, the last edit
        PopupList popup; // the Add Component and Pick list: searchable, opaque, scrolls
        std::vector<UiChoiceItem> popupItems;
        bool showEngine = false; // the Add list also offers Ui and RuntimeDerived components
        bool showRuntime = false; // the read-only Runtime section lists its components
        bool rawQuat = false; // quaternion rows show x y z w, else Euler angles in degrees
        std::vector<ComponentID> collapsed; // the components whose header hides its fields
        EulerMemo eulerMemo;
        // The colour picker, made when the first swatch opens it; `colorRow` is the row it edits while open
        std::unique_ptr<ColorPicker> picker;
        Row colorRow;
        // folded steps share a key: the seed keeps a recreated inspector's first drag out of an earlier one's step
        u64 colorSeed = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count());
        // The one field whose entry was refused: a note row under it says why; any edit, another
        // selection or a changed rotation mode clears it.
        struct FieldError
        {
            std::string path;
            u32 column = 0; // a Mat4's column
            u32 lane = 0;
            std::string key; // the message's localization key; empty: no error
            bool operator==(const FieldError&) const = default;
        } fieldError;
        bool errorDirty = false; // the error changed: the rows are rebuilt once no field is being typed in
        std::vector<ComponentID> sharedShown; // every shared component, the Runtime section's collapsed ones too
        std::vector<Slot> slots;
        std::unique_ptr<UiDocumentView> view;
        RowClasses classes;
        float thumb = THUMB; // kept clear at the row's end while the list scrolls
        panelui::Density density = panelui::Density::read(scope); // the spaces and sizes the rows are placed with
        u32 densityObserver = 0;

        // the model's selection: the inspector shows and edits it, the select command sets it
        EditorModel* model;
        const std::string& worldName;
        const std::vector<Entity>& selection;
        u64 seenSelection = 0; // the model's selection revision the rows were built for
        std::vector<Row> rows;
        std::vector<Shown> shown;
        std::vector<ComponentID> primaryAspect;
        u64 seenGeneration = 0;
        InspectorStats stats;

        std::vector<std::string> pending; // request lines update() dispatches
        // the agent layer: what a control client changed in the selected entity, with Revert and Keep
        EditorAgentLayer* agent = nullptr; // not owned: the shell's
        u64 seenChange = 0; // the layer's changeRevision the rows were built at
        bool seenAgentOn = false;
        std::vector<std::string> claimedChanges; // the unreviewed entries the rebuild in progress gave a change line
        std::optional<EditorAgentLayer::Change> rowChange; // the change the rebuild in progress puts under its fields
        std::string agentNote; // why the last Revert was refused, shown under the agent row
        u64 agentNoteStep = 0; // the step it was refused for: a newer step of the entity has its own block
        struct AgentAction
        {
            bool revert = false; // Revert (revertStep of `step`), else Keep (review of `entity`)
            u64 step = 0;
            Entity entity = Entity::undefined();
        };
        std::vector<AgentAction> agentActions;

        // the readout strip: under the header, for one selected entity, once the shell gave the telemetry
        // setting. The systems are found when the selection's aspect or the schedule changed, never per frame; their
        // costs are read again every systemcost::REFRESH_FRAMES while the entity is selected.
        struct SystemHit
        {
            std::string path; // "readout.system.<module::name>.<phase>"
            std::string qualified; // Module::Name, or Name for the local module
            const char* module = nullptr; // interner-owned
            const char* name = nullptr;
            bool fixed = false; // the fixed phase, else the update phase
            u64 stage = 0;
            bool enabled = true;
            float avgMs = 0.0f; // the scheduler's average
            float heldMs =
                -1.0f; // what shows: avgMs held until it moved by systemcost::STEP_MS; -1 before the first read
            bool warn = false; // over the budget (held with heldMs)
            bool dirty = false; // what shows moved since the rows were written
        };
        bool readoutOn = false;
        bool telemetryFull = false;
        float budgetMs = 2.0f;
        std::string hitWorld;
        u64 hitEntity = 0;
        u64 hitRevision = ~u64{0};
        std::vector<ComponentID> hitAspect;
        std::vector<SystemHit> hits; // execution order
        u32 sinceCosts = 0;
        u64 seenLastEdit = 0;
        Entity dragField = Entity::undefined(); // the number field a drag is editing
        u64 dragSerial = 0;
        // A slider drag in progress: a preview in the slot's slider and lane only, sent as one request when
        // the pointer lets go, if the slot still shows the row of `path` for the same selection
        struct Slide
        {
            u32 slot = NO_ROW;
            std::string path;
            std::vector<Entity> selection;
        } slide;
        enum class MenuFor : u8
        {
            AddComponent,
            Asset,
            Choice,
            Entity,
        } menuFor = MenuFor::AddComponent;
        u32 menuRow = NO_ROW;
        std::vector<AssetHandle> choices;
        Modal assetModal;
        Combo assetCombo;
        Dropdown assetParts;
        Image assetPreview;
        Label assetNote;
        Button assetApply;
        Button assetPing;
        Button assetGenerate;
        Row assetRow;
        std::vector<Entity> assetSelection;
        std::vector<AssetHandle> assetOriginals;
        std::string assetWorld;
        AssetHandle assetCandidate;
        std::vector<UiChoiceItem> assetItems, partItems;
        std::unique_ptr<EditorRenderedPreviews> assetLookup;
        u64 assetPreviewRevision = 0;
        bool assetPreviewPending = false;
        bool assetPreviewMissingNoted = false;
        std::string assetPendingRequest;
        u64 assetChooserGeneration = 0, assetPendingGeneration = 0;
        u64 assetRegistryRevision = 0;
        u64 rowsRegistryRevision = 0;
        static constexpr u64 ASSET_PREVIEW_SLOT = 0x53504954494e5301ull;
        std::vector<u64> entityChoices; // the open entity list's items after None: the ids they name

        [[nodiscard]] const World* world() const { return control->world(worldName); }

        [[nodiscard]] Entity primary() const { return selection.empty() ? Entity::undefined() : selection.front(); }

        [[nodiscard]] bool busy(const Widget& widget) const
        {
            const UiRouter& router = scope.router();
            return router.focused() == widget.entity() || router.pressed() == widget.entity();
        }

        // -- model -----------------------------------------------------------------

        // The schema'd and opaque components every selected entity has, by name.
        std::vector<ComponentID> sharedComponents(const World& target) const
        {
            std::vector<ComponentID> shared;
            if (selection.empty())
            {
                return shared;
            }
            ComponentID ids[MAX_COMPONENTS];
            const u32 count = std::min(target.getEntityAspectIds(primary(), ids), MAX_COMPONENTS);
            for (u32 i = 0; i < count; ++i)
            {
                if (ids[i] == getComponentId<DisabledComponent>())
                {
                    continue; // the header checkbox is its editor; a Remove of it is refused
                }
                if (std::ranges::all_of(selection,
                                        [&](const Entity entity) { return target.hasComponentById(entity, ids[i]); }))
                {
                    shared.push_back(ids[i]);
                }
            }
            // the Transform leads, as every placed entity has one; the rest by name
            std::ranges::sort(shared,
                              [](const ComponentID a, const ComponentID b)
                              {
                                  const ComponentID transform = getComponentId<TransformComponent>();
                                  return std::pair(a != transform, std::string_view(getComponentName(a))) <
                                      std::pair(b != transform, std::string_view(getComponentName(b)));
                              });
            return shared;
        }

        // A message of any table by its full key
        [[nodiscard]] std::string message(const std::string& full, const std::vector<MessageArg>& args = {}) const
        {
            if (!localization)
            {
                return full;
            }
            std::string scratch;
            return std::string(localization->resolve(InternedString(full), args, scratch));
        }

        [[nodiscard]] std::string tr(const std::string_view key, const std::vector<MessageArg>& args = {}) const
        {
            return message(std::string(TABLE) + std::string(key), args);
        }

        // display() for the row's text: an unset asset reads in the locale
        [[nodiscard]] std::string shownText(const Row& row, const std::byte* data) const
        {
            return row.kind == RowKind::Asset && !assetAt(row, data).isValid() ? tr("asset.none") : display(row, data);
        }

        // What an entity reference reads in the row: the entity's name and id, None, or the id marked missing.
        std::string entityText(const World& target, const Entity entity) const
        {
            if (!entity.isValid())
            {
                return tr("entity.none");
            }
            const std::string id = std::to_string(entity.id()); // as text: a number argument gets digit groups
            if (!target.isEntityAlive(entity))
            {
                return tr("entity.missing", {{"id", std::string_view(id)}});
            }
            std::string buffer;
            return tr("entity.named",
                      {{"name", std::string_view(entityLabel(buffer, target, entity))}, {"id", std::string_view(id)}});
        }

        // What the well of an entity reference reads (localized text; the row's value stays the id); empty for any
        // other row.
        std::string wellOf(const Row& row, const std::byte* data, const World& target) const
        {
            return row.kind == RowKind::Value && row.field.kind == FieldKind::Entity
                ? entityText(target, *reinterpret_cast<const Entity*>(data + row.offset))
                : std::string{};
        }

        // What an enum row shows: the value's name, or the number marked unknown when the names do not list it.
        std::string enumText(const Row& row) const
        {
            const i32 index = enumIndex(row.field, row.shown);
            return index >= 0 ? std::string(row.field.enumNames[index].name)
                              : tr("enum.unknown", {{"value", std::string_view(row.shown)}});
        }

        // The header row's name field shows this: the entity's name, the fallback label ("Cube #4") for an unnamed one.
        std::string primaryName(const World& target) const
        {
            std::string buffer;
            return selection.size() == 1 ? std::string(entityLabel(buffer, target, primary())) : std::string{};
        }

        EnabledState enabledOf(const World& target) const
        {
            EnabledState state;
            if (selection.empty())
            {
                return state;
            }
            const ComponentID disabled = getComponentId<DisabledComponent>();
            state.toggle = true;
            state.enabled = !target.hasComponentById(primary(), disabled);
            state.inactive = state.enabled && target.hasComponentById(primary(), getComponentId<InactiveComponent>());
            for (const Entity entity : selection)
            {
                if (target.isEntityAlive(entity) && target.hasComponentById(entity, disabled) == state.enabled)
                {
                    state.mixed = 1;
                }
            }
            return state;
        }

        std::string summary(const World& target) const
        {
            if (selection.empty())
            {
                return tr("nothing");
            }
            if (selection.size() > 1)
            {
                return tr("summary.many", {{"count", static_cast<std::int64_t>(selection.size())}});
            }
            std::string buffer;
            return tr("entity.named",
                      {{"name", entityLabel(buffer, target, primary())}, {"id", std::to_string(primary().id())}});
        }

        void addRow(Row row) { rows.push_back(std::move(row)); }

        // Equal fields, or one names a part of the other ("A.b" and "A.b.c", "A" and "A.b", "A.l" and "A.l[2]"): the
        // rule the control server applies to a step's fields
        static bool fieldsOverlap(const std::string_view a, const std::string_view b)
        {
            const auto [shorter, longer] = a.size() <= b.size() ? std::pair(a, b) : std::pair(b, a);
            return !shorter.empty() && longer.starts_with(shorter) &&
                (longer.size() == shorter.size() || longer[shorter.size()] == '.' || longer[shorter.size()] == '[');
        }

        static std::string localClock(const std::int64_t epochMs)
        {
            if (epochMs <= 0)
            {
                return "--:--:--";
            }
            const auto seconds = static_cast<std::time_t>(epochMs / 1000);
            std::tm local{};
            spite::platform::toLocalTime(seconds, local);
            return std::format("{:02}:{:02}:{:02}", local.tm_hour, local.tm_min, local.tm_sec);
        }

        // The agent block under the summary: who changed the entity and when with the count of changed fields,
        // Revert and Keep and, after a refusal, why. Each changed field has its own row under it (addChangeRow).
        void addAgentRows(const EditorAgentLayer::Change& change)
        {
            const std::string client = change.client.empty() ? tr("agent.unnamed") : change.client;
            const auto fields = static_cast<std::int64_t>(change.unreviewed.size());
            addRow(
                Row{.kind = RowKind::Agent,
                    .label = change.byEditor
                        ? tr("agent.pending", {{"fields", fields}})
                        : tr("agent.by", {{"client", client}, {"time", localClock(change.timeMs)}, {"fields", fields}}),
                    .path = std::to_string(change.step),
                    .editable = !change.byEditor && change.step != 0,
                    .count = change.stepEntities,
                    .changed = true});
            if (agentNoteStep != change.step)
            {
                agentNote.clear();
            }
            if (!agentNote.empty())
            {
                addRow(Row{.kind = RowKind::Note, .depth = 1, .label = tr("agent.refused", {{"reason", agentNote}})});
            }
        }

        // -- the readout strip --------------------------------------------------

        [[nodiscard]] bool stripOn() const { return readoutOn && selection.size() == 1; }

        // Takes the scheduler's reading of a phase (systemcost::hold); a change of the enabled flag shows too.
        void absorb(SystemHit& hit, const float avgMs, const bool enabled) const
        {
            hit.avgMs = avgMs;
            const bool moved = hit.enabled != enabled;
            hit.enabled = enabled;
            hit.dirty = systemcost::hold(hit.heldMs, hit.warn, avgMs, budgetMs) || moved || hit.dirty;
        }

        // Calls fn(index into hits, average ms, enabled) with the scheduler's current reading of each hit.
        template <typename Fn>
        void forTimings(const World& target, Fn&& fn) const
        {
            const auto text = [](const char* value) { return value ? std::string_view(value) : std::string_view{}; };
            for (const SystemDescriptor& system : target.getSystems())
            {
                for (sizet i = 0; i < hits.size(); ++i)
                {
                    const SystemHit& hit = hits[i];
                    if (text(hit.name) == text(system.name) && text(hit.module) == text(system.moduleName))
                    {
                        fn(i, (hit.fixed ? system.fixedTiming : system.updateTiming).avgMs, system.enabled);
                    }
                }
            }
        }

        // The phases of the world's systems that touch the primary entity (systemcost::touches).
        void matchSystems(const World& target)
        {
            SPITE_ZONE("EditorInspector.systems");
            std::vector<SystemHit> previous = std::move(hits);
            const bool sameEntity = hitWorld == worldName && hitEntity == primary().id();
            hitWorld = worldName;
            hitEntity = primary().id();
            hitAspect = primaryAspect;
            hitRevision = target.systemRevision();
            hits.clear();
            for (const SystemDescriptor& system : target.getSystems())
            {
                for (const bool fixed : {false, true})
                {
                    const std::optional<SystemPhaseDescriptor>& phase = fixed ? system.fixedUpdate : system.update;
                    if (!phase)
                    {
                        continue;
                    }
                    if (!systemcost::touches(*phase, primaryAspect))
                    {
                        continue;
                    }
                    SystemHit hit;
                    hit.module = system.moduleName;
                    hit.name = system.name;
                    hit.fixed = fixed;
                    hit.stage = phase->stage;
                    hit.qualified = system.moduleName && system.moduleName[0]
                        ? std::format("{}::{}", system.moduleName, system.name)
                        : std::string(system.name);
                    hit.path = std::format("readout.system.{}.{}", hit.qualified, fixed ? "fixed" : "update");
                    if (const auto was = std::ranges::find(previous, hit.path, &SystemHit::path);
                        sameEntity && was != previous.end())
                    {
                        hit.heldMs = was->heldMs;
                        hit.warn = was->warn;
                    }
                    absorb(hit, (fixed ? system.fixedTiming : system.updateTiming).avgMs, system.enabled);
                    hits.push_back(std::move(hit));
                }
            }
            std::ranges::stable_sort(hits, {}, [](const SystemHit& hit) { return std::pair(hit.fixed, hit.stage); });
        }

        // The systems the strip's tooltip lists: quiet the ones over budget, full all of them, the over-budget ones
        // first.
        [[nodiscard]] std::vector<const SystemHit*> listedHits() const
        {
            std::vector<const SystemHit*> listed;
            for (const SystemHit& hit : hits)
            {
                if (telemetryFull || hit.warn)
                {
                    listed.push_back(&hit);
                }
            }
            std::ranges::stable_partition(listed, [](const SystemHit* hit) { return hit->warn; });
            return listed;
        }

        [[nodiscard]] std::string costText(const SystemHit& hit) const
        {
            if (!hit.enabled)
            {
                return tr("readout.off");
            }
            return tr(hit.warn ? "readout.costOver" : "readout.cost",
                      {{"ms", std::round(static_cast<double>(hit.heldMs) * 100.0) / 100.0}});
        }

        [[nodiscard]] std::string systemsText() const
        {
            const auto count = static_cast<std::int64_t>(hits.size());
            const auto over = static_cast<std::int64_t>(std::ranges::count_if(hits, &SystemHit::warn));
            return over == 0 ? tr("readout.systems", {{"count", count}})
                             : tr("readout.systemsOver",
                                  {{"count", count}, {"over", over}, {"budget", static_cast<double>(budgetMs)}});
        }

        // The entity's last edit, from the provenance the agent layer reads once per command generation.
        [[nodiscard]] Cell lastEditCell()
        {
            const EditorAgentLayer::LastEdit* last = agent ? agent->lastEditOf(worldName, primary()) : nullptr;
            if (agent)
            {
                seenLastEdit = agent->lastEditRevision();
            }
            if (!last)
            {
                return Cell{.value = tr("readout.none"), .tip = tr("readout.edit.none")};
            }
            const std::string who =
                last->client.empty() ? last->source : std::format("{} ({})", last->client, last->source);
            const std::string time = localClock(last->timeMs);
            return Cell{.value = time, .tip = tr("readout.edit", {{"who", who}, {"time", time}})};
        }

        [[nodiscard]] std::array<Cell, CELLS> subtitleCells()
        {
            std::array<Cell, CELLS> cells;
            cells[0].value = tr("header.systems", {{"count", static_cast<std::int64_t>(hits.size())}});
            cells[0].tip = systemsText();
            for (const SystemHit& hit : hits)
                cells[0].tip +=
                    std::format("\n{}: {}", hit.fixed ? tr("readout.fixed", {{"name", hit.qualified}}) : hit.qualified,
                                costText(hit));
            cells[1] = lastEditCell();
            cells[1].value = tr("header.lastWrite", {{"time", cells[1].value}});
            return cells;
        }

        // What the subtitle draws. Tooltips read the latest values on hover.
        [[nodiscard]] static std::string cellsKey(const std::array<Cell, CELLS>& cells)
        {
            std::string key;
            for (const Cell& cell : cells)
            {
                key += cell.value;
                key += cell.warn ? "!\x1f" : "\x1f";
            }
            return key;
        }

        void updateSubtitle()
        {
            if (rows.size() < 2 || rows[1].kind != RowKind::Subtitle)
                return;
            Row& row = rows[1];
            auto cells = subtitleCells();
            if (row.cells == cells)
                return;
            const std::string key = cellsKey(cells);
            row.cells = std::move(cells);
            if (key == row.shown)
                return;
            row.shown = key;
            ++stats.refreshed;
            for (u32 s = 0; s < slots.size(); ++s)
            {
                if (slots[s].row == 1)
                    bind(s);
            }
        }

        // The costs again, every systemcost::REFRESH_FRAMES while the entity is selected and the panel shows.
        void readCosts(const World& target)
        {
            SPITE_ZONE("EditorInspector.costs");
            sinceCosts = 0;
            forTimings(target, [this](const sizet i, const float avgMs, const bool enabled)
                       { absorb(hits[i], avgMs, enabled); });
            if (std::ranges::any_of(hits, &SystemHit::dirty))
            {
                updateSubtitle();
                for (SystemHit& hit : hits)
                {
                    hit.dirty = false;
                }
            }
        }

        // The telemetry setting and the system budget in force (the shell, at start and at every editorSettings).
        void setTelemetry(const bool full, const float budget)
        {
            if (readoutOn && full == telemetryFull && budget == budgetMs)
            {
                return;
            }
            readoutOn = true;
            telemetryFull = full;
            budgetMs = budget;
            for (SystemHit& hit : hits)
            {
                hit.heldMs = -1.0f;
                absorb(hit, hit.avgMs, hit.enabled);
            }
            rebuild(false);
        }

        // The notes for the entries of a step that no change line took: the entry and the value it had, at most
        // MAX_NOTES and then "and N more".
        [[nodiscard]] std::vector<Row> unclaimedNotes(const EditorAgentLayer::Change& change) const
        {
            std::vector<Row> notes;
            sizet open = 0;
            for (const std::string& field : change.unreviewed)
            {
                if (std::ranges::contains(claimedChanges, field))
                {
                    continue;
                }
                if (open++ >= MAX_NOTES)
                {
                    continue;
                }
                const std::string shownField = pathWords(field);
                std::string label = shownField;
                if (const auto was = std::ranges::find(change.previous, field, &EditorAgentLayer::Previous::field);
                    was != change.previous.end() && !was->value.empty())
                {
                    // a string reads as itself, anything else as its JSON text
                    const Json value = Json::parse(was->value, nullptr, false);
                    label = tr(
                        "agent.was",
                        {{"field", shownField}, {"value", value.is_string() ? value.get<std::string>() : was->value}});
                }
                notes.push_back(
                    Row{.kind = RowKind::Note, .depth = 1, .label = std::move(label), .path = field, .changed = true});
            }
            if (open > MAX_NOTES)
            {
                notes.push_back(Row{.kind = RowKind::Note,
                                    .depth = 1,
                                    .label = tr("agent.more", {{"count", static_cast<std::int64_t>(open - MAX_NOTES)}}),
                                    .changed = true});
            }
            return notes;
        }

        // The line under a field the client changed: "client - time - was value", Revert and Keep. A field whose old
        // value was too large to carry reads without it.
        void addChangeRow(const EditorAgentLayer::Change& change, const std::string& path, const u32 depth)
        {
            if (!std::ranges::contains(change.unreviewed, path) || std::ranges::contains(claimedChanges, path))
            {
                return;
            }
            claimedChanges.push_back(path);
            const std::string client = change.client.empty() ? tr("agent.unnamed") : change.client;
            const std::string time = localClock(change.timeMs);
            std::string label = tr("agent.fieldBy", {{"client", client}, {"time", time}});
            if (const auto was = std::ranges::find(change.previous, path, &EditorAgentLayer::Previous::field);
                was != change.previous.end() && !was->value.empty())
            {
                // a string reads as itself, anything else as its JSON text
                const Json value = Json::parse(was->value, nullptr, false);
                label = tr("agent.fieldWas",
                           {{"client", client},
                            {"time", time},
                            {"value", value.is_string() ? value.get<std::string>() : was->value}});
            }
            addRow(Row{.kind = RowKind::Change,
                       .depth = depth,
                       .label = std::move(label),
                       .path = std::to_string(change.step),
                       .editable = !change.byEditor && change.step != 0,
                       .count = change.stepEntities,
                       .changed = true});
        }

        // A field's value row; the change line under it when a client changed it, the note when its entry was refused
        void addValueRow(Row row)
        {
            row.kind = RowKind::Value;
            row.euler = row.field.kind == FieldKind::Quat && row.editable && !rawQuat;
            const bool failed = !fieldError.key.empty() && fieldError.path == row.path && fieldError.column == row.lane;
            const u32 depth = row.depth + 1;
            const std::string path = row.path;
            const bool firstLane = row.lane == 0;
            addRow(std::move(row));
            if (rowChange && firstLane)
            {
                addChangeRow(*rowChange, path, depth);
            }
            if (failed)
            {
                addRow(Row{.kind = RowKind::Note, .depth = depth, .label = message(fieldError.key), .error = true});
            }
        }

        void addElement(const FieldDesc& field, const ComponentID component, const u32 offset, const std::string& path,
                        const u32 depth, const bool parentEditable, std::string label)
        {
            const bool persisted = (field.flags & fieldFlags::PERSISTED) != 0;
            const bool editable = parentEditable && persisted && (field.flags & fieldFlags::READONLY) == 0;
            Row row{.component = component,
                    .depth = depth,
                    .label = std::move(label),
                    .path = path,
                    .field = field,
                    .offset = offset,
                    .editable = editable};
            if (field.kind == FieldKind::Struct && isAssetHandle(*field.structSchema))
            {
                // bindings are immutable: a pick is remove + add, so READONLY does not stop it
                row.kind = RowKind::Asset;
                row.editable = parentEditable && persisted;
                addRow(std::move(row));
                return;
            }
            if (component == getComponentId<SkinComponent>() && field.kind == FieldKind::InternedString &&
                field.assetType == "SKELETON")
            {
                row.kind = RowKind::Asset;
                row.editable = parentEditable && persisted;
                addRow(std::move(row));
                return;
            }
            if (field.kind == FieldKind::Struct)
            {
                row.kind = RowKind::Group;
                row.editable = false; // its fields edit
                addRow(std::move(row));
                if (rowChange)
                {
                    addChangeRow(*rowChange, path, depth + 1);
                }
                for (const FieldDesc& inner : field.structSchema->fields)
                {
                    addField(inner, component, offset + inner.offset, path + "." + std::string(inner.name), depth + 1,
                             editable);
                }
                return;
            }
            if (field.kind == FieldKind::Mat4)
            {
                Row group = row;
                group.kind = RowKind::Group;
                group.editable = false; // its columns edit
                addRow(std::move(group));
                for (u32 column = 0; column < LANES; ++column)
                {
                    Row lane = row;
                    lane.kind = RowKind::Value;
                    lane.depth = depth + 1;
                    lane.label = tr("matrix.column", {{"index", static_cast<std::int64_t>(column)}});
                    lane.lane = column;
                    addValueRow(std::move(lane));
                }
                return;
            }
            row.color = (field.flags & fieldFlags::COLOR) != 0 &&
                (field.kind == FieldKind::Vec3 || field.kind == FieldKind::Vec4);
            addValueRow(std::move(row));
        }

        void addField(const FieldDesc& field, const ComponentID component, const u32 offset, const std::string& path,
                      const u32 depth, const bool parentEditable)
        {
            if (field.arrayCount == 0)
            {
                addElement(field, component, offset, path, depth, parentEditable, identifierWords(field.name));
                return;
            }
            addRow(Row{.kind = RowKind::Group,
                       .component = component,
                       .depth = depth,
                       .label = std::format("{} [{}]", identifierWords(field.name), field.arrayCount),
                       .path = path,
                       .field = field,
                       .offset = offset});
            FieldDesc element = field;
            element.arrayCount = 0;
            for (u32 i = 0; i < field.arrayCount; ++i)
            {
                addElement(element, component, offset + i * field.elementSize, std::format("{}[{}]", path, i),
                           depth + 1, parentEditable, std::format("[{}]", i));
            }
        }

        // Rebuilds the rows from the selection's shared components; with
        // onlyIfChanged, only when those differ from the shown ones.
        void rebuild(const bool onlyIfChanged)
        {
            const bool schemaChanged = rowsRegistryRevision != getComponentRegistryRevision();
            rowsRegistryRevision = getComponentRegistryRevision();
            if (schemaChanged && assetModal.isValid() && assetModal.isOpen())
                assetModal.close();
            SPITE_ZONE("EditorInspector.rebuild");
            model->pruneSelection();
            seenSelection = model->selectionRevision();
            const World* target = world();
            std::vector<ComponentID> components = target ? sharedComponents(*target) : std::vector<ComponentID>{};
            if (picker && picker->isOpen() && !std::ranges::contains(components, colorRow.component))
            {
                picker->close(); // the component is gone from every selected entity
            }
            primaryAspect.clear();
            if (!selection.empty())
            {
                ComponentID ids[MAX_COMPONENTS];
                primaryAspect.assign(ids, ids + std::min(target->getEntityAspectIds(primary(), ids), MAX_COMPONENTS));
            }
            if (onlyIfChanged && !schemaChanged && !rows.empty() &&
                (rows.back().kind == RowKind::Option) == !selection.empty() && components == sharedShown)
            {
                refresh(true);
                return;
            }
            sharedShown = components;

            ++stats.rebuilds;
            popup.close(); // its rows name what the old rows showed
            rows.clear();
            shown.clear();
            const EnabledState flag = target ? enabledOf(*target) : EnabledState{};
            addRow(Row{.kind = RowKind::Summary,
                       .label = target ? primaryName(*target) : std::string{},
                       .editable = target && selection.size() == 1,
                       .mixed = flag.mixed,
                       .toggle = flag.toggle,
                       .enabled = flag.enabled,
                       .inactive = flag.inactive,
                       .shown = target ? summary(*target) : tr("nothing")});
            if (target && stripOn())
            {
                if (hitWorld != worldName || hitEntity != primary().id() || hitAspect != primaryAspect ||
                    hitRevision != target->systemRevision())
                {
                    matchSystems(*target);
                }
                sinceCosts = 0;
                Row subtitle{.kind = RowKind::Subtitle, .cells = subtitleCells()};
                subtitle.shown = cellsKey(subtitle.cells);
                addRow(std::move(subtitle));
            }
            else
            {
                hits.clear();
                hitRevision = ~u64{0};
            }
            // what a control client changed in the one selected entity and nobody reviewed
            const EditorAgentLayer::Change* change =
                agent && target && selection.size() == 1 ? agent->changeOf(worldName, primary()) : nullptr;
            if (agent)
            {
                seenChange = agent->changeRevision();
                seenAgentOn = agent->enabled();
            }
            claimedChanges.clear();
            sizet agentEnd = 0;
            if (change)
            {
                addAgentRows(*change);
                agentEnd = rows.size();
                rowChange = *change;
            }
            else
            {
                agentNote.clear();
                rowChange.reset();
            }
            // a component's header and fields; `editable` false: the Runtime section's, shown without edits or Remove
            const auto addComponentRows = [&](const ComponentID component, const bool editable)
            {
                const ComponentMetadata& metadata = getComponentMetadata(component);
                Shown range{component, static_cast<u32>(rows.size()), 0};
                const bool folded = std::ranges::contains(collapsed, component);
                addRow(Row{.kind = RowKind::Header,
                           .component = component,
                           .label = metadata.schema ? componentTitle(component)
                                                    : tr("header.noSchema", {{"name", componentTitle(component)}}),
                           .path = std::string(metadata.name.toView()),
                           .editable = editable,
                           .shown = folded ? "closed" : "open",
                           // engine components are the default: only the others carry their group's tag
                           .tag = metadata.category == ComponentCategory::Engine
                               ? std::string{}
                               : tr(std::format("tag.{}", groupName(metadata.category)))});
                if (metadata.schema && !folded)
                {
                    for (const FieldDesc& field : metadata.schema->fields)
                    {
                        addField(field, component, field.offset,
                                 std::string(metadata.name.toView()) + "." + std::string(field.name), 1, editable);
                    }
                }
                range.end = static_cast<u32>(rows.size());
                shown.push_back(range);
            };
            std::vector<ComponentID> runtime;
            for (const ComponentID component : components)
            {
                if (getComponentMetadata(component).category == ComponentCategory::RuntimeDerived)
                {
                    runtime.push_back(component);
                    continue;
                }
                addComponentRows(component, true);
            }
            if (!runtime.empty())
            {
                addRow(Row{.kind = RowKind::Section,
                           .label = tr("runtime", {{"count", static_cast<std::int64_t>(runtime.size())}}),
                           .path = "Runtime",
                           .shown = showRuntime ? "on" : "off"});
                for (const ComponentID component : showRuntime ? runtime : std::vector<ComponentID>{})
                {
                    addComponentRows(component, false);
                }
            }
            if (!selection.empty())
            {
                addRow(Row{.kind = RowKind::Add, .label = tr("button.add")});
                addRow(Row{.kind = RowKind::Option,
                           .label = tr("option.engine"), // short: a label cannot ellipsize
                           .shown = showEngine ? "on" : "off"});
            }
            if (change)
            {
                // an entry no field line claimed (an asset swap, a struct, a component added or removed, a field of a
                // folded, Runtime or hidden component) keeps a note under the block
                const std::vector<Row> notes = unclaimedNotes(*change);
                rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(agentEnd), notes.begin(), notes.end());
                for (Shown& range : shown)
                {
                    range.first += static_cast<u32>(notes.size());
                    range.end += static_cast<u32>(notes.size());
                }
            }
            for (const Shown& range : shown)
            {
                const auto* data =
                    static_cast<const std::byte*>(target->getConstComponentDataById(primary(), range.component));
                for (u32 r = range.first; r < range.end; ++r)
                {
                    if (rows[r].kind == RowKind::Value || rows[r].kind == RowKind::Asset)
                    {
                        rows[r].shown = shownText(rows[r], data);
                        rows[r].name = wellOf(rows[r], data, *target);
                        rows[r].mixed = mixedOf(rows[r], *target);
                    }
                    // the fields the client changed (a component-level change marks the whole component)
                    if (change && !rows[r].path.empty() &&
                        std::ranges::any_of(change->unreviewed, [&](const std::string& field)
                                            { return fieldsOverlap(rows[r].path, field); }))
                    {
                        rows[r].changed = true;
                    }
                }
            }
            // slots the list hides keep no row; it rebinds the shown ones
            for (Slot& slot : slots)
            {
                slot.row = NO_ROW;
            }
            list.setRowCount(static_cast<u32>(rows.size()));
            showHint();
        }

        // With nothing selected the list (its one summary row) gives the slot to the hint, which says what is
        // selected and what to do.
        void showHint()
        {
            const bool none = selection.empty();
            if (none != hintShown)
            {
                hintShown = none;
                hint.setVisible(none);
                list.setVisible(!none);
            }
            if (none)
            {
                if (const std::string text = tr("nothing.hint"); hint.text() != text)
                {
                    hint.setText(text);
                }
            }
        }

        // Whether the component changed in any selected entity: a multi-edit moves the others too, and a difference
        // between the entities (the mixed marker) can come from any of them.
        bool modified(const World& target, const ComponentID component) const
        {
            return std::ranges::any_of(
                selection, [&](const Entity entity)
                { return target.isEntityAlive(entity) && target.wasComponentModifiedById(entity, component); });
        }

        // The lanes (bit 0 for a row of another kind) where a selected entity's value differs from the primary's, at
        // the precision the row shows.
        u8 mixedOf(const Row& row, const World& target) const
        {
            if (selection.size() < 2)
            {
                return 0;
            }
            const auto* base =
                static_cast<const std::byte*>(target.getConstComponentDataById(primary(), row.component));
            if (!base)
            {
                return 0;
            }
            UiNumericComponent range;
            numericRange(row.field, range);
            const double tolerance = 0.5 * std::pow(10.0, -static_cast<double>(range.decimals));
            const bool lanes = editorOf(row) == Editor::Numbers;
            const std::array<double, LANES> first =
                lanes ? laneValues(row, base, eulerMemo) : std::array<double, LANES>{};
            u8 mask = 0;
            forSelected(row,
                        [&](const Entity entity, const std::byte* data)
                        {
                            if (entity == primary())
                            {
                                return;
                            }
                            if (!lanes)
                            {
                                mask |= display(row, data) != display(row, base) ? 1 : 0;
                                return;
                            }
                            const std::array<double, LANES> mine = laneValues(row, data, eulerMemo);
                            for (u32 lane = 0; lane < valueLanes(row); ++lane)
                            {
                                if (std::abs(mine[lane] - first[lane]) > tolerance)
                                {
                                    mask |= static_cast<u8>(1u << lane);
                                }
                            }
                        });
            return mask;
        }

        // Re-reads the rows of modified components (all with `all`) and
        // rebinds the ones whose value changed.
        void refresh(const bool all)
        {
            const World* target = world();
            if (!target || selection.empty())
            {
                return;
            }
            SPITE_ZONE("EditorInspector.refresh");
            if (all)
            {
                refreshSummary(*target);
            }
            for (const Shown& range : shown)
            {
                if (!all && !modified(*target, range.component))
                {
                    continue;
                }
                const auto* data =
                    static_cast<const std::byte*>(target->getConstComponentDataById(primary(), range.component));
                for (u32 r = range.first; r < range.end; ++r)
                {
                    if (rows[r].kind == RowKind::Value || rows[r].kind == RowKind::Asset)
                    {
                        update(r, shownText(rows[r], data), mixedOf(rows[r], *target), wellOf(rows[r], data, *target));
                        if (picker && picker->isOpen() && rows[r].color && rows[r].path == colorRow.path)
                        {
                            picker->setColor(colorAt(rows[r], data)); // another writer changed the colour
                        }
                    }
                }
            }
        }

        // The header row: the name field, the summary text and the enabled checkbox, rebound when one of them moved.
        void refreshSummary(const World& target)
        {
            Row& row = rows[0];
            std::string label = primaryName(target);
            std::string text = summary(target);
            const EnabledState flag = enabledOf(target);
            if (row.label == label && row.shown == text && row.mixed == flag.mixed && row.toggle == flag.toggle &&
                row.enabled == flag.enabled && row.inactive == flag.inactive)
            {
                return;
            }
            row.label = std::move(label);
            row.shown = std::move(text);
            row.mixed = flag.mixed;
            row.toggle = flag.toggle;
            row.enabled = flag.enabled;
            row.inactive = flag.inactive;
            ++stats.refreshed;
            for (u32 s = 0; s < slots.size(); ++s)
            {
                if (slots[s].row == 0)
                {
                    bind(s);
                }
            }
        }

        void update(const u32 r, std::string value, const u8 mixed = 0, std::string name = {})
        {
            if (rows[r].shown == value && rows[r].mixed == mixed && rows[r].name == name)
            {
                return;
            }
            rows[r].shown = std::move(value);
            rows[r].mixed = mixed;
            rows[r].name = std::move(name);
            ++stats.refreshed;
            for (u32 s = 0; s < slots.size(); ++s)
            {
                if (slots[s].row == r)
                {
                    bind(s);
                }
            }
        }

        // -- view ------------------------------------------------------------------

        Slot& createSlot(const u32 index)
        {
            Slot& slot = slots.emplace_back();
            slot.root = scope.createPanel("inspector.row", topLeft());
            slot.root.setStyleClass(classes.root);
            const auto child = [&slot](Widget widget)
            {
                widget.setParent(slot.root);
                widget.setVisible(false);
            };
            slot.label = scope.createLabel("inspector.row.label", "", topLeft());
            child(slot.label);
            slot.assetIcon = scope.createLabel("inspector.row.assetIcon", "", topLeft());
            slot.assetIcon.setStyleClass(classes.label);
            slot.assetIcon.setAccessibleRole(UiRole::Presentation);
            slot.assetIcon.setInteractable(false);
            child(slot.assetIcon);
            labelTip.attach(slot.label);
            // before the lanes: tab stops follow creation order, and the slider sits left of its number
            UiSliderDesc sliderDesc;
            sliderDesc.rect = topLeft();
            slot.slider = scope.createSlider(InternedString("inspector.row.slider"), sliderDesc);
            child(slot.slider);
            slot.slider.onChange([this, index](const double value) { slid(index, value); });
            for (u32 lane = 0; lane < LANES; ++lane)
            {
                UiNumberDesc desc;
                desc.rect = topLeft();
                slot.numbers[lane] = scope.createNumberField("inspector.row.number", desc);
                slot.numbers[lane].setStyleClass(classes.field);
                child(slot.numbers[lane]);
                slot.numbers[lane].onNumber([this, index, lane](const double value)
                                            { editNumber(index, lane, value); });
                slot.numbers[lane].onChange([this, index, lane](std::string_view) { typed(index, lane); });
                slot.numbers[lane].onInvalid([this, index, lane](std::string_view) { rejected(index, lane); });
                // Enter commits and lets go: Ctrl+Z then undoes the entity edit, not the text
                slot.numbers[lane].onSubmit([this](std::string_view) { scope.router().clearFocus(); });
                // a mixed lane shows its marker again once the field lets go
                slot.numbers[lane].onFocus(
                    [this, index](const bool gained)
                    {
                        if (!gained)
                        {
                            slots[index].key.clear();
                        }
                    });
            }
            for (u32 lane = 0; lane < LANES; ++lane)
            {
                slot.axes[lane] = scope.createLabel("inspector.row.axis", "", topLeft());
                slot.axes[lane].setStyleClass(classes.axis[lane]);
                slot.axes[lane].setAccessibleRole(UiRole::Presentation); // the lane's own name carries the axis
                child(slot.axes[lane]);
            }
            slot.unit = scope.createLabel("inspector.row.unit", "", topLeft());
            slot.unit.setStyleClass(classes.unit);
            slot.unit.setAccessibleRole(UiRole::Presentation); // the lanes' and the slider's names carry the unit
            slot.unit.setInteractable(false);
            child(slot.unit);
            slot.tag = scope.createLabel("inspector.row.tag", "", topLeft());
            slot.tag.setStyleClass(classes.tag);
            child(slot.tag);
            slot.rule = scope.createPanel("inspector.row.rule", topLeft());
            slot.rule.setStyleClass(classes.rule);
            slot.rule.setInteractable(false);
            child(slot.rule);
            for (u32 cell = 0; cell < CELLS; ++cell)
            {
                slot.cells[cell] = scope.createLabel("inspector.row.cell", "", topLeft());
                child(slot.cells[cell]);
                cellTip.attach(slot.cells[cell]);
            }
            // the name field first, then the checkbox drawn to its right (tab stops follow creation order)
            slot.text = scope.createTextInput("inspector.row.text", topLeft());
            slot.text.setStyleClass(classes.field);
            child(slot.text);
            slot.text.onCommit([this, index](const std::string_view text) { editText(index, text); });
            slot.text.onSubmit([this](std::string_view) { scope.router().clearFocus(); });
            slot.text.onFocus(
                [this, index](const bool gained)
                {
                    if (!gained)
                    {
                        slots[index].key.clear();
                    }
                });
            UiRectDesc boxDesc = topLeft();
            boxDesc.contentSize = UiContentSizeComponent{.fitWidth = false, .fitHeight = false}; // bind sizes it
            slot.enabled = scope.createCheckbox("inspector.row.enabled", "", boxDesc, true);
            child(slot.enabled);
            // the label's line is taller than the row at a large text scale: bind fixes its height to the row's
            scope.world().getComponent<UiContentSizeComponent>(slot.enabled.label().entity()).fitHeight = false;
            slot.enabled.onChange([this, index](const bool checked) { checkChanged(index, checked); });
            slot.keep = scope.createButton("inspector.row.keep", "", topLeft());
            slot.keep.setStyleClass(classes.button);
            child(slot.keep);
            slot.keep.onClick([this, index] { keepPress(index); });
            slot.edge = scope.createPanel("inspector.row.edge", topLeft());
            slot.edge.setStyleClass(classes.edge);
            child(slot.edge);
            slot.edge.setInteractable(false);
            slot.button = scope.createButton("inspector.row.button", "", topLeft());
            slot.button.setStyleClass(classes.button);
            child(slot.button);
            slot.button.onClick([this, index] { press(index); });
            // the lists open from the press: a press that closes the open list must not reopen it on the click
            slot.button.onPress(
                [this, index](const u8 button, Vec2)
                {
                    if (button == 0)
                    {
                        openList(index);
                    }
                });
            slot.swatch = scope.createPanel("inspector.row.swatch", topLeft());
            slot.swatch.setStyleClass(InternedString("form.swatch"));
            child(slot.swatch);
            slot.swatch.setInteractable(false);
            scope.world().addComponent<UiVisualComponent>(slot.swatch.entity(), UiVisualComponent{});
            // an asset row takes a dragged asset of its handle's type, as a pick from the list
            scope.dragDrop().addTarget(
                slot.root.entity(), [this, index](const UiDragPayload& payload, Entity, Vec2)
                { return droppedAsset(index, payload).has_value(); },
                [this, index](const UiDropEvent& event)
                {
                    if (const std::optional<AssetHandle> handle = droppedAsset(index, event.payload, true))
                    {
                        pick(rows[slots[index].row], *handle);
                    }
                });
            return slot;
        }

        // The narrowest lane (UI pixels) that shows a number at the text scale in force.
        float measuredLane()
        {
            const UiTextMeasurer* measurer = scope.hierarchy().textMeasurer();
            if (!measurer || slots.empty())
            {
                return 0.0f;
            }
            // the sample's width changes with the text scale and the locale (its fallback fonts), nothing else
            if (laneScale != shownTextScale || laneRevision != shownRevision)
            {
                laneScale = shownTextScale;
                laneRevision = shownRevision;
                lane = std::ceil(measurer->measureText(slots.front().numbers[0].entity(), LANE_SAMPLE).x +
                                 FIELD_PAD * 2.0f);
                const Entity probe = slots.front().button.entity();
                revertWidth = std::ceil(measurer->measureText(probe, tr("agent.revert")).x + FIELD_PAD * 2.0f);
                keepWidth = std::ceil(measurer->measureText(probe, tr("agent.keep")).x + FIELD_PAD * 2.0f);
                axisGlyph = 0.0f;
                for (const auto* names : {&uiform::AXES, &uiform::CHANNELS})
                {
                    for (const char* name : *names)
                    {
                        axisGlyph =
                            std::max(axisGlyph, measurer->measureText(slots.front().axes[0].entity(), message(name)).x);
                    }
                }
                pickWidth = std::ceil(measurer->measureText(probe, tr("button.pick")).x + FIELD_PAD * 2.0f);
                modeWidth = std::ceil(std::max(measurer->measureText(probe, tr("quat.euler")).x,
                                               measurer->measureText(probe, tr("quat.raw")).x) +
                                      FIELD_PAD * 2.0f);
                optionWidth = std::max(OPTION_WIDTH,
                                       std::ceil(std::max(measurer->measureText(probe, tr("option.on")).x,
                                                          measurer->measureText(probe, tr("option.off")).x) +
                                                 FIELD_PAD * 2.0f));
                const Entity text = slots.front().enabled.label().entity();
                enabledWidth = std::ceil(CHECK_BOX + CHECK_GAP +
                                         std::max({measurer->measureText(text, tr("enabled")).x,
                                                   measurer->measureText(text, tr("enabled.mixed")).x,
                                                   measurer->measureText(text, tr("enabled.inactive")).x}) +
                                         FIELD_PAD);
            }
            return lane;
        }

        // force: also rewrites a field being typed in or dragged.
        void bind(const u32 s, const bool force = false)
        {
            Slot& slot = slots[s];
            if (slot.row >= rows.size())
            {
                return;
            }
            const Row& row = rows[slot.row];
            const float width = std::max(0.0f, list.size().x - thumb);
            const Editor editor = editorOf(row);
            const bool quat = editor == Editor::Numbers && row.field.kind == FieldKind::Quat;
            const bool failed = !fieldError.key.empty() && fieldError.path == row.path &&
                fieldError.column == row.lane && row.kind == RowKind::Value;
            // the narrowest a lane may be and still show its number (and its axis tag): measured at the current text
            // scale
            const float laneBase = editor == Editor::Numbers ? measuredLane() : 0.0f;
            const bool tagged = editor == Editor::Numbers && valueLanes(row) > 1;
            const float axisWidth = tagged ? std::ceil(axisGlyph + 2.0f * density.xxs) : 0.0f;
            const float laneMin = laneBase + axisWidth;
            const bool agentRow = row.kind == RowKind::Agent || row.kind == RowKind::Change;
            const bool toggled = row.kind == RowKind::Summary && row.toggle;
            const bool fitted = agentRow || toggled || row.kind == RowKind::Header || row.kind == RowKind::Asset ||
                row.kind == RowKind::Option || (row.kind == RowKind::Value && editor == Editor::Pick);
            if (fitted)
            {
                measuredLane(); // the buttons' fitted widths follow the text scale and the locale
            }
            const bool swatch = row.color && row.kind == RowKind::Value;
            const bool enumRow = row.kind == RowKind::Value && !row.field.enumNames.empty();
            const std::string enumShown = enumRow ? enumText(row) : std::string{};
            std::string key = std::format(
                "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
                static_cast<u8>(row.kind), row.path, row.lane, row.label, row.shown, row.name, row.editable, width,
                laneMin, row.changed, row.count, fitted ? revertWidth : 0.0f, fitted ? keepWidth : 0.0f,
                fitted ? pickWidth : 0.0f, fitted ? optionWidth : 0.0f, quat ? modeWidth : 0.0f, row.euler, row.mixed,
                failed ? fieldError.lane + 1 : 0, row.error, swatch, toggled ? enabledWidth : 0.0f, row.enabled,
                row.inactive, row.tag, laneScale, laneRevision);
            if (key == slot.key)
            {
                return;
            }
            ++stats.rebinds;

            // the row is a control high; its parts sit a hairline in from the row's edges
            const float pad = density.xs;
            const float gap = density.xxs;
            const float inset = density.hairline;
            const float height = density.control - 2.0f * inset;
            const float labelX = pad + static_cast<float>(row.depth) * density.lg;
            float split = std::max(labelX, width * LABEL_SHARE);
            // a colour row's swatch sits before its lanes (or its text)
            const float swatchWidth = swatch ? std::round(height * 1.5f) : 0.0f;
            const float swatchRoom = swatch ? swatchWidth + gap : 0.0f;
            // a number row's unit follows its lanes at the width of its text (the text scale is in the key)
            const std::string_view unitText =
                editor == Editor::Numbers ? std::string_view(row.field.unit) : std::string_view{};
            const UiTextMeasurer* unitMeasurer = unitText.empty() ? nullptr : scope.hierarchy().textMeasurer();
            const float unitWidth =
                unitMeasurer ? std::ceil(unitMeasurer->measureText(slot.unit.entity(), unitText).x) : 0.0f;
            const float unitRoom = unitWidth > 0.0f ? unitWidth + gap : 0.0f;
            if (editor == Editor::Numbers)
            {
                // the label column gives way to the lanes: vector rows start their fields further left
                const float lanesWide = static_cast<float>(valueLanes(row)) * laneMin +
                    gap * static_cast<float>(valueLanes(row) - 1) + pad + (quat ? modeWidth + gap : 0.0f) + swatchRoom +
                    unitRoom;
                split = std::min(split, std::max(labelX + LABEL_MIN, width - lanesWide));
            }
            const float valueWidth = std::max(0.0f, width - split - pad);
            bool numbers = false;
            bool text = false;
            bool button = false;
            Vec2 textAt{split, inset};
            Vec2 textSize{valueWidth, height};
            Vec2 buttonAt{split, inset};
            Vec2 buttonSize{valueWidth, height};
            Vec2 labelAt{labelX, inset};
            float labelWidth = std::max(0.0f, split - labelX - gap);
            bool cellsOn = false; // the subtitle's two cells
            std::string sectionText;
            const std::string mixedText = row.mixed ? message(uiform::MIXED) : std::string{};
            std::string keepText;
            std::string_view buttonText;
            bool keep = false;
            Vec2 keepAt{};
            Vec2 keepSize{};
            std::string_view keepIcon; // the Header row's caret, in `keep`
            bool tagOn = false; // the Header row's module tag and the rule before it
            Vec2 tagAt{};
            Vec2 tagSize{};
            bool ruleOn = false;
            Vec2 ruleAt{};
            Vec2 ruleSize{};
            bool addButton = false; // the Add button: the primary look, with room for its hard shadow
            bool check = false;
            bool checkLabelled = false;
            bool checkOn = false;
            std::string checkText;
            Vec2 checkAt{};
            Vec2 checkSize{};
            switch (row.kind)
            {
            case RowKind::Summary:
                {
                    // The name gives way to the enabled checkbox at the end of the row.
                    float left = pad;
                    float right = width - pad;
                    if (toggled)
                    {
                        check = true;
                        // up to half the row; a narrower row shows the box alone (its name says what it is)
                        const float cap = std::max(0.0f, width - 2.0f * pad) * 0.5f;
                        checkLabelled = enabledWidth <= cap;
                        checkSize = Vec2{checkLabelled ? enabledWidth : std::min(CHECK_BOX, cap), height};
                        checkAt = Vec2{std::max(left, right - checkSize.x), inset};
                        checkOn = row.enabled;
                        checkText = !checkLabelled ? std::string{}
                            : row.mixed            ? tr("enabled.mixed")
                            : row.inactive         ? tr("enabled.inactive")
                                                   : tr("enabled");
                        right = checkAt.x - gap;
                    }
                    if (row.editable)
                    {
                        text = true;
                        textAt = Vec2{left, inset};
                        textSize = Vec2{std::max(0.0f, right - left), height};
                        labelWidth = 0.0f;
                    }
                    else
                    {
                        labelAt = Vec2{left, inset};
                        labelWidth = std::max(0.0f, right - left);
                    }
                }
                break;
            case RowKind::Header:
                {
                    // [caret] TITLE [rule] [TAG] [remove]: the title and the tag take the width of their text (as
                    // painted: case and tracking included), the rule what is left between them
                    slot.label.setStyleClass(classes.header);
                    slot.tag.setStyleClass(classes.tag);
                    keep = true;
                    keepIcon = row.shown == "closed" ? "caret-right" : "caret-down";
                    keepAt = Vec2{labelX, inset};
                    keepSize = Vec2{height, height};
                    const float left = labelX + height + gap;
                    float right = width - pad;
                    button = row.editable;
                    if (button)
                    {
                        buttonSize = Vec2{height, height};
                        buttonAt = Vec2{std::max(left, right - height), inset};
                        right = buttonAt.x - gap;
                    }
                    const UiTextMeasurer* measurer = scope.hierarchy().textMeasurer();
                    const float room = std::max(0.0f, right - left);
                    const float title = std::min(
                        room, measurer ? std::ceil(measurer->measureText(slot.label.entity(), row.label).x) : room);
                    const float tagWidth =
                        std::min(std::max(0.0f, room - title - gap),
                                 measurer ? std::ceil(measurer->measureText(slot.tag.entity(), row.tag).x) : 0.0f);
                    labelAt = Vec2{left, inset};
                    labelWidth = title;
                    tagOn = tagWidth > 0.0f;
                    tagAt = Vec2{right - tagWidth, inset};
                    tagSize = Vec2{tagWidth, height};
                    const float ruleFrom = left + title + density.md;
                    const float ruleTo = tagOn ? tagAt.x - density.md : right;
                    ruleOn = ruleTo - ruleFrom >= density.xs;
                    ruleAt = Vec2{ruleFrom, inset + std::floor(height * 0.5f)};
                    ruleSize = Vec2{std::max(0.0f, ruleTo - ruleFrom), density.hairline};
                }
                break;
            case RowKind::Section:
                button = true;
                sectionText = tr(row.shown == "on" ? "section.open" : "section.closed", {{"label", row.label}});
                buttonText = sectionText;
                labelWidth = 0.0f;
                buttonAt = Vec2{pad, inset};
                buttonSize = Vec2{std::max(0.0f, width - 2.0f * pad), height};
                break;
            case RowKind::Group:
            case RowKind::Note:
                labelWidth = std::max(0.0f, width - labelX - pad);
                break;
            case RowKind::Subtitle:
                cellsOn = true;
                labelWidth = 0.0f;
                break;
            case RowKind::Agent:
            case RowKind::Change:
                // [Revert] [Keep] at the row's end; Revert only for a step that is the client's and still there
                keep = true;
                keepText = tr("agent.keep");
                button = row.editable;
                sectionText = tr("agent.revert");
                buttonText = sectionText;
                {
                    // a row too narrow for both buttons at their text widths shrinks them together, never over each
                    // other
                    const float room = std::max(0.0f, width - labelX - pad - (button ? gap : 0.0f));
                    const float need = keepWidth + (button ? revertWidth : 0.0f);
                    const float fit = need > room ? room / need : 1.0f;
                    const float keepFit = std::floor(keepWidth * fit);
                    const float revertFit = button ? std::floor(revertWidth * fit) : 0.0f;
                    keepSize = Vec2{keepFit, height};
                    keepAt = Vec2{std::max(labelX, width - keepFit - pad), inset};
                    buttonSize = Vec2{revertFit, height};
                    buttonAt = Vec2{std::max(labelX, keepAt.x - gap - revertFit), inset};
                }
                labelWidth = std::max(0.0f, (button ? buttonAt.x : keepAt.x) - gap - labelX);
                break;
            case RowKind::Asset:
                text = true;
                button = row.editable;
                sectionText = tr("button.pick");
                buttonText = sectionText;
                if (button)
                {
                    textSize.x = std::max(0.0f, valueWidth - pickWidth - gap);
                    buttonAt = Vec2{split + textSize.x + gap, inset};
                    buttonSize = Vec2{pickWidth, height};
                }
                if (!assetIcon(expectedAssetType(row.field, AssetType::UNKNOWN)).empty())
                {
                    textAt.x += height;
                    textSize.x = std::max(0.0f, textSize.x - height);
                }
                break;
            case RowKind::Add:
                button = true;
                addButton = true;
                sectionText = tr("button.add");
                buttonText = sectionText;
                labelWidth = 0.0f;
                // the class's 3 px hard shadow lies right of and below the button: xs is at least that in both
                // densities
                buttonAt = Vec2{pad, inset};
                buttonSize = Vec2{std::max(0.0f, width - 2.0f * pad - density.xs), std::max(0.0f, height - density.xs)};
                break;
            case RowKind::Option:
                button = true;
                sectionText = tr(row.shown == "on" ? "option.on" : "option.off");
                buttonText = sectionText;
                buttonAt = Vec2{std::max(labelX, width - optionWidth - pad), inset};
                buttonSize = Vec2{optionWidth, height};
                labelWidth = std::max(0.0f, buttonAt.x - labelX - gap);
                break;
            case RowKind::Value:
                numbers = editor == Editor::Numbers;
                text = editor == Editor::Text || editor == Editor::None || editor == Editor::Pick;
                button = editor == Editor::Choice || editor == Editor::Pick || quat || swatch;
                check = editor == Editor::Check;
                buttonText = row.mixed ? std::string_view(mixedText) : std::string_view(enumShown);
                if (check)
                {
                    // the box and its label (the mixed marker) take the whole value cell: a press anywhere toggles
                    checkAt = Vec2{split, inset};
                    checkSize = Vec2{valueWidth, height};
                    checkOn = row.shown == "true";
                    checkText = mixedText;
                }
                if (editor == Editor::Pick)
                {
                    sectionText = tr("button.pick");
                    buttonText = sectionText;
                    textSize.x = std::max(0.0f, valueWidth - pickWidth - gap);
                    buttonAt = Vec2{split + textSize.x + gap, inset};
                    buttonSize = Vec2{pickWidth, height};
                }
                if (quat)
                {
                    sectionText = tr(row.euler ? "quat.euler" : "quat.raw");
                    buttonText = sectionText;
                    buttonSize = Vec2{modeWidth, height};
                    buttonAt = Vec2{std::max(split, width - pad - modeWidth), inset};
                }
                if (swatch)
                {
                    buttonText = {};
                    buttonSize = Vec2{swatchWidth, height};
                    textAt.x += swatchRoom;
                    textSize.x = std::max(0.0f, textSize.x - swatchRoom);
                }
                break;
            }

            const std::string_view labelText =
                row.kind == RowKind::Summary ? std::string_view(row.shown) : std::string_view(row.label);
            place(slot.label, labelWidth > 0.0f, labelAt, Vec2{labelWidth, height});
            const std::string_view icon = row.kind == RowKind::Asset
                ? assetIcon(expectedAssetType(row.field, AssetType::UNKNOWN))
                : std::string_view{};
            place(slot.assetIcon, !icon.empty(), Vec2{split, inset}, Vec2{height, height});
            if (!icon.empty())
                slot.assetIcon.setIcon(icon);
            setText(slot.label, labelText);
            slot.label.setStyleClass(row.error ? classes.error : classes.labelOf(row.kind));
            place(slot.tag, tagOn, tagAt, tagSize);
            if (tagOn)
            {
                setText(slot.tag, row.tag);
            }
            place(slot.rule, ruleOn, ruleAt, ruleSize);
            const float cellRoom = std::max(0.0f, width - 2.0f * pad);
            float systemsWidth = 0.0f;
            if (cellsOn)
            {
                slot.cells[0].setStyleClass(classes.subtitle);
                const UiTextMeasurer* measurer = scope.hierarchy().textMeasurer();
                systemsWidth = measurer
                    ? std::min(cellRoom, std::ceil(measurer->measureText(slot.cells[0].entity(), row.cells[0].value).x))
                    : cellRoom * 0.5f;
            }
            for (u32 cell = 0; cell < CELLS; ++cell)
            {
                const float x = cell == 0 ? pad : pad + systemsWidth;
                const float w = cell == 0 ? systemsWidth : cellRoom - systemsWidth;
                place(slot.cells[cell], cellsOn, Vec2{x, inset}, Vec2{w, height});
                if (cellsOn)
                {
                    setText(slot.cells[cell], row.cells[cell].value);
                    slot.cells[cell].setStyleClass(classes.subtitle);
                    slot.cells[cell].setAccessibleName(row.cells[cell].value);
                }
            }

            const u32 lanes = numbers ? valueLanes(row) : 0;
            const World* target = world();
            const auto* data = (lanes || swatch) && target
                ? static_cast<const std::byte*>(target->getConstComponentDataById(primary(), row.component))
                : nullptr;
            const float lanesRoom = valueWidth - (quat ? modeWidth + gap : 0.0f) - swatchRoom - unitRoom;
            // a ranged float's slider takes what its lane does not need, up to two thirds of the room (the number stays
            // readable), while that is two controls wide at least
            const float sliderWidth = lanes == 1 && slidable(row)
                ? std::floor(std::min(lanesRoom - laneMin - gap, lanesRoom * 2.0f / 3.0f))
                : 0.0f;
            const bool sliderOn = data && sliderWidth >= 2.0f * height;
            const float sliderRoom = sliderOn ? sliderWidth + gap : 0.0f;
            const float laneWidth = lanes
                ? std::max(0.0f,
                           (lanesRoom - sliderRoom - gap * static_cast<float>(lanes - 1)) / static_cast<float>(lanes))
                : 0.0f;
            const bool previewing = slide.slot == s; // a slider drag shows its value: the lane keeps it too
            bool deferred = previewing;
            const std::array<double, LANES> laneAt =
                data ? laneValues(row, data, eulerMemo) : std::array<double, LANES>{};
            // a vector's lanes lead with their axis tag while each lane is wide enough for the tag and a field as wide
            const float tagPart = tagged && laneWidth >= 2.0f * axisWidth ? axisWidth : 0.0f;
            for (u32 lane = 0; lane < LANES; ++lane)
            {
                NumberField& field = slot.numbers[lane];
                const bool used = lane < lanes && data;
                if (used)
                {
                    // a vector's lanes are told apart for assistive tooling
                    const std::string axisName = lanes > 1
                        ? std::format("{} {}", labelText, (row.color ? "RGBA" : "XYZW")[lane])
                        : std::string(labelText);
                    const std::string name = unitText.empty()
                        ? axisName
                        : tr("unitName", {{"name", std::string_view(axisName)}, {"unit", unitText}});
                    field.setAccessibleName((row.mixed >> lane) & 1 ? tr("mixedName", {{"name", name}}) : name);
                    field.setStyleClass(failed && fieldError.lane == lane ? classes.fieldError : classes.field);
                }
                const float laneAt0 = split + swatchRoom + sliderRoom + static_cast<float>(lane) * (laneWidth + gap);
                const bool axisOn = used && tagPart > 0.0f;
                place(slot.axes[lane], axisOn, Vec2{laneAt0, inset}, Vec2{tagPart, height});
                if (axisOn)
                {
                    setText(slot.axes[lane], message(row.color ? uiform::CHANNELS[lane] : uiform::AXES[lane]));
                    slot.axes[lane].setStyleClass(row.color ? classes.axis[LANES - 1] : classes.axis[lane]);
                }
                place(field, used, Vec2{laneAt0 + tagPart, inset}, Vec2{laneWidth - tagPart, height});
                if (!used)
                {
                    continue;
                }
                if (!force && (busy(field) || previewing))
                {
                    deferred = true; // typing or dragging: the field keeps its own text
                    continue;
                }
                auto& numeric = scope.world().getComponent<UiNumericComponent>(field.entity());
                UiNumericComponent range = numeric;
                numericRange(row.field, range);
                if (range.decimals != numeric.decimals || range.min != numeric.min || range.max != numeric.max)
                {
                    numeric.decimals = range.decimals;
                    numeric.min = range.min;
                    numeric.max = range.max;
                    numeric.step = range.step;
                }
                field.setNumber(laneAt[lane]);
                if ((row.mixed >> lane) & 1)
                {
                    field.setText(mixedText);
                }
            }
            place(slot.slider, sliderOn, Vec2{split + swatchRoom, inset}, Vec2{sliderWidth, height});
            if (sliderOn)
            {
                // the primary's value; a drag in progress keeps its own
                if (slot.slider.min() != row.field.min || slot.slider.max() != row.field.max ||
                    slot.slider.step() != row.field.step)
                {
                    slot.slider.setRange(row.field.min, row.field.max, row.field.step);
                }
                if ((force || !previewing) && slot.slider.value() != laneAt[0])
                {
                    slot.slider.setValue(laneAt[0]);
                }
                slot.slider.setAccessibleName(
                    tr("sliderName",
                       {{"name",
                         unitText.empty() ? std::string(labelText)
                                          : tr("unitName", {{"name", labelText}, {"unit", unitText}})},
                        {"min", std::format("{}", row.field.min)},
                        {"max", std::format("{}", row.field.max)}}));
            }
            const bool unitOn = lanes > 0 && data && unitWidth > 0.0f;
            place(slot.unit, unitOn,
                  Vec2{split + swatchRoom + sliderRoom + static_cast<float>(lanes) * (laneWidth + gap), inset},
                  Vec2{unitWidth, height});
            if (unitOn)
            {
                setText(slot.unit, unitText);
            }

            place(slot.enabled, check, checkAt, checkSize);
            if (check)
            {
                // the box shows the primary's own value; the label and the name say mixed or inactive-by-parent
                if (slot.enabled.checked() != checkOn)
                {
                    slot.enabled.setChecked(checkOn);
                }
                Label enabledLabel = slot.enabled.label();
                setText(enabledLabel, checkText);
                if (enabledLabel.size().y != height)
                {
                    enabledLabel.setSize(Vec2{enabledLabel.size().x, height});
                }
                const std::string name = row.kind != RowKind::Summary ? std::string(labelText)
                    : row.inactive && !row.mixed                      ? tr("enabled.inactiveName")
                                                                      : tr("enabled");
                slot.enabled.setAccessibleName(row.mixed ? tr("mixedName", {{"name", name}}) : name);
            }
            place(slot.text, text, textAt, textSize);
            const std::string buttonName = quat ? tr("quat.name", {{"name", std::string(labelText)}})
                : editor == Editor::Pick        ? tr("pick.name", {{"name", std::string(labelText)}})
                : editor == Editor::Choice
                ? tr("enum.name", {{"name", std::string(labelText)}, {"value", std::string_view(buttonText)}})
                : std::string{};
            const std::string mixedName = row.mixed && row.kind == RowKind::Value && !numbers
                ? tr("mixedName", {{"name", std::string(labelText)}})
                : std::string{};
            const std::string swatchName = swatch
                ? tr(row.mixed ? "color.mixedName" : "color.name", {{"name", std::string(labelText)}})
                : std::string{};
            const std::string removeName =
                row.kind == RowKind::Header && button ? tr("remove.name", {{"name", row.label}}) : std::string{};
            slot.button.setAccessibleName(
                swatch                                                            ? std::string_view(swatchName)
                    : !removeName.empty()                                         ? std::string_view(removeName)
                    : quat || editor == Editor::Pick || editor == Editor::Choice  ? std::string_view(buttonName)
                    : row.kind == RowKind::Option || row.kind == RowKind::Section ? labelText
                                                                                  : std::string_view{});
            slot.text.setAccessibleName(editor == Editor::Pick && mixedName.empty() ? labelText
                                                                                    : std::string_view(mixedName));
            slot.text.setAccessibleNameKey(
                row.kind == RowKind::Summary && text ? InternedString(std::string(TABLE) + "name") : InternedString{});
            if (text)
            {
                const bool interactable = editor == Editor::Text || editor == Editor::Pick;
                if (slot.text.isInteractable() != interactable)
                {
                    slot.text.setInteractable(interactable);
                }
                // a read-only number reads with its unit
                const std::string withUnit =
                    editor == Editor::None && row.kind == RowKind::Value && !row.field.unit.empty() && !enumRow
                    ? std::format("{} {}", row.shown, row.field.unit)
                    : std::string{};
                const std::string& value = row.kind == RowKind::Summary ? row.label
                    : row.mixed                                         ? mixedText
                    : enumRow                                           ? enumShown
                    : editor == Editor::Pick                            ? row.name
                    : !withUnit.empty()                                 ? withUnit
                                                                        : row.shown;
                slot.text.setStyleClass(failed                             ? classes.fieldError
                                            : row.kind == RowKind::Summary ? classes.title
                                                                           : classes.field);
                if (!force && busy(slot.text))
                {
                    deferred = true;
                }
                else if (slot.text.value() != value)
                {
                    slot.text.setText(value);
                }
            }
            slot.button.setStyleClass(addButton                         ? classes.primary
                                          : row.kind == RowKind::Header ? classes.remove
                                                                        : classes.button);
            slot.keep.setStyleClass(row.kind == RowKind::Header ? classes.caret : classes.button);
            place(slot.button, button, buttonAt, buttonSize);
            if (button)
            {
                setText(slot.button, buttonText);
            }
            // mixed colours show the marker on the frame, the shared colour otherwise; a header's Remove is an x
            const bool remove = row.kind == RowKind::Header && button;
            const std::string_view wantIcon = swatch && row.mixed ? SWATCH_MIXED_ICON
                : remove                                          ? REMOVE_ICON
                                                                  : std::string_view{};
            if (slot.button.icon() != wantIcon)
            {
                slot.button.setIcon(wantIcon);
            }
            const bool filled = swatch && data && !row.mixed;
            place(slot.swatch, filled, buttonAt + Vec2{density.xxs, density.xxs},
                  Vec2{std::max(0.0f, buttonSize.x - 2.0f * density.xxs),
                       std::max(0.0f, buttonSize.y - 2.0f * density.xxs)});
            if (filled)
            {
                const auto* rgba = reinterpret_cast<const f32*>(data + row.offset);
                const Vec4 tint{unitOf(rgba[0]), unitOf(rgba[1]), unitOf(rgba[2]),
                                row.field.kind == FieldKind::Vec4 ? unitOf(rgba[3]) : 1.0f};
                auto& visual = scope.world().getComponent<UiVisualComponent>(slot.swatch.entity());
                if (visual.tint != tint)
                {
                    visual.tint = tint;
                    scope.hierarchy().markVisualDirty(slot.swatch.entity());
                }
            }
            // Revert undoes the client's whole step, also from one field's line: it says so on hover
            const bool tipped = (row.kind == RowKind::Agent || row.kind == RowKind::Change) && row.editable;
            if (tipped != slot.revertTipped)
            {
                slot.revertTipped = tipped;
                if (tipped)
                {
                    revertTip.attach(slot.button);
                }
                else
                {
                    revertTip.detach(slot.button);
                }
            }
            place(slot.keep, keep, keepAt, keepSize);
            if (keep)
            {
                setText(slot.keep, keepText);
            }
            if (slot.keep.icon() != keepIcon)
            {
                slot.keep.setIcon(keepIcon);
            }
            if (remove != slot.removeTipped)
            {
                slot.removeTipped = remove;
                slot.button.setTooltip(remove ? InternedString(std::string(TABLE) + "button.remove")
                                              : InternedString{});
            }
            // the caret names its section and reports whether it is open; a recycled slot's Keep has neither
            slot.keep.setAccessibleName(row.kind == RowKind::Header ? std::string_view(row.label) : std::string_view{});
            slot.keep.setAccessibleExpanded(row.kind != RowKind::Header ? UiExpandState::None
                                                : row.shown == "closed" ? UiExpandState::Collapsed
                                                                        : UiExpandState::Expanded);
            // the agent look: a changed field's row and the agent block carry the wash and a 2px edge
            const bool agentLook = row.changed || row.kind == RowKind::Agent || row.kind == RowKind::Note;
            // a section header's band spans the row: the editor themes' header band (the Forge themes' is flat)
            slot.root.setStyleClass(agentLook                         ? classes.changed
                                        : row.kind == RowKind::Header ? classes.sectionRow
                                                                      : classes.root);
            place(slot.edge, agentLook, Vec2{0.0f, 0.0f}, Vec2{EDGE_WIDTH, height});
            // a busy field is rebound once it lets go
            slot.key = deferred ? std::string{} : std::move(key);
        }

        // A density change: the rows take the new height and spaces; each shown slot re-places in the re-pool of the
        // list, a busy one at its next follow().
        void followDensity()
        {
            const panelui::Density now = panelui::Density::read(scope);
            if (now == density)
            {
                return;
            }
            density = now;
            for (Slot& slot : slots)
            {
                slot.key.clear();
            }
            list.setRowHeight(now.control);
        }

        // Puts the model's values back into the shown slots, busy fields included.
        void restoreAll()
        {
            for (u32 s = 0; s < slots.size(); ++s)
            {
                slots[s].key.clear();
                bind(s, true);
            }
        }

        // -- document ----------------------------------------------------------------

        // The rows slot bound a new element or its fields changed: the list goes in it and honours them. The rows
        // read the list's resolved size at every bind, so a new container (padding, offsets, a narrower column)
        // re-lays them out through the list's own resize.
        void fill(const Widget& slot)
        {
            if (scope.hierarchy().parentOf(list.entity()) != slot.entity())
            {
                list.setParent(slot);
            }
            panelui::putIn(scope, slot, hint);
            const UiSlotComponent& fields = scope.world().getConstComponent<UiSlotComponent>(slot.entity());
            list.setScrollEnabled(fields.scroll);
            thumb = fields.scroll ? THUMB : 0.0f;
            classes = fields.childClass.isValid() ? RowClasses(fields.childClass.toView()) : RowClasses();
            for (Slot& row : slots)
            {
                row.root.setStyleClass(classes.root);
                for (NumberField& number : row.numbers)
                {
                    number.setStyleClass(classes.field);
                }
                row.text.setStyleClass(classes.field);
                row.button.setStyleClass(classes.button);
                row.keep.setStyleClass(classes.button);
                row.edge.setStyleClass(classes.edge);
                row.key.clear(); // the label class and the width follow at the next bind
            }
        }

        // -- edits -------------------------------------------------------------------

        // One request for commands {command, args}; a batch (one undo step) for several.
        void submit(std::vector<Json> commands, const std::string& merge = {}, bool assetApply = false)
        {
            if (commands.empty())
            {
                return;
            }
            setFieldError({}); // an edit settles the refused entry
            Json request;
            if (commands.size() == 1)
            {
                request = std::move(commands.front());
            }
            else
            {
                request = {{"command", "batch"}, {"args", {{"commands", std::move(commands)}}}};
            }
            request["args"]["world"] = worldName;
            if (!merge.empty())
            {
                request["args"]["merge"] = merge;
            }
            pending.push_back(request.dump());
            if (assetApply)
            {
                assetPendingRequest = pending.back();
                assetPendingGeneration = assetChooserGeneration;
            }
        }

        const Row* slotRow(const u32 s) const
        {
            if (rowsRegistryRevision != getComponentRegistryRevision())
                return nullptr;
            return s < slots.size() && slots[s].row < rows.size() ? &rows[slots[s].row] : nullptr;
        }

        // The live selected entities' data of a row's component (those that
        // have it); one destroyed outside the protocol drops out at the next
        // follow().
        template <typename Fn>
        void forSelected(const Row& row, Fn&& fn) const
        {
            const World* target = world();
            if (!target)
            {
                return;
            }
            for (const Entity entity : selection)
            {
                if (const void* data = target->isEntityAlive(entity)
                        ? target->getConstComponentDataById(entity, row.component)
                        : nullptr)
                {
                    fn(entity, static_cast<const std::byte*>(data));
                }
            }
        }

        static Json set(const Entity entity, const std::string& path, Json value)
        {
            return {{"command", "set"}, {"args", {{"id", entity.id()}, {"path", path}, {"value", std::move(value)}}}};
        }

        // Typing in a number lane: text that is not a number turns the field to its error look at once
        void typed(const u32 s, const u32 lane)
        {
            Slot& slot = slots[s];
            const bool invalid =
                !scope.world().getConstComponent<UiNumericComponent>(slot.numbers[lane].entity()).valid;
            slot.numbers[lane].setStyleClass(invalid ? classes.fieldError : classes.field);
        }

        void setFieldError(FieldError next)
        {
            if (next != fieldError)
            {
                fieldError = std::move(next);
                errorDirty = true;
            }
        }

        // A commit of text that is not a number: the field goes back to its value and the note says why
        void rejected(const u32 s, const u32 lane)
        {
            if (const Row* row = slotRow(s))
            {
                setFieldError({row->path, row->lane, lane, uiform::NOT_NUMBER});
            }
            slots[s].key.clear(); // the error look returns after the field's own revert
        }

        // The note under a refused entry appears once no field is being typed in: the rows move
        void followErrors()
        {
            if (!std::ranges::any_of(slots, [this](const Slot& slot) { return busySlot(slot); }))
            {
                errorDirty = false;
                rebuild(false);
            }
        }

        void editNumber(const u32 s, const u32 lane, const double value)
        {
            const Row* row = slotRow(s);
            if (!row || editorOf(*row) != Editor::Numbers)
            {
                return;
            }
            std::string merge;
            const Entity field = slots[s].numbers[lane].entity();
            if (scope.router().pressed() == field)
            {
                // a drag: its steps fold into one undo step
                if (dragField != field)
                {
                    dragField = field;
                    ++dragSerial;
                }
                merge = std::format("inspector.drag.{}", dragSerial);
            }
            std::vector<Json> commands;
            forSelected(
                *row, [&](const Entity entity, const std::byte* data)
                { commands.push_back(set(entity, row->path, numberValue(*row, data, lane, value, eulerMemo))); });
            submit(std::move(commands), merge);
        }

        // The pointer holds a part of the slot's slider (its bar or thumb, or the root)
        [[nodiscard]] bool sliding(const Slot& slot) const
        {
            for (Entity at = scope.router().pressed(); at.isValid(); at = scope.hierarchy().parentOf(at))
            {
                if (at == slot.slider.entity())
                {
                    return true;
                }
                if (at == slot.root.entity())
                {
                    return false;
                }
            }
            return false;
        }

        // A slider change by input: a pointer drag previews (the lane shows the value) until the release, anything else
        // (a key, uiActivate setValue) is one request now
        void slid(const u32 s, const double value)
        {
            const Row* row = slotRow(s);
            if (!row || !slidable(*row))
            {
                return;
            }
            if (sliding(slots[s]))
            {
                if (slide.slot != s)
                {
                    slide = {s, row->path, selection};
                }
                slots[s].numbers[0].setNumber(value);
                return;
            }
            commitSlide(*row, value);
        }

        // One set per selected entity whose value differs, in one request (a batch, one undo step, for several)
        void commitSlide(const Row& row, const double value)
        {
            std::vector<Json> commands;
            forSelected(row,
                        [&](const Entity entity, const std::byte* data)
                        {
                            const double stored = rawLane(row, data, 0);
                            if (row.field.kind == FieldKind::F32 ? static_cast<f32>(stored) != static_cast<f32>(value)
                                                                 : stored != value)
                            {
                                commands.push_back(set(entity, row.path, numberValue(row, data, 0, value, eulerMemo)));
                            }
                        });
            submit(std::move(commands));
        }

        // The pointer let go of a previewing slider: its value is sent, unless the slot was given another row meanwhile
        void finishSlide()
        {
            const Slide done = std::exchange(slide, {});
            Slot& slot = slots[done.slot];
            if (const Row* row = slotRow(done.slot);
                row && row->path == done.path && done.selection == selection && slidable(*row))
            {
                commitSlide(*row, slot.slider.value());
            }
            slot.key.clear(); // the slider and the lane show the model again
        }

        // The header's name field: the entity's name through the commands the hierarchy's rename uses.
        void rename(const std::string_view text)
        {
            const World* target = world();
            if (!target || selection.size() != 1 || !target->isEntityAlive(primary()) ||
                entityName(*target, primary()) == text)
            {
                return;
            }
            const Entity entity = primary();
            submit({target->tryGetConstComponent<EntityNameComponent>(entity)
                        ? set(entity, "EntityNameComponent.name", std::string(text))
                        : Json{{"command", "addComponent"},
                               {"args",
                                {{"id", entity.id()},
                                 {"component", "EntityNameComponent"},
                                 {"value", {{"name", std::string(text)}}}}}}});
        }

        // The header checkbox: every selected entity takes the opposite of the primary's own flag, as it stands now
        // (not as painted), through setEntityEnabled for each one that differs; one batch (one undo step) for several.
        // A parent-disabled entity still takes its own flag.
        void toggleEnabled()
        {
            const World* target = world();
            if (!target || selection.empty())
            {
                return;
            }
            const bool enabled = target->hasComponentById(primary(), getComponentId<DisabledComponent>());
            std::vector<Json> commands;
            for (const Entity entity : selection)
            {
                if (target->isEntityAlive(entity) &&
                    target->hasComponentById(entity, getComponentId<DisabledComponent>()) == enabled)
                {
                    commands.push_back(
                        {{"command", "setEntityEnabled"}, {"args", {{"id", entity.id()}, {"enabled", enabled}}}});
                }
            }
            submit(std::move(commands));
        }

        // One set per selected entity whose value (display()'s text) is not `want` yet: picking what a row already
        // holds writes nothing.
        void writeAll(const Row& row, const std::string& want, const Json& value)
        {
            std::vector<Json> commands;
            forSelected(row,
                        [&](const Entity entity, const std::byte* data)
                        {
                            if (display(row, data) != want)
                            {
                                commands.push_back(set(entity, row.path, value));
                            }
                        });
            submit(std::move(commands));
        }

        // The box of the header (every selected entity takes the opposite of the primary's flag) or of a bool row
        // (every one takes what the box now shows).
        void checkChanged(const u32 s, const bool checked)
        {
            const Row* row = slotRow(s);
            if (row && row->kind == RowKind::Summary)
            {
                toggleEnabled();
            }
            else if (row && editorOf(*row) == Editor::Check)
            {
                writeAll(*row, checked ? "true" : "false", checked);
            }
        }

        void editText(const u32 s, const std::string_view text)
        {
            const Row* row = slotRow(s);
            if (!row || (editorOf(*row) != Editor::Text && editorOf(*row) != Editor::Pick))
            {
                return;
            }
            if (row->kind == RowKind::Summary)
            {
                rename(text);
                return;
            }
            Json value = textValue(*row, text);
            if (value.is_null())
            {
                setFieldError({row->path, row->lane, 0,
                               row->field.kind == FieldKind::EntityList ? std::string(TABLE) + "error.notIdList"
                                                                        : std::string(TABLE) + "error.notId"});
                slots[s].key.clear();
                bind(s, true);
                return;
            }
            if (editorOf(*row) == Editor::Pick)
            {
                setEntity(*row, value.get<u64>()); // a typed id: digits only, as the list's pick
                slots[s].key.clear();
                return;
            }
            std::vector<Json> commands;
            forSelected(*row, [&](const Entity entity, const std::byte*)
                        { commands.push_back(set(entity, row->path, value)); });
            submit(std::move(commands));
        }

        // Removes the component from the selected entities; an engine-owned one (not a game component) asks first,
        // as removing it can break what the engine builds on it. The command is built when the answer comes, from
        // the entities selected when the button was pressed.
        void removeComponent(const ComponentID component)
        {
            const auto remove = [this, component, entities = selection]
            {
                const World* target = world();
                std::vector<Json> commands;
                for (const Entity entity : entities)
                {
                    if (target && target->isEntityAlive(entity) && target->hasComponentById(entity, component))
                    {
                        commands.push_back(
                            {{"command", "removeComponent"},
                             {"args", {{"id", entity.id()}, {"component", getComponentName(component)}}}});
                    }
                }
                submit(std::move(commands));
            };
            if (!dialogs || getComponentMetadata(component).category == ComponentCategory::Game)
            {
                remove();
                return;
            }
            EditorConfirm ask;
            ask.title = tr("remove.title");
            ask.body = tr("remove.body", {{"component", componentTitle(component)}});
            ask.confirmLabel = tr("remove.confirm");
            ask.destructive = true;
            dialogs->confirm(std::move(ask),
                             [remove](const bool confirmed)
                             {
                                 if (confirmed)
                                 {
                                     remove();
                                 }
                             });
        }

        void press(const u32 s)
        {
            const Row* row = slotRow(s);
            if (!row)
            {
                return;
            }
            switch (row->kind)
            {
            case RowKind::Header:
                if (row->editable)
                {
                    removeComponent(row->component);
                }
                break;
            case RowKind::Section:
                pending.push_back(
                    Json{{"command", "inspectorFilter"}, {"args", {{"showRuntime", !showRuntime}}}}.dump());
                break;
            case RowKind::Value:
                if (row->color)
                {
                    openColor(s);
                }
                else if (editorOf(*row) == Editor::Numbers && row->field.kind == FieldKind::Quat)
                {
                    pending.push_back(
                        Json{{"command", "inspectorFilter"}, {"args", {{"rotationMode", rawQuat ? "euler" : "raw"}}}}
                            .dump());
                }
                break;
            case RowKind::Option:
                pending.push_back(
                    Json{{"command", "inspectorFilter"}, {"args", {{"showEngineComponents", !showEngine}}}}.dump());
                break;
            case RowKind::Agent:
            case RowKind::Change:
                // Revert: the client's step, as a new undoable step; the reply's refusal shows under the row
                if (row->editable)
                {
                    agentActions.push_back({true, std::stoull(row->path), primary()});
                }
                break;
            default:
                break;
            }
        }

        // -- colour ------------------------------------------------------------------

        static Vec4 colorAt(const Row& row, const std::byte* data)
        {
            const auto* rgba = reinterpret_cast<const f32*>(data + row.offset);
            return Vec4{rgba[0], rgba[1], rgba[2], row.field.kind == FieldKind::Vec4 ? rgba[3] : 1.0f};
        }

        // The swatch of slot s opens the picker on the primary entity's colour; a read-only field opens it read-only.
        // The picker edits the row it was opened for, whatever the list does meanwhile.
        void openColor(const u32 s)
        {
            const Row* row = slotRow(s);
            const World* target = world();
            const void* data = target && row ? target->getConstComponentDataById(primary(), row->component) : nullptr;
            if (!data)
            {
                return;
            }
            if (!picker)
            {
                picker = std::make_unique<ColorPicker>(scope, InternedString("inspector.color"));
                picker->onChange([this](const UiColorEdit& edit) { editColor(edit); });
                picker->onClose([this] { colorRow = Row{}; });
            }
            colorRow = *row;
            picker->setAlpha(row->field.kind == FieldKind::Vec4);
            picker->setColor(colorAt(*row, static_cast<const std::byte*>(data)));
            picker->setReadOnly(!row->editable);
            picker->open(slots[s].button);
        }

        // One set per selected entity with the picker's colour; the steps of one pointer gesture fold into one undo
        // step like a number drag.
        void editColor(const UiColorEdit& edit)
        {
            if (!colorRow.color || !colorRow.editable)
            {
                return;
            }
            Json value = Json::array({edit.color.x, edit.color.y, edit.color.z});
            if (colorRow.field.kind == FieldKind::Vec4)
            {
                value.push_back(edit.color.w);
            }
            std::vector<Json> commands;
            forSelected(colorRow, [&](const Entity entity, const std::byte*)
                        { commands.push_back(set(entity, colorRow.path, value)); });
            submit(std::move(commands),
                   edit.gesture ? std::format("inspector.color.{}.{}", colorSeed, edit.gesture) : std::string{});
        }

        // Keep: the agent's changes are reviewed (review), which clears the marks. A component header's caret (the
        // same second button) folds or unfolds its section, as the inspectorFilter command does.
        void keepPress(const u32 s)
        {
            const Row* row = slotRow(s);
            if (row && (row->kind == RowKind::Agent || row->kind == RowKind::Change))
            {
                agentActions.push_back({false, 0, primary()});
            }
            else if (row && row->kind == RowKind::Header)
            {
                Json names = Json::array();
                for (const ComponentID id : collapsed)
                {
                    if (id != row->component)
                    {
                        names.push_back(std::string(getComponentName(id)));
                    }
                }
                if (!std::ranges::contains(collapsed, row->component))
                {
                    names.push_back(row->path);
                }
                pending.push_back(Json{{"command", "inspectorFilter"}, {"args", {{"collapse", names}}}}.dump());
            }
        }

        // What the Add list offers, grouped by category then by name: the loaded, schema'd components some selected
        // entity lacks, of the Game and Engine categories (every category with showEngine). The same list the
        // inspector query reports, so a person's list and an LLM's are one.
        std::vector<ComponentID> addable() const
        {
            std::vector<ComponentID> offered;
            const World* target = world();
            if (!target)
            {
                return offered;
            }
            std::vector<ComponentID> ids(getRegisteredComponentCount());
            ids.resize(std::min<sizet>(ids.size(), getComponentIds(ids.data(), static_cast<u32>(ids.size()))));
            for (const ComponentID id : ids)
            {
                const ComponentMetadata& metadata = getComponentMetadata(id);
                const bool basic =
                    metadata.category == ComponentCategory::Game || metadata.category == ComponentCategory::Engine;
                if (!metadata.schema || !metadata.moveAndDestroy || (!basic && !showEngine) ||
                    id == getComponentId<DisabledComponent>() || // the header checkbox sets it; a hand add is refused
                    getComponentIdByName(metadata.name.toView().data()) != id ||
                    std::ranges::all_of(
                        selection, [&](const Entity entity)
                        { return !target->isEntityAlive(entity) || target->hasComponentById(entity, id); }))
                {
                    continue;
                }
                offered.push_back(id);
            }
            std::ranges::sort(offered,
                              [](const ComponentID a, const ComponentID b)
                              {
                                  const ComponentMetadata& left = getComponentMetadata(a);
                                  const ComponentMetadata& right = getComponentMetadata(b);
                                  return std::pair(groupRank(left.category), left.name.toView()) <
                                      std::pair(groupRank(right.category), right.name.toView());
                              });
            return offered;
        }

        void fillAddItems()
        {
            popupItems.clear();
            std::optional<ComponentCategory> group;
            for (const ComponentID id : addable())
            {
                const ComponentMetadata& metadata = getComponentMetadata(id);
                if (group != metadata.category)
                {
                    group = metadata.category;
                    popupItems.push_back(UiChoiceItem{.id = InternedString(std::format("group.{}", groupName(*group))),
                                                      .label = tr(std::format("group.{}", groupName(*group))),
                                                      .header = true});
                }
                popupItems.push_back(UiChoiceItem{.id = metadata.name, .label = componentTitle(id)});
            }
            if (popupItems.empty())
            {
                popupItems.push_back(
                    UiChoiceItem{.id = InternedString("add.none"), .label = tr("add.none"), .enabled = false});
            }
            popup.setItems(popupItems);
        }

        void openList(const u32 s)
        {
            const Row* row = slotRow(s);
            if (row && row->kind == RowKind::Add)
            {
                menuFor = MenuFor::AddComponent;
                fillAddItems();
                popup.setSelected(-1);
                popup.open(slots[s].button);
            }
            else if (row && row->kind == RowKind::Asset)
            {
                openAssetList(s);
            }
            else if (row && row->kind == RowKind::Value && editorOf(*row) == Editor::Choice)
            {
                openChoiceList(s);
            }
            else if (row && row->kind == RowKind::Value && editorOf(*row) == Editor::Pick)
            {
                openEntityList(s);
            }
        }

        void openChoiceList(const u32 s)
        {
            const Row& row = rows[slots[s].row];
            popupItems.clear();
            for (u32 i = 0; i < row.field.enumNames.size(); ++i)
            {
                popupItems.push_back(UiChoiceItem{.id = InternedString(std::format("choice.{}", i)),
                                                  .label = std::string(row.field.enumNames[i].name)});
            }
            menuFor = MenuFor::Choice;
            menuRow = slots[s].row;
            popup.setItems(popupItems);
            popup.setSelected(enumIndex(row.field, row.shown));
            popup.open(slots[s].button);
        }

        // None, then the world's entities by id up to MAX_ENTITY_CHOICES with a closing note when more exist; the
        // list keeps the ids, a pick acts on the entity it named when it was built.
        void openEntityList(const u32 s)
        {
            const World* target = world();
            const Row& row = rows[slots[s].row];
            const auto* data = target
                ? static_cast<const std::byte*>(target->getConstComponentDataById(primary(), row.component))
                : nullptr;
            if (!data)
            {
                return;
            }
            const Json reply = Json::parse(
                control->handle(
                    Json{{"id", 0}, {"query", "find"}, {"args", {{"world", worldName}, {"limit", MAX_ENTITY_CHOICES}}}}
                        .dump()),
                nullptr, false);
            const Json result = reply.is_object() && reply.value("ok", false) ? reply["result"] : Json::object();
            const Entity current = *reinterpret_cast<const Entity*>(data + row.offset);
            entityChoices.assign(1, Entity::undefined().id());
            popupItems.assign(1, UiChoiceItem{.id = InternedString("entity.0"), .label = tr("entity.none")});
            for (const Json& found : result.value("entities", Json::array()))
            {
                entityChoices.push_back(found.value("id", u64{0}));
                popupItems.push_back(
                    UiChoiceItem{.id = InternedString(std::format("entity.{}", entityChoices.size() - 1)),
                                 .label = entityText(*target, Entity(entityChoices.back()))});
            }
            const auto at = std::ranges::find(entityChoices, current.id());
            const u64 total = result.value("total", u64{0});
            if (total > entityChoices.size() - 1)
            {
                popupItems.push_back(
                    UiChoiceItem{.id = InternedString("entity.more"),
                                 .label = tr("pick.more",
                                             {{"shown", static_cast<std::int64_t>(entityChoices.size() - 1)},
                                              {"total", static_cast<std::int64_t>(total)}}),
                                 .enabled = false});
            }
            menuFor = MenuFor::Entity;
            menuRow = slots[s].row;
            popup.setItems(popupItems);
            popup.setSelected(at == entityChoices.end() ? -1 : static_cast<i32>(at - entityChoices.begin()));
            popup.open(slots[s].button);
        }

        // The index of a "<kind>.<n>" list item.
        static std::optional<u32> listIndex(const InternedString item)
        {
            const std::string_view text = item.toView();
            const sizet dot = text.find('.');
            u32 index = 0;
            if (dot == std::string_view::npos ||
                std::from_chars(text.data() + dot + 1, text.data() + text.size(), index).ec != std::errc{})
            {
                return std::nullopt;
            }
            return index;
        }

        void pickChoice(const Row& row, const u32 index)
        {
            if (index < row.field.enumNames.size())
            {
                const std::int64_t value = row.field.enumNames[index].value;
                writeAll(row, std::to_string(value), value);
            }
        }

        // A pick names an entity the list was built with, and a typed id one a person means: one that is not alive
        // (also destroyed since the list was built) is refused, with the note under the row.
        void pickEntity(const Row& row, const u32 index)
        {
            if (index < entityChoices.size())
            {
                setEntity(row, entityChoices[index]);
            }
        }

        void setEntity(const Row& row, const u64 id)
        {
            const World* target = world();
            if (!target)
            {
                return;
            }
            if (id != Entity::undefined().id() && !target->isEntityAlive(Entity(id)))
            {
                setFieldError({row.path, row.lane, 0, std::string(TABLE) + "error.entityGone"});
                return;
            }
            writeAll(row, std::to_string(id), id);
        }

        std::pair<std::string, bool> checkedDrop; // the last dragged asset checked against the manifest, and its result

        // The asset table the picker offers: the `assets` query's entries of
        // the handle's type (every type for an untyped handle).
        std::vector<AssetHandle> assetChoices(const AssetType type) const
        {
            Json args = Json::object();
            if (const auto it = std::ranges::find(ASSET_KINDS, type, &std::pair<AssetType, std::string_view>::first);
                it != std::end(ASSET_KINDS))
            {
                args["type"] = it->second;
            }
            const Json reply = Json::parse(
                engine->control().handle(Json{{"id", 0}, {"query", "assets"}, {"args", args}}.dump()), nullptr, false);
            std::vector<AssetHandle> handles;
            if (!reply.is_object() || !reply.value("ok", false))
            {
                return handles;
            }
            for (const Json& asset : reply["result"]["assets"])
            {
                const std::string& kind = asset["type"].get_ref<const std::string&>();
                const auto it = std::ranges::find(ASSET_KINDS, kind, &std::pair<AssetType, std::string_view>::second);
                if (it == std::end(ASSET_KINDS) || (type == AssetType::UNKNOWN && it->first == AssetType::SKELETON) ||
                    !editorAssetVisible(it->first, asset["name"].get_ref<const std::string&>()))
                    continue;
                handles.push_back(AssetHandle{.name = InternedString(asset["name"].get<std::string>().c_str()),
                                              .assetType = it->first,
                                              .subAssetIndex = 0});
            }
            std::ranges::sort(handles, {}, [](const AssetHandle& handle) { return handle.name.toView(); });
            return handles;
        }

        // The handle a drag payload ("asset.model|material|texture", the first name of its text) binds to the row of
        // slot s: none when the slot shows no editable asset row or the payload's type is not the handle's.
        std::optional<AssetHandle> droppedAsset(const u32 s, const UiDragPayload& payload, const bool fresh = false)
        {
            const World* target = world();
            const Row* row = slotRow(s);
            const void* data = target && row && row->kind == RowKind::Asset && row->editable
                ? target->getConstComponentDataById(primary(), row->component)
                : nullptr;
            const std::string_view type = payload.type.isValid() ? payload.type.toView() : std::string_view{};
            const std::string_view kind = type.starts_with("asset.") ? type.substr(6) : std::string_view{};
            const auto it = std::ranges::find(ASSET_KINDS, kind, &std::pair<AssetType, std::string_view>::second);
            if (!data || it == std::end(ASSET_KINDS) || payload.text.empty())
            {
                return std::nullopt;
            }
            if (row->field.assetType == "SKELETON")
                return std::nullopt;
            const AssetType bound =
                expectedAssetType(row->field, assetAt(*row, static_cast<const std::byte*>(data)).assetType);
            if (!row->field.assetType.empty() && bound == AssetType::UNKNOWN)
                return std::nullopt;
            const bool typed = std::ranges::find(ASSET_KINDS, bound, &std::pair<AssetType, std::string_view>::first) !=
                std::end(ASSET_KINDS);
            if (typed && bound != it->first)
            {
                return std::nullopt;
            }
            const std::string name = payload.text.substr(0, payload.text.find('\n'));
            // an unknown name is no asset: checked against the Pick list's query, once per dragged name
            const std::string key = std::string(kind) + '|' + name;
            if (fresh || key != checkedDrop.first)
            {
                const std::vector<AssetHandle> known = assetChoices(it->first);
                checkedDrop = {
                    key, std::ranges::any_of(known, [&](const AssetHandle& h) { return h.name.toView() == name; })};
            }
            if (!checkedDrop.second)
            {
                return std::nullopt;
            }
            return AssetHandle{.name = InternedString(name.c_str()), .assetType = it->first, .subAssetIndex = 0};
        }

        void openAssetList(const u32 s)
        {
            const World* target = world();
            const Row* row = slotRow(s);
            const void* data = target && row ? target->getConstComponentDataById(primary(), row->component) : nullptr;
            if (!data)
            {
                return;
            }
            const AssetHandle current = assetAt(*row, static_cast<const std::byte*>(data));
            const AssetType expected = expectedAssetType(row->field, current.assetType);
            choices = !row->field.assetType.empty() && expected == AssetType::UNKNOWN ? std::vector<AssetHandle>{}
                                                                                      : assetChoices(expected);
            if (expected == AssetType::SKELETON)
                std::erase_if(choices,
                              [&](const AssetHandle& candidate)
                              {
                                  for (Entity entity : selection)
                                  {
                                      const Json response =
                                          Json::parse(control->handle(Json{{"id", 0},
                                                                           {"query", "skeletonCompatibility"},
                                                                           {"args",
                                                                            {{"id", entity.id()},
                                                                             {"world", worldName},
                                                                             {"skeleton", candidate.name.toView()}}}}
                                                                          .dump()),
                                                      nullptr, false);
                                      if (!response.is_object() || !response.value("ok", false) ||
                                          !response["result"].value("compatible", false))
                                          return true;
                                  }
                                  return false;
                              });
            assetItems.clear();
            assetItems.reserve(choices.size());
            i32 selected = -1;
            for (u32 i = 0; i < choices.size(); ++i)
            {
                assetItems.push_back(UiChoiceItem{
                    .id = InternedString(std::format("asset.{}", i)),
                    .label = std::format("{} ({})", choices[i].name.toView(), to_string(choices[i].assetType)),
                    .icon = std::string(assetIcon(choices[i].assetType))});
                if (current.isValid() && choices[i].name == current.name && choices[i].assetType == current.assetType)
                    selected = static_cast<i32>(i);
            }
            assetRow = *row;
            assetRegistryRevision = rowsRegistryRevision;
            assetSelection = selection;
            assetWorld = worldName;
            assetOriginals.clear();
            for (Entity entity : assetSelection)
            {
                const auto* value =
                    static_cast<const std::byte*>(target->getConstComponentDataById(entity, row->component));
                if (!value)
                    return;
                assetOriginals.push_back(assetAt(*row, value));
            }
            ++assetChooserGeneration;
            assetPendingRequest.clear();
            assetCombo.setItems(assetItems);
            assetCombo.setInteractable(true);
            assetParts.setInteractable(true);
            assetCombo.setSelected(selected);
            assetCandidate = {};
            if (selected >= 0)
            {
                stageAsset(static_cast<u32>(selected), current.subAssetIndex);
            }
            else
            {
                stageAsset(NO_ROW);
                if (current.isValid())
                    assetNote.setText(tr("chooser.currentUnavailable"));
            }
            assetModal.open(slots[s].button);
        }

        void stageAsset(u32 index, u32 previousPart = NO_ROW)
        {
            if (assetRegistryRevision != getComponentRegistryRevision())
            {
                assetModal.close();
                return;
            }
            assetCandidate = index < choices.size() ? choices[index] : AssetHandle{};
            partItems.clear();
            i32 selected = -1;
            if (assetCandidate.isValid() && assetCandidate.assetType == AssetType::MESH)
            {
                const auto model = loadBakedModelDescriptor(assetCandidate.name, engine->assets().manifest());
                if (model)
                {
                    for (u32 i = 0; i < (*model)->submeshes.size(); ++i)
                    {
                        const auto name = (*model)->submeshes[i].name.toView();
                        partItems.push_back(
                            {.id = InternedString(std::format("part.{}", i)), .label = std::format("{}: {}", i, name)});
                    }
                    if (previousPart < partItems.size())
                        selected = static_cast<i32>(previousPart);
                    else if (partItems.size() == 1)
                        selected = 0;
                }
            }
            assetParts.setItems(partItems);
            assetParts.setSelected(selected);
            assetParts.setVisible(assetCandidate.assetType == AssetType::MESH);
            assetPing.setInteractable(assetCandidate.isValid() &&
                                      (assetCandidate.assetType == AssetType::MESH ||
                                       assetCandidate.assetType == AssetType::MATERIAL ||
                                       assetCandidate.assetType == AssetType::SKELETON));
            assetApply.setInteractable(assetCandidate.isValid() &&
                                       (assetCandidate.assetType != AssetType::MESH || selected >= 0));
            assetNote.setText(tr(assetCandidate.isValid() ? "chooser.preview" : "chooser.select"));
            if (assetCandidate.assetType == AssetType::SKELETON && assetCandidate.isValid())
                if (const auto skeleton = engine->assets().manifest().getSkeletonEntry(assetCandidate.name))
                    assetNote.setText(tr("chooser.skeleton",
                                         {{"model", skeleton->model.toView()},
                                          {"count", static_cast<std::int64_t>(skeleton->jointCount)}}));
            assetPreview.setVisible(false);
            const bool canPreview = assetCandidate.isValid() &&
                (assetCandidate.assetType == AssetType::MESH || assetCandidate.assetType == AssetType::MATERIAL);
            assetGenerate.setVisible(canPreview);
            assetPreviewMissingNoted = false;
            assetPreviewRevision = engine->services().contains<EditorPreviewJobs>()
                ? engine->services().get<EditorPreviewJobs>().revision()
                : 0;
            editorUi(*engine).uiView().clearImage(ASSET_PREVIEW_SLOT);
            assetPreviewPending = false;
            if (assetLookup)
                assetLookup->cancel();
            if (assetCandidate.isValid() &&
                (assetCandidate.assetType == AssetType::MESH || assetCandidate.assetType == AssetType::MATERIAL))
            {
                if (!assetLookup)
                    assetLookup = std::make_unique<EditorRenderedPreviews>(*engine);
                const auto directory = (writableDirectory(WritableFolder::Cache, {}, false) / "previews");
                if (assetCandidate.assetType == AssetType::MESH)
                    assetLookup->requestCachedModel(assetCandidate.name, directory);
                else if (assetCandidate.assetType == AssetType::MATERIAL)
                    assetLookup->requestCachedMaterial(assetCandidate.name, directory);
            }
        }

        void updateAssetPreview()
        {
            if (assetModal.isOpen() && assetRegistryRevision != getComponentRegistryRevision())
            {
                assetModal.close();
                return;
            }
            if (!assetModal.isOpen() || !assetLookup || !assetCandidate.isValid() ||
                (assetCandidate.assetType != AssetType::MESH && assetCandidate.assetType != AssetType::MATERIAL))
                return;
            assetLookup->update();
            auto& view = editorUi(*engine).uiView();
            const auto status = assetLookup->status();
            if (status == EditorRenderedPreviews::Status::Ready && !assetPreviewPending && !assetPreview.isVisible())
            {
                view.registerImageFile(ASSET_PREVIEW_SLOT, utf8Path(assetLookup->path()));
                assetPreviewPending = true;
            }
            else if ((status == EditorRenderedPreviews::Status::Missing ||
                      status == EditorRenderedPreviews::Status::Failed) &&
                     !assetPreviewMissingNoted)
            {
                assetNote.setText(tr("chooser.previewMissing"));
                assetPreviewMissingNoted = true;
            }
            if ((status == EditorRenderedPreviews::Status::Missing ||
                 status == EditorRenderedPreviews::Status::Failed || status == EditorRenderedPreviews::Status::Idle) &&
                engine->services().contains<EditorPreviewJobs>() &&
                assetPreviewRevision != engine->services().get<EditorPreviewJobs>().revision())
            {
                assetPreviewRevision = engine->services().get<EditorPreviewJobs>().revision();
                const auto directory = (writableDirectory(WritableFolder::Cache, {}, false) / "previews");
                if (assetCandidate.assetType == AssetType::MESH)
                    assetLookup->requestCachedModel(assetCandidate.name, directory);
                else
                    assetLookup->requestCachedMaterial(assetCandidate.name, directory);
            }
            if (assetPreviewPending)
                if (const auto info = view.imageInfo(ASSET_PREVIEW_SLOT); info && info->state != UiImageState::Pending)
                {
                    assetPreviewPending = false;
                    if (info->state == UiImageState::Ready && info->width && info->height)
                        assetPreview.setVisible(true);
                }
        }

        // Bindings are immutable: per entity, remove the component and add it
        // back with the picked handle and its other fields as they were.
        void pick(const Row& row, const AssetHandle& handle, bool staged = false)
        {
            if (!row.field.assetType.empty() && expectedAssetType(row.field, AssetType::UNKNOWN) != handle.assetType)
                return;
            if (row.field.assetType == "SKELETON" && row.component == getComponentId<SkinComponent>())
            {
                std::vector<Json> commands;
                for (Entity entity : selection)
                    commands.push_back(
                        {{"command", "setSkinSkeleton"},
                         {"args", {{"id", entity.id()}, {"skeleton", std::string(handle.name.toView())}}}});
                submit(std::move(commands), {}, staged);
                return;
            }
            const ComponentMetadata& metadata = getComponentMetadata(row.component);
            if (!metadata.schema || !metadata.copyPolicy)
            {
                SWARNING_LOG("inspector: {} cannot be copied to rebind its asset", metadata.name.toView());
                return;
            }
            const std::string name(metadata.name.toView());
            std::vector<Json> commands;
            forSelected(row,
                        [&](const Entity entity, const std::byte* data)
                        {
                            void* copy = ::operator new(metadata.size, std::align_val_t(metadata.alignment));
                            metadata.copyPolicy(copy, data);
                            *reinterpret_cast<AssetHandle*>(static_cast<std::byte*>(copy) + row.offset) = handle;
                            JsonFragmentWriter writer;
                            writeValue(*metadata.schema, copy, writer, FieldSet::Persisted);
                            std::string value;
                            writer.dump(value);
                            if (metadata.destructionPolicy)
                            {
                                metadata.destructionPolicy(copy);
                            }
                            ::operator delete(copy, std::align_val_t(metadata.alignment));
                            commands.push_back(
                                {{"command", "removeComponent"}, {"args", {{"id", entity.id()}, {"component", name}}}});
                            commands.push_back(
                                {{"command", "addComponent"},
                                 {"args", {{"id", entity.id()}, {"component", name}, {"value", Json::parse(value)}}}});
                        });
            submit(std::move(commands), {}, staged);
        }

        void activate(const InternedString item)
        {
            if (menuFor == MenuFor::AddComponent)
            {
                const World* target = world();
                const ComponentID id = getComponentIdByName(std::string(item.toView()).c_str());
                std::vector<Json> commands;
                for (const Entity entity : selection)
                {
                    if (target && id != INVALID_COMPONENT_ID && target->isEntityAlive(entity) &&
                        !target->hasComponentById(entity, id))
                    {
                        commands.push_back(
                            {{"command", "addComponent"},
                             {"args", {{"id", entity.id()}, {"component", std::string(item.toView())}}}});
                    }
                }
                submit(std::move(commands));
                return;
            }
            if (menuFor == MenuFor::Choice || menuFor == MenuFor::Entity)
            {
                const std::optional<u32> index = listIndex(item);
                if (index && menuRow < rows.size() && rows[menuRow].kind == RowKind::Value)
                {
                    menuFor == MenuFor::Choice ? pickChoice(rows[menuRow], *index) : pickEntity(rows[menuRow], *index);
                }
                return;
            }
            const std::optional<u32> index = listIndex(item);
            if (index && *index < choices.size() && menuRow < rows.size() && rows[menuRow].kind == RowKind::Asset)
            {
                pick(rows[menuRow], choices[*index]);
            }
        }

        // -- control -------------------------------------------------------------------

        void writeCollapsed(IFormatWriter& out) const
        {
            out.beginArray("collapse", static_cast<u32>(collapsed.size()));
            for (const ComponentID id : collapsed)
            {
                out.writeString("", getComponentName(id));
            }
            out.endArray();
        }

        // A select is about to replace the selection with another, the old one still in place: what the panel held
        // for the old entities goes
        void replacing()
        {
            agentNote.clear(); // a refusal belonged to the old entity
            list.setScrollOffset(Vec2{0.0f, 0.0f}); // another entity opens at its top
            // what was being typed belonged to the old entity: the field gives up its focus
            if (std::ranges::any_of(slots, [this](const Slot& slot) { return busySlot(slot); }))
            {
                scope.router().clearFocus();
            }
            if (picker)
            {
                picker->close(); // it edits the old entities
            }
            fieldError = {}; // the old entities' refused entry, the one the focus loss may have made too
            errorDirty = false; // the rows are rebuilt after the select
        }

        void registerCommands()
        {
            control->registerViewCommand(
                "inspectorFilter", OWNER,
                "sets what the Inspector's Add Component list offers: game and engine components, or with "
                "showEngineComponents every category (UI and runtime-derived too); showRuntime opens or closes the "
                "read-only Runtime section of the runtime-derived components; collapse names the components whose "
                "header hides its fields (all others unfold); the answer is the settings",
                {optionalArg("showEngineComponents", ArgKind::Boolean, "offer every category", "unchanged"),
                 optionalArg("showRuntime", ArgKind::Boolean, "list the runtime-derived components", "unchanged"),
                 optionalArg("collapse", ArgKind::Json, "an array of component names to fold, [] unfolds all",
                             "unchanged"),
                 optionalArg("rotationMode", ArgKind::String,
                             "euler (angles in degrees) or raw (x y z w) for quaternion fields", "unchanged",
                             ROTATION_MODES)},
                [this](const ControlArgs& args, IFormatWriter& out)
                {
                    std::optional<std::vector<ComponentID>> folded;
                    if (const std::optional<std::string> text = args.json("collapse"))
                    {
                        const Json names = Json::parse(*text, nullptr, false);
                        if (!names.is_array())
                        {
                            return std::string("collapse must be an array of component names");
                        }
                        folded.emplace();
                        for (const Json& name : names)
                        {
                            const ComponentID id = name.is_string()
                                ? getComponentIdByName(name.get<std::string>().c_str())
                                : INVALID_COMPONENT_ID;
                            if (id == INVALID_COMPONENT_ID)
                            {
                                return "unknown component " + name.dump();
                            }
                            if (!std::ranges::contains(*folded, id))
                            {
                                folded->push_back(id);
                            }
                        }
                    }
                    if (folded && *folded != collapsed)
                    {
                        collapsed = std::move(*folded);
                        rebuild(false);
                    }
                    if (const std::optional<std::string> mode = args.string("rotationMode"))
                    {
                        if ((*mode == "raw") != rawQuat)
                        {
                            rawQuat = *mode == "raw";
                            setFieldError({});
                            errorDirty = false;
                            rebuild(false);
                        }
                    }
                    if (const std::optional<bool> show = args.boolean("showEngineComponents"))
                    {
                        setShowEngine(*show);
                    }
                    if (const std::optional<bool> show = args.boolean("showRuntime"); show && *show != showRuntime)
                    {
                        showRuntime = *show;
                        rebuild(false);
                    }
                    out.writeBool("showEngineComponents", showEngine);
                    out.writeBool("showRuntime", showRuntime);
                    out.writeString("rotationMode", rawQuat ? "raw" : "euler");
                    writeCollapsed(out);
                    return std::string{};
                });
            control->registerQuery(
                "inspector", OWNER,
                "the inspector's selection, rows, stats and the Add Component list (name and "
                "category, in the order the list shows them); a subtitle row for one entity with cells "
                "{name systems|edit, value, tip, warn}; readout, telemetry, "
                "systemBudgetMs and systems (the systems matching the one selected entity: "
                "system, phase, enabled, avgMs, overBudget, and whether the density lists it); "
                "a row's color flag marks a COLOR field, picker the open colour picker (open, "
                "path, color: four numbers while open, alpha 1 for a Vec3); the summary row's "
                "enabled (the primary entity's own flag) and active (also no disabled parent), "
                "mixed when the selected entities' flags differ; DisabledComponent is not offered "
                "in addable (setEntityEnabled sets it); a value row's editor (none, check, "
                "choice, pick, numbers, text), the schema's unit, min, max and step when declared, and slider (an "
                "editable float with a finite range: a slider before its number lane; a drag previews and sends one "
                "set at the release); an entity row's value is the id and text its well; for an "
                "enum its choice (the stored value's name, empty when the names do not list it) "
                "and choices (value and name); collapse lists the components whose header is folded "
                "(inspectorFilter collapse), and a header row's collapsed and tag (its category's "
                "module tag) say whether its fields are hidden and what its tag reads",
                {},
                [this](const ControlArgs&, IFormatWriter& out)
                {
                    if (rowsRegistryRevision != getComponentRegistryRevision())
                        return std::string("inspector schema changed; retry after the editor updates");
                    writeSelection(out);
                    out.writeBool("showEngineComponents", showEngine);
                    out.writeBool("showRuntime", showRuntime);
                    out.writeString("rotationMode", rawQuat ? "raw" : "euler");
                    writeCollapsed(out);
                    const std::vector<ComponentID> offered = addable();
                    out.beginArray("addable", static_cast<u32>(offered.size()));
                    for (const ComponentID id : offered)
                    {
                        const ComponentMetadata& metadata = getComponentMetadata(id);
                        out.beginObject("");
                        out.writeString("name", metadata.name.toView());
                        out.writeString("category", componentCategoryName(metadata.category));
                        out.endObject();
                    }
                    out.endArray();
                    // the runtime-derived components the selection shares, whether or not the
                    // Runtime section lists their rows
                    std::vector<std::string_view> derived;
                    for (const ComponentID id : sharedShown)
                    {
                        if (getComponentMetadata(id).category == ComponentCategory::RuntimeDerived)
                        {
                            derived.push_back(getComponentMetadata(id).name.toView());
                        }
                    }
                    out.beginArray("runtime", static_cast<u32>(derived.size()));
                    for (const std::string_view name : derived)
                    {
                        out.writeString("", name);
                    }
                    out.endArray();
                    out.beginObject("stats");
                    writeValue(InspectorStats::spiteSchema(), &stats, out, FieldSet::All);
                    out.endObject();
                    // the readout strip's data, whatever the density lists
                    out.writeBool("readout", stripOn());
                    out.writeString("telemetry", telemetryFull ? TELEMETRY_FULL : TELEMETRY_QUIET);
                    out.writeF32("systemBudgetMs", budgetMs);
                    const std::vector<const SystemHit*> listed = listedHits();
                    // the scheduler's reading now, not the strip's last one (it may be 30 frames
                    // old, or not read at all while the panel is hidden)
                    std::vector<std::pair<float, bool>> now;
                    for (const SystemHit& hit : hits)
                    {
                        now.emplace_back(hit.avgMs, hit.enabled);
                    }
                    if (const World* target = world(); target && stripOn())
                    {
                        forTimings(*target, [&now](const sizet i, const float avgMs, const bool enabled)
                                   { now[i] = {avgMs, enabled}; });
                    }
                    out.beginArray("systems", static_cast<u32>(hits.size()));
                    for (sizet i = 0; i < hits.size(); ++i)
                    {
                        const SystemHit& hit = hits[i];
                        out.beginObject("");
                        out.writeString("system", hit.qualified);
                        out.writeString("phase", hit.fixed ? "fixed" : "update");
                        out.writeBool("enabled", now[i].second);
                        out.writeF32("avgMs", now[i].first);
                        out.writeBool("overBudget", budgetMs > 0.0f && now[i].first > budgetMs);
                        out.writeBool("listed", std::ranges::contains(listed, &hit));
                        out.endObject();
                    }
                    out.endArray();
                    // the colour picker: the row it edits and its colour while open
                    out.beginObject("picker");
                    const bool pickerOpen = picker && picker->isOpen();
                    out.writeBool("open", pickerOpen);
                    out.writeString("path", pickerOpen ? colorRow.path : std::string{});
                    out.beginArray("color", pickerOpen ? 4u : 0u);
                    if (pickerOpen)
                    {
                        const Vec4 color = picker->color();
                        for (const f32 channel : {color.x, color.y, color.z, color.w})
                        {
                            out.writeF32("", channel);
                        }
                    }
                    out.endArray();
                    out.endObject();
                    out.beginArray("rows", static_cast<u32>(rows.size()));
                    for (const Row& row : rows)
                    {
                        out.beginObject("");
                        out.writeString("kind", ROW_KIND_NAMES[static_cast<u8>(row.kind)]);
                        out.writeString("label", row.label);
                        out.writeString("path", row.path);
                        out.writeString("value", row.shown);
                        if (editorOf(row) == Editor::Pick)
                        {
                            out.writeString("text", row.name); // the well: localized, not the id
                        }
                        out.writeBool("editable", row.editable);
                        out.writeBool("changed", row.changed);
                        out.writeBool("mixed", row.mixed != 0);
                        if (row.toggle)
                        {
                            out.writeBool("enabled", row.enabled); // the primary's own flag
                            out.writeBool("active", row.enabled && !row.inactive);
                        }
                        out.writeBool("color", row.color);
                        if (row.kind == RowKind::Header)
                        {
                            out.writeBool("collapsed", row.shown == "closed");
                            out.writeString("tag", row.tag);
                        }
                        if (row.kind == RowKind::Subtitle)
                        {
                            constexpr std::array<std::string_view, CELLS> NAMES{"systems", "edit"};
                            out.beginArray("cells", CELLS);
                            for (u32 cell = 0; cell < CELLS; ++cell)
                            {
                                out.beginObject("");
                                out.writeString("name", NAMES[cell]);
                                out.writeString("value", row.cells[cell].value);
                                out.writeString("tip", row.cells[cell].tip);
                                out.writeBool("warn", row.cells[cell].warn);
                                out.endObject();
                            }
                            out.endArray();
                        }
                        if (row.kind == RowKind::Value)
                        {
                            constexpr std::array<std::string_view, 6> EDITORS{"none", "check",   "choice",
                                                                              "pick", "numbers", "text"};
                            out.writeString("editor", EDITORS[static_cast<u8>(editorOf(row))]);
                            // the schema's metadata, when declared; slider: the row draws one
                            if (!row.field.unit.empty())
                            {
                                out.writeString("unit", row.field.unit);
                            }
                            if (std::isfinite(row.field.min))
                            {
                                out.writeF64("min", row.field.min);
                            }
                            if (std::isfinite(row.field.max))
                            {
                                out.writeF64("max", row.field.max);
                            }
                            if (row.field.step > 0.0)
                            {
                                out.writeF64("step", row.field.step);
                            }
                            out.writeBool("slider", slidable(row));
                        }
                        if (row.kind == RowKind::Value && !row.field.enumNames.empty())
                        {
                            const i32 named = enumIndex(row.field, row.shown);
                            out.writeString("choice",
                                            named >= 0 ? row.field.enumNames[named].name : std::string_view{});
                            out.beginArray("choices", static_cast<u32>(row.field.enumNames.size()));
                            for (const EnumName& choice : row.field.enumNames)
                            {
                                out.beginObject("");
                                if (choice.value < 0) // only an i32 enum has negative values
                                {
                                    out.writeI32("value", static_cast<i32>(choice.value));
                                }
                                else
                                {
                                    out.writeU64("value", static_cast<u64>(choice.value));
                                }
                                out.writeString("name", choice.name);
                                out.endObject();
                            }
                            out.endArray();
                        }
                        out.writeBool("error", row.error);
                        out.writeU32("entities", row.count); // an agent row: its step's
                        out.writeU32("depth", row.depth);
                        out.endObject();
                    }
                    out.endArray();
                    return std::string{};
                });
        }

        void setShowEngine(const bool show)
        {
            if (show == showEngine)
            {
                return;
            }
            showEngine = show;
            for (u32 r = 0; r < rows.size(); ++r)
            {
                if (rows[r].kind == RowKind::Option)
                {
                    update(r, show ? "on" : "off");
                }
            }
            if (popup.isOpen() && menuFor == MenuFor::AddComponent)
            {
                fillAddItems();
            }
        }

        void writeSelection(IFormatWriter& out) const
        {
            out.writeString("world", worldName);
            out.beginArray("selection", static_cast<u32>(selection.size()));
            for (const Entity entity : selection)
            {
                out.writeU64("", entity.id());
            }
            out.endArray();
        }

        // -- frame ---------------------------------------------------------------------

        // Revert and Keep of the agent block: a refusal's reason (the reply's error names the later step that
        // touched the field) is kept and shown as a note row
        void runAgentActions()
        {
            std::vector<AgentAction> actions;
            actions.swap(agentActions);
            for (const AgentAction& action : actions)
            {
                const Json request = action.revert
                    ? Json{{"command", "revertStep"}, {"args", {{"world", worldName}, {"index", action.step}}}}
                    : Json{{"command", "review"}, {"args", {{"world", worldName}, {"id", action.entity.id()}}}};
                const Json reply = Json::parse(control->handle(request.dump()), nullptr, false);
                if (reply.is_object() && reply.value("ok", false))
                {
                    agentNote.clear();
                    continue;
                }
                agentNote = reply.is_object() ? reply.value("error", std::string{}) : std::string{};
                agentNoteStep = action.step;
                SWARNING_LOG("inspector: {} -> {}", request.dump(), reply.dump());
            }
            rebuild(false); // the note row, and the change the action settled
        }

        void dispatch()
        {
            if (!agentActions.empty())
            {
                runAgentActions();
            }
            if (pending.empty())
            {
                return;
            }
            std::vector<std::string> requests;
            requests.swap(pending);
            bool failed = false;
            for (const std::string& request : requests)
            {
                const std::string reply = control->handle(request);
                const Json parsed = Json::parse(reply, nullptr, false);
                const bool ok = parsed.is_object() && parsed.value("ok", false);
                if (!assetPendingRequest.empty() && request == assetPendingRequest &&
                    assetPendingGeneration == assetChooserGeneration)
                {
                    assetPendingRequest.clear();
                    if (ok)
                        assetModal.close();
                    else
                    {
                        assetNote.setText(tr(
                            "chooser.refused",
                            {{"reason", parsed.is_object() ? parsed.value("error", std::string{}) : std::string{}}}));
                        assetApply.setInteractable(true);
                        assetCombo.setInteractable(true);
                        assetParts.setInteractable(true);
                    }
                }
                if (!ok)
                {
                    SWARNING_LOG("inspector: {} -> {}", request, reply);
                    failed = true;
                }
            }
            if (failed)
            {
                restoreAll(); // fields showing a refused value go back to the model's
            }
        }

        void follow()
        {
            if (rowsRegistryRevision != getComponentRegistryRevision())
            {
                if (assetModal.isOpen())
                    assetModal.close();
                rebuild(false);
                return;
            }
            if (dragField.isValid() && scope.router().pressed() != dragField)
            {
                dragField = Entity::undefined();
            }
            if (slide.slot != NO_ROW && !sliding(slots[slide.slot]))
            {
                finishSlide();
            }
            // the section label and ids follow the locale; the lanes follow the text scale
            bool restyle = false;
            if (localization && localization->revision() != shownRevision)
            {
                shownRevision = localization->revision();
                rebuild(false);
                restyle = true;
            }
            if (styles->textScale() != shownTextScale)
            {
                shownTextScale = styles->textScale();
                restyle = true;
            }
            if (restyle)
            {
                for (Slot& slot : slots)
                {
                    slot.key.clear();
                }
            }
            if (errorDirty)
            {
                followErrors();
            }
            for (u32 s = 0; s < slots.size(); ++s)
            {
                if (slots[s].row != NO_ROW && slots[s].key.empty() && !busySlot(slots[s]))
                {
                    bind(s);
                }
            }
            // the shell pruned an entity that died since the last frame: rebuilt below as a dead one was
            const bool pruned = model->selectionRevision() != seenSelection;
            if (selection.empty() && !pruned)
            {
                return;
            }
            if (agent)
            {
                // the layer reads one entity's change once per command generation; a new answer, or the layer
                // switched on or off (a view command: no state generation moves), rebuilds the agent block
                if (selection.size() == 1)
                {
                    (void)agent->changeOf(worldName, primary());
                }
                if (agent->changeRevision() != seenChange || agent->enabled() != seenAgentOn)
                {
                    rebuild(false);
                    return;
                }
            }
            const World* target = world();
            if (stripOn() && target)
            {
                // one integer compare per frame: the schedule; the last edit and the costs follow their own reads
                if (target->systemRevision() != hitRevision)
                {
                    rebuild(false);
                    return;
                }
                if (agent)
                {
                    (void)agent->lastEditOf(worldName, primary());
                    if (agent->lastEditRevision() != seenLastEdit)
                    {
                        updateSubtitle();
                    }
                }
                // a hidden panel reads nothing (the counter keeps running: the first frame shown reads at once)
                if (++sinceCosts >= systemcost::REFRESH_FRAMES && panelui::shown(scope, host))
                {
                    readCosts(*target);
                }
            }
            bool changed = pruned || !target ||
                !std::ranges::all_of(selection,
                                     [target](const Entity entity) { return target->isEntityAlive(entity); });
            if (!changed)
            {
                ComponentID ids[MAX_COMPONENTS];
                const u32 count = std::min(target->getEntityAspectIds(primary(), ids), MAX_COMPONENTS);
                changed = !std::ranges::equal(std::span(ids, count), primaryAspect);
            }
            const u64 generation = control->stateGeneration();
            if (changed || generation != seenGeneration)
            {
                // a command may have changed any selected entity
                stats.resyncs += generation != seenGeneration;
                seenGeneration = generation;
                rebuild(true);
                return;
            }
            refresh(false);
        }

        bool busySlot(const Slot& slot) const
        {
            return busy(slot.text) || (slide.slot != NO_ROW && &slots[slide.slot] == &slot) ||
                std::ranges::any_of(slot.numbers, [this](const NumberField& f) { return busy(f); });
        }
    };

    EditorInspector::EditorInspector(Engine& engine, EditorModel& model) :
        m_state(std::make_unique<State>(engine, model))
    {
        State& state = *m_state;
        if (const auto layer = editorUi(engine).uiView().stylesheet().loadLayer(InternedString(STYLE_LAYER),
                                                                                resolveContentPath(STYLE_FILE));
            !layer)
        {
            SWARNING_LOG("inspector: {}", layer.error().message);
        }
        // a row's label may be ellipsized: hovering it shows the full path (component.field), like a field's doc
        state.labelTip = state.scope.createTooltip("inspector.tip");
        state.labelTip.label().setStyleClass(InternedString("ui.tooltip.text"));
        state.labelTip.onShow(
            [&state](const Entity target)
            {
                for (const State::Slot& slot : state.slots)
                {
                    if (slot.label.entity() == target && slot.row < state.rows.size())
                    {
                        const Row& row = state.rows[slot.row];
                        state.labelTip.label().setText(row.kind == RowKind::Value || row.kind == RowKind::Asset
                                                           ? row.path
                                                           : row.kind == RowKind::Summary ? row.shown
                                                                                          : row.label);
                    }
                }
            });
        state.cellTip = state.scope.createTooltip("inspector.cellTip");
        state.cellTip.label().setStyleClass(InternedString("ui.tooltip.text"));
        state.cellTip.onShow(
            [&state](const Entity target)
            {
                for (const State::Slot& slot : state.slots)
                {
                    for (u32 cell = 0; cell < CELLS; ++cell)
                    {
                        if (slot.cells[cell].entity() == target && slot.row < state.rows.size())
                        {
                            const Cell& shown = state.rows[slot.row].cells[cell];
                            state.cellTip.label().setText(shown.tip.empty() ? shown.value : shown.tip);
                        }
                    }
                }
            });
        state.revertTip = state.scope.createTooltip("inspector.revertTip");
        state.revertTip.label().setStyleClass(InternedString("ui.tooltip.text"));
        state.revertTip.onShow(
            [&state](const Entity target)
            {
                for (const State::Slot& slot : state.slots)
                {
                    if (slot.button.entity() == target && slot.row < state.rows.size())
                    {
                        state.revertTip.label().setText(state.tr(
                            "agent.revertScope", {{"count", static_cast<std::int64_t>(state.rows[slot.row].count)}}));
                    }
                }
            });
        UiVirtualListDesc desc;
        desc.rect = topLeft();
        desc.rect.layoutItem = panelui::fillItem(); // fills the slot when the document lays the slot out
        desc.rowHeight = state.density.control;
        desc.createRow = [&state](const u32 slot) -> Widget { return state.createSlot(slot).root; };
        desc.bindRow = [&state](const u32 slot, const u32 row)
        {
            state.slots[slot].row = row;
            state.bind(slot);
        };
        state.host = state.scope.createPanel("inspector.host", panelui::columnDesc());
        state.list = state.scope.createVirtualList("inspector", std::move(desc));
        state.hint = panelui::emptyNote(state.scope, "inspector.empty");
        state.densityObserver = state.scope.observeLayoutTokens([&state] { state.followDensity(); });
        state.popup = state.scope.createPopupList("inspector.popup", {.maxRows = POPUP_ROWS, .searchable = true});
        state.popup.onChoose([&state](const u32 index) { state.activate(state.popupItems[index].id); });
        state.assetModal = state.scope.createModal("inspector.assetChooser", {.padding = 0.0f});
        Panel assetContent = state.assetModal.content();
        assetContent.setLayout(panelui::stack(UiLayoutDirection::Column, 0.0f, {}, UiLayoutAlign::Stretch));
        panelui::bindGap(assetContent, panelui::SPACE_MD);
        panelui::bindPadding(assetContent, panelui::SPACE_LG, panelui::SPACE_LG, panelui::SPACE_LG, panelui::SPACE_LG);
        state.assetNote = state.scope.createLabel("inspector.assetChooser.note", state.tr("chooser.select"),
                                                  panelui::barDesc(Vec2{360.0f, 24.0f}));
        state.assetNote.setParent(assetContent);
        state.assetCombo =
            state.scope.createCombo("inspector.assetChooser.assets",
                                    {.rect = panelui::barDesc(Vec2{360.0f, 28.0f}), .maxRows = 10, .searchable = true});
        state.assetCombo.setAccessibleNameKey(InternedString("editor-inspector.chooser.asset"));
        state.assetCombo.setParent(assetContent);
        state.assetCombo.onChange([&state](u32 index) { state.stageAsset(index); });
        state.assetGenerate = state.scope.createButton("inspector.assetChooser.generate", state.tr("chooser.generate"),
                                                       panelui::barDesc(Vec2{180.0f, 28.0f}));
        state.assetGenerate.setParent(assetContent);
        state.assetGenerate.setVisible(false);
        state.assetGenerate.onClick(
            [&state]
            {
                if (state.assetRegistryRevision != getComponentRegistryRevision())
                {
                    state.assetModal.close();
                    return;
                }
                if (!state.assetCandidate.isValid() ||
                    (state.assetCandidate.assetType != AssetType::MESH &&
                     state.assetCandidate.assetType != AssetType::MATERIAL))
                    return;
                const char* type = state.assetCandidate.assetType == AssetType::MESH ? "model" : "material";
                state.pending.push_back(
                    Json{{"command", "generateAssetPreviews"},
                         {"args", {{"type", type}, {"name", std::string(state.assetCandidate.name.toView())}}}}
                        .dump());
                state.assetNote.setText(state.tr("chooser.previewRequested"));
                state.assetPreviewMissingNoted = true;
            });
        state.assetPreview =
            state.scope.createImage("inspector.assetChooser.preview", panelui::barDesc(Vec2{256.0f, 256.0f}));
        state.assetPreview.setImageSlot(State::ASSET_PREVIEW_SLOT);
        state.assetPreview.setParent(assetContent);
        state.assetPreview.setVisible(false);
        state.assetParts = state.scope.createDropdown("inspector.assetChooser.parts",
                                                      {.rect = panelui::barDesc(Vec2{360.0f, 28.0f}), .maxRows = 10});
        state.assetParts.setAccessibleNameKey(InternedString("editor-inspector.chooser.part"));
        state.assetParts.setParent(assetContent);
        state.assetParts.onChange([&state](u32) { state.assetApply.setInteractable(true); });
        Panel assetButtons = state.scope.createPanel(
            "inspector.assetChooser.buttons",
            panelui::barDesc(Vec2{360.0f, 30.0f}, panelui::stack(UiLayoutDirection::Row, 0.0f)));
        panelui::bindGap(assetButtons, panelui::SPACE_MD);
        assetButtons.setParent(assetContent);
        state.assetPing = state.scope.createButton("inspector.assetChooser.ping", state.tr("chooser.ping"),
                                                   panelui::barDesc(Vec2{100.0f, 28.0f}));
        state.assetPing.setParent(assetButtons);
        state.assetPing.onClick(
            [&state]
            {
                if (!state.assetCandidate.isValid())
                    return;
                std::string name(state.assetCandidate.name.toView());
                const char* type = state.assetCandidate.assetType == AssetType::MATERIAL ? "material" : "model";
                if (state.assetCandidate.assetType == AssetType::SKELETON)
                {
                    const auto skeleton = state.engine->assets().manifest().getSkeletonEntry(state.assetCandidate.name);
                    if (!skeleton)
                    {
                        state.assetNote.setText(state.tr("chooser.currentUnavailable"));
                        return;
                    }
                    name = skeleton->model.toView();
                }
                state.pending.push_back(
                    Json{{"command", "projectSelect"}, {"args", {{"type", type}, {"name", name}}}}.dump());
                state.pending.push_back(
                    Json{{"command", "dock"}, {"args", {{"kind", "show"}, {"panel", "project"}}}}.dump());
                state.assetModal.close();
            });
        state.assetApply = state.scope.createButton("inspector.assetChooser.apply", state.tr("chooser.apply"),
                                                    panelui::barDesc(Vec2{100.0f, 28.0f}));
        state.assetApply.setParent(assetButtons);
        state.assetApply.onClick(
            [&state]
            {
                if (state.assetRegistryRevision != getComponentRegistryRevision())
                {
                    state.assetModal.close();
                    return;
                }
                if (!state.assetPendingRequest.empty())
                    return;
                if (state.selection != state.assetSelection || state.worldName != state.assetWorld ||
                    !state.assetCandidate.isValid())
                {
                    state.assetNote.setText(state.tr("chooser.changed"));
                    return;
                }
                const auto allowed = state.assetChoices(state.assetCandidate.assetType);
                if (!std::ranges::any_of(allowed, [&state](const AssetHandle& h)
                                         { return h.name == state.assetCandidate.name; }))
                {
                    state.assetNote.setText(state.tr("chooser.currentUnavailable"));
                    return;
                }
                AssetHandle chosen = state.assetCandidate;
                if (chosen.assetType == AssetType::MESH)
                {
                    const auto descriptor = loadBakedModelDescriptor(chosen.name, state.engine->assets().manifest());
                    const i32 part = state.assetParts.selected();
                    if (!descriptor || part < 0 || static_cast<sizet>(part) >= (*descriptor)->submeshes.size())
                    {
                        state.assetNote.setText(state.tr("chooser.changed"));
                        return;
                    }
                    const std::string label = std::format("{}: {}", part, (*descriptor)->submeshes[part].name.toView());
                    if (static_cast<sizet>(part) >= state.partItems.size() || state.partItems[part].label != label)
                    {
                        state.assetNote.setText(state.tr("chooser.changed"));
                        return;
                    }
                    chosen.subAssetIndex = static_cast<u32>(part);
                }
                for (sizet i = 0; i < state.selection.size(); ++i)
                {
                    const Entity entity = state.selection[i];
                    if (!state.world() || !state.world()->isEntityAlive(entity) ||
                        !state.world()->hasComponentById(entity, state.assetRow.component))
                    {
                        state.assetNote.setText(state.tr("chooser.changed"));
                        return;
                    }
                    const auto* data = static_cast<const std::byte*>(
                        state.world()->getConstComponentDataById(entity, state.assetRow.component));
                    if (!data || i >= state.assetOriginals.size() ||
                        assetAt(state.assetRow, data) != state.assetOriginals[i])
                    {
                        state.assetNote.setText(state.tr("chooser.changed"));
                        return;
                    }
                }
                state.pick(state.assetRow, chosen, true);
                if (!state.assetPendingRequest.empty())
                {
                    state.assetApply.setInteractable(false);
                    state.assetCombo.setInteractable(false);
                    state.assetParts.setInteractable(false);
                }
                else
                    state.assetNote.setText(state.tr("chooser.changed"));
            });
        Button assetCancel = state.scope.createButton("inspector.assetChooser.cancel", state.tr("chooser.cancel"),
                                                      panelui::barDesc(Vec2{100.0f, 28.0f}));
        assetCancel.setParent(assetButtons);
        assetCancel.onClick([&state] { state.assetModal.close(); });
        state.assetModal.onClose(
            [&state]
            {
                if (!state.assetPendingRequest.empty())
                    std::erase(state.pending, state.assetPendingRequest);
                state.assetPendingRequest.clear();
                ++state.assetChooserGeneration;
                if (state.assetLookup)
                    state.assetLookup->cancel();
                editorUi(*state.engine).uiView().clearImage(State::ASSET_PREVIEW_SLOT);
            });
        state.seenGeneration = state.control->stateGeneration();
        state.shownTextScale = state.styles->textScale();
        state.shownRevision = state.localization ? state.localization->revision() : 0;
        state.registerCommands();
        model.setSelectListener([&state] { state.replacing(); }, [&state] { state.rebuild(false); });
        state.rebuild(false);
        state.scope.commit();

        // the code's contract: the rows slot; without a readable document, the fallback builds the default panel
        UiViewContract contract;
        contract.slot(InternedString(ROWS_SLOT), [&state](const Widget& slot) { state.fill(slot); });
        contract.fallback(
            [](UiScope& viewScope, const Widget& root)
            {
                Widget panel = root;
                panel.setPivot(Vec2{0.0f, 0.0f});
                panel.setLayout(panelui::stack(UiLayoutDirection::Column, 0.0f, {}, UiLayoutAlign::Stretch));
                panel.setLayoutItem(panelui::fillItem());
                Panel rows = viewScope.createPanel(InternedString(ROWS_SLOT), panelui::columnDesc());
                rows.setLayoutItem(panelui::fillItem());
                rows.setParent(panel);
            });
        state.view = std::make_unique<UiDocumentView>(editorUi(engine).ui(), std::string(VIEW), std::move(contract),
                                                      state.host.entity());
        if (!state.view->load(resolveContentPath(DOCUMENT)))
        {
            SWARNING_LOG("inspector: {} did not apply ({} problems): the default panel shows", DOCUMENT,
                         state.view->problems().size());
        }
    }

    EditorInspector::~EditorInspector()
    {
        m_state->model->setSelectListener({}, {});
        m_state->control->unregisterOwner(OWNER);
        m_state->scope.unobserveLayoutTokens(m_state->densityObserver);
        editorUi(*m_state->engine).uiView().stylesheet().unloadLayer(InternedString(STYLE_LAYER));
    }

    Widget EditorInspector::content() const { return m_state->host; }

    void EditorInspector::setDialogs(EditorDialogs* dialogs) { m_state->dialogs = dialogs; }

    void EditorInspector::setTelemetry(const bool full, const float systemBudgetMs)
    {
        m_state->setTelemetry(full, systemBudgetMs);
    }

    void EditorInspector::setAgentLayer(EditorAgentLayer* agent)
    {
        m_state->agent = agent;
        m_state->rebuild(false);
    }

    void EditorInspector::update()
    {
        m_state->updateAssetPreview();
        m_state->dispatch();
        m_state->follow();
    }

    namespace
    {
        const EditorPanelRegistrar PANEL_REGISTRATION{
            {.name = "inspector",
             .order = 40,
             .dock = {.zone = UiDockZone::Right, .ratio = 0.29f, .after = "project"},
             .minSize = {200.0f, 0.0f}, // a row's name and value; a cramped window keeps its ratios instead
             // the shell updates the inspector before every panel: the selection is shared state
             .make = [](EditorPanelContext& c) { return EditorPanel{c.inspector.content(), {}, {}}; }}};
    } // namespace
} // namespace spite
