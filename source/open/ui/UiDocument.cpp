#include "ui/UiDocument.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <fstream>
#include <optional>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "base/Logging.hpp"
#include "base/io/FileSystem.hpp"
#include "ecs/Aspect.hpp"
#include "ecs/Chunk.hpp"
#include "ecs/World.hpp"
#include "localization/Localization.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiIcons.hpp"

namespace spite
{
    namespace
    {
        // ordered: an export keeps "id" and "kind" first, and localized arguments keep their authored order
        using Json = nlohmann::ordered_json;
        using Clock = std::chrono::steady_clock;
        constexpr u32 NONE = ~0u;
        // an element is an object inside a "children" array: two JSON levels per element, plus the document and the
        // deepest value objects
        constexpr u32 MAX_JSON_DEPTH = 2 * UI_DOCUMENT_MAX_DEPTH + 8;
        constexpr float MAX_COORDINATE = 1.0e7f;

        constexpr std::array<std::pair<std::string_view, UiWidgetKind>, 20> KIND_NAMES = {{
            {"panel", UiWidgetKind::Panel},
            {"button", UiWidgetKind::Button},
            {"label", UiWidgetKind::Label},
            {"textInput", UiWidgetKind::TextInput},
            {"image", UiWidgetKind::Image},
            {"scrollView", UiWidgetKind::ScrollView},
            {"scrollThumb", UiWidgetKind::ScrollThumb},
            {"numberField", UiWidgetKind::NumberField},
            {"checkbox", UiWidgetKind::Checkbox},
            {"toggle", UiWidgetKind::Toggle},
            {"slider", UiWidgetKind::Slider},
            {"progressBar", UiWidgetKind::ProgressBar},
            {"radioGroup", UiWidgetKind::RadioGroup},
            {"radio", UiWidgetKind::Radio},
            {"canvas", UiWidgetKind::Canvas},
            {"dropdown", UiWidgetKind::Dropdown},
            {"combo", UiWidgetKind::Combo},
            {"tabBar", UiWidgetKind::TabBar},
            {"tab", UiWidgetKind::Tab},
            {"chart", UiWidgetKind::Chart},
        }};
        static_assert(KIND_NAMES.size() == static_cast<sizet>(UiWidgetKind::Chart) + 1);

        std::string_view kindName(const UiWidgetKind kind) { return KIND_NAMES[static_cast<u8>(kind)].first; }

        std::optional<UiWidgetKind> kindFromName(const std::string_view name)
        {
            const auto it = std::ranges::find(KIND_NAMES, name, &std::pair<std::string_view, UiWidgetKind>::first);
            return it == KIND_NAMES.end() ? std::nullopt : std::optional(it->second);
        }

        bool creatable(const UiWidgetKind kind)
        {
            switch (kind)
            {
            case UiWidgetKind::Panel:
            case UiWidgetKind::Label:
            case UiWidgetKind::Button:
            case UiWidgetKind::Image:
            case UiWidgetKind::TextInput:
            case UiWidgetKind::NumberField:
            case UiWidgetKind::Checkbox:
            case UiWidgetKind::Toggle:
            case UiWidgetKind::Slider:
            case UiWidgetKind::ProgressBar:
            case UiWidgetKind::ScrollView:
                return true;
            default:
                return false;
            }
        }

        // a widget whose children are its own parts: a document lists none
        bool partsOnly(const UiWidgetKind kind)
        {
            return kind == UiWidgetKind::TextInput || kind == UiWidgetKind::NumberField ||
                kind == UiWidgetKind::Checkbox || kind == UiWidgetKind::Toggle || kind == UiWidgetKind::Slider ||
                kind == UiWidgetKind::ProgressBar;
        }

        bool isText(const UiWidgetKind kind) { return kind == UiWidgetKind::Label || kind == UiWidgetKind::Button; }
        bool isCheckable(const UiWidgetKind kind)
        {
            return kind == UiWidgetKind::Checkbox || kind == UiWidgetKind::Toggle;
        }
        bool isRanged(const UiWidgetKind kind)
        {
            return kind == UiWidgetKind::Slider || kind == UiWidgetKind::ProgressBar ||
                kind == UiWidgetKind::NumberField;
        }

        struct LayoutSpec
        {
            UiLayoutDirection direction = UiLayoutDirection::Row;
            float gap = 0.0f;
            Vec4 padding{0.0f, 0.0f, 0.0f, 0.0f};
            UiLayoutJustify justify = UiLayoutJustify::Start;
            UiLayoutAlign align = UiLayoutAlign::Start;
            bool wrap = false;
            // the "$name" layout tokens of the block's numbers, in UiLayoutField order from FIRST_TOKEN
            static constexpr UiLayoutField FIRST_TOKEN = UiLayoutField::Gap;
            std::array<InternedString, 5> tokens{};
            bool operator==(const LayoutSpec&) const = default;
        };

        struct ItemSpec
        {
            float grow = 0.0f;
            float shrink = 1.0f;
            float basis = -1.0f;
            Vec2 minSize{0.0f, 0.0f};
            Vec2 maxSize{0.0f, 0.0f};
            UiLayoutAlign alignSelf = UiLayoutAlign::Auto;
            static constexpr UiLayoutField FIRST_TOKEN = UiLayoutField::Basis;
            std::array<InternedString, 5> tokens{};
            bool operator==(const ItemSpec&) const = default;
        };

        struct FitSpec
        {
            bool width = true;
            bool height = true;
            Vec2 padding{0.0f, 0.0f};
            static constexpr UiLayoutField FIRST_TOKEN = UiLayoutField::FitPaddingX;
            std::array<InternedString, 2> tokens{};
            bool operator==(const FitSpec&) const = default;
        };

        // the numbers a spec's tokens bind, in token order
        std::array<float*, 5> tokenFields(LayoutSpec& s)
        {
            return {&s.gap, &s.padding.x, &s.padding.y, &s.padding.z, &s.padding.w};
        }
        std::array<float*, 5> tokenFields(ItemSpec& s)
        {
            return {&s.basis, &s.minSize.x, &s.minSize.y, &s.maxSize.x, &s.maxSize.y};
        }
        std::array<float*, 2> tokenFields(FitSpec& s) { return {&s.padding.x, &s.padding.y}; }

        // a "$name" layout token: the name, invalid for any other value; `malformed` when it is "$" plus a bad name
        InternedString layoutToken(const std::string_view text, bool& malformed)
        {
            malformed = false;
            if (text.empty() || text[0] != '$')
            {
                return {};
            }
            const std::string_view name = text.substr(1);
            malformed =
                name.empty() || name.size() > UI_DOCUMENT_MAX_ID ||
                !std::ranges::all_of(
                    name, [](const char c)
                    { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-'; });
            return malformed ? InternedString{} : InternedString(name);
        }

        struct BindSpec
        {
            std::string source;
            std::string component;
            std::string field;
            UiBindTarget target = UiBindTarget::Text;
            InternedString format{};
            std::string argument = "value";
            bool operator==(const BindSpec&) const = default;
        };

        // A slot (UiSlotComponent): its children are code's
        struct SlotSpec
        {
            bool scroll = true;
            InternedString childClass{};
            bool operator==(const SlotSpec&) const = default;
        };

        // What a document says of one element, every field at its effective value: an absent key is the kind's
        // default, so a document and a live element compare field by field.
        struct Props
        {
            InternedString styleClass{};
            Vec2 anchor{0.0f, 0.0f};
            Vec2 offset{0.0f, 0.0f};
            Vec2 size{0.0f, 0.0f};
            Vec2 pivot{0.5f, 0.5f};
            i32 z = 0;
            std::optional<LayoutSpec> layout;
            std::optional<ItemSpec> item;
            std::optional<FitSpec> fit;
            bool visible = true;
            bool interactable = true;
            bool clip = false;
            bool landmark = false;
            InternedString tooltip{}; // the message key of the element's tooltip (Widget::setTooltip)
            std::string text;
            InternedString textKey{};
            std::string textArgs; // UiLocalizedTextComponent::args encoding
            bool rich = false;
            std::string icon; // a Phosphor icon name (UiLabelComponent::icon)
            u64 image = 0;
            bool checked = false;
            double value = 0.0;
            double min = 0.0;
            double max = 1.0;
            double step = 0.0;
            Vec2 contentSize{0.0f, 0.0f};
            std::optional<BindSpec> bind;
            std::optional<SlotSpec> slot;
            bool operator==(const Props&) const = default;
        };

        Props defaultProps(const UiWidgetKind kind)
        {
            Props props;
            if (kind == UiWidgetKind::NumberField)
            {
                const UiNumberDesc number;
                props.min = number.min;
                props.max = number.max;
                props.step = number.step;
            }
            return props;
        }

        constexpr std::array<std::string_view, 2> DIRECTIONS = {"row", "column"};
        constexpr std::array<std::string_view, 4> JUSTIFIES = {"start", "center", "end", "spaceBetween"};
        constexpr std::array<std::string_view, 5> ALIGNS = {"auto", "start", "center", "end", "stretch"};
        constexpr std::array<std::string_view, 3> BIND_TARGETS = {"text", "value", "visible"};
    } // namespace

    struct UiDocumentModel
    {
        struct Node
        {
            InternedString id{};
            UiWidgetKind kind = UiWidgetKind::Panel;
            bool opaque = false;
            u32 parent = NONE;
            std::vector<u32> children;
            std::string path;
            Props props;
        };
        std::vector<Node> nodes; // pre-order; [0] is the root
    };

    namespace
    {
        bool validName(const std::string_view name)
        {
            return !name.empty() && name.size() <= UI_DOCUMENT_MAX_ID &&
                std::ranges::none_of(name, [](const char c)
                                     { return static_cast<unsigned char>(c) < 0x20 || c == '/' || c == '#'; });
        }

        std::string formatNumber(const double value)
        {
            std::array<char, 32> buffer{};
            const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
            return error == std::errc{} ? std::string(buffer.data(), end) : std::string("0");
        }

        template <sizet N>
        std::optional<u8> choiceOf(const std::array<std::string_view, N>& names, const std::string_view value)
        {
            const auto it = std::ranges::find(names, value);
            return it == names.end() ? std::nullopt : std::optional(static_cast<u8>(it - names.begin()));
        }

        template <sizet N>
        std::string choices(const std::array<std::string_view, N>& names)
        {
            std::string allowed;
            for (const std::string_view name : names)
            {
                allowed += (allowed.empty() ? "" : ", ") + std::string(name);
            }
            return allowed;
        }

        // A document parsed in one SAX pass (no DOM): untrusted text is bounded in bytes before, in JSON depth and in
        // elements while it is read, a duplicate key ends the read (JSON parsers keep the last of two), and each
        // element's keys are typed as they come and checked against its kind when it closes. The tree's id paths are
        // made after the read, as an element's id may follow its children.
        class Reader final : public nlohmann::json_sax<Json>
        {
        public:
            Reader(const UiViewContract& contract, const Localization* localization, UiDocumentModel& model,
                   std::vector<UiDocumentProblem>& problems) :
                m_contract(contract), m_localization(localization), m_model(model), m_problems(problems)
            {
            }

            bool failed() const { return m_failed; }
            u64 keyChecks() const { return m_keyChecks; }

            void report(const std::string& path, std::string message, const bool error)
            {
                m_failed |= error;
                if (m_problems.size() < UI_DOCUMENT_REPORT_CAP)
                {
                    m_problems.push_back({path, std::move(message), error});
                }
            }

            // after a complete read: paths, then the problems that named an element by index
            void finish()
            {
                if (!m_sawVersion)
                {
                    report("", std::format("'version' must be {}", UI_DOCUMENT_VERSION), true);
                }
                if (m_model.nodes.empty() && !m_failed)
                {
                    report("", "the document has no 'root' element", true);
                }
                std::unordered_map<InternedString, u32> occurrences;
                sizet pathBytes = 0;
                for (u32 index = 0; index < m_model.nodes.size(); ++index)
                {
                    UiDocumentModel::Node& node = m_model.nodes[index];
                    if (node.parent == NONE)
                    {
                        node.path = std::string(node.id.toView());
                    }
                    // siblings sharing an id (pooled rows, repeated lines) are told apart by occurrence: "line#1"
                    occurrences.clear();
                    for (const u32 kid : node.children)
                    {
                        UiDocumentModel::Node& child = m_model.nodes[kid];
                        const u32 occurrence = occurrences[child.id]++;
                        const sizet length = node.path.size() + 1 + child.id.toView().size() + (occurrence ? 11 : 0);
                        pathBytes += length;
                        if (length > UI_DOCUMENT_MAX_PATH || pathBytes > UI_DOCUMENT_MAX_PATH_BYTES)
                        {
                            report("",
                                   length > UI_DOCUMENT_MAX_PATH
                                       ? std::format("an element's id path is longer than {} bytes (nesting times id "
                                                     "length): refused",
                                                     UI_DOCUMENT_MAX_PATH)
                                       : std::format("the id paths of all elements exceed {} bytes: refused",
                                                     UI_DOCUMENT_MAX_PATH_BYTES),
                                   true);
                            m_pending.clear();
                            return;
                        }
                        child.path = node.path + "/" + std::string(child.id.toView());
                        if (occurrence != 0)
                        {
                            child.path += std::format("#{}", occurrence);
                        }
                    }
                }
                for (auto& [index, message, error] : m_pending)
                {
                    report(index < m_model.nodes.size() ? m_model.nodes[index].path : std::string{}, std::move(message),
                           error);
                }
                m_pending.clear();
            }

            // the contract's required ids and kinds against the parsed tree
            void checkContract()
            {
                for (const UiViewContract::Element& wanted : m_contract.elements())
                {
                    u32 found = 0;
                    UiDocumentModel::Node* first = nullptr;
                    for (UiDocumentModel::Node& node : m_model.nodes)
                    {
                        if (node.id == wanted.id)
                        {
                            first = found++ == 0 ? &node : first;
                        }
                    }
                    const std::string id(wanted.id.toView());
                    if (!first)
                    {
                        report(id,
                               std::format("the view's contract needs element '{}' ({})", id, kindName(wanted.kind)),
                               wanted.required);
                    }
                    else if (first->kind != wanted.kind)
                    {
                        report(first->path,
                               std::format("the contract declares '{}' a {}, the document a {}", id,
                                           kindName(wanted.kind), kindName(first->kind)),
                               wanted.required);
                    }
                    else if (wanted.slot && found > 1)
                    {
                        // a slot is matched by id wherever the document puts it
                        report(first->path, std::format("{} elements are '{}', the view's slot: one only", found, id),
                               true);
                    }
                    else if (found > 1)
                    {
                        report(first->path,
                               std::format("{} elements are '{}': the contract binds the first", found, id), false);
                    }
                    else if (wanted.slot && !first->props.slot)
                    {
                        first->props.slot.emplace(); // the contract makes it a slot
                        if (!first->children.empty())
                        {
                            report(first->path,
                                   std::format("'{}' is a slot: it lists no children (code builds them)", id), true);
                        }
                    }
                    if (wanted.slot && first && first->kind == wanted.kind && first->opaque)
                    {
                        report(first->path, std::format("'{}' is the view's slot: it cannot be opaque", id), true);
                    }
                }
            }

            // -- json_sax ---------------------------------------------------------------------------------

            bool null() override { return scalar(Scalar{}); }
            bool boolean(const bool value) override { return scalar(Scalar{.type = Scalar::Bool, .boolean = value}); }
            bool number_integer(const number_integer_t value) override
            {
                return scalar(Scalar{.type = Scalar::Integer, .number = static_cast<double>(value), .integer = value});
            }
            bool number_unsigned(const number_unsigned_t value) override
            {
                return scalar(Scalar{.type = Scalar::Unsigned,
                                     .number = static_cast<double>(value),
                                     .integer = value <= static_cast<u64>(std::numeric_limits<std::int64_t>::max())
                                         ? static_cast<std::int64_t>(value)
                                         : std::numeric_limits<std::int64_t>::max(),
                                     .unsignedValue = value});
            }
            bool number_float(const number_float_t value, const string_t&) override
            {
                return scalar(Scalar{.type = Scalar::Float, .number = value});
            }
            bool string(string_t& value) override { return scalar(Scalar{.type = Scalar::String, .text = &value}); }
            bool binary(binary_t&) override { return scalar(Scalar{}); }

            bool start_object(std::size_t) override
            {
                if (!enter(true))
                {
                    return false;
                }
                const Level parent = m_levels.empty() ? Level{} : m_levels.back();
                Level level{.scope = Scope::Skip, .node = parent.node};
                if (m_levels.empty())
                {
                    level.scope = Scope::Document;
                }
                else if (parent.scope == Scope::Skip)
                {
                    level.scope = Scope::Skip;
                }
                else if (parent.scope == Scope::Children || (parent.scope == Scope::Document && m_key == "root"))
                {
                    if (parent.scope == Scope::Document && !m_model.nodes.empty())
                    {
                        level.scope = Scope::Skip;
                    }
                    else if (!openElement(parent.scope == Scope::Children ? parent.node : NONE, level))
                    {
                        return false;
                    }
                }
                else if (parent.scope == Scope::Floats)
                {
                    m_floatBad = true;
                }
                else if (parent.scope == Scope::TextArgs)
                {
                    problem(parent.node,
                            std::format("textArgs '{}' must be a finite number or a one-line string", m_key), true);
                }
                else if (parent.scope == Scope::Element)
                {
                    Pending& pending = m_pending_elements[m_elementDepth - 1];
                    if (m_key == "layout")
                    {
                        level.scope = Scope::Layout;
                        pending.layout.emplace();
                    }
                    else if (m_key == "item")
                    {
                        level.scope = Scope::Item;
                        pending.item.emplace();
                    }
                    else if (m_key == "fit")
                    {
                        level.scope = Scope::Fit;
                        pending.fit.emplace();
                    }
                    else if (m_key == "bind")
                    {
                        level.scope = Scope::Bind;
                        pending.bind.emplace();
                    }
                    else if (m_key == "textArgs")
                    {
                        level.scope = Scope::TextArgs;
                        pending.textArgs.emplace();
                    }
                    else if (m_key == "slot")
                    {
                        level.scope = Scope::Slot;
                        pending.slot.emplace();
                    }
                    else
                    {
                        wrongValue(parent);
                    }
                }
                else
                {
                    wrongValue(parent);
                }
                m_levels.push_back(level);
                return true;
            }

            bool end_object() override
            {
                const Level level = m_levels.back();
                m_levels.pop_back();
                --m_depth;
                if (level.scope == Scope::Element)
                {
                    closeElement(level.node);
                }
                else if (level.scope == Scope::Item)
                {
                    const ItemSpec& item = *m_pending_elements[m_elementDepth - 1].item;
                    if ((item.maxSize.x > 0.0f && item.minSize.x > item.maxSize.x) ||
                        (item.maxSize.y > 0.0f && item.minSize.y > item.maxSize.y))
                    {
                        problem(level.node, "'item.min' must not exceed 'item.max' (a 0 max is unbounded)", true);
                    }
                }
                else if (level.scope == Scope::Bind)
                {
                    const BindSpec& bind = *m_pending_elements[m_elementDepth - 1].bind;
                    if (bind.source.empty() || bind.component.empty() || bind.field.empty())
                    {
                        problem(level.node, "'bind' needs source, component and field", true);
                    }
                }
                return true;
            }

            bool start_array(std::size_t) override
            {
                if (!enter(false))
                {
                    return false;
                }
                if (m_levels.empty())
                {
                    report("", "a UI document is an object {\"version\": 1, \"root\": {...}}", true);
                    m_levels.push_back(Level{});
                    return true;
                }
                const Level parent = m_levels.back();
                Level level{.scope = Scope::Skip, .node = parent.node};
                if (parent.scope == Scope::Floats)
                {
                    m_floatBad = true;
                }
                else if (parent.scope == Scope::TextArgs)
                {
                    problem(parent.node,
                            std::format("textArgs '{}' must be a finite number or a one-line string", m_key), true);
                }
                else if (parent.scope == Scope::Element && m_key == "children")
                {
                    level.scope = Scope::Children;
                }
                else if (const std::optional<FloatTarget> target = floatTarget(parent.scope); target)
                {
                    level.scope = Scope::Floats;
                    level.target = *target;
                    m_floatCount = 0;
                    m_floatTokens = {};
                }
                else if (parent.scope != Scope::Skip)
                {
                    wrongValue(parent);
                }
                m_levels.push_back(level);
                return true;
            }

            bool end_array() override
            {
                const Level level = m_levels.back();
                m_levels.pop_back();
                --m_depth;
                if (level.scope == Scope::Floats)
                {
                    storeFloats(level);
                }
                return true;
            }

            bool key(string_t& name) override
            {
                KeySet& keys = m_keys[m_depth - 1];
                // linear below a few keys (every element), hashed past them (a hostile object of 100k keys)
                constexpr sizet LINEAR_KEYS = 16;
                bool duplicate;
                if (keys.list.size() < LINEAR_KEYS)
                {
                    duplicate = std::ranges::find(keys.list, name) != keys.list.end();
                    m_keyChecks += keys.list.size();
                }
                else
                {
                    if (keys.hashed.empty())
                    {
                        keys.hashed.insert(keys.list.begin(), keys.list.end());
                        m_keyChecks += keys.list.size();
                    }
                    duplicate = !keys.hashed.insert(name).second;
                    ++m_keyChecks;
                }
                if (duplicate)
                {
                    m_fatal = "duplicate key '" + name + "'";
                    return false;
                }
                keys.list.push_back(name);
                m_key = name;
                return true;
            }

            bool parse_error(std::size_t position, const std::string&, const nlohmann::detail::exception& e) override
            {
                m_fatal = std::format("byte {}: {}", position, e.what());
                return false;
            }

            const std::string& fatal() const { return m_fatal; }

        private:
            enum class Scope : u8
            {
                Document,
                Element,
                Children,
                Layout,
                Item,
                Fit,
                Bind,
                TextArgs,
                Slot,
                Floats,
                Skip,
            };

            enum class FloatTarget : u8
            {
                Anchor,
                Offset,
                Size,
                Pivot,
                ContentSize,
                LayoutPadding,
                ItemMin,
                ItemMax,
                FitPadding,
            };

            struct Level
            {
                Scope scope = Scope::Skip;
                u32 node = NONE;
                FloatTarget target = FloatTarget::Anchor;
            };

            struct Scalar
            {
                enum Type : u8
                {
                    Null,
                    Bool,
                    Integer,
                    Unsigned,
                    Float,
                    String,
                };
                Type type = Null;
                bool boolean = false;
                double number = 0.0;
                std::int64_t integer = 0;
                u64 unsignedValue = 0;
                std::string* text = nullptr;

                bool isNumber() const { return type == Integer || type == Unsigned || type == Float; }
            };

            // what an element's keys said, typed, until it closes and its kind decides what applies
            struct Pending
            {
                std::string id;
                std::optional<std::string> kind;
                std::optional<std::string> styleClass;
                std::array<std::optional<Vec2>, 5> vectors; // anchor, offset, size, pivot, contentSize
                std::optional<i32> z;
                std::optional<LayoutSpec> layout;
                std::optional<ItemSpec> item;
                std::optional<FitSpec> fit;
                std::optional<bool> visible;
                std::optional<bool> interactable;
                std::optional<bool> clip;
                std::optional<bool> landmark;
                std::optional<std::string> tooltip;
                std::optional<bool> opaque;
                std::optional<std::string> text;
                std::optional<std::string> textKey;
                std::optional<std::string> textArgs;
                std::optional<bool> rich;
                std::optional<std::string> icon;
                std::optional<u64> image;
                std::optional<bool> checked;
                std::optional<double> value;
                std::optional<double> min;
                std::optional<double> max;
                std::optional<double> step;
                std::optional<BindSpec> bind;
                std::optional<SlotSpec> slot;
            };

            bool enter(const bool object)
            {
                if (++m_depth > MAX_JSON_DEPTH)
                {
                    m_fatal = std::format("nested deeper than {} levels", MAX_JSON_DEPTH);
                    return false;
                }
                if (m_keys.size() < m_depth)
                {
                    m_keys.emplace_back();
                }
                m_keys[m_depth - 1].list.clear();
                m_keys[m_depth - 1].hashed.clear();
                (void)object;
                return true;
            }

            bool openElement(const u32 parent, Level& level)
            {
                if (m_model.nodes.size() >= UI_DOCUMENT_MAX_NODES)
                {
                    m_fatal = std::format("more than {} elements", UI_DOCUMENT_MAX_NODES);
                    return false;
                }
                if (m_elementDepth >= UI_DOCUMENT_MAX_DEPTH)
                {
                    m_fatal = std::format("elements nest deeper than {}", UI_DOCUMENT_MAX_DEPTH);
                    return false;
                }
                const auto index = static_cast<u32>(m_model.nodes.size());
                UiDocumentModel::Node& node = m_model.nodes.emplace_back();
                node.parent = parent;
                if (parent != NONE)
                {
                    m_model.nodes[parent].children.push_back(index);
                }
                if (m_pending_elements.size() <= m_elementDepth)
                {
                    m_pending_elements.emplace_back();
                }
                Pending& pending = m_pending_elements[m_elementDepth];
                pending = Pending{};
                ++m_elementDepth;
                level.scope = Scope::Element;
                level.node = index;
                return true;
            }

            void problem(const u32 node, std::string message, const bool error)
            {
                m_failed |= error;
                if (m_pending.size() < UI_DOCUMENT_REPORT_CAP)
                {
                    m_pending.push_back({node, std::move(message), error});
                }
            }

            std::string_view where(const Scope scope) const
            {
                return scope == Scope::Layout  ? "layout."
                    : scope == Scope::Item     ? "item."
                    : scope == Scope::Fit      ? "fit."
                    : scope == Scope::Bind     ? "bind."
                    : scope == Scope::TextArgs ? "textArgs."
                    : scope == Scope::Slot     ? "slot."
                                               : "";
            }

            // a value of a shape its key does not take: an error for a known key, ignored for an unknown one
            void wrongValue(const Level& parent)
            {
                if (parent.scope == Scope::Children)
                {
                    problem(parent.node, "an element must be an object", true);
                }
                else if (parent.scope == Scope::Document)
                {
                    if (m_key == "root")
                    {
                        problem(NONE, "'root' must be an element object", true);
                    }
                    else if (m_key != "version")
                    {
                        problem(NONE, std::format("unknown key '{}' in the document: ignored", m_key), false);
                    }
                    else
                    {
                        problem(NONE, std::format("'version' must be {}", UI_DOCUMENT_VERSION), true);
                    }
                }
                else if (known(parent.scope, m_key))
                {
                    problem(parent.node, std::format("'{}{}' has the wrong type", where(parent.scope), m_key), true);
                }
                else
                {
                    problem(parent.node, std::format("unknown key '{}{}': ignored", where(parent.scope), m_key), false);
                }
            }

            static bool known(const Scope scope, const std::string_view key)
            {
                static constexpr std::array<std::string_view, 33> ELEMENT = {
                    "id",   "kind",    "class",    "anchor",      "offset",  "size",         "pivot",
                    "z",    "layout",  "item",     "fit",         "visible", "interactable", "clip",
                    "text", "textKey", "textArgs", "rich",        "image",   "checked",      "value",
                    "min",  "max",     "step",     "contentSize", "bind",    "opaque",       "children",
                    "slot", "version", "landmark", "tooltip",     "icon"};
                static constexpr std::array<std::string_view, 2> SLOT = {"scroll", "childClass"};
                static constexpr std::array<std::string_view, 6> LAYOUT = {"direction", "gap",   "padding",
                                                                           "justify",   "align", "wrap"};
                static constexpr std::array<std::string_view, 6> ITEM = {"grow", "shrink", "basis",
                                                                         "min",  "max",    "alignSelf"};
                static constexpr std::array<std::string_view, 3> FIT = {"width", "height", "padding"};
                static constexpr std::array<std::string_view, 6> BIND = {"source", "component", "field",
                                                                         "target", "format",    "argument"};
                const auto in = [&](const auto& names) { return std::ranges::find(names, key) != names.end(); };
                switch (scope)
                {
                case Scope::Element:
                    return key != "version" && in(ELEMENT);
                case Scope::Layout:
                    return in(LAYOUT);
                case Scope::Item:
                    return in(ITEM);
                case Scope::Fit:
                    return in(FIT);
                case Scope::Bind:
                    return in(BIND);
                case Scope::Slot:
                    return in(SLOT);
                default:
                    return false;
                }
            }

            std::optional<FloatTarget> floatTarget(const Scope scope) const
            {
                if (scope == Scope::Element)
                {
                    return m_key == "anchor"     ? std::optional(FloatTarget::Anchor)
                        : m_key == "offset"      ? std::optional(FloatTarget::Offset)
                        : m_key == "size"        ? std::optional(FloatTarget::Size)
                        : m_key == "pivot"       ? std::optional(FloatTarget::Pivot)
                        : m_key == "contentSize" ? std::optional(FloatTarget::ContentSize)
                                                 : std::nullopt;
                }
                if (scope == Scope::Layout && m_key == "padding")
                {
                    return FloatTarget::LayoutPadding;
                }
                if (scope == Scope::Item && (m_key == "min" || m_key == "max"))
                {
                    return m_key == "min" ? FloatTarget::ItemMin : FloatTarget::ItemMax;
                }
                if (scope == Scope::Fit && m_key == "padding")
                {
                    return FloatTarget::FitPadding;
                }
                return std::nullopt;
            }

            static constexpr std::string_view FLOAT_KEYS[] = {"anchor",   "offset",      "size",
                                                              "pivot",    "contentSize", "layout.padding",
                                                              "item.min", "item.max",    "fit.padding"};

            void storeFloats(const Level& level)
            {
                const u32 wanted = level.target == FloatTarget::LayoutPadding ? 4 : 2;
                const std::string_view key = FLOAT_KEYS[static_cast<u8>(level.target)];
                if (m_floatCount != wanted || m_floatBad)
                {
                    problem(level.node,
                            std::format("'{}' must be an array of {} numbers within +-{}", key, wanted, MAX_COORDINATE),
                            true);
                    m_floatBad = false;
                    return;
                }
                if (level.target >= FloatTarget::LayoutPadding &&
                    std::any_of(m_floats.begin(), m_floats.begin() + wanted, [](const float f) { return f < 0.0f; }))
                {
                    problem(level.node, std::format("'{}' must not be negative", key), true);
                    return;
                }
                const Vec2 pair{m_floats[0], m_floats[1]};
                Pending& pending = m_pending_elements[m_elementDepth - 1];
                switch (level.target)
                {
                case FloatTarget::Anchor:
                case FloatTarget::Offset:
                case FloatTarget::Size:
                case FloatTarget::Pivot:
                case FloatTarget::ContentSize:
                    pending.vectors[static_cast<u8>(level.target)] = pair;
                    break;
                case FloatTarget::LayoutPadding:
                    pending.layout->padding = {m_floats[0], m_floats[1], m_floats[2], m_floats[3]};
                    break;
                case FloatTarget::ItemMin:
                    pending.item->minSize = pair;
                    break;
                case FloatTarget::ItemMax:
                    pending.item->maxSize = pair;
                    break;
                case FloatTarget::FitPadding:
                    pending.fit->padding = pair;
                    break;
                }
                if (level.target >= FloatTarget::LayoutPadding)
                {
                    std::ranges::copy(std::span(m_floatTokens).first(wanted), arraySlots(level.target).begin());
                }
            }

            // the pending spec's token slots of an array key, in its order
            std::span<InternedString> arraySlots(const FloatTarget target)
            {
                Pending& pending = m_pending_elements[m_elementDepth - 1];
                switch (target)
                {
                case FloatTarget::LayoutPadding:
                    return std::span(pending.layout->tokens).subspan(1, 4);
                case FloatTarget::ItemMin:
                    return std::span(pending.item->tokens).subspan(1, 2);
                case FloatTarget::ItemMax:
                    return std::span(pending.item->tokens).subspan(3, 2);
                case FloatTarget::FitPadding:
                    return pending.fit->tokens;
                default:
                    return {};
                }
            }

            // a "$name" string in a layout, item or fit number position: binds every slot of the key (all of an
            // array key's)
            bool bindToken(const Level& level, const std::string_view text)
            {
                if (level.scope != Scope::Layout && level.scope != Scope::Item && level.scope != Scope::Fit)
                {
                    return false;
                }
                Pending& pending = m_pending_elements[m_elementDepth - 1];
                const std::optional<FloatTarget> array = floatTarget(level.scope);
                const std::span<InternedString> tokens = array       ? arraySlots(*array)
                    : level.scope == Scope::Layout && m_key == "gap" ? std::span(pending.layout->tokens).first(1)
                    : level.scope == Scope::Item && m_key == "basis" ? std::span(pending.item->tokens).first(1)
                                                                     : std::span<InternedString>{};
                bool malformed = false;
                const InternedString token = layoutToken(text, malformed);
                if (tokens.empty() || (!token.isValid() && !malformed))
                {
                    return false;
                }
                if (malformed)
                {
                    problem(level.node,
                            std::format("'{}{}' token \"{}\": a name is 1 to {} of A-Z a-z 0-9 _ . -",
                                        where(level.scope), m_key, text, UI_DOCUMENT_MAX_ID),
                            true);
                    return true;
                }
                std::ranges::fill(tokens, token);
                return true;
            }

            std::optional<float> coordinate(const Scalar& value, const u32 node, const std::string_view key)
            {
                if (!value.isNumber() || !std::isfinite(value.number) || std::abs(value.number) > MAX_COORDINATE)
                {
                    problem(node, std::format("'{}' must be a number within +-{}", key, MAX_COORDINATE), true);
                    return std::nullopt;
                }
                return static_cast<float>(value.number);
            }

            std::optional<float> nonNegative(const std::optional<float> number, const u32 node,
                                             const std::string_view key)
            {
                if (number && *number < 0.0f)
                {
                    problem(node, std::format("'{}' must not be negative", key), true);
                    return std::nullopt;
                }
                return number;
            }

            std::optional<std::string> text(const Scalar& value, const u32 node, const std::string_view key,
                                            const sizet limit)
            {
                if (value.type != Scalar::String || value.text->size() > limit)
                {
                    problem(node, std::format("'{}' must be a string of at most {} bytes", key, limit), true);
                    return std::nullopt;
                }
                return std::move(*value.text);
            }

            std::optional<bool> flag(const Scalar& value, const u32 node, const std::string_view key)
            {
                if (value.type != Scalar::Bool)
                {
                    problem(node, std::format("'{}' must be true or false", key), true);
                    return std::nullopt;
                }
                return value.boolean;
            }

            template <typename Enum, sizet N>
            void choice(const Scalar& value, const u32 node, const std::string_view key,
                        const std::array<std::string_view, N>& names, Enum& out)
            {
                const std::optional<u8> index =
                    value.type == Scalar::String ? choiceOf(names, *value.text) : std::nullopt;
                if (!index)
                {
                    problem(node, std::format("'{}' must be one of {}", key, choices(names)), true);
                    return;
                }
                out = static_cast<Enum>(*index);
            }

            bool scalar(const Scalar& value)
            {
                if (m_levels.empty())
                {
                    report("", "a UI document is an object {\"version\": 1, \"root\": {...}}", true);
                    return true;
                }
                const Level& level = m_levels.back();
                const u32 node = level.node;
                if (value.type == Scalar::String && bindToken(level, *value.text))
                {
                    return true;
                }
                switch (level.scope)
                {
                case Scope::Skip:
                    return true;
                case Scope::Document:
                    if (m_key == "version")
                    {
                        m_sawVersion = value.type == Scalar::Unsigned && value.unsignedValue == UI_DOCUMENT_VERSION;
                        if (!m_sawVersion)
                        {
                            problem(NONE, std::format("'version' must be {}", UI_DOCUMENT_VERSION), true);
                            m_sawVersion = true; // reported once
                        }
                    }
                    else
                    {
                        wrongValue(level);
                    }
                    return true;
                case Scope::Children:
                    problem(node, "an element must be an object", true);
                    return true;
                case Scope::Floats:
                    if (m_floatCount < 4 && value.isNumber() && std::isfinite(value.number) &&
                        std::abs(value.number) <= MAX_COORDINATE)
                    {
                        m_floats[m_floatCount] = static_cast<float>(value.number);
                    }
                    else if (bool malformed = false; m_floatCount < 4 && level.target >= FloatTarget::LayoutPadding &&
                             value.type == Scalar::String &&
                             (m_floatTokens[m_floatCount] = layoutToken(*value.text, malformed)).isValid())
                    {
                        m_floats[m_floatCount] = 0.0f; // the field's default until the token applies
                    }
                    else if (malformed)
                    {
                        problem(node,
                                std::format("'{}' token \"{}\": a name is 1 to {} of A-Z a-z 0-9 _ . -",
                                            FLOAT_KEYS[static_cast<u8>(level.target)], *value.text, UI_DOCUMENT_MAX_ID),
                                true);
                        m_floats[m_floatCount] = 0.0f; // reported once, not again as a bad array
                    }
                    else
                    {
                        m_floatBad = true;
                    }
                    ++m_floatCount;
                    return true;
                case Scope::Element:
                    elementScalar(value, level);
                    return true;
                case Scope::Layout:
                    {
                        LayoutSpec& layout = *m_pending_elements[m_elementDepth - 1].layout;
                        if (m_key == "direction")
                        {
                            choice(value, node, "layout.direction", DIRECTIONS, layout.direction);
                        }
                        else if (m_key == "gap")
                        {
                            layout.gap = nonNegative(coordinate(value, node, "layout.gap"), node, "layout.gap")
                                             .value_or(layout.gap);
                        }
                        else if (m_key == "justify")
                        {
                            choice(value, node, "layout.justify", JUSTIFIES, layout.justify);
                        }
                        else if (m_key == "align")
                        {
                            choice(value, node, "layout.align", ALIGNS, layout.align);
                        }
                        else if (m_key == "wrap")
                        {
                            layout.wrap = flag(value, node, "layout.wrap").value_or(layout.wrap);
                        }
                        else
                        {
                            wrongValue(level);
                        }
                        return true;
                    }
                case Scope::Item:
                    {
                        ItemSpec& item = *m_pending_elements[m_elementDepth - 1].item;
                        if (m_key == "grow" || m_key == "shrink" || m_key == "basis")
                        {
                            float& target = m_key == "grow" ? item.grow : m_key == "shrink" ? item.shrink : item.basis;
                            std::optional<float> number = coordinate(value, node, "item." + m_key);
                            target = (m_key == "basis" ? number : nonNegative(number, node, "item." + m_key))
                                         .value_or(target);
                        }
                        else if (m_key == "alignSelf")
                        {
                            choice(value, node, "item.alignSelf", ALIGNS, item.alignSelf);
                        }
                        else
                        {
                            wrongValue(level);
                        }
                        return true;
                    }
                case Scope::Fit:
                    {
                        FitSpec& fit = *m_pending_elements[m_elementDepth - 1].fit;
                        if (m_key == "width" || m_key == "height")
                        {
                            bool& target = m_key == "width" ? fit.width : fit.height;
                            target = flag(value, node, "fit." + m_key).value_or(target);
                        }
                        else
                        {
                            wrongValue(level);
                        }
                        return true;
                    }
                case Scope::Bind:
                    {
                        BindSpec& bind = *m_pending_elements[m_elementDepth - 1].bind;
                        if (m_key == "source" || m_key == "component" || m_key == "field" || m_key == "argument")
                        {
                            std::string& target = m_key == "source" ? bind.source
                                : m_key == "component"              ? bind.component
                                : m_key == "field"                  ? bind.field
                                                                    : bind.argument;
                            target = text(value, node, "bind." + m_key, UI_DOCUMENT_MAX_ID).value_or(target);
                        }
                        else if (m_key == "target")
                        {
                            choice(value, node, "bind.target", BIND_TARGETS, bind.target);
                        }
                        else if (m_key == "format")
                        {
                            const std::optional<std::string> format =
                                text(value, node, "bind.format", UI_DOCUMENT_MAX_ID);
                            if (format && !isValidMessageKey(*format))
                            {
                                problem(node, std::format("bind.format '{}' is not a message key", *format), true);
                            }
                            else if (format)
                            {
                                bind.format = InternedString(*format);
                            }
                        }
                        else
                        {
                            wrongValue(level);
                        }
                        return true;
                    }
                case Scope::TextArgs:
                    textArg(value, node);
                    return true;
                case Scope::Slot:
                    {
                        SlotSpec& slot = *m_pending_elements[m_elementDepth - 1].slot;
                        if (m_key == "scroll")
                        {
                            slot.scroll = flag(value, node, "slot.scroll").value_or(slot.scroll);
                        }
                        else if (m_key == "childClass")
                        {
                            const std::optional<std::string> name =
                                text(value, node, "slot.childClass", UI_DOCUMENT_MAX_ID);
                            if (name && !name->empty() && !validName(*name))
                            {
                                problem(node, "'slot.childClass' must be a name without '/', '#' or control characters",
                                        true);
                            }
                            else if (name && !name->empty())
                            {
                                slot.childClass = InternedString(*name);
                            }
                        }
                        else
                        {
                            wrongValue(level);
                        }
                        return true;
                    }
                }
                return true;
            }

            void textArg(const Scalar& value, const u32 node)
            {
                std::string& out = *m_pending_elements[m_elementDepth - 1].textArgs;
                if (m_key.empty() || m_key.size() > UI_DOCUMENT_MAX_ID ||
                    m_key.find_first_of("=\n\r") != std::string::npos)
                {
                    problem(node,
                            std::format("textArgs name '{}' must be 1 to {} bytes without '=' or a line break", m_key,
                                        UI_DOCUMENT_MAX_ID),
                            true);
                    return;
                }
                std::string encoded;
                if (value.type == Scalar::Integer || value.type == Scalar::Unsigned)
                {
                    encoded = value.type == Scalar::Unsigned ? std::to_string(value.unsignedValue)
                                                             : std::to_string(value.integer);
                }
                else if (value.type == Scalar::Float && std::isfinite(value.number))
                {
                    encoded = formatNumber(value.number);
                }
                else if (value.type == Scalar::String && value.text->size() <= UI_DOCUMENT_MAX_TEXT &&
                         value.text->find_first_of("\n\r") == std::string::npos)
                {
                    encoded = "'" + *value.text;
                }
                else
                {
                    problem(node, std::format("textArgs '{}' must be a finite number or a one-line string", m_key),
                            true);
                    return;
                }
                out += (out.empty() ? "" : "\n") + m_key + "=" + encoded;
            }

            void elementScalar(const Scalar& value, const Level& level)
            {
                Pending& pending = m_pending_elements[m_elementDepth - 1];
                const u32 node = level.node;
                const std::string& key = m_key;
                if (key == "id")
                {
                    if (value.type != Scalar::String || !validName(*value.text))
                    {
                        problem(node,
                                std::format("an element needs an 'id': 1 to {} bytes, no '/', '#' or control character",
                                            UI_DOCUMENT_MAX_ID),
                                true);
                    }
                    else
                    {
                        pending.id = std::move(*value.text);
                    }
                }
                else if (key == "kind")
                {
                    pending.kind = value.type == Scalar::String ? *value.text : std::string("?");
                }
                else if (key == "class")
                {
                    pending.styleClass = text(value, node, key, UI_DOCUMENT_MAX_ID);
                    if (pending.styleClass && !validName(*pending.styleClass))
                    {
                        problem(node, "'class' must be a name without '/', '#' or control characters", true);
                    }
                }
                else if (key == "z")
                {
                    if ((value.type != Scalar::Integer && value.type != Scalar::Unsigned) ||
                        value.integer < std::numeric_limits<i32>::min() ||
                        value.integer > std::numeric_limits<i32>::max())
                    {
                        problem(node, "'z' must be a 32-bit integer", true);
                    }
                    else
                    {
                        pending.z = static_cast<i32>(value.integer);
                    }
                }
                else if (key == "visible" || key == "interactable" || key == "clip" || key == "opaque" ||
                         key == "landmark")
                {
                    std::optional<bool>& target = key == "visible" ? pending.visible
                        : key == "interactable"                    ? pending.interactable
                        : key == "clip"                            ? pending.clip
                        : key == "landmark"                        ? pending.landmark
                                                                   : pending.opaque;
                    target = flag(value, node, key);
                }
                else if (key == "text")
                {
                    pending.text = text(value, node, key, UI_DOCUMENT_MAX_TEXT);
                }
                else if (key == "tooltip")
                {
                    pending.tooltip = text(value, node, key, UI_DOCUMENT_MAX_ID);
                    if (pending.tooltip && !isValidMessageKey(*pending.tooltip))
                    {
                        problem(node, std::format("tooltip '{}' is not a message key", *pending.tooltip), true);
                    }
                }
                else if (key == "textKey")
                {
                    pending.textKey = text(value, node, key, UI_DOCUMENT_MAX_ID);
                    if (pending.textKey && !isValidMessageKey(*pending.textKey))
                    {
                        problem(node, std::format("textKey '{}' is not a message key", *pending.textKey), true);
                    }
                }
                else if (key == "rich")
                {
                    pending.rich = flag(value, node, key);
                }
                else if (key == "icon")
                {
                    pending.icon = text(value, node, key, UI_DOCUMENT_MAX_ID);
                    if (pending.icon && !pending.icon->empty() && uiIconCodepoint(*pending.icon) == 0)
                    {
                        problem(node,
                                std::format("icon '{}' is not an icon name (closest: '{}')", *pending.icon,
                                            uiIconClosestName(*pending.icon)),
                                true);
                    }
                }
                else if (key == "checked")
                {
                    pending.checked = flag(value, node, key);
                }
                else if (key == "image")
                {
                    if (value.type != Scalar::Unsigned)
                    {
                        problem(node, "'image' must be an unsigned integer image slot", true);
                    }
                    else
                    {
                        pending.image = value.unsignedValue;
                    }
                }
                else if (key == "value" || key == "min" || key == "max" || key == "step")
                {
                    std::optional<double>& target = key == "value" ? pending.value
                        : key == "min"                             ? pending.min
                        : key == "max"                             ? pending.max
                                                                   : pending.step;
                    if (!value.isNumber() || !std::isfinite(value.number))
                    {
                        problem(node, std::format("'{}' must be a finite number", key), true);
                    }
                    else
                    {
                        target = value.number;
                    }
                }
                else if (known(Scope::Element, key))
                {
                    problem(node, std::format("'{}' has the wrong type", key), true);
                }
                else
                {
                    problem(node, std::format("unknown key '{}': ignored", key), false);
                }
            }

            void closeElement(const u32 index)
            {
                --m_elementDepth;
                Pending& pending = m_pending_elements[m_elementDepth];
                UiDocumentModel::Node& node = m_model.nodes[index];
                if (pending.id.empty())
                {
                    problem(index,
                            std::format("an element needs an 'id': 1 to {} bytes, no '/', '#' or control character",
                                        UI_DOCUMENT_MAX_ID),
                            true);
                    return;
                }
                node.id = InternedString(pending.id);
                if (pending.kind)
                {
                    const std::optional<UiWidgetKind> kind = kindFromName(*pending.kind);
                    if (!kind)
                    {
                        problem(index, std::format("unknown kind \"{}\"", *pending.kind), true);
                        return;
                    }
                    node.kind = *kind;
                }
                const UiWidgetKind kind = node.kind;
                node.opaque = !creatable(kind) || pending.opaque.value_or(false);
                const bool opaque = node.opaque;
                Props& props = node.props;
                props = defaultProps(kind);
                if (pending.styleClass && validName(*pending.styleClass))
                {
                    props.styleClass = InternedString(*pending.styleClass);
                }
                props.anchor = pending.vectors[0].value_or(props.anchor);
                props.offset = pending.vectors[1].value_or(props.offset);
                props.size = pending.vectors[2].value_or(props.size);
                props.pivot = pending.vectors[3].value_or(props.pivot);
                props.z = pending.z.value_or(props.z);
                props.layout = pending.layout;
                props.item = pending.item;
                props.fit = pending.fit;
                props.visible = pending.visible.value_or(props.visible);
                props.interactable = pending.interactable.value_or(props.interactable);
                props.clip = pending.clip.value_or(props.clip);
                props.landmark = pending.landmark.value_or(props.landmark);
                const auto applies = [&](const std::string_view key, const bool fits)
                {
                    if (!fits)
                    {
                        problem(index,
                                std::format("'{}' does not apply to {} {}: ignored", key, opaque ? "opaque" : "kind",
                                            kindName(kind)),
                                false);
                    }
                    return fits;
                };
                if (pending.vectors[4] && applies("contentSize", !opaque && kind == UiWidgetKind::ScrollView))
                {
                    props.contentSize = *pending.vectors[4];
                }
                if (pending.text &&
                    applies("text", !opaque && (isText(kind) || isCheckable(kind) || kind == UiWidgetKind::TextInput)))
                {
                    props.text = std::move(*pending.text);
                }
                if (pending.tooltip && isValidMessageKey(*pending.tooltip))
                {
                    props.tooltip = InternedString(*pending.tooltip);
                    if (m_localization && !m_localization->contains(props.tooltip))
                    {
                        problem(index,
                                std::format("tooltip '{}' is in no string table: it shows the key", *pending.tooltip),
                                false);
                    }
                }
                if (pending.textKey && applies("textKey", !opaque && isText(kind)) &&
                    isValidMessageKey(*pending.textKey))
                {
                    props.textKey = InternedString(*pending.textKey);
                    if (m_localization && !m_localization->contains(props.textKey))
                    {
                        problem(index,
                                std::format("textKey '{}' is in no string table: it shows the key", *pending.textKey),
                                false);
                    }
                }
                if (pending.textArgs && applies("textArgs", !opaque && isText(kind)))
                {
                    props.textArgs = std::move(*pending.textArgs);
                }
                if (pending.rich && applies("rich", !opaque && isText(kind)))
                {
                    props.rich = *pending.rich;
                }
                if (pending.icon && applies("icon", !opaque && isText(kind)))
                {
                    props.icon = std::move(*pending.icon);
                }
                if (pending.image && applies("image", !opaque && kind == UiWidgetKind::Image))
                {
                    props.image = *pending.image;
                }
                if (pending.checked && applies("checked", !opaque && isCheckable(kind)))
                {
                    props.checked = *pending.checked;
                }
                const bool ranged = !opaque && isRanged(kind);
                if (pending.value && applies("value", ranged))
                {
                    props.value = *pending.value;
                }
                if (pending.min && applies("min", ranged))
                {
                    props.min = *pending.min;
                }
                if (pending.max && applies("max", ranged))
                {
                    props.max = *pending.max;
                }
                if (pending.step &&
                    applies("step", !opaque && (kind == UiWidgetKind::Slider || kind == UiWidgetKind::NumberField)))
                {
                    props.step = *pending.step;
                }
                if (kind == UiWidgetKind::NumberField && !opaque &&
                    !(std::isfinite(props.min) && std::isfinite(props.max) && props.min <= props.max))
                {
                    problem(index, "numberField 'min' and 'max' must be finite with min <= max", true);
                }
                if (kind == UiWidgetKind::NumberField && !opaque && !(std::isfinite(props.step) && props.step > 0.0))
                {
                    problem(index, "numberField 'step' must be a positive finite number", true);
                }
                if (pending.bind && applies("bind", !opaque))
                {
                    props.bind = std::move(pending.bind);
                    if (!props.bind->source.empty() && m_contract.sourceOf(props.bind->source) == Entity::undefined())
                    {
                        problem(index,
                                std::format("bind source '{}' is not one the view's contract declares: the binding is "
                                            "not made",
                                            props.bind->source),
                                false);
                    }
                }
                if (pending.slot && applies("slot", !opaque && kind == UiWidgetKind::Panel))
                {
                    props.slot = pending.slot;
                    if (!node.children.empty())
                    {
                        problem(index, "a slot lists no children: code builds them", true);
                    }
                }
                if (props.textKey.isValid() && !props.text.empty())
                {
                    problem(index, "'text' is ignored beside 'textKey'", false);
                    props.text.clear();
                }
                if (!props.textArgs.empty() && !props.textKey.isValid())
                {
                    problem(index, "'textArgs' needs 'textKey'", true);
                }
                if (!node.children.empty() && (opaque || partsOnly(kind)))
                {
                    problem(index,
                            std::format("a {} lists no children: {}", kindName(kind),
                                        opaque ? "an opaque element keeps its own" : "its children are its parts"),
                            true);
                }
            }

            struct Deferred
            {
                u32 node;
                std::string message;
                bool error;
            };

            const UiViewContract& m_contract;
            const Localization* m_localization;
            UiDocumentModel& m_model;
            std::vector<UiDocumentProblem>& m_problems;
            std::vector<Deferred> m_pending; // problems of elements whose path is not known yet
            std::vector<Level> m_levels;
            struct KeySet
            {
                std::vector<std::string> list;
                std::unordered_set<std::string> hashed; // built once list passes LINEAR_KEYS
            };
            std::vector<KeySet> m_keys; // per JSON depth, reused
            u64 m_keyChecks = 0; // key comparisons and hash probes made by the duplicate check
            std::vector<Pending> m_pending_elements; // per open element depth, reused
            std::string m_key;
            std::string m_fatal;
            std::array<float, 4> m_floats{};
            std::array<InternedString, 4> m_floatTokens{}; // the "$name" entries of the array being read
            u32 m_floatCount = 0;
            bool m_floatBad = false;
            u32 m_depth = 0;
            u32 m_elementDepth = 0;
            bool m_sawVersion = false;
            bool m_failed = false;
        };

        // the element's id path inside the view (export and adopt)
        InternedString idOf(const World& world, const Entity element)
        {
            const auto* identity = world.tryGetConstComponent<UiIdentityComponent>(element);
            return identity ? identity->id : InternedString{};
        }

        // a code-built element a document can only keep and restyle
        bool opaqueElement(const World& world, const Entity element, const UiWidgetKind kind)
        {
            return !creatable(kind) || world.hasComponent<UiDockComponent>(element) ||
                world.hasComponent<UiMenuComponent>(element) || world.hasComponent<UiTableComponent>(element) ||
                world.hasComponent<UiVirtualListComponent>(element) ||
                world.hasComponent<UiOverlayComponent>(element) || world.hasComponent<UiCanvasComponent>(element) ||
                world.hasComponent<UiDockPanelComponent>(element);
        }

        bool isSlot(const World& world, const Entity element) { return world.hasComponent<UiSlotComponent>(element); }

        // the element children are listed under: a scroll view's content panel
        Entity childHost(const UiHierarchyManager& hierarchy, const Entity element, const UiWidgetKind kind)
        {
            if (kind != UiWidgetKind::ScrollView)
            {
                return element;
            }
            const World& world = hierarchy.world();
            const InternedString content(std::string(idOf(world, element).toView()) + ".content");
            for (const Entity child : hierarchy.childrenOf(element))
            {
                if (idOf(world, child) == content)
                {
                    return child;
                }
            }
            return element;
        }

        // A live element read back as document props (export, and the baseline when a document first meets a
        // code-built element).
        Props readProps(const UiHierarchyManager& hierarchy, const Entity element, const UiWidgetKind kind,
                        const bool opaque)
        {
            const World& world = hierarchy.world();
            Props props = defaultProps(kind);
            if (const auto* identity = world.tryGetConstComponent<UiIdentityComponent>(element))
            {
                props.styleClass = identity->styleClass;
            }
            if (const auto* rect = world.tryGetConstComponent<UiRectComponent>(element))
            {
                props.anchor = rect->anchor;
                props.offset = rect->offset;
                props.size = rect->size;
                props.pivot = rect->pivot;
                props.z = rect->zOffset;
            }
            if (const auto* layout = world.tryGetConstComponent<UiLayoutComponent>(element))
            {
                props.layout = LayoutSpec{layout->direction, layout->gap,   layout->padding,
                                          layout->justify,   layout->align, layout->wrap};
            }
            if (const auto* item = world.tryGetConstComponent<UiLayoutItemComponent>(element))
            {
                props.item =
                    ItemSpec{item->grow, item->shrink, item->basis, item->minSize, item->maxSize, item->alignSelf};
            }
            if (const auto* fit = world.tryGetConstComponent<UiContentSizeComponent>(element))
            {
                props.fit = FitSpec{fit->fitWidth, fit->fitHeight, fit->padding};
            }
            if (const auto* tokens = world.tryGetConstComponent<UiLayoutTokensComponent>(element))
            {
                // a bound field reads as the document's placeholder (the default): it compares by its token
                const auto read = [&](auto& spec)
                {
                    using Spec = std::remove_cvref_t<decltype(*spec)>;
                    Spec base;
                    for (sizet i = 0; spec && i < spec->tokens.size(); ++i)
                    {
                        spec->tokens[i] =
                            (*tokens)[static_cast<UiLayoutField>(static_cast<sizet>(Spec::FIRST_TOKEN) + i)];
                        if (spec->tokens[i].isValid())
                        {
                            *tokenFields(*spec)[i] = *tokenFields(base)[i];
                        }
                    }
                };
                read(props.layout);
                read(props.item);
                read(props.fit);
            }
            if (const auto* state = world.tryGetConstComponent<UiStateComponent>(element))
            {
                props.visible = state->visible;
                props.interactable = state->interactable;
                props.clip = state->clipsChildren;
                props.landmark = state->landmark;
            }
            if (const auto* tip = world.tryGetConstComponent<UiTooltipComponent>(element))
            {
                props.tooltip = tip->key;
            }
            if (opaque)
            {
                return props;
            }
            if (const auto* slot = world.tryGetConstComponent<UiSlotComponent>(element);
                slot && kind == UiWidgetKind::Panel)
            {
                props.slot = SlotSpec{slot->scroll, slot->childClass};
            }
            if (isText(kind))
            {
                if (const auto* localized = world.tryGetConstComponent<UiLocalizedTextComponent>(element))
                {
                    props.textKey = localized->key;
                    props.textArgs = localized->args;
                }
                else if (const auto* label = world.tryGetConstComponent<UiLabelComponent>(element))
                {
                    props.text = label->text;
                }
                if (const auto* label = world.tryGetConstComponent<UiLabelComponent>(element))
                {
                    props.rich = label->rich;
                    props.icon = label->icon;
                }
            }
            else if (kind == UiWidgetKind::TextInput)
            {
                if (const auto* value = world.tryGetConstComponent<UiValueComponent>(element))
                {
                    props.text = value->text;
                }
            }
            else if (kind == UiWidgetKind::NumberField)
            {
                if (const auto* numeric = world.tryGetConstComponent<UiNumericComponent>(element))
                {
                    props.value = numeric->value;
                    props.min = numeric->min;
                    props.max = numeric->max;
                    props.step = numeric->step;
                }
            }
            else if (isCheckable(kind))
            {
                if (const auto* toggle = world.tryGetConstComponent<UiToggleComponent>(element))
                {
                    props.checked = toggle->checked;
                }
                const InternedString labelId(std::string(idOf(world, element).toView()) + ".label");
                for (const Entity child : hierarchy.childrenOf(element))
                {
                    const auto* label = world.tryGetConstComponent<UiLabelComponent>(child);
                    if (label && idOf(world, child) == labelId)
                    {
                        props.text = label->text;
                    }
                }
            }
            else if (kind == UiWidgetKind::Slider)
            {
                if (const auto* slider = world.tryGetConstComponent<UiSliderComponent>(element))
                {
                    props.value = slider->value;
                    props.min = slider->min;
                    props.max = slider->max;
                    props.step = slider->step;
                }
            }
            else if (kind == UiWidgetKind::ProgressBar)
            {
                if (const auto* progress = world.tryGetConstComponent<UiProgressComponent>(element))
                {
                    props.value = progress->value;
                    props.min = progress->min;
                    props.max = progress->max;
                }
            }
            else if (kind == UiWidgetKind::Image)
            {
                if (const auto* image = world.tryGetConstComponent<UiImageComponent>(element))
                {
                    props.image = image->imageSlot;
                }
            }
            else if (kind == UiWidgetKind::ScrollView)
            {
                if (const auto* scroll = world.tryGetConstComponent<UiScrollComponent>(element))
                {
                    props.contentSize = scroll->contentSize;
                }
            }
            return props;
        }

        Json vec2Json(const Vec2 value) { return Json::array({value.x, value.y}); }

        // the props that differ from the kind's defaults
        void writeProps(Json& out, const Props& props, const UiWidgetKind kind)
        {
            const Props base = defaultProps(kind);
            if (props.styleClass.isValid())
            {
                out["class"] = props.styleClass.toView();
            }
            const auto vec = [&](const char* key, const Vec2 value, const Vec2 fallback)
            {
                if (!(value == fallback))
                {
                    out[key] = vec2Json(value);
                }
            };
            vec("anchor", props.anchor, base.anchor);
            vec("offset", props.offset, base.offset);
            vec("size", props.size, base.size);
            vec("pivot", props.pivot, base.pivot);
            if (props.z != 0)
            {
                out["z"] = props.z;
            }
            // a layout number bound to a token is written as its "$name"
            const auto number = [](const float value, const InternedString token)
            { return token.isValid() ? Json("$" + std::string(token.toView())) : Json(value); };
            if (props.layout)
            {
                const LayoutSpec& layout = *props.layout;
                const auto& t = layout.tokens;
                Json& json = out["layout"] = Json::object();
                json["direction"] = DIRECTIONS[static_cast<u8>(layout.direction)];
                json["gap"] = number(layout.gap, t[0]);
                json["padding"] = Json::array({number(layout.padding.x, t[1]), number(layout.padding.y, t[2]),
                                               number(layout.padding.z, t[3]), number(layout.padding.w, t[4])});
                json["justify"] = JUSTIFIES[static_cast<u8>(layout.justify)];
                json["align"] = ALIGNS[static_cast<u8>(layout.align)];
                json["wrap"] = layout.wrap;
            }
            if (props.item)
            {
                const ItemSpec& item = *props.item;
                const auto& t = item.tokens;
                Json& json = out["item"] = Json::object();
                json["grow"] = item.grow;
                json["shrink"] = item.shrink;
                json["basis"] = number(item.basis, t[0]);
                json["min"] = Json::array({number(item.minSize.x, t[1]), number(item.minSize.y, t[2])});
                json["max"] = Json::array({number(item.maxSize.x, t[3]), number(item.maxSize.y, t[4])});
                json["alignSelf"] = ALIGNS[static_cast<u8>(item.alignSelf)];
            }
            if (props.fit)
            {
                const auto& t = props.fit->tokens;
                out["fit"] = Json{
                    {"width", props.fit->width},
                    {"height", props.fit->height},
                    {"padding", Json::array({number(props.fit->padding.x, t[0]), number(props.fit->padding.y, t[1])})}};
            }
            if (!props.visible)
            {
                out["visible"] = false;
            }
            if (!props.interactable)
            {
                out["interactable"] = false;
            }
            if (props.clip)
            {
                out["clip"] = true;
            }
            if (props.landmark)
            {
                out["landmark"] = true;
            }
            if (props.tooltip.isValid())
            {
                out["tooltip"] = props.tooltip.toView();
            }
            if (props.textKey.isValid())
            {
                out["textKey"] = props.textKey.toView();
                if (!props.textArgs.empty())
                {
                    Json& args = out["textArgs"] = Json::object();
                    for (sizet begin = 0; begin < props.textArgs.size();)
                    {
                        const sizet end = std::min(props.textArgs.find('\n', begin), props.textArgs.size());
                        const std::string_view line(props.textArgs.data() + begin, end - begin);
                        begin = end + 1;
                        const sizet equals = line.find('=');
                        if (equals == std::string_view::npos)
                        {
                            continue;
                        }
                        const std::string name(line.substr(0, equals));
                        const std::string_view value = line.substr(equals + 1);
                        std::int64_t integer = 0;
                        double real = 0.0;
                        if (value.starts_with('\''))
                        {
                            args[name] = value.substr(1);
                        }
                        else if (const auto parsed =
                                     std::from_chars(value.data(), value.data() + value.size(), integer);
                                 parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size())
                        {
                            args[name] = integer;
                        }
                        else if (const auto parsedReal =
                                     std::from_chars(value.data(), value.data() + value.size(), real);
                                 parsedReal.ec == std::errc{} && parsedReal.ptr == value.data() + value.size())
                        {
                            args[name] = real;
                        }
                        else
                        {
                            args[name] = value;
                        }
                    }
                }
            }
            else if (!props.text.empty())
            {
                out["text"] = props.text;
            }
            if (props.rich)
            {
                out["rich"] = true;
            }
            if (!props.icon.empty())
            {
                out["icon"] = props.icon;
            }
            if (props.image != 0)
            {
                out["image"] = props.image;
            }
            if (props.checked)
            {
                out["checked"] = true;
            }
            if (isRanged(kind))
            {
                const auto number = [&](const char* key, const double value, const double fallback)
                {
                    if (value != fallback)
                    {
                        out[key] = value;
                    }
                };
                number("value", props.value, base.value);
                number("min", props.min, base.min);
                number("max", props.max, base.max);
                number("step", props.step, base.step);
            }
            vec("contentSize", props.contentSize, base.contentSize);
            if (props.slot)
            {
                Json& slot = out["slot"] = Json::object();
                slot["scroll"] = props.slot->scroll;
                if (props.slot->childClass.isValid())
                {
                    slot["childClass"] = props.slot->childClass.toView();
                }
            }
        }

        // what an export must stay within to be importable again; the first breach is kept
        struct ExportLimits
        {
            u32 nodes = 0;
            std::string problem;
        };

        void exportElement(const UiHierarchyManager& hierarchy, const Entity element, Json& out, ExportLimits& limits,
                           const u32 depth, const sizet pathLength)
        {
            const World& world = hierarchy.world();
            const auto& identity = world.getConstComponent<UiIdentityComponent>(element);
            const std::string_view id = identity.id.toView();
            const sizet length = pathLength + (depth ? 1 : 0) + id.size();
            if (!limits.problem.empty())
            {
                return;
            }
            if (!validName(id))
            {
                limits.problem = std::format("element id '{}' cannot be written: ids are 1 to {} bytes with no '/', "
                                             "'#' or control character",
                                             id, UI_DOCUMENT_MAX_ID);
                return;
            }
            if (++limits.nodes > UI_DOCUMENT_MAX_NODES || depth >= UI_DOCUMENT_MAX_DEPTH ||
                length > UI_DOCUMENT_MAX_PATH)
            {
                limits.problem = std::format("the tree exceeds the document limits ({} elements, {} deep, {}-byte id "
                                             "paths)",
                                             UI_DOCUMENT_MAX_NODES, UI_DOCUMENT_MAX_DEPTH, UI_DOCUMENT_MAX_PATH);
                return;
            }
            const bool opaque = opaqueElement(world, element, identity.kind);
            out["id"] = identity.id.toView();
            out["kind"] = kindName(identity.kind);
            if (opaque && creatable(identity.kind))
            {
                out["opaque"] = true;
            }
            writeProps(out, readProps(hierarchy, element, identity.kind, opaque), identity.kind);
            if (opaque || partsOnly(identity.kind) || isSlot(world, element))
            {
                return; // a slot's children are code's: a document never lists them
            }
            Json children = Json::array();
            for (const Entity child : hierarchy.childrenOf(childHost(hierarchy, element, identity.kind)))
            {
                if (idOf(world, child).isValid())
                {
                    exportElement(hierarchy, child, children.emplace_back(Json::object()), limits, depth + 1,
                                  length + 11);
                }
            }
            if (!children.empty())
            {
                out["children"] = std::move(children);
            }
        }

        double millisecondsSince(const Clock::time_point start)
        {
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        }

        UiDocumentStats& freshStats(UiDocumentStats& stats)
        {
            const u64 applies = stats.applies;
            const u64 refused = stats.refused;
            const u64 keyChecks = stats.keyChecks;
            stats = {};
            stats.applies = applies;
            stats.refused = refused;
            stats.keyChecks = keyChecks;
            return stats;
        }
    } // namespace

    // -- UiViewContract ---------------------------------------------------------

    UiViewContract& UiViewContract::require(const InternedString id, const UiWidgetKind kind, AttachFn attach)
    {
        m_elements.push_back({id, kind, true, false, std::move(attach)});
        return *this;
    }

    UiViewContract& UiViewContract::optional(const InternedString id, const UiWidgetKind kind, AttachFn attach)
    {
        m_elements.push_back({id, kind, false, false, std::move(attach)});
        return *this;
    }

    UiViewContract& UiViewContract::slot(const InternedString id, AttachFn fill)
    {
        m_elements.push_back({id, UiWidgetKind::Panel, true, true, std::move(fill)});
        return *this;
    }

    UiViewContract& UiViewContract::source(const std::string_view name, const Entity entity)
    {
        m_sources.emplace_back(std::string(name), entity);
        return *this;
    }

    UiViewContract& UiViewContract::fallback(BuildFn build)
    {
        m_fallback = std::move(build);
        return *this;
    }

    const UiViewContract::Element* UiViewContract::find(const InternedString id) const
    {
        const auto it = std::ranges::find(m_elements, id, &Element::id);
        return it == m_elements.end() ? nullptr : &*it;
    }

    Entity UiViewContract::sourceOf(const std::string_view name) const
    {
        const auto it = std::ranges::find(m_sources, name, &std::pair<std::string, Entity>::first);
        return it == m_sources.end() ? Entity::undefined() : it->second;
    }

    u32 UiViewContract::countCallbacksIn(const ImageRange& image) const
    {
        u32 count = m_fallback && image.contains(m_fallback) ? 1 : 0;
        for (const Element& element : m_elements)
        {
            count += element.attach && image.contains(element.attach) ? 1 : 0;
        }
        return count;
    }

    // -- UiDocumentView ---------------------------------------------------------

    struct UiDocumentView::Live
    {
        Entity entity = Entity::undefined();
        Entity host = Entity::undefined(); // where its document children go (a scroll view's content)
        UiWidgetKind kind = UiWidgetKind::Panel;
        bool owned = false; // created by this view's scope: removable
        u32 node = NONE; // in the applied model
        u32 binding = 0;
        u64 seen = 0; // the apply that last met it
    };

    UiDocumentView::UiDocumentView(UiContext& context, std::string name, UiViewContract contract, const Entity parent) :
        m_context(&context), m_name(std::move(name)), m_contract(std::move(contract)), m_parent(parent),
        m_scope(std::make_unique<UiScope>(context))
    {
        if (context.documents().find(m_name))
        {
            SWARNING_LOG("UI document view '{}': a view of that name exists; find() answers the first", m_name);
        }
        context.documents().m_views.push_back(this);
    }

    UiDocumentView::UiDocumentView(UiContext& context, const Entity root) :
        m_context(&context), m_name(std::format("@{}", root.id())), m_parent(context.hierarchy().parentOf(root)),
        m_root(root), m_adopted(true), m_scope(std::make_unique<UiScope>(context))
    {
        context.documents().m_views.push_back(this);
    }

    UiDocumentView::~UiDocumentView()
    {
        *m_alive = false;
        if (m_context)
        {
            std::erase(m_context->documents().m_views, this);
        }
    }

    Widget UiDocumentView::element(const InternedString id) const
    {
        const auto it = m_bound.find(id);
        if (it == m_bound.end() || !m_context)
        {
            return {};
        }
        UiScope* owner = m_context->ownerOf(it->second);
        return owner ? Widget(owner, it->second, owner->generation()) : Widget{};
    }

    std::string UiDocumentView::exportText() const
    {
        return rootAlive() ? exportUiDocument(*m_context, m_root) : std::string{};
    }

    bool UiDocumentView::rootAlive() const
    {
        return m_context && m_context->hierarchy().isElement(m_root) &&
            m_context->hierarchy().world().isEntityAlive(m_root);
    }

    bool UiDocumentView::load(const std::filesystem::path& path)
    {
        m_path = path;
        m_watch = true;
        m_pending = false;
        std::error_code error;
        if (m_entry.path() != path)
        {
            m_entry.assign(path, error);
            error.clear();
        }
        m_stamp = std::filesystem::last_write_time(path, error);
        m_size = std::filesystem::file_size(path, error);
        if (error || m_size > UI_DOCUMENT_MAX_BYTES)
        {
            refuse({{path.generic_string(),
                     error ? "cannot read the file: " + error.message()
                           : std::format("the file is larger than {} bytes", UI_DOCUMENT_MAX_BYTES),
                     true}});
            return false;
        }
        std::string text;
        std::string why;
        if (!readUiDocumentFile(path, text, why))
        {
            refuse({{path.generic_string(), std::move(why), true}});
            return false;
        }
        return apply(text);
    }

    bool readUiDocumentFile(const std::filesystem::path& path, std::string& text, std::string& error)
    {
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(path, ec);
        std::ifstream file(path, std::ios::binary);
        if (ec || !file)
        {
            error = "cannot read the file";
            return false;
        }
        // a file that grew since the size was taken is still read no further than the limit
        text.resize(static_cast<sizet>(std::min<std::uintmax_t>(size, UI_DOCUMENT_MAX_BYTES)) + 1);
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<sizet>(file.gcount()));
        if (text.size() > UI_DOCUMENT_MAX_BYTES)
        {
            error = std::format("the file is larger than {} bytes", UI_DOCUMENT_MAX_BYTES);
            return false;
        }
        return true;
    }

    Expected<UiComposition> readUiComposition(const std::filesystem::path& path)
    {
        const auto fail = [](std::string message) -> Expected<UiComposition>
        { return std::unexpected(makeError(ErrorCode::JsonParseFailed, std::move(message))); };
        std::string text, why;
        if (!readUiDocumentFile(path, text, why))
        {
            return fail(why);
        }
        try
        {
            const Json spec = Json::parse(text);
            const auto asset = [](const std::string& name)
            {
                const auto file = pathFromUtf8(name);
                return name.starts_with("assets/ui/") && name.find('\\') == std::string::npos &&
                    file.extension() == ".json" &&
                    std::ranges::none_of(file,
                                         [](const auto& component) { return component == ".." || component == "."; });
            };
            const auto size = spec.at("screen").get<std::array<float, 2>>();
            const Json& views = spec.at("views");
            if (spec.at("version") != 1 || !views.is_array() || views.empty() || views.size() > 32 ||
                !std::isfinite(size[0]) || !std::isfinite(size[1]) || size[0] <= 0 || size[1] <= 0)
            {
                return fail("invalid UI composition version, artboard or views");
            }
            UiComposition result;
            result.name = spec.at("name").get<std::string>();
            result.baseStyle = spec.value("baseStyle", std::string("assets/ui/engine.json"));
            result.theme = spec.value("theme", std::string{});
            result.style = spec.value("style", std::string{});
            result.screen = {size[0], size[1]};
            if (result.name.empty() || !asset(result.baseStyle) || (!result.theme.empty() && !asset(result.theme)) ||
                (!result.style.empty() && !asset(result.style)))
            {
                return fail("invalid UI composition name or stylesheet path");
            }
            std::unordered_set<std::string> names, paths;
            for (const Json& entry : views)
            {
                UiCompositionView view;
                view.name = entry.at("name").get<std::string>();
                view.path = entry.at("path").get<std::string>();
                view.fillWidth = entry.value("fillWidth", false);
                view.previewVisible = entry.value("previewVisible", true);
                view.required = entry.value("required", std::vector<std::string>{});
                std::unordered_set<std::string_view> required;
                for (const auto& id : view.required)
                {
                    if (id.empty() || id.size() > UI_DOCUMENT_MAX_ID || id.find_first_of("/#") != std::string::npos ||
                        !required.insert(id).second)
                    {
                        return fail("invalid or duplicate required widget id");
                    }
                }
                if (entry.contains("parent"))
                {
                    const auto& parent = entry.at("parent");
                    view.parent = parent.at("view").get<std::string>();
                    view.slot = parent.at("slot").get<std::string>();
                    if (!names.contains(view.parent) || view.slot.empty())
                    {
                        return fail("composition mounts must name a slot in a preceding view");
                    }
                }
                if (view.name.empty() || !names.insert(view.name).second || !asset(view.path) ||
                    !paths.insert(view.path).second)
                {
                    return fail("invalid or duplicate composition view");
                }
                result.views.push_back(std::move(view));
            }
            const auto actions = spec.value("actions", Json::array());
            if (!actions.is_array() || actions.size() > UI_DOCUMENT_MAX_NODES)
            {
                return fail("invalid composition actions");
            }
            std::unordered_set<std::string> bindings;
            for (const Json& entry : actions)
            {
                UiCompositionAction action{entry.at("view").get<std::string>(),
                                           entry.at("id").get<std::string>(),
                                           entry.at("event").get<std::string>(),
                                           entry.at("command").get<std::string>(),
                                           {}};
                const auto args = entry.value("args", Json::object());
                if (!names.contains(action.view) || action.id.empty() || action.event.empty() ||
                    action.command.empty() || !args.is_object() || args.contains("world") ||
                    !bindings.insert(Json::array({action.view, action.id, action.event}).dump()).second)
                {
                    return fail("invalid or duplicate composition action");
                }
                action.args = args.dump();
                result.actions.push_back(std::move(action));
            }
            return result;
        }
        catch (const Json::exception& error)
        {
            return fail(error.what());
        }
    }

    std::vector<std::string> UiComposition::protectedIds(const std::string_view view) const
    {
        std::vector<std::string> ids;
        for (const auto& entry : views)
        {
            if (entry.name == view)
            {
                ids.insert(ids.end(), entry.required.begin(), entry.required.end());
            }
            if (entry.parent == view)
            {
                ids.push_back(entry.slot);
            }
        }
        for (const auto& action : actions)
        {
            if (action.view == view)
            {
                ids.push_back(action.id);
            }
        }
        std::ranges::sort(ids);
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        return ids;
    }

    bool UiDocumentView::poll()
    {
        if (!m_watch || m_path.empty() || m_applying)
        {
            return false;
        }
        const Clock::time_point now = Clock::now();
        if (now - m_lastPoll < WATCH_INTERVAL)
        {
            return false;
        }
        m_lastPoll = now;
        std::error_code error;
        // one stat for both (the entry caches what the lookup returned); refreshing the entry load made keeps a poll
        // free of allocations (a quiescent editor allocates nothing per frame)
        m_entry.refresh(error);
        const auto stamp = error ? std::filesystem::file_time_type{} : m_entry.last_write_time(error);
        const u64 size = error ? 0 : m_entry.file_size(error);
        if (error)
        {
            return false;
        }
        if (stamp != m_stamp || size != m_size)
        {
            // still being written, maybe: apply once it held for an interval
            m_stamp = stamp;
            m_size = size;
            m_pending = true;
            return false;
        }
        if (!m_pending)
        {
            return false;
        }
        m_pending = false;
        const std::filesystem::path path = m_path;
        return load(path);
    }

    bool UiDocumentView::apply(const std::string_view text)
    {
        if (!m_context)
        {
            return false;
        }
        if (m_applying)
        {
            refuse(
                {{"", "a document applies while another is being applied (from a contract callback): refused", true}});
            return false;
        }
        if (m_adopted && !rootAlive())
        {
            refuse({{"", "the element this view adopted no longer exists", true}});
            return false;
        }
        const Clock::time_point start = Clock::now();
        std::vector<UiDocumentProblem> problems;
        if (text.size() > UI_DOCUMENT_MAX_BYTES)
        {
            refuse({{"", std::format("the document is larger than {} bytes", UI_DOCUMENT_MAX_BYTES), true}});
            return false;
        }
        auto model = std::make_unique<UiDocumentModel>();
        model->nodes.reserve(m_model ? m_model->nodes.size() : 16);
        Reader reader{m_contract, m_context->hierarchy().localization(), *model, problems};
        const bool parsed = Json::sax_parse(text, &reader);
        m_stats.keyChecks = reader.keyChecks();
        if (!parsed || !reader.fatal().empty())
        {
            refuse({{"", reader.fatal().empty() ? std::string("not a JSON document") : reader.fatal(), true}});
            return false;
        }
        reader.finish();
        if (!reader.failed())
        {
            reader.checkContract();
        }
        if (!reader.failed() && m_adopted && !model->nodes.empty() &&
            model->nodes[0].id != idOf(m_context->hierarchy().world(), m_root))
        {
            reader.report(model->nodes[0].path,
                          std::format("the root's id must be the adopted element's, '{}'",
                                      idOf(m_context->hierarchy().world(), m_root).toView()),
                          true);
        }
        // a live slot under the root stays one whichever view applies (an adopted one, a document that does not say
        // "slot"): a document listing children under its id is refused
        if (const UiHierarchyManager& hierarchy = m_context->hierarchy();
            !reader.failed() && hierarchy.isElement(m_root))
        {
            const World& world = hierarchy.world();
            std::unordered_set<InternedString> slots;
            for (const Chunk* chunk : world.getChunksContaining(Aspect(getComponentId<UiSlotComponent>())))
            {
                for (const Entity entity : chunk->entities())
                {
                    for (Entity at = entity; at != Entity::undefined(); at = hierarchy.parentOf(at))
                    {
                        if (at == m_root)
                        {
                            slots.insert(idOf(world, entity));
                            break;
                        }
                    }
                }
            }
            for (const UiDocumentModel::Node& node : model->nodes)
            {
                if (!node.props.slot && !node.children.empty() && slots.contains(node.id))
                {
                    reader.report(
                        node.path,
                        std::format("'{}' is a slot: it lists no children (code builds them)", node.id.toView()), true);
                }
            }
        }
        if (reader.failed())
        {
            refuse(std::move(problems));
            return false;
        }
        return applyModel(std::move(model), std::string(text), millisecondsSince(start), std::move(problems));
    }

    void UiDocumentView::refuse(std::vector<UiDocumentProblem> problems)
    {
        ++m_stats.refused;
        m_problems = std::move(problems);
        if (!m_context || m_context->hierarchy().isElement(m_root) || !m_contract.fallbackBuild())
        {
            return;
        }
        std::vector<UiDocumentProblem> kept = std::move(m_problems);
        buildFallback();
        kept.push_back({"", "no document applied: the view's fallback build made the tree", false});
        m_problems = std::move(kept);
    }

    bool UiDocumentView::buildFallback()
    {
        if (!m_context || !m_contract.fallbackBuild() || m_applying)
        {
            return false;
        }
        m_applying = true;
        // everything this view built goes (a fresh scope); the fallback builds afresh
        m_scope->clear(); // the scope object stays: scope() references and its handles' owner pointers remain valid
        m_live.clear();
        m_placed.clear();
        m_bound.clear();
        m_slotFields.clear();
        m_model.reset();
        m_text.clear();
        m_fallbackTree = true;
        Panel root;
        if (m_adopted)
        {
            UiScope* owner = m_context->ownerOf(m_root);
            root = owner ? Panel(owner, m_root, owner->generation()) : Panel{};
        }
        else
        {
            root = m_scope->createPanel(InternedString(m_name.empty() ? std::string("view") : m_name));
            if (m_context->hierarchy().isElement(m_parent))
            {
                m_context->hierarchy().attach(root.entity(), m_parent);
            }
            m_root = root.entity();
        }
        const auto alive = m_alive;
        const UiViewContract::BuildFn build = m_contract.fallbackBuild();
        build(*m_scope, root);
        if (!*alive)
        {
            return true;
        }
        m_applying = false;
        m_problems.clear();
        std::vector<std::pair<UiViewContract::AttachFn, Widget>> attach;
        resolveContract(attach);
        runAttach(attach);
        return true;
    }

    bool UiDocumentView::applyModel(std::unique_ptr<UiDocumentModel> model, std::string text, const double parseMs,
                                    std::vector<UiDocumentProblem> problems)
    {
        m_applying = true;
        const Clock::time_point start = Clock::now();
        UiHierarchyManager& hierarchy = m_context->hierarchy();
        World& world = hierarchy.world();
        UiDocumentStats& stats = freshStats(m_stats);
        ++stats.applies;
        stats.parseMs = parseMs;
        stats.nodes = static_cast<u32>(model->nodes.size());
        const u64 epoch = stats.applies;
        const auto warn = [&](const std::string& path, std::string message)
        {
            if (problems.size() < UI_DOCUMENT_REPORT_CAP)
            {
                problems.push_back({path, std::move(message), false});
            }
        };
        const auto handle = [&](const Entity entity) -> Widget
        {
            if (m_scope->owns(entity))
            {
                return Widget(m_scope.get(), entity, m_scope->generation());
            }
            UiScope* owner = m_context->ownerOf(entity);
            return owner ? Widget(owner, entity, owner->generation()) : Widget{};
        };

        // live elements not met yet under a host, by id: built once per host the first time a node misses there
        // (siblings sharing an id in child order, met in document order)
        struct Unmet
        {
            std::vector<Entity> entities;
            sizet next = 0; // the next one to hand out
        };
        std::unordered_map<u64, std::unordered_map<InternedString, Unmet>> untracked;
        std::unordered_set<u64> tracked;
        const auto untrackedChild = [&](const Entity host, const InternedString id) -> Entity
        {
            auto [it, made] = untracked.try_emplace(host.id());
            if (made)
            {
                if (tracked.empty())
                {
                    for (const auto& [path, live] : m_live)
                    {
                        tracked.insert(live->entity.id());
                    }
                }
                for (const Entity child : hierarchy.childrenOf(host))
                {
                    const InternedString childId = idOf(world, child);
                    if (childId.isValid() && !tracked.contains(child.id()))
                    {
                        it->second[childId].entities.push_back(child);
                    }
                }
            }
            const auto found = it->second.find(id);
            if (found == it->second.end() || found->second.next >= found->second.entities.size())
            {
                return Entity::undefined();
            }
            return found->second.entities[found->second.next++];
        };

        const auto create = [&](const UiDocumentModel::Node& node) -> Entity
        {
            UiScope& scope = *m_scope;
            switch (node.kind)
            {
            case UiWidgetKind::Panel:
                return scope.createPanel(node.id).entity();
            case UiWidgetKind::Label:
                return scope.createLabel(node.id, {}).entity();
            case UiWidgetKind::Button:
                return scope.createButton(node.id, {}).entity();
            case UiWidgetKind::Image:
                return scope.createImage(node.id).entity();
            case UiWidgetKind::TextInput:
                return scope.createTextInput(node.id).entity();
            case UiWidgetKind::NumberField:
                return scope.createNumberField(node.id).entity();
            case UiWidgetKind::Checkbox:
                return scope.createCheckbox(node.id, {}).entity();
            case UiWidgetKind::Toggle:
                return scope.createToggle(node.id, {}).entity();
            case UiWidgetKind::Slider:
                return scope.createSlider(node.id).entity();
            case UiWidgetKind::ProgressBar:
                return scope.createProgressBar(node.id).entity();
            case UiWidgetKind::ScrollView:
                return scope.createScrollView(node.id, {}).entity();
            default:
                return Entity::undefined();
            }
        };

        // writes the fields that differ; how many
        const auto write = [&](Live& live, const Props& from, const Props& to, const std::string& path) -> u32
        {
            const Entity entity = live.entity;
            const Widget widget = handle(entity);
            if (!widget.isValid())
            {
                return 0;
            }
            u32 written = 0;
            const auto changed = [&](const bool differs)
            {
                written += differs ? 1 : 0;
                return differs;
            };
            Widget w = widget;
            if (changed(from.styleClass != to.styleClass))
            {
                w.setStyleClass(to.styleClass);
            }
            if (changed(!(from.anchor == to.anchor)))
            {
                w.setAnchor(to.anchor);
            }
            if (changed(!(from.offset == to.offset)))
            {
                w.setOffset(to.offset);
            }
            if (changed(!(from.size == to.size)))
            {
                w.setSize(to.size);
            }
            if (changed(!(from.pivot == to.pivot)))
            {
                w.setPivot(to.pivot);
            }
            if (changed(from.z != to.z))
            {
                w.setZOffset(to.z);
            }
            // the token bindings of the blocks written, first: setLayout and setLayoutItem apply them
            const bool layoutWritten = from.layout != to.layout || from.item != to.item || from.fit != to.fit;
            if (layoutWritten)
            {
                const auto* bound = world.tryGetConstComponent<UiLayoutTokensComponent>(entity);
                UiLayoutTokensComponent tokens = bound ? *bound : UiLayoutTokensComponent{};
                const auto copy = [&](const auto& before, const auto& after)
                {
                    using Spec = std::remove_cvref_t<decltype(*after)>;
                    if (before == after)
                    {
                        return;
                    }
                    for (sizet i = 0; i < Spec{}.tokens.size(); ++i)
                    {
                        tokens[static_cast<UiLayoutField>(static_cast<sizet>(Spec::FIRST_TOKEN) + i)] =
                            after ? after->tokens[i] : InternedString{};
                    }
                };
                copy(from.layout, to.layout);
                copy(from.item, to.item);
                copy(from.fit, to.fit);
                if (tokens.empty() && bound)
                {
                    world.removeComponent<UiLayoutTokensComponent>(entity);
                }
                else if (!tokens.empty() && bound)
                {
                    *world.tryGetComponent<UiLayoutTokensComponent>(entity) = tokens;
                }
                else if (!tokens.empty())
                {
                    world.addComponent<UiLayoutTokensComponent>(entity, tokens);
                }
            }
            if (changed(from.layout != to.layout))
            {
                if (to.layout)
                {
                    const LayoutSpec& spec = *to.layout;
                    UiLayoutComponent layout;
                    layout.direction = spec.direction;
                    layout.gap = spec.gap;
                    layout.padding = spec.padding;
                    layout.justify = spec.justify;
                    layout.align = spec.align;
                    layout.wrap = spec.wrap;
                    w.setLayout(layout);
                }
                else
                {
                    world.removeComponent<UiLayoutComponent>(entity);
                    hierarchy.markLayoutDirty(entity);
                }
            }
            if (changed(from.item != to.item))
            {
                if (to.item)
                {
                    const ItemSpec& spec = *to.item;
                    UiLayoutItemComponent item;
                    item.grow = spec.grow;
                    item.shrink = spec.shrink;
                    item.basis = spec.basis;
                    item.minSize = spec.minSize;
                    item.maxSize = spec.maxSize;
                    item.alignSelf = spec.alignSelf;
                    w.setLayoutItem(item);
                }
                else
                {
                    world.removeComponent<UiLayoutItemComponent>(entity);
                    hierarchy.markLayoutDirty(entity);
                }
            }
            if (changed(from.fit != to.fit))
            {
                if (to.fit)
                {
                    UiContentSizeComponent fit;
                    fit.fitWidth = to.fit->width;
                    fit.fitHeight = to.fit->height;
                    fit.padding = to.fit->padding;
                    if (auto* existing = world.tryGetComponent<UiContentSizeComponent>(entity))
                    {
                        *existing = fit;
                        hierarchy.markLayoutDirty(entity);
                    }
                    else
                    {
                        world.addComponent<UiContentSizeComponent>(entity, fit);
                        hierarchy.markLayoutDirty(entity);
                    }
                }
                else
                {
                    world.removeComponent<UiContentSizeComponent>(entity);
                    hierarchy.markLayoutDirty(entity);
                }
            }
            if (layoutWritten)
            {
                m_context->applyLayoutTokens(entity);
            }
            if (changed(from.visible != to.visible))
            {
                w.setVisible(to.visible);
            }
            if (changed(from.interactable != to.interactable))
            {
                w.setInteractable(to.interactable);
            }
            if (changed(from.clip != to.clip))
            {
                world.getComponent<UiStateComponent>(entity).clipsChildren = to.clip;
                hierarchy.markLayoutDirty(entity);
            }
            if (changed(from.landmark != to.landmark))
            {
                w.setLandmark(to.landmark);
            }
            if (changed(from.tooltip != to.tooltip))
            {
                w.setTooltip(to.tooltip);
            }
            const UiWidgetKind kind = live.kind;
            if (isText(kind))
            {
                if (changed(from.textKey != to.textKey || from.textArgs != to.textArgs || from.text != to.text))
                {
                    if (to.textKey.isValid())
                    {
                        w.as<TextWidget>().setLocalizedText(to.textKey);
                        if (auto* localized = world.tryGetComponent<UiLocalizedTextComponent>(entity);
                            localized && localized->args != to.textArgs)
                        {
                            localized->args = to.textArgs;
                            hierarchy.refreshLocalized(entity);
                        }
                    }
                    else if (kind == UiWidgetKind::Label)
                    {
                        w.as<Label>().setText(to.text);
                    }
                    else
                    {
                        w.as<Button>().setText(to.text);
                    }
                }
                if (changed(from.rich != to.rich))
                {
                    w.as<TextWidget>().setRichText(to.rich);
                }
                if (changed(from.icon != to.icon))
                {
                    w.as<TextWidget>().setIcon(to.icon);
                }
            }
            else if (kind == UiWidgetKind::TextInput)
            {
                if (changed(from.text != to.text))
                {
                    w.as<TextInput>().setText(to.text);
                }
            }
            else if (isCheckable(kind))
            {
                if (changed(from.text != to.text))
                {
                    w.as<Checkable>().label().setText(to.text);
                }
                if (changed(from.checked != to.checked))
                {
                    w.as<Checkable>().setChecked(to.checked);
                }
            }
            else if (kind == UiWidgetKind::Slider)
            {
                if (changed(from.min != to.min || from.max != to.max || from.step != to.step))
                {
                    w.as<Slider>().setRange(to.min, to.max, to.step);
                }
                if (changed(from.value != to.value))
                {
                    w.as<Slider>().setValue(to.value);
                }
            }
            else if (kind == UiWidgetKind::ProgressBar)
            {
                if (changed(from.min != to.min || from.max != to.max))
                {
                    w.as<ProgressBar>().setRange(to.min, to.max);
                }
                if (changed(from.value != to.value))
                {
                    w.as<ProgressBar>().setValue(to.value);
                }
            }
            else if (kind == UiWidgetKind::NumberField)
            {
                if (changed(from.min != to.min || from.max != to.max || from.step != to.step || from.value != to.value))
                {
                    auto& numeric = world.getComponent<UiNumericComponent>(entity);
                    numeric.min = to.min;
                    numeric.max = to.max;
                    numeric.step = to.step;
                    w.as<NumberField>().setNumber(to.value);
                }
            }
            else if (kind == UiWidgetKind::Image)
            {
                if (changed(from.image != to.image))
                {
                    w.as<Image>().setImageSlot(to.image);
                }
            }
            else if (kind == UiWidgetKind::ScrollView)
            {
                if (changed(!(from.contentSize == to.contentSize)))
                {
                    w.as<ScrollView>().setContentSize(to.contentSize);
                }
            }
            if (changed(from.bind != to.bind))
            {
                if (live.binding != 0)
                {
                    widget.scope()->unbind(live.binding);
                    live.binding = 0;
                }
                if (to.bind)
                {
                    const BindSpec& spec = *to.bind;
                    const Entity source = m_contract.sourceOf(spec.source);
                    if (source != Entity::undefined())
                    {
                        UiBindingDesc desc;
                        desc.source = source;
                        desc.component = spec.component;
                        desc.field = spec.field;
                        desc.target = spec.target;
                        desc.format = spec.format;
                        desc.argument = spec.argument;
                        if (const Expected<u32> bound = widget.scope()->bind(widget, desc))
                        {
                            live.binding = *bound;
                        }
                        else
                        {
                            warn(path, "binding refused: " + std::string(bound.error().message));
                        }
                    }
                }
            }
            // never removed: a slot stays one, and a document without "slot" (another view's) keeps its fields
            if (to.slot && changed(from.slot != to.slot))
            {
                UiSlotComponent slot;
                slot.scroll = to.slot->scroll;
                slot.childClass = to.slot->childClass;
                if (auto* existing = world.tryGetComponent<UiSlotComponent>(entity))
                {
                    *existing = slot;
                }
                else
                {
                    world.addComponent<UiSlotComponent>(entity, slot);
                }
            }
            return written;
        };

        const UiDocumentModel* previous = m_model.get();
        std::vector<Live*> placed(model->nodes.size(), nullptr);
        std::vector<u8> orderDirty(model->nodes.size(), 0);
        // replaced elements, destroyed once every element moved out of them (their children may live on)
        std::vector<Entity> doomed;
        const auto otherViewRoot = [&](const Entity entity)
        {
            return entity != m_root &&
                std::ranges::any_of(m_context->documents().views(),
                                    [&](const UiDocumentView* view) { return view != this && view->m_root == entity; });
        };
        // the view's slot of that id not placed yet in this apply, wherever it is in this view's tree (a slot's
        // children and another document view's tree are not searched)
        const auto findSlot = [&](const InternedString id) -> Entity
        {
            if (!hierarchy.isElement(m_root))
            {
                return Entity::undefined();
            }
            for (std::vector<Entity> stack{m_root}; !stack.empty();)
            {
                const Entity entity = stack.back();
                stack.pop_back();
                const auto* identity = world.tryGetConstComponent<UiIdentityComponent>(entity);
                if (!identity)
                {
                    continue;
                }
                if (isSlot(world, entity))
                {
                    if (identity->id == id &&
                        std::ranges::none_of(placed, [&](const Live* live) { return live && live->entity == entity; }))
                    {
                        return entity;
                    }
                    continue;
                }
                if (opaqueElement(world, entity, identity->kind) || partsOnly(identity->kind) || otherViewRoot(entity))
                {
                    continue;
                }
                for (const Entity child : hierarchy.childrenOf(entity))
                {
                    stack.push_back(child);
                }
            }
            return Entity::undefined();
        };
        for (u32 index = 0; index < model->nodes.size(); ++index)
        {
            const UiDocumentModel::Node& node = model->nodes[index];
            Entity host = m_parent;
            if (node.parent != NONE)
            {
                if (!placed[node.parent])
                {
                    continue; // under an element that was not placed
                }
                host = placed[node.parent]->host;
            }
            Props baseline;
            const Props* from = nullptr;
            Live* live = nullptr;
            if (const auto it = m_live.find(node.path); it != m_live.end() && world.isEntityAlive(it->second->entity))
            {
                live = it->second.get();
                if (live->kind != node.kind ||
                    (live->owned && previous && live->node != NONE &&
                     previous->nodes[live->node].opaque != node.opaque))
                {
                    if (!live->owned)
                    {
                        warn(node.path,
                             std::format("the element is a {}, not a {}, and belongs to code: kept as it is",
                                         kindName(live->kind), kindName(node.kind)));
                        live->node = NONE; // the new model has no node for it
                        live->seen = epoch;
                        continue;
                    }
                    // recreated: the old one goes with the unmet records below
                    live = nullptr;
                }
                else
                {
                    from = previous && live->node != NONE ? &previous->nodes[live->node].props : nullptr;
                    if (!from)
                    {
                        baseline = readProps(hierarchy, live->entity, live->kind, node.opaque);
                        from = &baseline;
                    }
                    if (previous && live->node != NONE)
                    {
                        const UiDocumentModel::Node& old = previous->nodes[live->node];
                        orderDirty[index] =
                            old.children.size() != node.children.size() ||
                            !std::ranges::equal(
                                old.children, node.children, {}, [&](const u32 kid) { return previous->nodes[kid].id; },
                                [&](const u32 kid) { return model->nodes[kid].id; });
                    }
                    else
                    {
                        orderDirty[index] = 1;
                    }
                }
            }
            // a slot is matched by id, not id path: one the document moved keeps its entity and the code's content
            Entity movedSlot = Entity::undefined();
            bool moved = false;
            if (!live && node.props.slot && !(m_adopted && index == 0))
            {
                movedSlot = findSlot(node.id);
                moved = movedSlot != Entity::undefined();
                const auto record = movedSlot == Entity::undefined()
                    ? m_live.end()
                    : std::ranges::find_if(m_live,
                                           [&](const auto& entry) { return entry.second->entity == movedSlot; });
                if (record != m_live.end())
                {
                    std::unique_ptr<Live> taken = std::move(record->second);
                    m_live.erase(record);
                    auto [at, inserted] = m_live.try_emplace(node.path);
                    if (!inserted && at->second->owned && world.isEntityAlive(at->second->entity))
                    {
                        doomed.push_back(at->second->entity);
                    }
                    at->second = std::move(taken);
                    live = at->second.get();
                    from = previous && live->node != NONE ? &previous->nodes[live->node].props : nullptr;
                    if (!from)
                    {
                        baseline = readProps(hierarchy, live->entity, live->kind, node.opaque);
                        from = &baseline;
                    }
                    orderDirty[index] = 1;
                    movedSlot = Entity::undefined();
                }
            }
            if (!live)
            {
                Entity entity = Entity::undefined();
                bool owned = false;
                if (m_adopted && index == 0)
                {
                    entity = m_root;
                }
                else if (m_fallbackTree && index == 0 && hierarchy.isElement(m_root) && idOf(world, m_root) == node.id)
                {
                    entity = m_root; // a fallback build's root: its elements are reused by id
                }
                else if (movedSlot != Entity::undefined())
                {
                    entity = movedSlot; // a code-built slot elsewhere in the tree
                }
                else if (hierarchy.isElement(host))
                {
                    entity = untrackedChild(host, node.id);
                }
                if (entity != Entity::undefined())
                {
                    const UiWidgetKind kind = world.getConstComponent<UiIdentityComponent>(entity).kind;
                    if (kind != node.kind)
                    {
                        warn(node.path,
                             std::format("the element is a {}, not a {}, and belongs to code: kept as it is",
                                         kindName(kind), kindName(node.kind)));
                        continue;
                    }
                    owned = m_scope->owns(entity);
                    baseline = readProps(hierarchy, entity, kind, node.opaque);
                }
                else if (!node.opaque)
                {
                    entity = create(node);
                    owned = true;
                    ++stats.created;
                    baseline = defaultProps(node.kind);
                    if (hierarchy.isElement(host))
                    {
                        hierarchy.attach(entity, host);
                    }
                    if (node.parent != NONE)
                    {
                        orderDirty[node.parent] = 1;
                    }
                }
                else
                {
                    warn(node.path,
                         std::format("no {} '{}' exists here, and a document cannot build one", kindName(node.kind),
                                     node.id.toView()));
                    continue;
                }
                from = &baseline;
                auto made = std::make_unique<Live>();
                made->entity = entity;
                made->host = childHost(hierarchy, entity, node.kind);
                made->kind = node.kind;
                made->owned = owned;
                orderDirty[index] = 1;
                live = made.get();
                auto [slot, inserted] = m_live.try_emplace(node.path);
                if (!inserted && slot->second->owned && slot->second->entity != entity &&
                    m_scope->owns(slot->second->entity))
                {
                    doomed.push_back(slot->second->entity);
                }
                slot->second = std::move(made);
            }
            // it moved (this view's slot), or its document parent was recreated or is gone: it follows; an element
            // code reparented elsewhere stays there
            if (const Entity parent = hierarchy.parentOf(live->entity); !(m_adopted && index == 0) &&
                hierarchy.isElement(host) && parent != host &&
                (moved || !world.isEntityAlive(parent) || std::ranges::contains(doomed, parent)))
            {
                hierarchy.attach(live->entity, host);
                if (node.parent != NONE)
                {
                    orderDirty[node.parent] = 1;
                }
            }
            if (*from == node.props)
            {
                ++stats.unchanged;
            }
            else if (const u32 written = write(*live, *from, node.props, node.path); written != 0)
            {
                stats.fieldsWritten += written;
                stats.updated += live->seen != 0 || !live->owned ? 1 : 0;
            }
            live->node = index;
            live->seen = epoch;
            placed[index] = live;
        }

        // sibling order: only where a document's child list changed or an element was placed anew
        for (u32 index = 0; index < model->nodes.size(); ++index)
        {
            if (!orderDirty[index] || !placed[index])
            {
                continue;
            }
            const std::vector<u32>& kids = model->nodes[index].children;
            std::vector<Entity> wanted;
            wanted.reserve(kids.size());
            for (const u32 kid : kids)
            {
                if (placed[kid])
                {
                    wanted.push_back(placed[kid]->entity);
                }
            }
            const std::span<const Entity> current = hierarchy.childrenOf(placed[index]->host);
            const std::unordered_set<Entity> wantedSet(wanted.begin(), wanted.end());
            std::vector<Entity> order;
            for (const Entity child : current)
            {
                if (wantedSet.contains(child))
                {
                    order.push_back(child);
                }
            }
            if (order != wanted)
            {
                hierarchy.setChildOrder(placed[index]->host, wanted);
            }
        }

        // what the document no longer names: the view's own elements go, code's stay
        for (auto it = m_live.begin(); it != m_live.end();)
        {
            Live& live = *it->second;
            if (live.seen == epoch)
            {
                ++it;
                continue;
            }
            if (live.owned && m_scope->owns(live.entity))
            {
                m_scope->destroy(Widget(m_scope.get(), live.entity, m_scope->generation()));
                ++stats.destroyed;
            }
            else if (!live.owned && world.isEntityAlive(live.entity))
            {
                warn(it->first, "no longer in the document, but built by code: kept");
            }
            it = m_live.erase(it);
        }
        for (const Entity entity : doomed)
        {
            if (world.isEntityAlive(entity) && m_scope->owns(entity))
            {
                m_scope->destroy(Widget(m_scope.get(), entity, m_scope->generation()));
                ++stats.destroyed;
            }
        }
        // a fallback build's elements the document did not take over go: they were never tracked
        if (m_fallbackTree && hierarchy.isElement(m_root))
        {
            std::unordered_set<u64> kept;
            for (const Live* live : placed)
            {
                if (live)
                {
                    kept.insert(live->entity.id());
                }
            }
            for (std::vector<Entity> stack{m_root}; !stack.empty();)
            {
                const Entity entity = stack.back();
                stack.pop_back();
                const auto* identity = world.tryGetConstComponent<UiIdentityComponent>(entity);
                if (!kept.contains(entity.id()) && m_scope->owns(entity))
                {
                    m_scope->destroy(Widget(m_scope.get(), entity, m_scope->generation()));
                    ++stats.destroyed;
                }
                if (!identity || partsOnly(identity->kind) || opaqueElement(world, entity, identity->kind) ||
                    isSlot(world, entity))
                {
                    continue; // its children are its own parts, or code's
                }
                for (const Entity child : hierarchy.childrenOf(childHost(hierarchy, entity, identity->kind)))
                {
                    stack.push_back(child);
                }
            }
        }
        m_fallbackTree = false;
        m_scope->commit();
        m_root = placed.empty() || !placed[0] ? Entity::undefined() : placed[0]->entity;
        m_placed = std::move(placed);
        m_model = std::move(model);
        m_text = std::move(text);
        m_problems = std::move(problems);
        stats.applyMs = millisecondsSince(start);
        m_applying = false;

        std::vector<std::pair<UiViewContract::AttachFn, Widget>> attach;
        resolveContract(attach);
        runAttach(attach);
        return true;
    }

    void UiDocumentView::resolveContract(std::vector<std::pair<UiViewContract::AttachFn, Widget>>& attach)
    {
        const UiHierarchyManager& hierarchy = m_context->hierarchy();
        const World& world = hierarchy.world();
        std::unordered_map<InternedString, Entity> found;
        for (const UiViewContract::Element& wanted : m_contract.elements())
        {
            found.try_emplace(wanted.id, Entity::undefined());
        }
        m_documentOnly.clear();
        m_documentOnlyCount = 0;
        const auto meet = [&](const InternedString id, const Entity entity)
        {
            if (const auto it = found.find(id); it != found.end())
            {
                if (it->second == Entity::undefined())
                {
                    it->second = entity;
                }
            }
            else if (m_model && ++m_documentOnlyCount <= UI_DOCUMENT_REPORT_CAP)
            {
                m_documentOnly.emplace_back(id.toView());
            }
        };
        if (m_model)
        {
            // document order, so a duplicate id binds the first as checkContract reports
            for (u32 index = 0; index < m_placed.size(); ++index)
            {
                if (m_placed[index])
                {
                    meet(m_model->nodes[index].id, m_placed[index]->entity);
                }
            }
            // code's elements the document named with another kind: kept, no longer in the model
            for (const auto& [path, live] : m_live)
            {
                if (live->node == NONE && world.isEntityAlive(live->entity))
                {
                    meet(idOf(world, live->entity), live->entity);
                }
            }
        }
        else if (hierarchy.isElement(m_root))
        {
            for (std::vector<Entity> stack{m_root}; !stack.empty();)
            {
                const Entity entity = stack.back();
                stack.pop_back();
                meet(idOf(world, entity), entity);
                if (isSlot(world, entity))
                {
                    continue; // code's content
                }
                for (const Entity child : hierarchy.childrenOf(entity))
                {
                    stack.push_back(child);
                }
            }
        }
        std::ranges::sort(m_documentOnly);
        m_codeOnly.clear();
        for (const UiViewContract::Element& wanted : m_contract.elements())
        {
            const Entity entity = found[wanted.id];
            if (entity == Entity::undefined() ||
                world.getConstComponent<UiIdentityComponent>(entity).kind != wanted.kind)
            {
                m_codeOnly.push_back(wanted.id);
                m_bound.erase(wanted.id);
                m_slotFields.erase(wanted.id);
                continue;
            }
            const auto [it, made] = m_bound.try_emplace(wanted.id, entity);
            bool slotChanged = false;
            if (wanted.slot)
            {
                World& slotWorld = hierarchy.world();
                if (!slotWorld.hasComponent<UiSlotComponent>(entity))
                {
                    slotWorld.addComponent<UiSlotComponent>(entity, UiSlotComponent{}); // a fallback build's panel
                }
                const UiSlotComponent& fields = slotWorld.getConstComponent<UiSlotComponent>(entity);
                const auto [seen, first] = m_slotFields.try_emplace(wanted.id, fields);
                slotChanged = first || !(seen->second == fields);
                seen->second = fields;
            }
            if (!made && it->second == entity && !slotChanged)
            {
                continue;
            }
            it->second = entity;
            if (wanted.attach)
            {
                UiScope* owner = m_context->ownerOf(entity);
                if (owner)
                {
                    attach.emplace_back(wanted.attach, Widget(owner, entity, owner->generation()));
                }
            }
        }
    }

    void UiDocumentView::runAttach(std::vector<std::pair<UiViewContract::AttachFn, Widget>>& attach)
    {
        // last, after the tree is consistent: a callback may destroy widgets or this view
        const std::shared_ptr<bool> alive = m_alive;
        for (auto& [fn, widget] : attach)
        {
            if (!*alive)
            {
                return;
            }
            if (widget.isValid())
            {
                fn(widget);
            }
        }
    }

    // -- UiDocuments --------------------------------------------------------------

    UiDocuments::~UiDocuments()
    {
        // views a presenter owns outlive the context: they only lose it
        for (UiDocumentView* view : m_views)
        {
            view->m_context = nullptr;
        }
        m_views.clear();
        m_adopted.clear();
    }

    UiDocumentView* UiDocuments::find(const std::string_view name) const
    {
        const auto it = std::ranges::find(m_views, name, &UiDocumentView::name);
        return it == m_views.end() || ((*it)->adopted() && !(*it)->rootAlive()) ? nullptr : *it;
    }

    UiDocumentView& UiDocuments::adopt(const Entity root)
    {
        const std::string name = std::format("@{}", root.id());
        if (UiDocumentView* existing = find(name))
        {
            return *existing;
        }
        // views over destroyed roots go (a stale "@id" must not outlive its element; the list stays bounded)
        std::erase_if(m_adopted, [&](const std::unique_ptr<UiDocumentView>& view)
                      { return view->name() == name || !view->rootAlive(); });
        return *m_adopted.emplace_back(new UiDocumentView(*m_context, root));
    }

    u32 UiDocuments::poll()
    {
        // by index, copying nothing per frame; a re-apply's callbacks may add or destroy views, so the walk starts over
        // after one (the views polled already are throttled and answer at once)
        u32 applied = 0;
        for (sizet i = 0; i < m_views.size(); ++i)
        {
            if (m_views[i]->poll())
            {
                ++applied;
                i = static_cast<sizet>(-1);
            }
        }
        return applied;
    }

    u32 UiDocuments::countCallbacksIn(const ImageRange& image) const
    {
        u32 count = 0;
        for (const UiDocumentView* view : m_views)
        {
            count += view->contract().countCallbacksIn(image);
        }
        return count;
    }

    std::string exportUiDocument(const UiContext& context, const Entity root, std::string* problem)
    {
        const UiHierarchyManager& hierarchy = context.hierarchy();
        if (!hierarchy.isElement(root) || !hierarchy.world().hasComponent<UiIdentityComponent>(root))
        {
            return {};
        }
        Json document = Json::object();
        document["version"] = UI_DOCUMENT_VERSION;
        ExportLimits limits;
        exportElement(hierarchy, root, document["root"] = Json::object(), limits, 0, 0);
        std::string text;
        if (limits.problem.empty())
        {
            // live text may hold bytes that are no UTF-8; they are replaced, never thrown on
            text = document.dump(2, ' ', false, Json::error_handler_t::replace);
            if (text.size() > UI_DOCUMENT_MAX_BYTES)
            {
                limits.problem = std::format("the exported document is larger than {} bytes", UI_DOCUMENT_MAX_BYTES);
            }
        }
        if (limits.problem.empty())
        {
            // what the importer would refuse must not be exported: the export is the undo inverse of an import
            std::vector<UiDocumentProblem> found;
            UiDocumentModel model;
            const UiViewContract none;
            Reader reader{none, nullptr, model, found};
            if (!Json::sax_parse(text, &reader) || !reader.fatal().empty())
            {
                limits.problem = reader.fatal().empty() ? std::string("the export is not a document") : reader.fatal();
            }
            else
            {
                reader.finish();
                const auto error = std::ranges::find_if(found, [](const UiDocumentProblem& p) { return p.error; });
                if (error != found.end())
                {
                    limits.problem = (error->path.empty() ? "" : error->path + ": ") + error->message;
                }
            }
        }
        if (!limits.problem.empty())
        {
            if (problem)
            {
                *problem = std::move(limits.problem);
            }
            return {};
        }
        return text;
    }
} // namespace spite
