#include "uiview/UiStyleSheet.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <format>
#include <string>
#include <chrono>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <nlohmann/json.hpp>

#include "base/Logging.hpp"
#include "base/io/FileSystem.hpp"
#include "ui/UiIcons.hpp"

#if defined(SPITE_PLATFORM_WINDOWS)
#define NOMINMAX
#include <windows.h>
#endif

namespace spite
{
    namespace
    {
        using Json = nlohmann::json;

        // Json::parse keeps the last of two equal keys, so a textual merge that doubled a block dropped
        // the first silently; this pass names the key and its path instead.
        struct DuplicateKeyFinder final : nlohmann::json_sax<Json>
        {
            struct Frame
            {
                std::unordered_set<std::string> keys;
                std::string current; // the key whose value is being read: the path step of this level
            };
            std::vector<Frame> frames;
            std::string duplicate;

            bool open()
            {
                frames.emplace_back();
                return true;
            }
            bool close()
            {
                frames.pop_back();
                return true;
            }
            bool null() override { return true; }
            bool boolean(bool) override { return true; }
            bool number_integer(number_integer_t) override { return true; }
            bool number_unsigned(number_unsigned_t) override { return true; }
            bool number_float(number_float_t, const string_t&) override { return true; }
            bool string(string_t&) override { return true; }
            bool binary(binary_t&) override { return true; }
            bool start_object(std::size_t) override { return open(); }
            bool end_object() override { return close(); }
            bool start_array(std::size_t) override { return open(); }
            bool end_array() override { return close(); }
            bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override
            {
                return false;
            }
            bool key(string_t& name) override
            {
                if (!frames.back().keys.insert(name).second)
                {
                    duplicate = "duplicate key '" + name + "' under '";
                    for (sizet i = 0; i + 1 < frames.size(); ++i)
                    {
                        duplicate += frames[i].current + (i + 2 < frames.size() ? "." : "");
                    }
                    duplicate += "'";
                    return false;
                }
                frames.back().current = name;
                return true;
            }
        };

        struct StyleBlock
        {
            std::optional<u32> backgroundColor;
            std::optional<InternedString> backgroundImage;
            std::optional<u32> backgroundTint;
            std::optional<Vec4> cornerRadius;
            std::optional<InternedString> font;
            std::optional<float> textSize;
            std::optional<u32> textColor;
            std::optional<UiTextAlign> textAlign;
            std::optional<UiTextWrap> textWrap;
            std::optional<UiTextOverflow> textOverflow;
            std::optional<UiTextVAlign> textVAlign;
            std::optional<u32> textMaxLines;
            std::optional<float> textLineHeight;
            std::optional<float> textLetterSpacing;
            std::optional<UiTextTransform> textTransform;
            std::optional<u16> fontWeight;
            std::optional<InternedString> icon;
            std::optional<u32> caretColor;
            std::optional<u32> selectionColor;
            std::optional<u32> placeholderColor;
            std::optional<float> borderWidth;
            std::optional<u32> borderColor;
            std::optional<float> outlineWidth;
            std::optional<u32> outlineColor;
            std::optional<float> outlineOffset;
            std::optional<u32> gradientColor;
            std::optional<float> gradientAngle;
            std::optional<u32> shadowColor;
            std::optional<Vec2> shadowOffset;
            std::optional<float> shadowBlur;
            std::optional<float> shadowSpread;
            std::optional<bool> shadowInset;
            std::optional<std::array<UiShadow, UI_MAX_SHADOWS - 1>> shadowLayers; // the array form's entries 1..3
            std::optional<float> opacity;
            std::optional<UiTransitionSet> transition;
            std::array<InternedString, static_cast<sizet>(UiStyleField::Count)> tokens{}; // by UiStyleField
        };

        struct StyleEntry
        {
            StyleBlock base;
            std::array<StyleBlock, 5> states; // by UiInteractionState; Normal stays empty
        };

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

        constexpr std::array<std::pair<std::string_view, UiInteractionState>, 4> STATE_NAMES = {{
            {"focused", UiInteractionState::Focused},
            {"hover", UiInteractionState::Hover},
            {"pressed", UiInteractionState::Pressed},
            {"disabled", UiInteractionState::Disabled},
        }};

        bool hasAnyField(const StyleEntry& entry)
        {
            const auto any = [](const StyleBlock& b)
            {
                return b.backgroundColor || b.backgroundImage || b.backgroundTint || b.cornerRadius || b.font ||
                    b.textSize || b.textColor || b.textAlign || b.textWrap || b.textOverflow || b.textVAlign ||
                    b.textMaxLines || b.textLineHeight || b.textLetterSpacing || b.textTransform || b.fontWeight ||
                    b.icon || b.caretColor || b.selectionColor || b.placeholderColor || b.borderWidth ||
                    b.borderColor || b.outlineWidth || b.outlineColor || b.outlineOffset || b.gradientColor ||
                    b.gradientAngle || b.shadowColor || b.shadowOffset || b.shadowBlur || b.shadowSpread ||
                    b.shadowInset || b.shadowLayers || b.opacity || b.transition;
            };
            return any(entry.base) || std::ranges::any_of(entry.states, any);
        }

        struct ParseError
        {
            std::string message;
        };

        const Json* member(const Json& object, const char* key)
        {
            const auto it = object.find(key);
            return it == object.end() ? nullptr : &*it;
        }

        // Where a token a layer used was defined: the defining layer and the alias chain from the written name.
        struct TokenOrigin
        {
            InternedString layer;
            std::vector<InternedString> chain;
        };

        // The "vars" objects a layer's tokens read, in lookup order: the layer's own, the active theme
        // layers top first, the base sheet's. The base sheet itself reads the themes first, its own vars last, so its
        // vars are defaults a theme overrides.
        struct TokenScope
        {
            struct Source
            {
                InternedString layer;
                const Json* vars = nullptr;
            };
            std::vector<Source> sources;
            bool ownFirst = false; // sources[0] is the parsed layer's own vars
            bool borrowed = false; // a token read through the themes or the base sheet
            std::unordered_map<InternedString, TokenOrigin> used;
        };

        std::string chainText(const std::vector<InternedString>& chain)
        {
            std::string text;
            for (const InternedString& name : chain)
            {
                text += (text.empty() ? "$" : " -> $") + std::string(name.toView());
            }
            return text;
        }

        constexpr bool isTokenNameChar(const char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-';
        }

        std::string definedTokens(const TokenScope& scope)
        {
            std::vector<std::string> names;
            for (const TokenScope::Source& source : scope.sources)
            {
                if (source.vars && source.vars->is_object())
                {
                    for (const auto& item : source.vars->items())
                    {
                        names.push_back(item.key());
                    }
                }
            }
            std::ranges::sort(names);
            names.erase(std::unique(names.begin(), names.end()), names.end());
            constexpr sizet SHOWN = 24;
            std::string text;
            for (sizet i = 0; i < names.size() && i < SHOWN; ++i)
            {
                text += (i ? ", " : "") + names[i];
            }
            if (names.size() > SHOWN)
            {
                text += std::format(", ... {} more", names.size() - SHOWN);
            }
            return text.empty() ? "none" : text;
        }

        // The value `name` stands for: the first source defining it, an alias ("$other") followed by the same lookup.
        // `chain` holds the names already followed (the alias being checked first).
        const Json& resolveToken(TokenScope& scope, std::string name, const std::string& where,
                                 std::vector<InternedString> chain, TokenOrigin& origin)
        {
            for (;;)
            {
                const InternedString current(name);
                const bool again = std::ranges::find(chain, current) != chain.end();
                chain.push_back(current);
                if (again)
                {
                    throw ParseError{std::format("{}: token cycle {}", where, chainText(chain))};
                }
                const Json* value = nullptr;
                sizet found = 0;
                for (; found < scope.sources.size(); ++found)
                {
                    const Json* vars = scope.sources[found].vars;
                    if (vars && vars->is_object())
                    {
                        if (const auto it = vars->find(name); it != vars->end())
                        {
                            value = &*it;
                            break;
                        }
                    }
                }
                if (!value)
                {
                    throw ParseError{std::format("{}: unknown token '${}'{}; defined tokens: {}", where, name,
                                                 chain.size() > 1 ? " (" + chainText(chain) + ")" : std::string{},
                                                 definedTokens(scope))};
                }
                scope.borrowed |= !scope.ownFirst || found != 0;
                if (value->is_string() && value->get_ref<const std::string&>().size() > 1 &&
                    value->get_ref<const std::string&>()[0] == '$')
                {
                    name = value->get_ref<const std::string&>().substr(1);
                    continue;
                }
                origin = {scope.sources[found].layer, std::move(chain)};
                return *value;
            }
        }

        // A token string in a field position takes its resolved value.
        const Json* member(const Json& object, const char* key, TokenScope& scope, const std::string& where,
                           InternedString& token)
        {
            const Json* node = member(object, key);
            if (node && node->is_string())
            {
                const std::string& text = node->get_ref<const std::string&>();
                if (text.size() > 1 && text[0] == '$')
                {
                    TokenOrigin origin;
                    const Json& value = resolveToken(scope, text.substr(1), where, {}, origin);
                    token = InternedString(std::string_view(text).substr(1));
                    scope.used.try_emplace(token, std::move(origin));
                    return &value;
                }
            }
            return node;
        }

        const Json& expectObject(const Json& node, std::string_view where)
        {
            if (!node.is_object())
            {
                throw ParseError{std::format("{}: expected an object", where)};
            }
            return node;
        }

        float parseNumber(const Json& node, std::string_view where)
        {
            if (!node.is_number())
            {
                throw ParseError{std::format("{}: expected a number", where)};
            }
            const float value = node.get<float>();
            if (!std::isfinite(value))
            {
                throw ParseError{std::format("{}: expected a finite number", where)};
            }
            return value;
        }

        float parseNonNegative(const Json& node, std::string_view where)
        {
            const float value = parseNumber(node, where);
            if (value < 0.0f)
            {
                throw ParseError{std::format("{}: expected a number >= 0", where)};
            }
            return value;
        }

        float parseUnit(const Json& node, std::string_view where)
        {
            const float value = parseNumber(node, where);
            if (value < 0.0f || value > 1.0f)
            {
                throw ParseError{std::format("{}: expected a number in 0..1", where)};
            }
            return value;
        }

        Vec2 parseVec2(const Json& node, std::string_view where)
        {
            if (!node.is_array() || node.size() != 2)
            {
                throw ParseError{std::format("{}: expected [x, y]", where)};
            }
            return Vec2{parseNumber(node[0], where), parseNumber(node[1], where)};
        }

        // One number (all four corners) or [topLeft, topRight, bottomRight, bottomLeft].
        Vec4 parseCornerRadius(const Json& node, std::string_view where)
        {
            if (!node.is_array())
            {
                return Vec4{parseNumber(node, where)};
            }
            if (node.size() != 4)
            {
                throw ParseError{
                    std::format("{}: expected a number or [topLeft, topRight, bottomRight, bottomLeft]", where)};
            }
            return Vec4{parseNumber(node[0], where), parseNumber(node[1], where), parseNumber(node[2], where),
                        parseNumber(node[3], where)};
        }

        // The decoration objects are strict: a misspelled key would otherwise silently draw nothing.
        const Json& expectKeys(const Json& node, std::string_view where, std::initializer_list<std::string_view> keys)
        {
            expectObject(node, where);
            for (const auto& item : node.items())
            {
                if (std::ranges::find(keys, item.key()) == keys.end())
                {
                    throw ParseError{std::format("{}: unknown key '{}'", where, item.key())};
                }
            }
            return node;
        }

        InternedString parseName(const Json& node, std::string_view where)
        {
            if (!node.is_string())
            {
                throw ParseError{std::format("{}: expected a string", where)};
            }
            return InternedString(node.get<std::string>());
        }

        u32 parseColor(const Json& node, std::string_view where)
        {
            if (!node.is_array() || node.size() != 4)
            {
                throw ParseError{std::format("{}: expected [r, g, b, a]", where)};
            }
            u32 packed = 0;
            for (u32 channel = 0; channel < 4; ++channel)
            {
                const float value = std::clamp(parseNumber(node[channel], where), 0.0f, 1.0f);
                packed |= static_cast<u32>(std::lround(value * 255.0f)) << (channel * 8);
            }
            return packed;
        }

        UiTextAlign parseAlign(const Json& node, std::string_view where)
        {
            const std::string value = node.is_string() ? node.get<std::string>() : std::string{};
            if (value == "left")
            {
                return UiTextAlign::Left;
            }
            if (value == "center")
            {
                return UiTextAlign::Center;
            }
            if (value == "right")
            {
                return UiTextAlign::Right;
            }
            throw ParseError{std::format("{}: expected left, center or right", where)};
        }

        template <class E, sizet N>
        E parseEnum(const Json& node, std::string_view where,
                    const std::array<std::pair<std::string_view, E>, N>& names)
        {
            const std::string value = node.is_string() ? node.get<std::string>() : std::string{};
            for (const auto& [name, enumerator] : names)
            {
                if (name == value)
                {
                    return enumerator;
                }
            }
            std::string expected;
            for (const auto& [name, enumerator] : names)
            {
                expected += (expected.empty() ? "" : ", ") + std::string(name);
            }
            throw ParseError{std::format("{}: expected one of {}", where, expected)};
        }

        UiTransitionSpec parseTransitionSpec(const Json& node, const std::string& where)
        {
            expectKeys(node, where, {"duration", "delay", "ease"});
            UiTransitionSpec spec;
            if (const Json* duration = member(node, "duration"))
            {
                spec.duration = parseNonNegative(*duration, where + ".duration");
            }
            if (const Json* delay = member(node, "delay"))
            {
                spec.delay = parseNonNegative(*delay, where + ".delay");
            }
            if (const Json* ease = member(node, "ease"))
            {
                const std::optional<UiEase> named =
                    ease->is_string() ? uiEaseFromName(ease->get_ref<const std::string&>()) : std::nullopt;
                if (!named)
                {
                    throw ParseError{
                        std::format("{}.ease: expected a UiEase name (linear, inQuad, outQuad, ...)", where)};
                }
                spec.ease = *named;
            }
            return spec;
        }

        UiTransitionSet parseTransition(const Json& node, const std::string& where)
        {
            expectObject(node, where);
            UiTransitionSet set;
            if (const Json* all = member(node, "all"))
            {
                set.fill(parseTransitionSpec(*all, where + ".all"));
            }
            for (const auto& [name, spec] : node.items())
            {
                if (name == "all")
                {
                    continue;
                }
                const auto it = std::ranges::find(UI_TRANSITION_PROPERTY_NAMES, name);
                if (it == UI_TRANSITION_PROPERTY_NAMES.end())
                {
                    throw ParseError{std::format("{}: unknown property '{}'", where, name)};
                }
                set[static_cast<sizet>(it - UI_TRANSITION_PROPERTY_NAMES.begin())] =
                    parseTransitionSpec(spec, where + "." + name);
            }
            return set;
        }

        // One entry of a shadow value; every key is optional and an unknown one fails the load.
        struct ParsedShadow
        {
            UiShadow value;
            std::array<bool, 5> present{}; // color, offset, blur, spread, inset
            std::array<InternedString, 5> tokens{};
        };

        ParsedShadow parseShadowEntry(const Json& node, const std::string& at, TokenScope& scope)
        {
            expectKeys(node, at, {"color", "offset", "blur", "spread", "inset"});
            ParsedShadow parsed;
            const auto key = [&](const char* name, const u32 slot, auto&& parse)
            {
                InternedString token;
                if (const Json* value = member(node, name, scope, at + "." + name, token))
                {
                    parse(*value, at + "." + name);
                    parsed.present[slot] = true;
                    parsed.tokens[slot] = token;
                }
            };
            key("color", 0, [&](const Json& v, const std::string& w) { parsed.value.color = parseColor(v, w); });
            key("offset", 1, [&](const Json& v, const std::string& w) { parsed.value.offset = parseVec2(v, w); });
            key("blur", 2, [&](const Json& v, const std::string& w) { parsed.value.blur = parseNonNegative(v, w); });
            key("spread", 3, [&](const Json& v, const std::string& w) { parsed.value.spread = parseNumber(v, w); });
            key("inset", 4,
                [&](const Json& v, const std::string& w)
                {
                    if (!v.is_boolean())
                    {
                        throw ParseError{std::format("{}: expected true or false", w)};
                    }
                    parsed.value.inset = v.get<bool>();
                });
            return parsed;
        }

        StyleBlock parseBlock(const Json& node, const std::string& where, TokenScope& scope)
        {
            expectObject(node, where);
            StyleBlock block;
            const auto field =
                [&](const Json& object, const char* key, const std::string& at, const UiStyleField id, auto&& parse)
            {
                InternedString token;
                if (const Json* value = member(object, key, scope, at + "." + key, token))
                {
                    parse(*value, at + "." + key);
                    block.tokens[static_cast<u8>(id)] = token;
                }
            };
            if (const Json* background = member(node, "background"))
            {
                expectObject(*background, where + ".background");
                field(*background, "color", where + ".background", UiStyleField::BackgroundColor,
                      [&](const Json& v, const std::string& at) { block.backgroundColor = parseColor(v, at); });
                field(*background, "image", where + ".background", UiStyleField::BackgroundImage,
                      [&](const Json& v, const std::string& at) { block.backgroundImage = parseName(v, at); });
                field(*background, "tint", where + ".background", UiStyleField::BackgroundTint,
                      [&](const Json& v, const std::string& at) { block.backgroundTint = parseColor(v, at); });
            }
            if (const Json* shape = member(node, "shape"))
            {
                expectObject(*shape, where + ".shape");
                field(*shape, "cornerRadius", where + ".shape", UiStyleField::CornerRadius,
                      [&](const Json& v, const std::string& at) { block.cornerRadius = parseCornerRadius(v, at); });
            }
            if (const Json* border = member(node, "border"))
            {
                const std::string at = where + ".border";
                expectKeys(*border, at, {"width", "color"});
                field(*border, "width", at, UiStyleField::BorderWidth,
                      [&](const Json& v, const std::string& w) { block.borderWidth = parseNonNegative(v, w); });
                field(*border, "color", at, UiStyleField::BorderColor,
                      [&](const Json& v, const std::string& w) { block.borderColor = parseColor(v, w); });
            }
            if (const Json* outline = member(node, "outline"))
            {
                const std::string at = where + ".outline";
                expectKeys(*outline, at, {"width", "color", "offset"});
                field(*outline, "width", at, UiStyleField::OutlineWidth,
                      [&](const Json& v, const std::string& w) { block.outlineWidth = parseNonNegative(v, w); });
                field(*outline, "color", at, UiStyleField::OutlineColor,
                      [&](const Json& v, const std::string& w) { block.outlineColor = parseColor(v, w); });
                field(*outline, "offset", at, UiStyleField::OutlineOffset,
                      [&](const Json& v, const std::string& w) { block.outlineOffset = parseNumber(v, w); });
            }
            if (const Json* gradient = member(node, "gradient"))
            {
                const std::string at = where + ".gradient";
                expectKeys(*gradient, at, {"color", "angle"});
                field(*gradient, "color", at, UiStyleField::GradientColor,
                      [&](const Json& v, const std::string& w) { block.gradientColor = parseColor(v, w); });
                field(*gradient, "angle", at, UiStyleField::GradientAngle,
                      [&](const Json& v, const std::string& w) { block.gradientAngle = parseNumber(v, w); });
            }
            {
                // one object: entry 0, each key resolving alone; an array: the whole stack, which replaces the lower
                // levels' (a missing key is its default). The value may be one "$token".
                const std::string at = where + ".shadow";
                InternedString whole;
                if (const Json* shadow = member(node, "shadow", scope, at, whole))
                {
                    const bool stack = shadow->is_array();
                    if (stack && shadow->size() > UI_MAX_SHADOWS)
                    {
                        throw ParseError{std::format("{}[{}]: at most {} shadows", at, UI_MAX_SHADOWS, UI_MAX_SHADOWS)};
                    }
                    const auto entry = [&](const sizet index)
                    {
                        return stack ? parseShadowEntry((*shadow)[index], std::format("{}[{}]", at, index), scope)
                                     : parseShadowEntry(*shadow, at, scope);
                    };
                    const ParsedShadow head = !stack || !shadow->empty() ? entry(0) : ParsedShadow{};
                    const auto set = [&](const u32 slot, const UiStyleField id, auto& target, const auto& value)
                    {
                        if (stack || head.present[slot])
                        {
                            target = value;
                            block.tokens[static_cast<u8>(id)] = head.tokens[slot].isValid() ? head.tokens[slot] : whole;
                        }
                    };
                    set(0, UiStyleField::ShadowColor, block.shadowColor, head.value.color);
                    set(1, UiStyleField::ShadowOffset, block.shadowOffset, head.value.offset);
                    set(2, UiStyleField::ShadowBlur, block.shadowBlur, head.value.blur);
                    set(3, UiStyleField::ShadowSpread, block.shadowSpread, head.value.spread);
                    set(4, UiStyleField::ShadowInset, block.shadowInset, head.value.inset);
                    if (stack)
                    {
                        std::array<UiShadow, UI_MAX_SHADOWS - 1> layers{};
                        InternedString layerToken = whole;
                        for (sizet index = 1; index < shadow->size(); ++index)
                        {
                            const ParsedShadow parsed = entry(index);
                            layers[index - 1] = parsed.value;
                            for (const InternedString& token : parsed.tokens)
                            {
                                if (!layerToken.isValid() && token.isValid())
                                {
                                    layerToken = token; // the first token an entry used stands for the field
                                }
                            }
                        }
                        block.shadowLayers = layers;
                        block.tokens[static_cast<u8>(UiStyleField::ShadowLayers)] = layerToken;
                    }
                }
            }
            field(node, "opacity", where, UiStyleField::Opacity,
                  [&](const Json& v, const std::string& w) { block.opacity = parseUnit(v, w); });
            if (const Json* transition = member(node, "transition"))
            {
                block.transition = parseTransition(*transition, where + ".transition");
            }
            if (const Json* text = member(node, "text"))
            {
                const std::string at = where + ".text";
                expectObject(*text, at);
                field(*text, "font", at, UiStyleField::Font,
                      [&](const Json& v, const std::string& w) { block.font = parseName(v, w); });
                field(*text, "size", at, UiStyleField::TextSize,
                      [&](const Json& v, const std::string& w) { block.textSize = parseNumber(v, w); });
                field(*text, "color", at, UiStyleField::TextColor,
                      [&](const Json& v, const std::string& w) { block.textColor = parseColor(v, w); });
                field(*text, "align", at, UiStyleField::TextAlign,
                      [&](const Json& v, const std::string& w) { block.textAlign = parseAlign(v, w); });
                field(*text, "wrap", at, UiStyleField::TextWrap,
                      [&](const Json& v, const std::string& w)
                      {
                          block.textWrap = parseEnum<UiTextWrap, 3>(
                              v, w,
                              {{{"none", UiTextWrap::None}, {"lines", UiTextWrap::Lines}, {"word", UiTextWrap::Word}}});
                      });
                field(*text, "overflow", at, UiStyleField::TextOverflow,
                      [&](const Json& v, const std::string& w)
                      {
                          block.textOverflow =
                              parseEnum<UiTextOverflow, 3>(v, w,
                                                           {{{"clip", UiTextOverflow::Clip},
                                                             {"ellipsis", UiTextOverflow::Ellipsis},
                                                             {"middleEllipsis", UiTextOverflow::MiddleEllipsis}}});
                      });
                field(*text, "verticalAlign", at, UiStyleField::TextVAlign,
                      [&](const Json& v, const std::string& w)
                      {
                          block.textVAlign = parseEnum<UiTextVAlign, 3>(v, w,
                                                                        {{{"top", UiTextVAlign::Top},
                                                                          {"center", UiTextVAlign::Center},
                                                                          {"bottom", UiTextVAlign::Bottom}}});
                      });
                field(
                    *text, "maxLines", at, UiStyleField::TextMaxLines, [&](const Json& v, const std::string& w)
                    { block.textMaxLines = static_cast<u32>(std::lround(std::min(parseNonNegative(v, w), 1.0e6f))); });
                field(*text, "lineHeight", at, UiStyleField::TextLineHeight,
                      [&](const Json& v, const std::string& w)
                      {
                          const float multiple = parseNumber(v, w);
                          if (multiple <= 0.0f)
                          {
                              throw ParseError{std::format("{}: expected a number > 0", w)};
                          }
                          block.textLineHeight = multiple;
                      });
                field(*text, "letterSpacing", at, UiStyleField::TextLetterSpacing,
                      [&](const Json& v, const std::string& w) { block.textLetterSpacing = parseNumber(v, w); });
                field(*text, "transform", at, UiStyleField::TextTransform,
                      [&](const Json& v, const std::string& w)
                      {
                          block.textTransform =
                              parseEnum<UiTextTransform, 3>(v, w,
                                                            {{{"none", UiTextTransform::None},
                                                              {"uppercase", UiTextTransform::Uppercase},
                                                              {"lowercase", UiTextTransform::Lowercase}}});
                      });
                field(*text, "weight", at, UiStyleField::FontWeight,
                      [&](const Json& v, const std::string& w)
                      {
                          const float weight = parseNumber(v, w);
                          if (weight < 1.0f || weight > 1000.0f)
                          {
                              throw ParseError{std::format("{}: expected a weight 1..1000", w)};
                          }
                          block.fontWeight = static_cast<u16>(std::lround(weight));
                      });
                field(*text, "icon", at, UiStyleField::Icon,
                      [&](const Json& v, const std::string& w)
                      {
                          if (!v.is_string())
                          {
                              throw ParseError{std::format("{}: expected an icon name", w)};
                          }
                          const std::string& name = v.get_ref<const std::string&>();
                          if (!name.empty() && uiIconCodepoint(name) == 0)
                          {
                              throw ParseError{std::format("{}: unknown icon '{}' (closest: '{}')", w,
                                                           std::string_view(name).substr(0, 64),
                                                           uiIconClosestName(name))};
                          }
                          block.icon = InternedString(name); // empty: no icon, over one a lower level names
                      });
                field(*text, "caretColor", at, UiStyleField::CaretColor,
                      [&](const Json& v, const std::string& w) { block.caretColor = parseColor(v, w); });
                field(*text, "selectionColor", at, UiStyleField::SelectionColor,
                      [&](const Json& v, const std::string& w) { block.selectionColor = parseColor(v, w); });
                field(*text, "placeholderColor", at, UiStyleField::PlaceholderColor,
                      [&](const Json& v, const std::string& w) { block.placeholderColor = parseColor(v, w); });
            }
            return block;
        }

        StyleEntry parseEntry(const Json& node, const std::string& where, TokenScope& scope)
        {
            StyleEntry entry;
            entry.base = parseBlock(node, where, scope);
            if (const Json* states = member(node, "states"))
            {
                expectObject(*states, where + ".states");
                for (const auto& [name, state] : STATE_NAMES)
                {
                    if (const Json* variant = member(*states, std::string(name).c_str()))
                    {
                        const std::string at = std::format("{}.states.{}", where, name);
                        StyleBlock& block = entry.states[static_cast<u8>(state)];
                        block = parseBlock(*variant, at, scope);
                        // the measure sizes a label with the resting style, so a state may not change its lines
                        if (block.textWrap || block.textOverflow || block.textMaxLines || block.textLineHeight ||
                            block.textLetterSpacing || block.textTransform)
                        {
                            throw ParseError{std::format(
                                "{}.text: wrap, overflow, maxLines, lineHeight, letterSpacing and transform shape the "
                                "layout and belong to the resting style, not a state",
                                at)};
                        }
                    }
                }
            }
            return entry;
        }
    } // namespace

    std::string_view uiWidgetKindName(const UiWidgetKind kind) { return KIND_NAMES[static_cast<u8>(kind)].first; }

    std::optional<UiInteractionState> parseUiInteractionState(const std::string_view name)
    {
        if (name == "normal")
        {
            return UiInteractionState::Normal;
        }
        for (const auto& [stateName, state] : STATE_NAMES)
        {
            if (stateName == name)
            {
                return state;
            }
        }
        return std::nullopt;
    }

    struct UiStyleSheet::Content
    {
        StyleBlock defaults;
        std::array<StyleEntry, KIND_NAMES.size()> kinds;
        std::unordered_map<InternedString, StyleEntry> elements;
        std::unordered_map<InternedString, StyleEntry> classes;
        std::unordered_map<u64, InternedString> images;
        std::unordered_map<InternedString, std::vector<InternedString>> fontFallbacks;
        std::unordered_map<InternedString, std::vector<std::pair<u16, InternedString>>> fontWeights;
        std::vector<InternedString> vars;
        std::unordered_map<InternedString, TokenOrigin> used; // by the name a field wrote

        static Content parse(const Json& root, TokenScope& scope)
        {
            expectObject(root, "stylesheet");
            Content content;
            if (const Json* declared = member(root, "vars"))
            {
                for (const auto& [name, value] : expectObject(*declared, "vars").items())
                {
                    if (name.empty() || !std::ranges::all_of(name, isTokenNameChar))
                    {
                        throw ParseError{std::format("vars.{}: a token name is A-Z a-z 0-9 _ . - and not empty", name)};
                    }
                    if (value.is_string() && value.get_ref<const std::string&>().size() > 1 &&
                        value.get_ref<const std::string&>()[0] == '$')
                    {
                        TokenOrigin ignored;
                        resolveToken(scope, value.get_ref<const std::string&>().substr(1), "vars." + name,
                                     {InternedString(name)}, ignored);
                    }
                    content.vars.push_back(InternedString(name));
                }
                std::ranges::sort(content.vars, {}, &InternedString::toView);
            }
            if (const Json* defaults = member(root, "defaults"))
            {
                content.defaults = parseBlock(*defaults, "defaults", scope);
            }
            if (const Json* kinds = member(root, "kinds"))
            {
                expectObject(*kinds, "kinds");
                for (const auto& [name, kind] : KIND_NAMES)
                {
                    if (const Json* entry = member(*kinds, std::string(name).c_str()))
                    {
                        content.kinds[static_cast<u8>(kind)] = parseEntry(*entry, std::format("kinds.{}", name), scope);
                    }
                }
            }
            if (const Json* elements = member(root, "elements"))
            {
                for (const auto& [id, entry] : expectObject(*elements, "elements").items())
                {
                    content.elements[InternedString(id)] = parseEntry(entry, "elements." + id, scope);
                }
            }
            if (const Json* classes = member(root, "classes"))
            {
                for (const auto& [name, entry] : expectObject(*classes, "classes").items())
                {
                    content.classes[InternedString(name)] = parseEntry(entry, "classes." + name, scope);
                }
            }
            if (const Json* images = member(root, "images"))
            {
                for (const auto& [key, asset] : expectObject(*images, "images").items())
                {
                    u64 slot = 0;
                    const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), slot);
                    if (error != std::errc{} || end != key.data() + key.size() || slot == 0)
                    {
                        throw ParseError{std::format("images.{}: keys are non-zero image slots", key)};
                    }
                    content.images[slot] = parseName(asset, "images." + key);
                }
            }
            if (const Json* fallbacks = member(root, "fontFallbacks"))
            {
                for (const auto& [font, chain] : expectObject(*fallbacks, "fontFallbacks").items())
                {
                    const std::string where = "fontFallbacks." + font;
                    if (!chain.is_array())
                    {
                        throw ParseError{std::format("{}: expected an array of font names", where)};
                    }
                    std::vector<InternedString>& names = content.fontFallbacks[InternedString(font)];
                    for (const Json& name : chain)
                    {
                        names.push_back(parseName(name, where));
                    }
                }
            }
            if (const Json* weights = member(root, "fontWeights"))
            {
                for (const auto& [font, faces] : expectObject(*weights, "fontWeights").items())
                {
                    const std::string where = "fontWeights." + font;
                    std::vector<std::pair<u16, InternedString>>& declared = content.fontWeights[InternedString(font)];
                    for (const auto& [weight, face] : expectObject(faces, where).items())
                    {
                        unsigned number = 0;
                        const auto [end, error] = std::from_chars(weight.data(), weight.data() + weight.size(), number);
                        if (error != std::errc{} || end != weight.data() + weight.size() || number < 1 || number > 1000)
                        {
                            throw ParseError{std::format("{}.{}: keys are weights 1..1000", where, weight)};
                        }
                        declared.emplace_back(static_cast<u16>(number), parseName(face, where + "." + weight));
                    }
                }
            }
            content.used = std::move(scope.used);
            return content;
        }
    };

    struct UiStyleSheet::Layer
    {
        InternedString name{};
        std::filesystem::path path;
        std::filesystem::file_time_type stamp{}; // the file's write time when it loaded
        bool theme = false;
        Content content;
        Json vars; // the "vars" object, what the tokens of other layers read
        std::string source; // the file text, kept while a token of the layer reads through the themes or the base
        bool borrows = false; // so a change to those layers re-parses it
        Json pending; // the parsed file between readLayer and replaceLayers
    };

    namespace
    {
        constexpr u32 MAX_CLASS_LIST = 3; // names read from a class list
        // per layer: element and class variants and bases, kind variant and base, defaults
        constexpr u32 CHAIN_PER_LAYER = 2 + 2 * MAX_CLASS_LIST + 2 + 1;

        struct StyleKey
        {
            u64 id = 0;
            u64 styleClass = 0;
            u8 kind = 0;
            u8 state = 0;
            bool operator==(const StyleKey&) const = default;
        };

        struct StyleKeyHash
        {
            sizet operator()(const StyleKey& key) const noexcept
            {
                u64 h = key.id * 0x9E3779B97F4A7C15ull;
                h = (h ^ (h >> 29) ^ key.styleClass) * 0xBF58476D1CE4E5B9ull;
                h = (h ^ (h >> 32) ^ (u64{key.kind} << 8 | key.state)) * 0x94D049BB133111EBull;
                return static_cast<sizet>(h ^ (h >> 31));
            }
        };
    } // namespace

    namespace
    {
        template <class L>
        std::vector<L*> pointersTo(std::vector<L>& layers)
        {
            std::vector<L*> order;
            order.reserve(layers.size() + 1);
            for (L& layer : layers)
            {
                order.push_back(&layer);
            }
            return order;
        }

        // `order` is a layer set, the base sheet first; `self` the layer whose tokens are read.
        template <class L>
        TokenScope scopeFor(const std::vector<L*>& order, const sizet self)
        {
            TokenScope scope;
            const auto source = [&](const sizet index)
            {
                const L& layer = *order[index];
                return TokenScope::Source{layer.name.isValid() ? layer.name : InternedString("base"), &layer.vars};
            };
            if (self != 0)
            {
                scope.sources.push_back(source(self));
                scope.ownFirst = true;
            }
            for (sizet index = order.size(); index-- > 1;)
            {
                if (order[index]->theme && index != self)
                {
                    scope.sources.push_back(source(index));
                }
            }
            scope.sources.push_back(source(0));
            return scope;
        }
    } // namespace

    // `named` holds the names any layer gives an element or class entry: an id or class outside it resolves like
    // the invalid one, so the cache keys stay bounded by the sheet, not by the ids the views invent.
    struct UiStyleSheet::Cache
    {
        std::unordered_set<InternedString> named;
        std::unordered_map<StyleKey, UiStyle, StyleKeyHash> styles;
        std::vector<UiTransitionSet> transitions; // the distinct non-empty sets styles name, index plus one
    };

    struct UiStyleSheet::Watcher
    {
        static constexpr auto POLL_INTERVAL = std::chrono::milliseconds(100);
        std::chrono::steady_clock::time_point lastPoll{};
#if defined(SPITE_PLATFORM_WINDOWS)
        static constexpr auto SETTLE = std::chrono::milliseconds(50); // an editor's save is several writes
        std::vector<std::pair<std::filesystem::path, HANDLE>> directories;
        std::chrono::steady_clock::time_point lastChange{};
        bool pending = false;

        ~Watcher() { close(); }

        void close()
        {
            for (const auto& entry : directories)
            {
                FindCloseChangeNotification(entry.second);
            }
            directories.clear();
        }
#else
        std::array<std::filesystem::file_time_type, MAX_LAYERS> observed{};
#endif
    };

    UiStyleSheet::UiStyleSheet() : m_layers(1), m_cache(std::make_unique<Cache>()) {}

    UiStyleSheet::~UiStyleSheet() = default;

    void UiStyleSheet::bump()
    {
        ++m_generation;
        m_cache->styles.clear();
        m_cache->transitions.clear();
        m_cache->named.clear();
        for (const Layer& layer : m_layers)
        {
            for (const auto& entry : layer.content.elements)
            {
                m_cache->named.insert(entry.first);
            }
            for (const auto& entry : layer.content.classes)
            {
                m_cache->named.insert(entry.first);
            }
        }
        armWatch();
    }

    void UiStyleSheet::armWatch()
    {
        if (!m_watcher)
        {
            return;
        }
#if defined(SPITE_PLATFORM_WINDOWS)
        m_watcher->close();
        for (const Layer& layer : m_layers)
        {
            const std::filesystem::path directory = layer.path.parent_path();
            if (layer.path.empty() ||
                std::ranges::any_of(m_watcher->directories,
                                    [&](const auto& entry) { return entry.first == directory; }))
            {
                continue;
            }
            const HANDLE handle = FindFirstChangeNotificationW(
                directory.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
            if (handle == INVALID_HANDLE_VALUE)
            {
                SWARNING_LOG("stylesheet '{}': cannot watch '{}'", layer.path.string(), directory.string());
                continue;
            }
            m_watcher->directories.emplace_back(directory, handle);
        }
#else
        for (sizet index = 0; index < m_layers.size(); ++index)
            m_watcher->observed[index] = m_layers[index].stamp;
#endif
    }

    // Reads the file into layer.pending and sets its vars, source and path; the content parses in replaceLayers,
    // once the layer set its tokens read from is known.
    Expected<void> UiStyleSheet::readLayer(Layer& layer, const std::filesystem::path& path,
                                           const std::span<const UiTokenOverride> overrides)
    {
        std::error_code ignored;
        layer.stamp = std::filesystem::last_write_time(path, ignored);
        layer.path = path;
        auto text = readTextFile(path);
        if (!text)
        {
            return std::unexpected(text.error());
        }
        return parseLayer(layer, *text, overrides);
    }

    Expected<void> UiStyleSheet::parseLayer(Layer& layer, const std::string_view text,
                                            const std::span<const UiTokenOverride> overrides)
    {
        const auto fail = [&](const ErrorCode code, const std::string& message)
        { return std::unexpected(makeError(code, std::format("stylesheet '{}': {}", layer.path.string(), message))); };
        try
        {
            DuplicateKeyFinder finder;
            Json::sax_parse(text, &finder);
            if (!finder.duplicate.empty())
            {
                return fail(ErrorCode::JsonParseFailed, finder.duplicate);
            }
            layer.pending = Json::parse(text);
        }
        catch (const Json::exception& error)
        {
            return fail(ErrorCode::JsonParseFailed, error.what());
        }
        const auto vars = layer.pending.is_object() ? layer.pending.find("vars") : layer.pending.end();
        layer.vars = vars != layer.pending.end() ? *vars : Json{};
        for (const UiTokenOverride& token : overrides)
        {
            if (const auto it =
                    layer.vars.is_object() ? layer.vars.find(std::string(token.name.toView())) : layer.vars.end();
                it != layer.vars.end())
            {
                Json color = Json::array();
                for (u32 channel = 0; channel < 4; ++channel)
                {
                    color.push_back(static_cast<float>((token.color >> (channel * 8)) & 0xFFu) / 255.0f);
                }
                *it = std::move(color);
            }
        }
        layer.source = text;
        return {};
    }

    // The one way the layer set changes: `order` is the set that will exist, base sheet first, as pointers to the
    // layers kept and to the `fresh` ones (read, content not parsed). Every fresh layer parses, and when
    // `tokensMayChange` (a base or theme layer changed) every kept layer that borrowed a token re-parses against
    // the new set; the set is replaced only when all of them resolve, so a failure leaves everything as it was.
    Expected<void> UiStyleSheet::replaceLayers(std::vector<Layer*> order, const std::span<Layer* const> fresh,
                                               const bool tokensMayChange)
    {
        std::vector<std::pair<sizet, Content>> refreshed;
        for (sizet index = 0; index < order.size(); ++index)
        {
            Layer& layer = *order[index];
            const bool isFresh = std::ranges::find(fresh, order[index]) != fresh.end();
            if (!isFresh && !(tokensMayChange && layer.borrows))
            {
                continue;
            }
            const auto fail = [&](const ErrorCode code, const std::string& message)
            {
                return std::unexpected(
                    makeError(code,
                              std::format("stylesheet '{}': {}{}", layer.path.string(), message,
                                          isFresh ? "" : " (re-resolved after a change to the base or theme layers)")));
            };
            try
            {
                TokenScope scope = scopeFor(order, index);
                if (isFresh)
                {
                    layer.content = Content::parse(layer.pending, scope);
                    layer.borrows = scope.borrowed;
                    layer.pending = Json{};
                    if (!layer.borrows)
                    {
                        layer.source = {};
                    }
                }
                else
                {
                    refreshed.emplace_back(index, Content::parse(Json::parse(layer.source), scope));
                }
            }
            catch (const Json::exception& error)
            {
                return fail(ErrorCode::JsonParseFailed, error.what());
            }
            catch (const ParseError& error)
            {
                return fail(ErrorCode::JsonConversionFailed, error.message);
            }
        }
        std::vector<Layer> next;
        next.reserve(order.size());
        for (Layer* layer : order)
        {
            next.push_back(std::move(*layer));
        }
        for (auto& [index, content] : refreshed)
        {
            next[index].content = std::move(content);
        }
        m_layers = std::move(next);
        return {};
    }

    Expected<void> UiStyleSheet::load(const std::filesystem::path& path)
    {
        Layer fresh;
        if (auto read = readLayer(fresh, path); !read)
        {
            return read;
        }
        std::vector<Layer*> order = pointersTo(m_layers);
        order[0] = &fresh;
        Layer* const changed[] = {&fresh};
        if (auto replaced = replaceLayers(std::move(order), changed, true); !replaced)
        {
            return replaced;
        }
        bump();
        return {};
    }

    bool UiStyleSheet::setTextScale(const float scale)
    {
        if (!std::isfinite(scale) || scale <= 0.0f)
        {
            return false;
        }
        const float clamped = std::clamp(scale, MIN_TEXT_SCALE, MAX_TEXT_SCALE);
        if (clamped == m_textScale)
        {
            return false;
        }
        m_textScale = clamped;
        bump();
        return true;
    }

    bool UiStyleSheet::unload()
    {
        Layer fresh;
        fresh.pending = Json::object();
        fresh.vars = Json::object();
        std::vector<Layer*> order = pointersTo(m_layers);
        order[0] = &fresh;
        Layer* const changed[] = {&fresh};
        if (auto replaced = replaceLayers(std::move(order), changed, true); !replaced)
        {
            SWARNING_LOG("{}; keeping the base sheet", replaced.error().message);
            return false;
        }
        bump();
        return true;
    }

    Expected<void> UiStyleSheet::loadLayer(const InternedString name, const std::filesystem::path& path)
    {
        return loadNamedLayer(name, path, std::nullopt);
    }

    Expected<void> UiStyleSheet::loadLayerText(const InternedString name, const std::string_view text)
    {
        return loadNamedLayer(name, {}, text);
    }

    Expected<void> UiStyleSheet::loadNamedLayer(const InternedString name, const std::filesystem::path& path,
                                                const std::optional<std::string_view> text)
    {
        const auto existing =
            std::find_if(m_layers.begin() + 1, m_layers.end(), [&](const Layer& layer) { return layer.name == name; });
        if (existing == m_layers.end() && m_layers.size() == MAX_LAYERS)
        {
            return std::unexpected(
                makeError(ErrorCode::JsonConversionFailed,
                          std::format("stylesheet '{}': at most {} layers", path.string(), MAX_LAYERS)));
        }
        Layer fresh;
        fresh.name = name;
        fresh.theme = existing != m_layers.end() && existing->theme;
        if (auto read = text ? parseLayer(fresh, *text, {}) : readLayer(fresh, path); !read)
        {
            return read;
        }
        std::vector<Layer*> order = pointersTo(m_layers);
        if (existing != m_layers.end())
        {
            order[static_cast<sizet>(existing - m_layers.begin())] = &fresh;
        }
        else
        {
            order.push_back(&fresh);
        }
        Layer* const changed[] = {&fresh};
        if (auto replaced = replaceLayers(std::move(order), changed, fresh.theme); !replaced)
        {
            return replaced;
        }
        bump();
        return {};
    }

    bool UiStyleSheet::unloadLayer(const InternedString name)
    {
        const auto layer = std::find_if(m_layers.begin() + 1, m_layers.end(),
                                        [&](const Layer& candidate) { return candidate.name == name; });
        if (layer == m_layers.end())
        {
            return false;
        }
        std::vector<Layer*> order = pointersTo(m_layers);
        order.erase(order.begin() + (layer - m_layers.begin()));
        if (auto replaced = replaceLayers(std::move(order), {}, layer->theme); !replaced)
        {
            SWARNING_LOG("{}; keeping layer '{}'", replaced.error().message, name.toView());
            return false;
        }
        bump();
        return true;
    }

    Expected<void> UiStyleSheet::setTheme(const InternedString name, const std::span<const std::filesystem::path> paths,
                                          const UiThemeDensity& density)
    {
        return setTheme(name, paths, density, std::vector<UiTokenOverride>(m_overrides));
    }

    Expected<void> UiStyleSheet::setTheme(const InternedString name, const std::span<const std::filesystem::path> paths,
                                          const UiThemeDensity& density,
                                          const std::span<const UiTokenOverride> overrides)
    {
        const auto validName = [](const std::string_view text)
        {
            return !text.empty() &&
                std::ranges::all_of(text, [](const char c)
                                    { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'; });
        };
        if (!validName(name.toView()))
        {
            return std::unexpected(
                makeError(ErrorCode::JsonConversionFailed,
                          std::format("theme name '{}': only A-Z a-z 0-9 _ - are allowed", name.toView())));
        }
        const bool densityLayer = !density.file.empty();
        if (density.name.isValid() ? !validName(density.name.toView()) : densityLayer)
        {
            return std::unexpected(
                makeError(ErrorCode::JsonConversionFailed,
                          std::format("density name '{}': only A-Z a-z 0-9 _ - are allowed", density.name.toView())));
        }
        const auto others = static_cast<sizet>(std::ranges::count(m_layers, false, &Layer::theme));
        const sizet count = paths.size() + (densityLayer ? 1 : 0);
        if (paths.empty() || others + count > MAX_LAYERS)
        {
            return std::unexpected(
                makeError(ErrorCode::JsonConversionFailed,
                          std::format("theme '{}': needs 1 to {} layer files", name.toView(), MAX_LAYERS - others)));
        }
        std::vector<Layer> incoming(count);
        std::vector<Layer*> order;
        for (Layer& layer : m_layers)
        {
            if (!layer.theme)
            {
                order.push_back(&layer);
            }
        }
        std::vector<Layer*> changed;
        for (sizet i = 0; i < count; ++i)
        {
            const bool isDensity = i >= paths.size();
            incoming[i].name = InternedString(isDensity               ? std::format("density.{}", density.name.toView())
                                                  : paths.size() == 1 ? std::format("theme.{}", name.toView())
                                                                      : std::format("theme.{}.{}", name.toView(), i));
            incoming[i].theme = true;
            if (auto read = readLayer(incoming[i], isDensity ? density.file : paths[i], overrides); !read)
            {
                return read;
            }
            order.push_back(&incoming[i]);
            changed.push_back(&incoming[i]);
        }
        if (auto replaced = replaceLayers(std::move(order), changed, true); !replaced)
        {
            return replaced;
        }
        m_theme = name;
        m_density = density.name.isValid() ? density.name : InternedString(DEFAULT_UI_DENSITY);
        m_overrides.assign(overrides.begin(), overrides.end());
        bump();
        return {};
    }

    void UiStyleSheet::watch(const bool enabled)
    {
        if (enabled != (m_watcher != nullptr))
        {
            m_watcher = enabled ? std::make_unique<Watcher>() : nullptr;
            armWatch();
        }
    }

    bool UiStyleSheet::pollWatch()
    {
        Watcher* watcher = m_watcher.get();
        if (!watcher)
        {
            return false;
        }
#if defined(SPITE_PLATFORM_WINDOWS)
        if (watcher->directories.empty())
        {
            return false;
        }
#endif
        const auto now = std::chrono::steady_clock::now();
        if (now - watcher->lastPoll < Watcher::POLL_INTERVAL)
        {
            return false;
        }
        watcher->lastPoll = now;
#if defined(SPITE_PLATFORM_WINDOWS)
        // Bounded drain: a deleted directory leaves its handle signalled and FindNextChangeNotification failing;
        // that handle is closed and dropped, so the loop cannot spin.
        for (u32 drained = 0; drained < 2 * MAX_LAYERS && !watcher->directories.empty(); ++drained)
        {
            std::array<HANDLE, MAX_LAYERS> handles;
            const auto count = static_cast<DWORD>(std::min(watcher->directories.size(), handles.size()));
            for (DWORD i = 0; i < count; ++i)
            {
                handles[i] = watcher->directories[i].second;
            }
            const DWORD signaled = WaitForMultipleObjects(count, handles.data(), FALSE, 0);
            if (signaled >= count)
            {
                break;
            }
            watcher->pending = true;
            watcher->lastChange = now;
            if (!FindNextChangeNotification(handles[signaled]))
            {
                SWARNING_LOG("stylesheet watch: '{}' is gone; no longer watched",
                             watcher->directories[signaled].first.string());
                FindCloseChangeNotification(handles[signaled]);
                watcher->directories.erase(watcher->directories.begin() + signaled);
            }
        }
        if (!watcher->pending || now - watcher->lastChange < Watcher::SETTLE)
        {
            return false;
        }
        watcher->pending = false;
#else
        bool stillChanging = false;
#endif
        struct Changed
        {
            sizet index;
            std::filesystem::file_time_type stamp;
            std::string failure;
        };
        std::vector<Changed> changedOnDisk;
        for (sizet index = 0; index < m_layers.size(); ++index)
        {
            const Layer& layer = m_layers[index];
            std::error_code error;
            const auto stamp = layer.path.empty() ? layer.stamp : std::filesystem::last_write_time(layer.path, error);
#if !defined(SPITE_PLATFORM_WINDOWS)
            if (!error && std::exchange(watcher->observed[index], stamp) != stamp && stamp != layer.stamp)
                stillChanging = true;
#endif
            if (!error && stamp != layer.stamp)
            {
                changedOnDisk.push_back({index, stamp, {}});
            }
        }
#if !defined(SPITE_PLATFORM_WINDOWS)
        // Wait for two matching observations before parsing files that a save may still be changing.
        if (stillChanging)
            return false;
#endif
        const auto reload = [this](const sizet index) -> Expected<void>
        {
            const Layer& layer = m_layers[index];
            Layer fresh;
            fresh.name = layer.name;
            fresh.theme = layer.theme;
            const bool tokensMayChange = index == 0 || layer.theme;
            std::vector<Layer*> order = pointersTo(m_layers);
            order[index] = &fresh;
            Layer* const changed[] = {&fresh};
            auto loaded = readLayer(fresh, layer.path,
                                    layer.theme ? std::span<const UiTokenOverride>(m_overrides)
                                                : std::span<const UiTokenOverride>());
            return loaded ? replaceLayers(std::move(order), changed, tokensMayChange) : loaded;
        };
        // A layer can fail only because of the order of this pass (the base file uses a token a theme file, edited in
        // the same poll, defines): the failed ones are tried again while a pass still reloads a layer.
        bool reloaded = false;
        for (bool progress = true; progress && !changedOnDisk.empty();)
        {
            progress = false;
            std::erase_if(changedOnDisk,
                          [&](Changed& entry)
                          {
                              auto loaded = reload(entry.index);
                              if (!loaded)
                              {
                                  entry.failure = loaded.error().message;
                                  return false;
                              }
                              progress = reloaded = true;
                              return true;
                          });
        }
        for (const Changed& failed : changedOnDisk)
        {
            SWARNING_LOG("{}; keeping the previous content", failed.failure);
            m_layers[failed.index].stamp = failed.stamp; // not retried until the file changes again
        }
        if (reloaded)
        {
            bump();
        }
        return reloaded;
    }

    bool UiStyleSheet::hasLayer(const InternedString name) const
    {
        return std::any_of(m_layers.begin() + 1, m_layers.end(),
                           [&](const Layer& layer) { return layer.name == name; });
    }

    template <bool Trace>
    UiStyle UiStyleSheet::resolveImpl(const InternedString id, const UiWidgetKind kind, const UiInteractionState state,
                                      const InternedString styleClass, UiStyleOrigins* origins) const
    {
        // Most specific level first; within a level the topmost layer first.
        std::array<const StyleBlock*, CHAIN_PER_LAYER * MAX_LAYERS> chain{};
        [[maybe_unused]] std::conditional_t<Trace, std::array<UiStyleOrigin, CHAIN_PER_LAYER * MAX_LAYERS>, char>
            origin{};
        u32 count = 0;
        const auto add = [&](const StyleBlock& block, [[maybe_unused]] const UiStyleLevel level,
                             [[maybe_unused]] const u32 layer, [[maybe_unused]] const bool variant)
        {
            if constexpr (Trace)
            {
                origin[count] = {level, static_cast<u8>(layer), variant};
            }
            chain[count++] = &block;
        };
        const auto pushLevel = [&](const UiStyleLevel level, const auto& select)
        {
            for (u32 layer = static_cast<u32>(m_layers.size()); layer-- > 0;)
            {
                if (const StyleEntry* entry = select(m_layers[layer].content))
                {
                    if (state != UiInteractionState::Normal)
                    {
                        add(entry->states[static_cast<u8>(state)], level, layer, true);
                    }
                    add(entry->base, level, layer, false);
                }
            }
        };
        const auto lookup = [](const auto& map, const InternedString key) -> const StyleEntry*
        {
            const auto it = map.find(key);
            return it == map.end() ? nullptr : &it->second;
        };
        pushLevel(UiStyleLevel::Element, [&](const Content& content) { return lookup(content.elements, id); });
        if (styleClass.isValid())
        {
            // a class list ("<family>.label dropdown.label", a composite widget's part) reads its names in order
            const std::string_view names = styleClass.toView();
            u32 listed = 0;
            for (sizet from = 0; from < names.size() && listed < MAX_CLASS_LIST;)
            {
                const sizet end = std::min(names.find(' ', from), names.size());
                if (end > from)
                {
                    const InternedString name =
                        end - from == names.size() ? styleClass : InternedString(names.substr(from, end - from));
                    pushLevel(UiStyleLevel::Class,
                              [&](const Content& content) { return lookup(content.classes, name); });
                    ++listed;
                }
                from = end + 1;
            }
        }
        pushLevel(UiStyleLevel::Kind, [&](const Content& content) { return &content.kinds[static_cast<u8>(kind)]; });
        for (u32 layer = static_cast<u32>(m_layers.size()); layer-- > 0;)
        {
            add(m_layers[layer].content.defaults, UiStyleLevel::Defaults, layer, false);
        }

        UiStyle style;
        const auto pick = [&]([[maybe_unused]] const UiStyleField fieldId, auto StyleBlock::* field, auto& out)
        {
            for (u32 i = 0; i < count; ++i)
            {
                if (const auto& value = chain[i]->*field)
                {
                    out = *value;
                    if constexpr (Trace)
                    {
                        (*origins)[static_cast<u8>(fieldId)] = origin[i];
                        (*origins)[static_cast<u8>(fieldId)].token = chain[i]->tokens[static_cast<u8>(fieldId)];
                    }
                    return;
                }
            }
        };
        pick(UiStyleField::BackgroundColor, &StyleBlock::backgroundColor, style.backgroundColor);
        pick(UiStyleField::BackgroundImage, &StyleBlock::backgroundImage, style.backgroundImage);
        pick(UiStyleField::BackgroundTint, &StyleBlock::backgroundTint, style.backgroundTint);
        pick(UiStyleField::CornerRadius, &StyleBlock::cornerRadius, style.cornerRadius);
        pick(UiStyleField::Font, &StyleBlock::font, style.font);
        pick(UiStyleField::TextSize, &StyleBlock::textSize, style.textSize);
        style.textSize *= m_textScale;
        pick(UiStyleField::TextColor, &StyleBlock::textColor, style.textColor);
        pick(UiStyleField::TextAlign, &StyleBlock::textAlign, style.textAlign);
        pick(UiStyleField::TextWrap, &StyleBlock::textWrap, style.textWrap);
        pick(UiStyleField::TextOverflow, &StyleBlock::textOverflow, style.textOverflow);
        pick(UiStyleField::TextVAlign, &StyleBlock::textVAlign, style.textVAlign);
        pick(UiStyleField::TextMaxLines, &StyleBlock::textMaxLines, style.textMaxLines);
        pick(UiStyleField::TextLineHeight, &StyleBlock::textLineHeight, style.textLineHeight);
        pick(UiStyleField::TextLetterSpacing, &StyleBlock::textLetterSpacing, style.textLetterSpacing);
        pick(UiStyleField::TextTransform, &StyleBlock::textTransform, style.textTransform);
        pick(UiStyleField::FontWeight, &StyleBlock::fontWeight, style.fontWeight);
        pick(UiStyleField::Icon, &StyleBlock::icon, style.icon);
        pick(UiStyleField::CaretColor, &StyleBlock::caretColor, style.caretColor);
        pick(UiStyleField::SelectionColor, &StyleBlock::selectionColor, style.selectionColor);
        pick(UiStyleField::PlaceholderColor, &StyleBlock::placeholderColor, style.placeholderColor);
        pick(UiStyleField::BorderWidth, &StyleBlock::borderWidth, style.borderWidth);
        pick(UiStyleField::BorderColor, &StyleBlock::borderColor, style.borderColor);
        pick(UiStyleField::GradientColor, &StyleBlock::gradientColor, style.gradientColor);
        pick(UiStyleField::GradientAngle, &StyleBlock::gradientAngle, style.gradientAngle);
        pick(UiStyleField::ShadowColor, &StyleBlock::shadowColor, style.shadows[0].color);
        pick(UiStyleField::ShadowOffset, &StyleBlock::shadowOffset, style.shadows[0].offset);
        pick(UiStyleField::ShadowBlur, &StyleBlock::shadowBlur, style.shadows[0].blur);
        pick(UiStyleField::ShadowSpread, &StyleBlock::shadowSpread, style.shadows[0].spread);
        pick(UiStyleField::ShadowInset, &StyleBlock::shadowInset, style.shadows[0].inset);
        std::array<UiShadow, UI_MAX_SHADOWS - 1> layers{};
        pick(UiStyleField::ShadowLayers, &StyleBlock::shadowLayers, layers);
        std::ranges::copy(layers, style.shadows.begin() + 1);
        pick(UiStyleField::Opacity, &StyleBlock::opacity, style.opacity);
        pick(UiStyleField::OutlineWidth, &StyleBlock::outlineWidth, style.outlineWidth);
        pick(UiStyleField::OutlineColor, &StyleBlock::outlineColor, style.outlineColor);
        pick(UiStyleField::OutlineOffset, &StyleBlock::outlineOffset, style.outlineOffset);
        for (u32 i = 0; i < count; ++i)
        {
            if (const auto& set = chain[i]->transition)
            {
                const bool moves = std::ranges::any_of(*set, [](const UiTransitionSpec& spec)
                                                       { return spec.duration > 0.0f || spec.delay > 0.0f; });
                if (moves)
                {
                    auto& sets = m_cache->transitions;
                    auto known = std::ranges::find(sets, *set);
                    if (known == sets.end())
                    {
                        known = sets.insert(sets.end(), *set);
                    }
                    style.transition = static_cast<u16>(known - sets.begin() + 1);
                }
                if constexpr (Trace)
                {
                    (*origins)[static_cast<u8>(UiStyleField::Transition)] = origin[i];
                }
                break;
            }
        }
        return style;
    }

    UiStyle UiStyleSheet::resolve(InternedString id, const UiWidgetKind kind, const UiInteractionState state,
                                  InternedString styleClass) const
    {
        Cache& cache = *m_cache;
        if (id.isValid() && !cache.named.contains(id))
        {
            id = {};
        }
        if (styleClass.isValid() && !cache.named.contains(styleClass) &&
            styleClass.toView().find(' ') == std::string_view::npos)
        {
            styleClass = {};
        }
        const StyleKey key{id.id(), styleClass.id(), static_cast<u8>(kind), static_cast<u8>(state)};
        if (const auto it = cache.styles.find(key); it != cache.styles.end())
        {
            return it->second;
        }
        const UiStyle style = resolveImpl<false>(id, kind, state, styleClass, nullptr);
        cache.styles.emplace(key, style);
        return style;
    }

    UiStyle UiStyleSheet::explain(const InternedString id, const UiWidgetKind kind, const UiInteractionState state,
                                  const InternedString styleClass, UiStyleOrigins& origins) const
    {
        origins = {};
        return resolveImpl<true>(id, kind, state, styleClass, &origins);
    }

    UiTransitionSet UiStyleSheet::transitionSet(const u16 transition) const
    {
        return transition == 0 || transition > m_cache->transitions.size() ? UiTransitionSet{}
                                                                           : m_cache->transitions[transition - 1u];
    }

    std::optional<UiStyleTokenOrigin> UiStyleSheet::tokenOrigin(const UiStyleOrigin& origin) const
    {
        if (!origin.token.isValid() || origin.layer >= m_layers.size())
        {
            return std::nullopt;
        }
        const auto& used = m_layers[origin.layer].content.used;
        const auto it = used.find(origin.token);
        if (it == used.end())
        {
            return std::nullopt;
        }
        const auto& chain = it->second.chain;
        const bool overridden = !chain.empty() &&
            std::ranges::any_of(m_overrides,
                                [&](const UiTokenOverride& token) { return token.name == chain.back(); }) &&
            std::ranges::any_of(m_layers,
                                [&](const Layer& layer) { return layer.theme && layer.name == it->second.layer; });
        return UiStyleTokenOrigin{it->second.layer, chain, overridden};
    }

    template <class T, class Pick>
    std::unordered_map<InternedString, T> UiStyleSheet::tokensWhere(const Pick& pick) const
    {
        std::vector<const Layer*> order;
        for (const Layer& layer : m_layers)
        {
            order.push_back(&layer);
        }
        TokenScope scope = scopeFor(order, 0);
        std::unordered_map<InternedString, T> tokens;
        for (const TokenScope::Source& source : scope.sources)
        {
            if (!source.vars || !source.vars->is_object())
            {
                continue;
            }
            for (const auto& item : source.vars->items())
            {
                const InternedString name(item.key());
                if (tokens.contains(name))
                {
                    continue; // a higher source defines it
                }
                try
                {
                    TokenOrigin origin;
                    if (const std::optional<T> value = pick(resolveToken(scope, item.key(), "vars", {}, origin)))
                    {
                        tokens.emplace(name, *value);
                    }
                }
                catch (const ParseError&)
                {
                    // an alias a loaded layer never reads may dangle: it publishes nothing
                }
            }
        }
        return tokens;
    }

    std::unordered_map<InternedString, float> UiStyleSheet::numericTokens() const
    {
        return tokensWhere<float>([](const Json& value) -> std::optional<float>
                                  { return value.is_number() ? std::optional(value.get<float>()) : std::nullopt; });
    }

    std::unordered_map<InternedString, u32> UiStyleSheet::colorTokens() const
    {
        return tokensWhere<u32>(
            [](const Json& value) -> std::optional<u32>
            {
                return value.is_array() && value.size() == 4 ? std::optional(parseColor(value, "vars")) : std::nullopt;
            });
    }

    sizet UiStyleSheet::cachedStyleCount() const { return m_cache->styles.size(); }

    std::vector<UiStyleLayerInfo> UiStyleSheet::layers() const
    {
        std::vector<UiStyleLayerInfo> infos;
        infos.reserve(m_layers.size());
        for (const Layer& layer : m_layers)
        {
            UiStyleLayerInfo& info = infos.emplace_back();
            info.name = layer.name.isValid() ? layer.name : InternedString("base");
            info.path = layer.path;
            info.elementCount = static_cast<u32>(layer.content.elements.size());
            info.tokens = layer.content.vars;
            info.theme = layer.theme;
            for (const auto& entry : layer.content.classes)
            {
                info.classes.push_back(entry.first);
            }
            std::ranges::sort(info.classes, {}, &InternedString::toView);
            for (const auto& [name, kind] : KIND_NAMES)
            {
                if (hasAnyField(layer.content.kinds[static_cast<u8>(kind)]))
                {
                    info.kinds.push_back(name);
                }
            }
        }
        return infos;
    }

    std::optional<InternedString> UiStyleSheet::imageFor(const u64 imageSlot) const
    {
        for (auto layer = m_layers.rbegin(); layer != m_layers.rend(); ++layer)
        {
            if (const auto it = layer->content.images.find(imageSlot); it != layer->content.images.end())
            {
                return it->second;
            }
        }
        return std::nullopt;
    }

    InternedString UiStyleSheet::fontFaceFor(const InternedString font, const u16 weight) const
    {
        for (auto layer = m_layers.rbegin(); layer != m_layers.rend(); ++layer)
        {
            const auto it = layer->content.fontWeights.find(font);
            if (it == layer->content.fontWeights.end())
            {
                continue;
            }
            InternedString best{};
            int bestDistance = 0;
            for (const auto& [declared, face] : it->second)
            {
                const int distance = std::abs(static_cast<int>(declared) - static_cast<int>(weight));
                if (!best.isValid() || distance < bestDistance || (distance == bestDistance && declared > weight))
                {
                    best = face;
                    bestDistance = distance;
                }
            }
            if (best.isValid())
            {
                return best;
            }
        }
        return {};
    }

    std::span<const InternedString> UiStyleSheet::fontFallbacksFor(const InternedString font) const
    {
        for (auto layer = m_layers.rbegin(); layer != m_layers.rend(); ++layer)
        {
            if (const auto it = layer->content.fontFallbacks.find(font); it != layer->content.fontFallbacks.end())
            {
                return it->second;
            }
        }
        return {};
    }
} // namespace spite
