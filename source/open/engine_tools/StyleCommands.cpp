#include "engine/StyleCommands.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <format>
#include <string>
#include <span>
#include <string_view>
#include <vector>

#include "base/ServiceLocator.hpp"
#include "control/ControlServer.hpp"
#include "ecs/World.hpp"
#include "serialization/FormatWalk.hpp"
#include "ui/UiComponents.hpp"
#include "uiview/UiViewContext.hpp"
#include "base/ContentRoot.hpp"
#include "base/io/FileSystem.hpp"

namespace spite
{
    namespace
    {
        constexpr ArgDesc STYLE_WORLD_ARG =
            optionalArg("world", ArgKind::String, "the world whose UiView to act on", "the first UI world, else scene");
        constexpr std::array<std::string_view, 5> LEVEL_NAMES = {"element", "class", "kind", "defaults", "builtIn"};
        constexpr std::string_view BASE_LAYER = "base";
        constexpr const char* THEME_ROOT = "assets/ui/themes"; // one <name>.json layer file per theme
        constexpr const char* DENSITY_ROOT = "assets/ui/density"; // one <name>.json token layer per density

        std::filesystem::path themePath(const std::string& name)
        {
            return resolveContentPath(THEME_ROOT) / pathFromUtf8(name + ".json");
        }

        std::string uiView(const ControlServer& control, const ControlArgs& args, const DefaultUiWorldFn& fallback,
                           UiViewContext*& out, const World*& world)
        {
            const std::string name = args.string("world").value_or(fallback());
            world = control.world(name);
            if (!world)
            {
                return control.unknownWorld(name);
            }
            if (!world->services().contains<UiViewContext>())
            {
                return "world '" + name + "' has no UiView";
            }
            out = &world->services().get<UiViewContext>();
            return {};
        }

        std::string colorText(const u32 rgba)
        {
            return std::format("#{:02x}{:02x}{:02x}{:02x}", rgba & 0xFF, (rgba >> 8) & 0xFF, (rgba >> 16) & 0xFF,
                               rgba >> 24);
        }

        std::string_view alignName(const UiTextAlign align)
        {
            return align == UiTextAlign::Center ? "center" : align == UiTextAlign::Right ? "right" : "left";
        }

        std::string_view vAlignName(const UiTextVAlign align)
        {
            return align == UiTextVAlign::Top ? "top" : align == UiTextVAlign::Bottom ? "bottom" : "center";
        }
    } // namespace

    static void writeStyleAnim(IFormatWriter& out, const std::string_view key, const UiStyleAnimComponent& anim)
    {
        const auto packed = [](const Vec4& color)
        {
            const auto channel = [](const float value)
            { return static_cast<u32>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
            return channel(color.x) | (channel(color.y) << 8) | (channel(color.z) << 16) | (channel(color.w) << 24);
        };
        const auto color = [&](const std::string_view name, const Vec4& displayed, const Vec4& target)
        {
            out.beginObject(name);
            out.writeString("value", colorText(packed(displayed)));
            out.writeString("target", colorText(packed(target)));
            out.endObject();
        };
        const auto scalar = [&](const std::string_view name, const float displayed, const float target)
        {
            out.beginObject(name);
            out.writeF32("value", displayed);
            out.writeF32("target", target);
            out.endObject();
        };
        out.beginObject(key);
        color("backgroundColor", anim.backgroundColor, anim.targetBackgroundColor);
        color("textColor", anim.textColor, anim.targetTextColor);
        color("borderColor", anim.borderColor, anim.targetBorderColor);
        scalar("borderWidth", anim.borderWidth, anim.targetBorderWidth);
        scalar("opacity", anim.opacity, anim.targetOpacity);
        // the shadow stack, entries 0.. up to the last that shows or will show something
        u32 shadows = 0;
        for (u32 i = 0; i < UI_MAX_SHADOWS; ++i)
        {
            shadows = anim.shadowColor[i].w > 0.0f || anim.targetShadowColor[i].w > 0.0f ? i + 1 : shadows;
        }
        out.beginArray("shadows", shadows);
        for (u32 i = 0; i < shadows; ++i)
        {
            out.beginObject("");
            color("color", anim.shadowColor[i], anim.targetShadowColor[i]);
            scalar("offsetX", anim.shadowOffset[i].x, anim.targetShadowOffset[i].x);
            scalar("offsetY", anim.shadowOffset[i].y, anim.targetShadowOffset[i].y);
            scalar("blur", anim.shadowBlur[i], anim.targetShadowBlur[i]);
            scalar("spread", anim.shadowSpread[i], anim.targetShadowSpread[i]);
            out.writeBool("inset", ((anim.shadowInset >> i) & 1u) != 0);
            out.endObject();
        }
        out.endArray();
        color("outlineColor", anim.outlineColor, anim.targetOutlineColor);
        scalar("outlineWidth", anim.outlineWidth, anim.targetOutlineWidth);
        scalar("outlineOffset", anim.outlineOffset, anim.targetOutlineOffset);
        scalar("cornerRadiusTopLeft", anim.cornerRadius.x, anim.targetCornerRadius.x);
        scalar("cornerRadiusTopRight", anim.cornerRadius.y, anim.targetCornerRadius.y);
        scalar("cornerRadiusBottomRight", anim.cornerRadius.z, anim.targetCornerRadius.z);
        scalar("cornerRadiusBottomLeft", anim.cornerRadius.w, anim.targetCornerRadius.w);
        out.endObject();
    }

    std::vector<std::string> availableUiThemes()
    {
        std::vector<std::string> names;
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(resolveContentPath(THEME_ROOT), ignored))
        {
            if (entry.path().extension() == ".json")
            {
                names.push_back(utf8Path(entry.path().stem()));
            }
        }
        std::ranges::sort(names);
        return names;
    }

    std::filesystem::path uiThemeFile(const std::string& name) { return themePath(name); }

    std::vector<std::string> availableUiDensities()
    {
        std::vector<std::string> names{std::string(DEFAULT_UI_DENSITY)};
        std::error_code ignored;
        for (const auto& entry : std::filesystem::directory_iterator(resolveContentPath(DENSITY_ROOT), ignored))
        {
            if (entry.path().extension() == ".json")
            {
                if (std::string stem = utf8Path(entry.path().stem()); stem != DEFAULT_UI_DENSITY)
                {
                    names.push_back(std::move(stem));
                }
            }
        }
        std::ranges::sort(names);
        return names;
    }

    UiThemeDensity uiThemeDensity(const std::string& name)
    {
        if (name == DEFAULT_UI_DENSITY)
        {
            return {InternedString(name), {}};
        }
        return {InternedString(name), resolveContentPath(DENSITY_ROOT) / pathFromUtf8(name + ".json")};
    }

    void registerStyleCommands(ControlServer& control, DefaultUiWorldFn defaultWorld)
    {
        control.registerQuery(
            "uiStyle", "engine",
            "the style a UI element resolves to (element > class > kind > defaults > built-in, the topmost layer per "
            "level) with the level, layer and \"$token\" (when the value was written as one, with the layer defining "
            "it "
            "and the alias chain; tokenOverridden: true when an editor setting such as dataColor replaced the theme "
            "file's value) each field came from, and the active theme; colors are #rrggbbaa; text inputs read "
            "caretColor, "
            "selectionColor and placeholderColor (the \"text\" block's placeholderColor colors "
            "TextInput::setPlaceholder)",
            {requiredArg("entity", ArgKind::Entity, "a UI element"),
             optionalArg("state", ArgKind::String, "normal, focused, hover, pressed or disabled", "normal"),
             STYLE_WORLD_ARG},
            [&control, defaultWorld](const ControlArgs& args, IFormatWriter& out)
            {
                UiViewContext* view = nullptr;
                const World* world = nullptr;
                if (std::string error = uiView(control, args, defaultWorld, view, world); !error.empty())
                {
                    return error;
                }
                const std::optional<u64> id = args.integer("entity");
                const Entity entity(id.value_or(0));
                const auto* identity = id && world->isEntityAlive(entity)
                    ? world->tryGetConstComponent<UiIdentityComponent>(entity)
                    : nullptr;
                if (!identity)
                {
                    return "entity " + std::to_string(id.value_or(0)) + " is not a live UI element";
                }
                const std::string stateName = args.string("state").value_or("normal");
                const auto state = parseUiInteractionState(stateName);
                if (!state)
                {
                    return std::string("state is normal, focused, hover, pressed or disabled");
                }

                UiStyleOrigins origins;
                const UiStyleSheet& sheet = view->stylesheetFor(entity);
                const UiStyle style =
                    sheet.explain(identity->id, identity->kind, *state, identity->styleClass, origins);
                const std::vector<UiStyleLayerInfo> layers = sheet.layers();
                out.writeString("id", identity->id.toView());
                out.writeString("kind", uiWidgetKindName(identity->kind));
                out.writeString("styleClass", identity->styleClass.toView());
                out.writeString("state", stateName);
                out.writeString("theme", sheet.theme().toView());
                out.beginObject("fields");
                const auto field = [&](const UiStyleField id, const auto& write)
                {
                    const auto index = static_cast<u8>(id);
                    out.beginObject(UI_STYLE_FIELD_NAMES[index]);
                    write();
                    const UiStyleOrigin& origin = origins[index];
                    out.writeString("level", LEVEL_NAMES[static_cast<u8>(origin.level)]);
                    if (origin.level != UiStyleLevel::BuiltIn)
                    {
                        out.writeString("layer", layers[origin.layer].name.toView());
                        out.writeBool("stateVariant", origin.stateVariant);
                        if (origin.token.isValid())
                        {
                            out.writeString("token", origin.token.toView());
                            if (const auto token = sheet.tokenOrigin(origin))
                            {
                                out.writeString("tokenLayer", token->layer.toView());
                                if (token->overridden)
                                {
                                    out.writeBool("tokenOverridden", true);
                                }
                                if (token->chain.size() > 1)
                                {
                                    out.beginArray("tokenChain", static_cast<u32>(token->chain.size()));
                                    for (const InternedString& name : token->chain)
                                    {
                                        out.writeString("", name.toView());
                                    }
                                    out.endArray();
                                }
                            }
                        }
                    }
                    out.endObject();
                };
                field(UiStyleField::BackgroundColor,
                      [&] { out.writeString("value", colorText(style.backgroundColor)); });
                field(UiStyleField::BackgroundImage, [&] { out.writeString("value", style.backgroundImage.toView()); });
                field(UiStyleField::BackgroundTint, [&] { out.writeString("value", colorText(style.backgroundTint)); });
                field(UiStyleField::CornerRadius,
                      [&]
                      {
                          out.beginArray("value", 4); // top-left, top-right, bottom-right, bottom-left
                          for (u32 corner = 0; corner < 4; ++corner)
                          {
                              out.writeF32("", style.cornerRadius[corner]);
                          }
                          out.endArray();
                      });
                field(UiStyleField::Font, [&] { out.writeString("value", style.font.toView()); });
                field(UiStyleField::TextSize, [&] { out.writeF32("value", style.textSize); });
                field(UiStyleField::TextColor, [&] { out.writeString("value", colorText(style.textColor)); });
                field(UiStyleField::TextAlign, [&] { out.writeString("value", alignName(style.textAlign)); });
                field(UiStyleField::TextWrap,
                      [&]
                      {
                          out.writeString("value",
                                          style.textWrap == UiTextWrap::Word        ? "word"
                                              : style.textWrap == UiTextWrap::Lines ? "lines"
                                                                                    : "none");
                      });
                field(UiStyleField::TextOverflow,
                      [&]
                      {
                          out.writeString("value",
                                          style.textOverflow == UiTextOverflow::Ellipsis             ? "ellipsis"
                                              : style.textOverflow == UiTextOverflow::MiddleEllipsis ? "middleEllipsis"
                                                                                                     : "clip");
                      });
                field(UiStyleField::TextVAlign, [&] { out.writeString("value", vAlignName(style.textVAlign)); });
                field(UiStyleField::TextMaxLines, [&] { out.writeU32("value", style.textMaxLines); });
                field(UiStyleField::TextLineHeight, [&] { out.writeF32("value", style.textLineHeight); });
                field(UiStyleField::TextLetterSpacing, [&] { out.writeF32("value", style.textLetterSpacing); });
                field(UiStyleField::TextTransform,
                      [&]
                      {
                          out.writeString("value",
                                          style.textTransform == UiTextTransform::Uppercase       ? "uppercase"
                                              : style.textTransform == UiTextTransform::Lowercase ? "lowercase"
                                                                                                  : "none");
                      });
                field(UiStyleField::FontWeight, [&] { out.writeU32("value", style.fontWeight); });
                field(UiStyleField::Icon, [&] { out.writeString("value", style.icon.toView()); });
                field(UiStyleField::CaretColor, [&] { out.writeString("value", colorText(style.caretColor)); });
                field(UiStyleField::SelectionColor, [&] { out.writeString("value", colorText(style.selectionColor)); });
                field(UiStyleField::PlaceholderColor,
                      [&] { out.writeString("value", colorText(style.placeholderColor)); });
                field(UiStyleField::BorderWidth, [&] { out.writeF32("value", style.borderWidth); });
                field(UiStyleField::BorderColor, [&] { out.writeString("value", colorText(style.borderColor)); });
                field(UiStyleField::GradientColor, [&]
                      { out.writeString("value", style.gradientColor ? colorText(*style.gradientColor) : "none"); });
                field(UiStyleField::GradientAngle, [&] { out.writeF32("value", style.gradientAngle); });
                // ShadowColor..ShadowInset are entry 0 of the shadow stack, ShadowLayers its entries 1..3 up to the
                // last one set
                field(UiStyleField::ShadowColor, [&] { out.writeString("value", colorText(style.shadows[0].color)); });
                field(UiStyleField::ShadowOffset,
                      [&]
                      {
                          out.beginObject("value");
                          out.writeF32("x", style.shadows[0].offset.x);
                          out.writeF32("y", style.shadows[0].offset.y);
                          out.endObject();
                      });
                field(UiStyleField::ShadowBlur, [&] { out.writeF32("value", style.shadows[0].blur); });
                field(UiStyleField::ShadowSpread, [&] { out.writeF32("value", style.shadows[0].spread); });
                field(UiStyleField::ShadowInset, [&] { out.writeBool("value", style.shadows[0].inset); });
                field(UiStyleField::ShadowLayers,
                      [&]
                      {
                          u32 layers = 0;
                          for (u32 i = 1; i < UI_MAX_SHADOWS; ++i)
                          {
                              layers = style.shadows[i] != UiShadow{} ? i : layers;
                          }
                          out.beginArray("value", layers);
                          for (u32 i = 1; i <= layers; ++i)
                          {
                              const UiShadow& shadow = style.shadows[i];
                              out.beginObject("");
                              out.writeString("color", colorText(shadow.color));
                              out.writeF32("offsetX", shadow.offset.x);
                              out.writeF32("offsetY", shadow.offset.y);
                              out.writeF32("blur", shadow.blur);
                              out.writeF32("spread", shadow.spread);
                              out.writeBool("inset", shadow.inset);
                              out.endObject();
                          }
                          out.endArray();
                      });
                field(UiStyleField::Opacity, [&] { out.writeF32("value", style.opacity); });
                field(UiStyleField::OutlineWidth, [&] { out.writeF32("value", style.outlineWidth); });
                field(UiStyleField::OutlineColor, [&] { out.writeString("value", colorText(style.outlineColor)); });
                field(UiStyleField::OutlineOffset, [&] { out.writeF32("value", style.outlineOffset); });
                field(UiStyleField::Transition,
                      [&]
                      {
                          const UiTransitionSet set = sheet.transitionSet(style.transition);
                          out.beginObject("value");
                          for (sizet i = 0; i < set.size(); ++i)
                          {
                              if (set[i].duration > 0.0f || set[i].delay > 0.0f)
                              {
                                  out.beginObject(UI_TRANSITION_PROPERTY_NAMES[i]);
                                  out.writeF32("duration", set[i].duration);
                                  out.writeF32("delay", set[i].delay);
                                  out.writeString("ease", uiEaseName(set[i].ease));
                                  out.endObject();
                              }
                          }
                          out.endObject();
                      });
                out.endObject();
                if (const auto* anim = world->tryGetConstComponent<UiStyleAnimComponent>(entity))
                {
                    writeStyleAnim(out, "animated", *anim);
                }
                return std::string{};
            });

        control.registerQuery(
            "uiFonts", "engine",
            "the fonts of a world's UI and their glyph atlases: per font its fallback chain, glyphs by state (pending: "
            "the metrics are known and the MSDF cell waits for generation; failed: the pages are full) and pages "
            "(rows used, version, whether the page has a texture), plus the per-update glyph budget, the cells "
            "generated so far and the elements waiting on a glyph or a page texture",
            {STYLE_WORLD_ARG},
            [&control, defaultWorld](const ControlArgs& args, IFormatWriter& out)
            {
                UiViewContext* view = nullptr;
                const World* world = nullptr;
                if (std::string error = uiView(control, args, defaultWorld, view, world); !error.empty())
                {
                    return error;
                }
                out.writeF64("glyphBudgetMs", view->glyphBudgetMs());
                out.writeU64("glyphsGenerated", view->glyphsGenerated());
                out.writeU32("waitingElements", view->glyphWaitingCount());
                const std::vector<UiFontInfo> fonts = view->fontInfo();
                out.beginArray("fonts", static_cast<u32>(fonts.size()));
                for (const UiFontInfo& font : fonts)
                {
                    out.beginObject("");
                    out.writeString("name", font.name);
                    out.beginArray("fallbacks", static_cast<u32>(font.fallbacks.size()));
                    for (const std::string& name : font.fallbacks)
                    {
                        out.writeString("", name);
                    }
                    out.endArray();
                    out.beginObject("glyphs");
                    out.writeU32("ready", font.ready);
                    out.writeU32("blank", font.blank);
                    out.writeU32("pending", font.pending);
                    out.writeU32("failed", font.failed);
                    out.writeU32("absent", font.absent);
                    out.endObject();
                    out.beginArray("pages", static_cast<u32>(font.pages.size()));
                    for (const UiFontPageInfo& page : font.pages)
                    {
                        out.beginObject("");
                        out.writeU32("atlasPage", page.atlasPage);
                        out.writeU32("usedRows", page.usedRows);
                        out.writeU64("version", page.version);
                        out.writeBool("resolved", page.resolved);
                        out.endObject();
                    }
                    out.endArray();
                    out.endObject();
                }
                out.endArray();
                return std::string{};
            });

        control.registerQuery(
            "uiLayers", "engine",
            "the stylesheet layers of a world's UI in order, the base sheet first: name, file (empty: not loaded from "
            "one), whether the active theme added it, and how many classes, kinds, tokens and element rules it holds",
            {STYLE_WORLD_ARG},
            [&control, defaultWorld](const ControlArgs& args, IFormatWriter& out)
            {
                UiViewContext* view = nullptr;
                const World* world = nullptr;
                if (std::string error = uiView(control, args, defaultWorld, view, world); !error.empty())
                {
                    return error;
                }
                const std::vector<UiStyleLayerInfo> layers = view->stylesheet().layers();
                out.writeString("theme", view->stylesheet().theme().toView());
                out.beginArray("layers", static_cast<u32>(layers.size()));
                for (const UiStyleLayerInfo& layer : layers)
                {
                    out.beginObject("");
                    out.writeString("name", layer.name.toView());
                    std::string file = utf8Path(layer.path);
                    std::ranges::replace(file, '\\', '/');
                    out.writeString("path", file);
                    out.writeBool("theme", layer.theme);
                    out.writeU32("classes", static_cast<u32>(layer.classes.size()));
                    out.writeU32("kinds", static_cast<u32>(layer.kinds.size()));
                    out.writeU32("tokens", static_cast<u32>(layer.tokens.size()));
                    out.writeU32("elements", layer.elementCount);
                    out.endObject();
                }
                out.endArray();
                return std::string{};
            });

        control.registerViewCommand(
            "loadLayer", "engine",
            "adds a stylesheet layer over the loaded ones, or reloads the named layer in place; a failed load keeps "
            "the previous content",
            {requiredArg("name", ArgKind::String, "the layer name, not 'base' (the base sheet)"),
             requiredPath("path", "ui", "a stylesheet JSON file under assets/ui, relative to it"), STYLE_WORLD_ARG},
            [&control, defaultWorld](const ControlArgs& args, IFormatWriter& out)
            {
                UiViewContext* view = nullptr;
                const World* world = nullptr;
                if (std::string error = uiView(control, args, defaultWorld, view, world); !error.empty())
                {
                    return error;
                }
                const std::string name = args.string("name").value_or("");
                if (name.empty() || name == BASE_LAYER)
                {
                    return std::string("the layer name must be non-empty and not 'base'");
                }
                const std::string path = args.string("path").value_or("");
                std::filesystem::path file;
                if (std::string error = control.resolveFile("ui", path, file); !error.empty())
                {
                    return error;
                }
                if (auto loaded = view->stylesheet().loadLayer(InternedString(name), file); !loaded)
                {
                    return "cannot load '" + path + "' as a stylesheet";
                }
                out.writeU32("layers", static_cast<u32>(view->stylesheet().layers().size()));
                return std::string{};
            });

        control.registerQuery("uiThemes", "engine",
                              "the active UI theme and the themes available under assets/ui/themes", {STYLE_WORLD_ARG},
                              [&control, defaultWorld](const ControlArgs& args, IFormatWriter& out)
                              {
                                  UiViewContext* view = nullptr;
                                  const World* world = nullptr;
                                  if (std::string error = uiView(control, args, defaultWorld, view, world);
                                      !error.empty())
                                  {
                                      return error;
                                  }
                                  out.writeString("active", view->stylesheet().theme().toView());
                                  out.writeString("density", view->stylesheet().density().toView());
                                  const auto names = [&out](const char* key, const std::vector<std::string>& values)
                                  {
                                      out.beginArray(key, static_cast<u32>(values.size()));
                                      for (const std::string& name : values)
                                      {
                                          out.writeString("", name);
                                      }
                                      out.endArray();
                                  };
                                  names("available", availableUiThemes());
                                  names("densities", availableUiDensities());
                                  return std::string{};
                              });

        control.registerViewCommand(
            "uiTheme", "engine",
            "switches the UI theme (the layer file assets/ui/themes/<name>.json) in one generation: the previous "
            "theme's layer is replaced and every element repaints next frame, which the reply waits for (settled: "
            "true); a failed load keeps the previous theme and lists the themes. density (uiThemes lists them) picks "
            "the spacing token set the theme reads (assets/ui/density/<name>.json); left out, the current one stays",
            {requiredArg("name", ArgKind::String, "a theme file stem, e.g. editor-dark"),
             optionalArg("density", ArgKind::String, "a density of uiThemes, e.g. compact", "the current density"),
             STYLE_WORLD_ARG},
            [&control, defaultWorld](const ControlArgs& args, IFormatWriter& out)
            {
                UiViewContext* view = nullptr;
                const World* world = nullptr;
                if (std::string error = uiView(control, args, defaultWorld, view, world); !error.empty())
                {
                    return error;
                }
                const std::string name = args.string("name").value_or("");
                const std::string density =
                    args.string("density").value_or(std::string(view->stylesheet().density().toView()));
                const auto listed = [](const std::vector<std::string>& names)
                {
                    std::string text;
                    for (const std::string& each : names)
                    {
                        text += (text.empty() ? "" : ", ") + each;
                    }
                    return text.empty() ? std::string("none") : text;
                };
                // a name that is no theme or density lists the valid ones, not a file path
                std::filesystem::path path;
                if (std::string error = control.resolveFile("ui", "themes/" + name + ".json", path); !error.empty())
                {
                    return error;
                }
                if (std::error_code ignored; !std::filesystem::exists(path, ignored))
                {
                    return "uiTheme: no theme '" + name + "'; valid: " + listed(availableUiThemes());
                }
                UiThemeDensity layer{InternedString(density), {}};
                if (density != DEFAULT_UI_DENSITY)
                {
                    if (std::string error = control.resolveFile("ui", "density/" + density + ".json", layer.file);
                        !error.empty())
                    {
                        return error;
                    }
                    if (std::error_code ignored; !std::filesystem::exists(layer.file, ignored))
                    {
                        return "uiTheme: no density '" + density + "'; valid: " + listed(availableUiDensities());
                    }
                }
                if (auto set = view->stylesheet().setTheme(InternedString(name), std::span(&path, 1), layer); !set)
                {
                    return "cannot load theme '" + name + "' at density '" + density + "': " + set.error().message +
                        "; themes: " + listed(availableUiThemes()) + "; densities: " + listed(availableUiDensities());
                }
                // the elements restyle and repaint in the next frame: answer once it ran
                return control.replyAfterFrames(
                    1,
                    [name, density,
                     layers = static_cast<u32>(view->stylesheet().layers().size())](IFormatWriter& result)
                    {
                        result.writeString("theme", name);
                        result.writeString("density", density);
                        result.writeU32("layers", layers);
                        return std::string{};
                    },
                    out);
            });
        control.suggestArgValues("uiTheme", "name", &availableUiThemes);

        control.registerViewCommand(
            "unloadLayer", "engine", "removes a stylesheet layer added by loadLayer",
            {requiredArg("name", ArgKind::String, "the layer name"), STYLE_WORLD_ARG},
            [&control, defaultWorld](const ControlArgs& args, IFormatWriter& out)
            {
                UiViewContext* view = nullptr;
                const World* world = nullptr;
                if (std::string error = uiView(control, args, defaultWorld, view, world); !error.empty())
                {
                    return error;
                }
                const std::string name = args.string("name").value_or("");
                // the base sheet never unloads; a theme's layer is the theme's, so uiTheme
                // changes it and a loose unload cannot leave the UI half themed
                for (const UiStyleLayerInfo& layer : view->stylesheet().layers())
                {
                    if (layer.name.toView() == name && (layer.theme || name == BASE_LAYER))
                    {
                        return "'" + name + "' is the base sheet or the active theme's layer " +
                            "and stays: switch the theme with uiTheme";
                    }
                }
                if (!view->stylesheet().unloadLayer(InternedString(name)))
                {
                    return "no stylesheet layer '" + name + "', or another layer reads its tokens";
                }
                out.writeU32("layers", static_cast<u32>(view->stylesheet().layers().size()));
                return std::string{};
            });
    }
} // namespace spite
