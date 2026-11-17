#include "UiPainter.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <optional>
#include <tuple>

#include "UiTextBlock.hpp"
#include "base/Logging.hpp"
#include "ecs/Chunk.hpp"
#include "ecs/World.hpp"
#include "profiling/Zone.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiIcons.hpp"

namespace spite
{
    namespace
    {
        bool isEmpty(const UiRect& rect) { return rect.size.x <= 0.0f || rect.size.y <= 0.0f; }

        UiStyle restingStyle(const World& world, const UiStyleSheet& styles, const Entity element)
        {
            const auto* identity = world.tryGetConstComponent<UiIdentityComponent>(element);
            const auto* state = world.tryGetConstComponent<UiStateComponent>(element);
            return styles.resolve(identity ? identity->id : InternedString{},
                                  identity ? identity->kind : UiWidgetKind::Panel,
                                  uiInteractionState(!state || state->interactable, false, false, false),
                                  identity ? identity->styleClass : InternedString{});
        }

        // The clip owner's resting corner radius on each clip corner that is one of its own corners; a corner an
        // outer clip cut stays square.
        Vec4 clipRadii(const UiPaintInputs& inputs, UiClipOwnerCache& cache, const Entity element, const UiRect& clip)
        {
            const Entity owner = inputs.layout.clipOwner(element);
            if (owner == Entity::undefined())
            {
                return Vec4{};
            }
            if (cache.owner != owner)
            {
                cache.owner = owner;
                cache.rect = inputs.layout.absoluteRect(owner);
                const Vec4 resting =
                    restingStyle(inputs.world, inputs.stylesFor(owner), owner).cornerRadius * inputs.scale;
                const float half = std::min(cache.rect.size.x, cache.rect.size.y) * 0.5f;
                for (u32 corner = 0; corner < 4; ++corner)
                {
                    cache.radii[corner] = std::max(std::min(resting[corner], half), 0.0f);
                }
            }
            if (cache.radii.x <= 0.0f && cache.radii.y <= 0.0f && cache.radii.z <= 0.0f && cache.radii.w <= 0.0f)
            {
                return Vec4{};
            }
            const Vec2 ownerMax = cache.rect.position + cache.rect.size;
            const Vec2 clipMax = clip.position + clip.size;
            const bool left = clip.position.x == cache.rect.position.x;
            const bool top = clip.position.y == cache.rect.position.y;
            const bool right = clipMax.x == ownerMax.x;
            const bool bottom = clipMax.y == ownerMax.y;
            const Vec4 r = cache.radii;
            return Vec4{left && top ? r.x : 0.0f, right && top ? r.y : 0.0f, right && bottom ? r.z : 0.0f,
                        left && bottom ? r.w : 0.0f};
        }

        Vec2 scaledBy(const UiVisualEffect& effect, const Vec2 point)
        {
            return Vec2{effect.scale.x * point.x + effect.offset.x, effect.scale.y * point.y + effect.offset.y};
        }

        UiRect transformed(const UiVisualEffect& effect, const UiRect& rect)
        {
            return UiRect{scaledBy(effect, rect.position),
                          Vec2{effect.scale.x * rect.size.x, effect.scale.y * rect.size.y}};
        }

        float uniformScale(const UiVisualEffect& effect) { return 0.5f * (effect.scale.x + effect.scale.y); }

        // The element's own style opacity composes with the visual override's opacity and tint; no effect: identity.
        u32 tintedColor(const UiVisualEffect* effect, const float styleOpacity, const u32 color)
        {
            static const UiVisualEffect identity;
            const UiVisualEffect& e = effect ? *effect : identity;
            const auto channel = [&](const u32 shift, const float factor)
            {
                const float value = static_cast<float>((color >> shift) & 0xFFu) * std::clamp(factor, 0.0f, 1.0f);
                return static_cast<u32>(value + 0.5f);
            };
            return channel(0, e.tint.x) | (channel(8, e.tint.y) << 8) | (channel(16, e.tint.z) << 16) |
                (channel(24, e.tint.w * e.opacity * styleOpacity) << 24);
        }

        Vec4 unpackColor(const u32 color)
        {
            constexpr float SCALE = 1.0f / 255.0f;
            return Vec4{static_cast<float>(color & 0xFFu) * SCALE, static_cast<float>((color >> 8) & 0xFFu) * SCALE,
                        static_cast<float>((color >> 16) & 0xFFu) * SCALE,
                        static_cast<float>((color >> 24) & 0xFFu) * SCALE};
        }

        u32 packColor(const Vec4& color)
        {
            const auto channel = [](const float value)
            { return static_cast<u32>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
            return channel(color.x) | (channel(color.y) << 8) | (channel(color.z) << 16) | (channel(color.w) << 24);
        }

        constexpr bool hasAlpha(const u32 color) { return (color >> 24) != 0; }
        // the largest glyph size painted, whatever the layout and text scales multiply a style into
        constexpr float MAX_TEXT_PX = 2048.0f;

        FontId resolveFont(const UiFonts fonts, const InternedString name)
        {
            for (FontId id = 0; id < fonts.size(); ++id)
            {
                if (fonts[id]->name == name)
                {
                    return id;
                }
            }
            return fonts.empty() ? NO_FONT : 0; // the first registered font is the default
        }

        // The face of `family` for a style's fontWeight: the one its stylesheet's fontWeights declares nearest to it,
        // else the family itself.
        FontId weightedFont(const UiStyleSheet& styles, const UiFonts fonts, const FontId family, const u16 weight)
        {
            if (weight == 0 || family == NO_FONT)
            {
                return family;
            }
            const InternedString face = styles.fontFaceFor(fonts[family]->name, weight);
            for (FontId id = 0; face.isValid() && id < fonts.size(); ++id)
            {
                if (fonts[id]->name == face)
                {
                    return id;
                }
            }
            return family;
        }

        // Whether the element shows an icon: its label's own name, else its style's, a label only. `text`
        // becomes the glyph (bytes in `glyph`), empty for a name the table lacks, and the font family the icons font.
        bool iconText(const UiLabelComponent* label, const UiStyle& style, char (&glyph)[4], std::string_view& text)
        {
            if (!label)
            {
                return false;
            }
            const std::string_view name = !label->icon.empty() ? std::string_view(label->icon)
                : style.icon.isValid()                         ? style.icon.toView()
                                                               : std::string_view{};
            if (name.empty())
            {
                return false;
            }
            const u32 codepoint = uiIconCodepoint(name);
            text = codepoint != 0 ? std::string_view(glyph, uiIconUtf8(codepoint, glyph)) : std::string_view{};
            return true;
        }

        // Whether the style's text transform reaches the element's text: an icon glyph keeps its form and markup its
        // source case (its spans index the source text).
        bool transformsText(const UiStyle& style, const UiLabelComponent* label, const bool icon)
        {
            return style.textTransform != UiTextTransform::None && !icon && !(label && label->rich);
        }

        // The font a label or input draws with: an icon's family is the icons font whatever the style names.
        FontId styleFont(const UiStyleSheet& styles, const UiFonts fonts, const UiStyle& style, const bool icon,
                         FontId* family = nullptr)
        {
            static const InternedString ICONS(UI_ICON_FONT);
            const FontId base = resolveFont(fonts, icon ? ICONS : style.font);
            if (family)
            {
                *family = base;
            }
            return weightedFont(styles, fonts, base, style.fontWeight);
        }

        // The spans of parsed rich text in the label's style, sizes at `scale` (1: unscaled, as measured). Returns
        // true while an inline sprite is unresolved (its box is a square until then).
        bool buildSpans(const UiRichText& parsed, const UiStyle& style, const UiStyleSheet& styles, const UiFonts fonts,
                        const FontId baseFont, const FontId family, const float scale, const u32 baseColor,
                        UiTextureTable& textures, std::vector<UiPaintSpan>& out)
        {
            out.clear();
            out.reserve(parsed.runs.size());
            bool pending = false;
            const float baseSize = std::min(style.textSize * scale, MAX_TEXT_PX);
            for (const UiRichRun& run : parsed.runs)
            {
                UiPaintSpan span;
                span.begin = run.begin;
                span.end = run.end;
                span.size =
                    std::min(run.sizePx > 0.0f ? run.sizePx * scale * styles.textScale() : baseSize * run.sizeScale,
                             MAX_TEXT_PX);
                span.font = baseFont;
                if (run.weight != 0)
                {
                    const FontId face = weightedFont(styles, fonts, family, run.weight);
                    span.font = face == family ? baseFont : face;
                }
                span.color = run.hasColor ? run.color : baseColor;
                if (!run.sprite.empty())
                {
                    span.sprite = textures.forAsset(InternedString(run.sprite));
                    const UiTextureTable::Entry& entry = textures.entries[span.sprite];
                    if (entry.sprite && entry.sprite->height > 0)
                    {
                        span.aspect =
                            static_cast<float>(entry.sprite->width) / static_cast<float>(entry.sprite->height);
                    }
                    pending = pending || (!entry.bindlessIndex && !entry.failed);
                    span.color = run.hasColor ? run.color : 0xFFFFFFFFu;
                }
                out.push_back(span);
            }
            return pending;
        }
    } // namespace

    const UiRichText& UiRichTextCache::get(const std::string_view markup)
    {
        const bool oversize = markup.size() > UiRichLimits::MAX_MARKUP_BYTES;
        if (oversize ? m_oversizeKey == markup : false)
        {
            return m_oversize;
        }
        if (!oversize)
        {
            if (const auto it = m_entries.find(markup); it != m_entries.end())
            {
                return it->second;
            }
            if (m_entries.size() >= MAX_ENTRIES)
            {
                m_entries.clear();
            }
        }
        ++parses;
        UiRichText& parsed = oversize ? m_oversize : m_entries[std::string(markup)];
        if (oversize)
        {
            m_oversizeKey = markup; // one slot: a literal fallback is as big as its text, so it is never cached by key
        }
        std::string error;
        if (!parseUiRichText(markup, parsed, error))
        {
            // a malformed text shows as written and logs once per distinct text, to a bounded count
            if (++malformed <= 16)
            {
                SWARNING_LOG("rich text shown as written: {} ({})", error, markup.substr(0, 80));
            }
        }
        return parsed;
    }

    static u32 shadowInsetMask(const UiStyle& style)
    {
        u32 mask = 0;
        for (u32 i = 0; i < UI_MAX_SHADOWS; ++i)
        {
            mask |= style.shadows[i].inset ? 1u << i : 0u;
        }
        return mask;
    }

    UiStyleAnimComponent animOf(const UiStyle& style)
    {
        UiStyleAnimComponent anim;
        anim.backgroundColor = anim.targetBackgroundColor = unpackColor(style.backgroundColor);
        anim.textColor = anim.targetTextColor = unpackColor(style.textColor);
        anim.borderColor = anim.targetBorderColor = unpackColor(style.borderColor);
        anim.borderWidth = anim.targetBorderWidth = style.borderWidth;
        for (u32 i = 0; i < UI_MAX_SHADOWS; ++i)
        {
            const UiShadow& shadow = style.shadows[i];
            anim.shadowColor[i] = anim.targetShadowColor[i] = unpackColor(shadow.color);
            anim.shadowOffset[i] = anim.targetShadowOffset[i] = shadow.offset;
            anim.shadowBlur[i] = anim.targetShadowBlur[i] = shadow.blur;
            anim.shadowSpread[i] = anim.targetShadowSpread[i] = shadow.spread;
        }
        anim.shadowInset = anim.targetShadowInset = shadowInsetMask(style);
        anim.opacity = anim.targetOpacity = style.opacity;
        anim.outlineColor = anim.targetOutlineColor = unpackColor(style.outlineColor);
        anim.outlineWidth = anim.targetOutlineWidth = style.outlineWidth;
        anim.outlineOffset = anim.targetOutlineOffset = style.outlineOffset;
        anim.cornerRadius = anim.targetCornerRadius = style.cornerRadius;
        return anim;
    }

    bool animShadowTargets(const UiStyleAnimComponent& anim, const UiStyle& style, const u32 i)
    {
        const UiShadow& shadow = style.shadows[i];
        if (!hasAlpha(shadow.color))
        {
            return anim.targetShadowColor[i].w == 0.0f; // an absent entry fades out in place: its shape stays
        }
        return anim.targetShadowColor[i] == unpackColor(shadow.color) && anim.targetShadowOffset[i] == shadow.offset &&
            anim.targetShadowBlur[i] == shadow.blur && anim.targetShadowSpread[i] == shadow.spread &&
            ((anim.targetShadowInset >> i) & 1u) == (shadow.inset ? 1u : 0u);
    }

    bool animTargetsStyle(const UiStyleAnimComponent& anim, const UiStyle& style)
    {
        for (u32 i = 0; i < UI_MAX_SHADOWS; ++i)
        {
            if (!animShadowTargets(anim, style, i))
            {
                return false;
            }
        }
        return anim.targetBackgroundColor == unpackColor(style.backgroundColor) &&
            anim.targetTextColor == unpackColor(style.textColor) &&
            anim.targetBorderColor == unpackColor(style.borderColor) && anim.targetBorderWidth == style.borderWidth &&
            anim.targetOpacity == style.opacity && anim.targetOutlineColor == unpackColor(style.outlineColor) &&
            anim.targetOutlineWidth == style.outlineWidth && anim.targetOutlineOffset == style.outlineOffset &&
            anim.targetCornerRadius == style.cornerRadius;
    }

    void applyAnim(UiStyle& style, const UiStyleAnimComponent& anim)
    {
        style.backgroundColor = packColor(anim.backgroundColor);
        style.textColor = packColor(anim.textColor);
        style.borderColor = packColor(anim.borderColor);
        style.borderWidth = std::max(anim.borderWidth, 0.0f);
        for (u32 i = 0; i < UI_MAX_SHADOWS; ++i)
        {
            style.shadows[i] = {packColor(anim.shadowColor[i]), anim.shadowOffset[i],
                                std::max(anim.shadowBlur[i], 0.0f), anim.shadowSpread[i],
                                ((anim.shadowInset >> i) & 1u) != 0};
        }
        style.opacity = std::clamp(anim.opacity, 0.0f, 1.0f);
        style.outlineColor = packColor(anim.outlineColor);
        style.outlineWidth = std::max(anim.outlineWidth, 0.0f);
        style.outlineOffset = anim.outlineOffset;
        style.cornerRadius = Vec4{std::max(anim.cornerRadius.x, 0.0f), std::max(anim.cornerRadius.y, 0.0f),
                                  std::max(anim.cornerRadius.z, 0.0f), std::max(anim.cornerRadius.w, 0.0f)};
    }

    UiTextureId UiTextureTable::forAsset(const InternedString asset)
    {
        const auto [it, inserted] = byAsset.try_emplace(asset, static_cast<UiTextureId>(entries.size()));
        if (inserted)
        {
            entries.push_back({.asset = asset});
            pending.push_back(it->second);
        }
        return it->second;
    }

    UiTextureId UiTextureTable::forBindless(const u32 bindlessIndex)
    {
        const auto [it, inserted] = byBindless.try_emplace(bindlessIndex, static_cast<UiTextureId>(entries.size()));
        if (inserted)
        {
            entries.push_back({.bindlessIndex = bindlessIndex});
        }
        return it->second;
    }

    UiTextureId UiTextureTable::forFile(const InternedString file)
    {
        const auto [it, inserted] = byFile.try_emplace(file, static_cast<UiTextureId>(entries.size()));
        if (inserted)
        {
            entries.push_back({.file = file});
            pending.push_back(it->second);
        }
        return it->second;
    }

    void UiTextureTable::releaseIfUnused(const UiTextureId id)
    {
        if (id == NO_UI_TEXTURE || id >= entries.size() || !entries[id].file.isValid() || entries[id].released ||
            std::ranges::any_of(imageSlots, [id](const auto& slot) { return slot.second == id; }))
        {
            return;
        }
        entries[id].released = true;
        byFile.erase(entries[id].file);
        std::erase(pending, id);
        released.push_back(id);
    }

    UiTextureId UiTextureTable::forFont(const FontId font, const u32 page)
    {
        const auto id = static_cast<UiTextureId>(entries.size());
        entries.push_back({.font = font, .page = page});
        pending.push_back(id);
        return id;
    }

    UiTextureId UiTextureTable::forImageSlot(const u64 imageSlot, const UiStyleSheet& styles)
    {
        if (const auto registered = imageSlots.find(imageSlot); registered != imageSlots.end())
        {
            return registered->second;
        }
        const std::optional<InternedString> asset = styles.imageFor(imageSlot);
        return asset ? forAsset(*asset) : NO_UI_TEXTURE;
    }

    void UiVisualSweep::build(const UiHierarchyManager& hierarchy, const World& world)
    {
        m_spans.clear();
        m_stack.clear();
        m_next = 0;
        m_last = 0;
        const float layoutScale = hierarchy.layoutScale();
        const UiVisualComponent identity;
        for (const Entity owner : hierarchy.visualOwners())
        {
            const auto* visual = world.tryGetConstComponent<UiVisualComponent>(owner);
            const std::optional<UiPaintRange> range = hierarchy.paintRangeOf(owner);
            if (!visual || !range ||
                (visual->opacity == identity.opacity && visual->tint == identity.tint &&
                 visual->scale == identity.scale && visual->translate == identity.translate))
            {
                continue;
            }
            const UiRect rect = hierarchy.absoluteRectOf(owner);
            const Vec2 centre = rect.position + rect.size * 0.5f;
            Span span{range->start, range->start + range->count, {}};
            span.effect.opacity = visual->opacity;
            span.effect.tint = visual->tint;
            span.effect.scale = Vec2{std::max(visual->scale.x, 0.0f), std::max(visual->scale.y, 0.0f)}; // no mirroring
            span.effect.offset =
                Vec2{centre.x - span.effect.scale.x * centre.x, centre.y - span.effect.scale.y * centre.y} +
                visual->translate * layoutScale;
            m_spans.push_back(span);
        }
        std::ranges::sort(m_spans, {}, &Span::start);
    }

    void UiVisualSweep::seek(const u32 position)
    {
        if (position < m_last)
        {
            m_stack.clear();
            m_next = 0;
        }
        m_last = position;
        while (!m_stack.empty() && m_stack.back().end <= position)
        {
            m_stack.pop_back();
        }
        for (; m_next < m_spans.size() && m_spans[m_next].start <= position; ++m_next)
        {
            Span span = m_spans[m_next];
            if (span.end <= position)
            {
                continue;
            }
            if (!m_stack.empty())
            {
                const UiVisualEffect& outer = m_stack.back().effect;
                UiVisualEffect& own = span.effect;
                own.opacity *= outer.opacity;
                own.tint = Vec4{own.tint.x * outer.tint.x, own.tint.y * outer.tint.y, own.tint.z * outer.tint.z,
                                own.tint.w * outer.tint.w};
                own.offset =
                    Vec2{outer.scale.x * own.offset.x + outer.offset.x, outer.scale.y * own.offset.y + outer.offset.y};
                own.scale = Vec2{outer.scale.x * own.scale.x, outer.scale.y * own.scale.y};
            }
            m_stack.push_back(span);
        }
    }

    const UiVisualEffect* UiVisualSweep::at(const u32 position) const
    {
        for (auto it = m_stack.rbegin(); it != m_stack.rend(); ++it)
        {
            if (it->start <= position)
            {
                return &it->effect;
            }
        }
        return nullptr;
    }

    void paintElement(const UiPaintInputs& inputs, UiTextureTable& textures, UiClipOwnerCache& clipCache,
                      const Entity entity, const UiVisualSweep* visuals, const UiPaintRow* row,
                      std::vector<UiPaintRecord>& records, std::vector<std::string>& spareTexts)
    {
        if (!inputs.layout.isVisible(entity))
        {
            return;
        }
        UiRect target{Vec2{0.0f, 0.0f}, inputs.targetSize};
        if (!inputs.windows.empty())
        {
            const u32 at = inputs.layout.paintIndex(entity);
            for (const UiWindowSurface& window : inputs.windows)
            {
                if (at >= window.start && at < window.end)
                {
                    target = window.rect;
                    break;
                }
            }
        }
        const UiVisualEffect* effect = visuals ? visuals->current() : nullptr;
        UiRect rect = inputs.layout.absoluteRect(entity);
        UiRect clip = inputs.layout.isClipped(entity) ? inputs.layout.clipRect(entity).intersect(target) : target;
        Vec4 radii;
        float shapeScale = 1.0f;
        if (effect)
        {
            radii = clipRadii(inputs, clipCache, entity, clip);
            // a clip owned inside the transformed subtree moves with it; one above it stays
            if (const Entity owner = inputs.layout.clipOwner(entity); owner != Entity::undefined())
            {
                if (const UiVisualEffect* clipEffect = visuals->at(inputs.layout.paintIndex(owner)))
                {
                    clip = transformed(*clipEffect, clip).intersect(target);
                    radii = radii * uniformScale(*clipEffect);
                }
            }
            rect = transformed(*effect, rect);
            shapeScale = uniformScale(*effect);
        }
        if (isEmpty(rect.intersect(clip)))
        {
            return;
        }
        const sizet firstRecord = records.size();
        UiPaintRow looked;
        if (!row)
        {
            std::tie(looked.identity, looked.state, looked.image, looked.label, looked.value, looked.edit,
                     looked.anim) =
                inputs.world
                    .tryGetConstComponents<UiIdentityComponent, UiStateComponent, UiImageComponent, UiLabelComponent,
                                           UiValueComponent, UiTextEditComponent, UiStyleAnimComponent>(entity);
            row = &looked;
        }
        const auto [identity, state, image, label, value, edit, anim] = *row;
        bool interactable = !state || state->interactable;
        if (label && inputs.ui)
        {
            const Entity parent = inputs.ui->hierarchy().parentOf(entity);
            const auto* parentIdentity = parent.isValid() && inputs.world.isEntityAlive(parent)
                ? inputs.world.tryGetConstComponent<UiIdentityComponent>(parent)
                : nullptr;
            if (parentIdentity && parentIdentity->kind == UiWidgetKind::Dropdown)
            {
                const auto* parentState = inputs.world.tryGetConstComponent<UiStateComponent>(parent);
                interactable = !parentState || parentState->interactable;
            }
        }
        const auto* dropdown =
            identity && (identity->kind == UiWidgetKind::Dropdown || identity->kind == UiWidgetKind::Combo)
            ? inputs.world.tryGetConstComponent<UiDropdownComponent>(entity)
            : nullptr;
        const UiInteractionState interaction =
            uiInteractionState(interactable, entity == inputs.pressed || (dropdown && dropdown->open),
                               entity == inputs.hovered, entity == inputs.focused);
        const UiStyleSheet& styles = inputs.stylesFor(entity);
        UiStyle style =
            styles.resolve(identity ? identity->id : InternedString{}, identity ? identity->kind : UiWidgetKind::Panel,
                           interaction, identity ? identity->styleClass : InternedString{});
        // A transitioning element paints the animated values; one whose component and style disagree is
        // reconciled after the repaint (the view starts the tweens, so this frame still shows the old values).
        if (style.transition != 0 ? !anim || !animTargetsStyle(*anim, style) : anim != nullptr)
        {
            if (inputs.transitions)
            {
                inputs.transitions->push_back({entity, style});
            }
        }
        if (style.transition != 0 && anim)
        {
            applyAnim(style, *anim);
        }

        UiPaintRecord record;
        record.element = entity;
        record.rect = rect;
        record.clip = clip;
        record.clipRadii = effect ? radii : clipRadii(inputs, clipCache, entity, clip);
        const float decorScale = inputs.scale * shapeScale;
        // A shadow whose element is wholly outside its clip is not painted above (the early return), even when its
        // blur would reach in. The outer entries paint under the fill in index order; the inset ones over it, below.
        bool insetShadows = false;
        for (const UiShadow& entry : style.shadows)
        {
            insetShadows = insetShadows || (entry.inset && hasAlpha(entry.color));
            if (entry.inset || !hasAlpha(entry.color))
            {
                continue;
            }
            const float spread = entry.spread * decorScale;
            UiPaintRecord shadow = record;
            shadow.kind = UiPaintKind::Shadow;
            shadow.rect.position = rect.position + entry.offset * decorScale - Vec2{spread, spread};
            shadow.rect.size = rect.size + Vec2{2.0f * spread, 2.0f * spread};
            if (shadow.rect.size.x <= 0.0f || shadow.rect.size.y <= 0.0f)
            {
                continue; // a negative spread that eats the box
            }
            shadow.color = entry.color;
            for (u32 corner = 0; corner < 4; ++corner)
            {
                const float radius = style.cornerRadius[corner] * decorScale;
                shadow.cornerRadius[corner] = radius > 0.0f ? std::max(radius + spread, 0.0f) : radius;
            }
            shadow.blur = entry.blur * decorScale;
            shadow.shadowOffset = entry.offset * decorScale;
            shadow.shadowSpread = spread;
            records.push_back(shadow);
        }
        const u32 fillEnd = style.gradientColor.value_or(style.backgroundColor);
        const bool bordered = style.borderWidth > 0.0f && hasAlpha(style.borderColor);
        if (hasAlpha(style.backgroundColor) || hasAlpha(fillEnd) || bordered)
        {
            record.kind = UiPaintKind::Fill;
            record.color = style.backgroundColor;
            record.cornerRadius = style.cornerRadius * decorScale;
            record.gradientColor = fillEnd;
            record.gradientAngle = style.gradientAngle;
            record.borderColor = bordered ? style.borderColor : 0u;
            record.borderWidth = bordered ? style.borderWidth * decorScale : 0.0f;
            records.push_back(record);
            record.gradientColor = 0;
            record.borderColor = 0;
            record.borderWidth = 0.0f;
        }
        // The outline is a border-only Box over the rect grown by offset + width; it paints after the element's own
        // records, so a later sibling or child can cover it, and takes only the ancestors' clip.
        std::optional<UiPaintRecord> outline;
        if (style.outlineWidth > 0.0f && hasAlpha(style.outlineColor))
        {
            const float reach = (style.outlineOffset + style.outlineWidth) * decorScale;
            UiPaintRecord ring = record;
            ring.kind = UiPaintKind::Outline;
            ring.rect = UiRect{rect.position - Vec2{reach, reach}, rect.size + Vec2{2.0f * reach, 2.0f * reach}};
            ring.color = 0;
            for (u32 corner = 0; corner < 4; ++corner)
            {
                const float radius = style.cornerRadius[corner];
                ring.cornerRadius[corner] = radius > 0.0f ? std::max(radius * decorScale + reach, 0.0f) : 0.0f;
            }
            ring.gradientColor = 0;
            ring.borderColor = style.outlineColor;
            ring.borderWidth = style.outlineWidth * decorScale;
            if (ring.rect.size.x > 0.0f && ring.rect.size.y > 0.0f)
            {
                outline = ring;
            }
        }
        record.cornerRadius = Vec4{};
        record.kind = UiPaintKind::Image;
        record.color = style.backgroundTint;
        record.imageScale = decorScale;
        if (style.backgroundImage.isValid())
        {
            record.texture = textures.forAsset(style.backgroundImage);
            records.push_back(record);
        }
        if (image && image->imageSlot != 0)
        {
            record.texture = textures.forImageSlot(image->imageSlot, styles);
            if (record.texture != NO_UI_TEXTURE)
            {
                records.push_back(record);
            }
        }
        // An inset shadow lives in the padding box (CSS): the rect shrinks by the painted border and each non-zero
        // radius by the same width (the fill's inner edge); a border that eats the box leaves nothing to shadow.
        const float insetBorder = bordered ? style.borderWidth * decorScale : 0.0f;
        if (insetShadows && rect.size.x > 2.0f * insetBorder && rect.size.y > 2.0f * insetBorder)
        {
            UiPaintRecord inset;
            inset.element = entity;
            inset.kind = UiPaintKind::InsetShadow;
            inset.rect = UiRect{rect.position + Vec2{insetBorder, insetBorder},
                                rect.size - Vec2{2.0f * insetBorder, 2.0f * insetBorder}};
            inset.clip = clip;
            inset.clipRadii = record.clipRadii;
            for (u32 corner = 0; corner < 4; ++corner)
            {
                const float radius = style.cornerRadius[corner] * decorScale;
                inset.cornerRadius[corner] = radius > 0.0f ? std::max(radius - insetBorder, 0.0f) : 0.0f;
            }
            for (const UiShadow& entry : style.shadows)
            {
                if (entry.inset && hasAlpha(entry.color))
                {
                    inset.color = entry.color;
                    inset.blur = entry.blur * decorScale;
                    inset.shadowOffset = entry.offset * decorScale;
                    inset.shadowSpread = entry.spread * decorScale;
                    records.push_back(inset);
                }
            }
        }

        std::string_view text = label ? std::string_view(label->text)
            : value                   ? std::string_view(value->text)
                                      : std::string_view{};
        // an input composing text shows it at the caret, as part of the text it lays out
        const bool composing = edit && edit->editing && value && !edit->composition.empty();
        const u32 compositionStart = composing ? static_cast<u32>(std::min<sizet>(edit->caret, value->text.size())) : 0;
        if (composing)
        {
            thread_local std::string composed; // an element's text view lives to the end of its paint
            composed.clear();
            composed.reserve(value->text.size() + edit->composition.size());
            composed.append(value->text, 0, compositionStart)
                .append(edit->composition)
                .append(value->text, compositionStart);
            text = composed;
        }
        char iconGlyph[4];
        const bool icon = !composing && iconText(label, style, iconGlyph, text);
        FontId family = NO_FONT;
        const FontId font = styleFont(styles, inputs.fonts, style, icon, &family);
        const float textSize = std::min(style.textSize * inputs.scale * shapeScale, MAX_TEXT_PX);
        const float letterSpacing = style.textLetterSpacing * inputs.scale * shapeScale;
        // The text a transforming style lays out; the caret, selection and composition offsets stay in the stored
        // text and measure its converted prefixes. Valid until the next call.
        const bool transforms = transformsText(style, label, icon);
        thread_local std::string casedText;
        const auto shown = [&](const std::string_view source)
        {
            return transforms ? applyUiTextTransform(style.textTransform, inputs.caseLocale, source, casedText)
                              : source;
        };
        // An input being edited shows its selection under the text and
        // its caret over it, at the text's own pen positions.
        std::optional<UiPaintRecord> caret;
        // text wider than its rect scrolls to keep the caret inside and is cut at the rect
        UiRect textRect = rect;
        UiRect textClip = clip;
        if (edit)
        {
            if (const auto* fit = inputs.world.tryGetConstComponent<UiContentSizeComponent>(entity))
            {
                const float inset = std::clamp(fit->padding.x * inputs.scale * shapeScale, 0.0f, rect.size.x * 0.5f);
                textRect.position.x += inset;
                textRect.size.x -= 2.0f * inset;
                textClip = clip.intersect(textRect);
            }
        }
        const Vec4 elementRadii = record.clipRadii;
        Vec4 textRadii = textClip == clip ? elementRadii : clipRadii(inputs, clipCache, entity, textClip);
        UiTextAlign textAlign = style.textAlign;
        if (edit && edit->editing && value && font != NO_FONT)
        {
            const UiFont& fontData = *inputs.fonts[font];
            const auto penAt = [&](const u32 offset)
            {
                return layoutUiText(inputs.fonts, font, shown(text.substr(0, std::min<sizet>(offset, text.size()))),
                                    textSize, nullptr, nullptr, letterSpacing);
            };
            const float textWidth = penAt(static_cast<u32>(text.size()));
            const float slack = textRect.size.x - textWidth;
            const u32 insertion = composing ? compositionStart + edit->compositionCursor : edit->caret;
            const float caretWidth = std::max(1.0f, inputs.scale);
            const bool keeps = inputs.textArea && inputs.textArea->owner == entity;
            float startX = textRect.position.x +
                (style.textAlign == UiTextAlign::Center      ? slack * 0.5f
                     : style.textAlign == UiTextAlign::Right ? slack
                                                             : 0.0f);
            if (slack < 0.0f)
            {
                const float caretPen = penAt(insertion);
                float scroll = keeps ? inputs.textArea->scroll * inputs.scale : 0.0f;
                scroll = std::max(std::min(scroll, caretPen), caretPen + caretWidth - textRect.size.x);
                scroll = std::clamp(scroll, 0.0f, textWidth + caretWidth - textRect.size.x);
                startX = textRect.position.x - scroll;
                textClip = textClip.intersect(textRect);
                textRect = UiRect{Vec2{startX, rect.position.y}, Vec2{textWidth, rect.size.y}};
                textAlign = UiTextAlign::Left;
                textRadii = textClip == clip ? elementRadii : clipRadii(inputs, clipCache, entity, textClip);
                if (inputs.textArea)
                {
                    inputs.textArea->scroll = scroll / inputs.scale;
                }
            }
            else if (keeps)
            {
                inputs.textArea->scroll = 0.0f;
            }
            const float lineHeight = (fontData.ascender - fontData.descender) * textSize;
            const float top = rect.position.y + (rect.size.y - lineHeight) * 0.5f;
            UiPaintRecord fill;
            fill.element = entity;
            fill.clip = textClip;
            fill.clipRadii = textRadii;
            fill.kind = UiPaintKind::Fill;
            const u32 compositionEnd = compositionStart + static_cast<u32>(edit->composition.size());
            if (composing)
            {
                // the composition is underlined; the clause being converted has a heavier line
                const float thickness = std::max(1.0f, inputs.scale);
                const float from = startX + penAt(compositionStart);
                const float to = startX + penAt(compositionEnd);
                fill.color = style.textColor;
                fill.gradientColor = fill.color;
                fill.rect = UiRect{Vec2{from, top + lineHeight - thickness}, Vec2{to - from, thickness}};
                records.push_back(fill);
                if (edit->compositionSelected > 0)
                {
                    const float clauseFrom = startX + penAt(compositionStart + edit->compositionCursor);
                    const float clauseTo =
                        startX + penAt(compositionStart + edit->compositionCursor + edit->compositionSelected);
                    fill.rect = UiRect{Vec2{clauseFrom, top + lineHeight - 2.0f * thickness},
                                       Vec2{clauseTo - clauseFrom, thickness}};
                    records.push_back(fill);
                }
            }
            else if (edit->anchor != edit->caret)
            {
                const float from = startX + penAt(std::min(edit->anchor, edit->caret));
                const float to = startX + penAt(std::max(edit->anchor, edit->caret));
                fill.rect = UiRect{Vec2{from, top}, Vec2{to - from, lineHeight}};
                fill.color = style.selectionColor;
                fill.gradientColor = fill.color;
                records.push_back(fill);
            }
            fill.rect = UiRect{Vec2{startX + penAt(insertion), top}, Vec2{caretWidth, lineHeight}};
            if (inputs.textArea)
            {
                // where the input method puts its candidate window: the composition, else the caret
                const float from = startX + penAt(composing ? compositionStart : edit->caret);
                const float to = composing ? startX + penAt(compositionEnd) : from + fill.rect.size.x;
                inputs.textArea->owner = entity;
                inputs.textArea->area = {Vec2{from, top}, Vec2{to - from, lineHeight}, fill.rect.position.x - from};
            }
            fill.color = style.caretColor;
            fill.gradientColor = fill.color;
            caret = fill;
        }
        if (edit && !edit->editing && inputs.textArea && inputs.textArea->owner == entity)
        {
            inputs.textArea->owner = Entity::undefined(); // no longer edited: its area is gone with it
        }
        // a label that opted in parses its markup (once per text, cached) and paints its plain text in spans
        const UiRichText* parsed = label && label->rich && !icon && inputs.richCache && font != NO_FONT && !text.empty()
            ? &inputs.richCache->get(text)
            : nullptr;
        // an empty input with nothing composing shows its placeholder in place of the text; the caret above was laid
        // out against the real (empty) text
        std::string_view hint;
        if (value && text.empty() && !composing && !icon)
        {
            if (const auto* placeholder = inputs.world.tryGetConstComponent<UiPlaceholderComponent>(entity))
            {
                hint = placeholder->text;
            }
        }
        if (!hint.empty() || (parsed ? !parsed->plain.empty() : !text.empty()))
        {
            record.kind = UiPaintKind::Text;
            record.color = hint.empty() ? style.textColor : style.placeholderColor;
            record.texture = NO_UI_TEXTURE;
            if (!spareTexts.empty())
            {
                record.text = std::move(spareTexts.back());
                spareTexts.pop_back();
            }
            record.text.assign(parsed ? std::string_view(parsed->plain) : shown(hint.empty() ? text : hint));
            record.font = font;
            record.textSize = textSize;
            record.textLetterSpacing = letterSpacing;
            record.textAlign = textAlign;
            record.rect = textRect;
            record.clip = textClip;
            record.clipRadii = textRadii;
            if (parsed)
            {
                buildSpans(*parsed, style, styles, inputs.fonts, font, family, inputs.scale * shapeScale,
                           style.textColor, textures, record.spans);
            }
            // an input at rest cuts its text at its rect like a label does; an edited one lays out plain, as its caret
            // does
            const bool cutsAtRect = value && !(edit && edit->editing) && style.textOverflow >= UiTextOverflow::Ellipsis;
            if ((label && (parsed || usesTextBlock(style) || style.textVAlign != UiTextVAlign::Center)) || cutsAtRect)
            {
                record.textBlock = true;
                record.textWrap = style.textWrap;
                record.textOverflow = style.textOverflow;
                record.textVAlign = style.textVAlign;
                record.textMaxLines = style.textMaxLines;
                record.textLineHeight = style.textLineHeight;
                if (const auto* fit = inputs.world.tryGetConstComponent<UiContentSizeComponent>(entity);
                    fit && !edit && (fit->fitWidth || fit->fitHeight))
                {
                    record.textInset = fit->padding * (inputs.scale * shapeScale);
                }
            }
            records.push_back(std::move(record));
        }
        if (caret)
        {
            records.push_back(*caret);
        }
        if (inputs.ui && inputs.ui->canvasCount() != 0)
        {
            if (const UiCanvasContent* content = inputs.ui->canvasContent(entity))
            {
                // the canvas's primitives sit over the element's own fill and text, under its children
                UiPaintRecord canvas;
                canvas.element = entity;
                canvas.kind = UiPaintKind::Canvas;
                canvas.rect = rect;
                canvas.clip = clip.intersect(rect);
                canvas.clipRadii =
                    canvas.clip == clip ? elementRadii : clipRadii(inputs, clipCache, entity, canvas.clip);
                canvas.color = 0xFFFFFFFFu; // the tint and opacity below fold into it
                canvas.gradientColor = style.textColor;
                canvas.font = font;
                canvas.textSize = textSize;
                canvas.textLetterSpacing = letterSpacing;
                canvas.textAlign = style.textAlign;
                canvas.canvasScale = effect ? Vec2{inputs.scale * effect->scale.x, inputs.scale * effect->scale.y}
                                            : Vec2{inputs.scale, inputs.scale};
                canvas.primitives = static_cast<u32>(content->primitives.size());
                canvas.generation = content->generation;
                canvas.canvas = content;
                records.push_back(std::move(canvas));
            }
        }
        if (outline)
        {
            records.push_back(*outline);
        }
        if (effect || style.opacity < 1.0f)
        {
            sizet kept = firstRecord;
            for (sizet i = firstRecord; i < records.size(); ++i)
            {
                UiPaintRecord& r = records[i];
                r.color = tintedColor(effect, style.opacity, r.color);
                bool spanVisible = false;
                for (UiPaintSpan& span : r.spans)
                {
                    span.color = tintedColor(effect, style.opacity, span.color);
                    spanVisible = spanVisible || hasAlpha(span.color);
                }
                r.gradientColor = tintedColor(effect, style.opacity, r.gradientColor);
                r.borderColor = tintedColor(effect, style.opacity, r.borderColor);
                if (!hasAlpha(r.borderColor))
                {
                    r.borderWidth = 0.0f; // a faded-out ring shows the fill, it does not fade the edge
                }
                if (hasAlpha(r.color) || spanVisible || hasAlpha(r.gradientColor) ||
                    (r.borderWidth > 0.0f && hasAlpha(r.borderColor)))
                {
                    if (kept != i)
                    {
                        records[kept] = std::move(r);
                    }
                    ++kept;
                }
            }
            records.erase(records.begin() + static_cast<std::ptrdiff_t>(kept), records.end());
        }
    }

    Vec2 measureUiText(const World& world, const UiStyleSheet& styles, const UiFonts fonts, const Entity element,
                       const std::string_view text, const UiCaseLocale caseLocale, const float textSize)
    {
        UiStyle style = restingStyle(world, styles, element);
        if (textSize > 0.0f)
        {
            style.textSize = textSize;
        }
        char glyph[4];
        std::string_view shown = text;
        const auto* label = world.tryGetConstComponent<UiLabelComponent>(element);
        const bool icon = iconText(label, style, glyph, shown);
        thread_local std::string cased;
        if (transformsText(style, label, icon))
        {
            shown = applyUiTextTransform(style.textTransform, caseLocale, shown, cased);
        }
        const FontId fontId = styleFont(styles, fonts, style, icon);
        if (fontId == NO_FONT)
        {
            return Vec2{0.0f, style.textSize};
        }
        const UiFont& font = *fonts[fontId];
        return Vec2{layoutUiText(fonts, fontId, shown, style.textSize, nullptr, nullptr, style.textLetterSpacing),
                    (font.ascender - font.descender) * style.textSize};
    }

    bool usesTextBlock(const UiStyle& style)
    {
        return style.textWrap != UiTextWrap::None || style.textOverflow != UiTextOverflow::Clip ||
            style.textMaxLines != 0 || style.textLineHeight != 1.0f;
    }

    // the block path of measureUiTextBlock, apart so the single-line path stays as small as measureUiText
    static UiTextMeasure measureBlock(const UiStyle& style, const UiFonts fonts, const FontId fontId,
                                      const float fontLine, const std::string_view text, const float width,
                                      UiTextMeasureCache& cache)
    {
        UiTextMeasureCache::Key key;
        key.text = std::hash<std::string_view>{}(text);
        key.length = text.size();
        key.font = fontId;
        key.size = std::bit_cast<u32>(style.textSize);
        key.width = std::bit_cast<u32>(width > 0.0f ? width : -1.0f); // no width: unbounded
        key.lineHeight = std::bit_cast<u32>(style.textLineHeight);
        key.letterSpacing = std::bit_cast<u32>(style.textLetterSpacing);
        key.maxLines = style.textMaxLines;
        key.wrap = static_cast<u8>(style.textWrap);
        key.overflow = static_cast<u8>(style.textOverflow);
        key.transform = static_cast<u8>(style.textTransform);
        if (const UiTextMeasure* known = cache.find(key))
        {
            ++cache.hits;
            return *known;
        }
        ++cache.layouts;
        thread_local UiTextBlock block;
        layoutUiTextBlock(fonts, fontId, text, style.textSize,
                          {width, style.textWrap, style.textOverflow, style.textMaxLines, style.textLetterSpacing},
                          block);
        const UiTextMeasure measured{
            Vec2{block.width, fontLine * style.textLineHeight * static_cast<float>(block.lines.size())},
            style.textWrap == UiTextWrap::Word};
        cache.insert(key, measured);
        return measured;
    }

    UiTextMeasure measureUiTextBlock(const World& world, const UiStyleSheet& styles, const UiFonts fonts,
                                     const Entity element, std::string_view text, const float width,
                                     UiTextMeasureCache& cache, const UiCaseLocale caseLocale)
    {
        const UiStyle style = restingStyle(world, styles, element);
        char glyph[4];
        const auto* label = world.tryGetConstComponent<UiLabelComponent>(element);
        const bool icon = iconText(label, style, glyph, text);
        thread_local std::string cased;
        if (transformsText(style, label, icon))
        {
            text = applyUiTextTransform(style.textTransform, caseLocale, text, cased); // laid out, and keyed, converted
        }
        const FontId fontId = styleFont(styles, fonts, style, icon);
        if (fontId == NO_FONT)
        {
            return {Vec2{0.0f, style.textSize}, false};
        }
        const UiFont& font = *fonts[fontId];
        const float fontLine = (font.ascender - font.descender) * style.textSize;
        if (!usesTextBlock(style))
        {
            return {Vec2{layoutUiText(fonts, fontId, text, style.textSize, nullptr, nullptr, style.textLetterSpacing),
                         fontLine},
                    false};
        }
        return measureBlock(style, fonts, fontId, fontLine, text, width, cache);
    }

    namespace
    {
        // each channel of `a` scaled by the matching channel of `b` (white: unchanged)
        u32 modulated(const u32 a, const u32 b)
        {
            if (b == 0xFFFFFFFFu)
            {
                return a;
            }
            const auto channel = [&](const u32 shift)
            { return ((((a >> shift) & 0xFFu) * ((b >> shift) & 0xFFu) + 127u) / 255u) << shift; };
            return channel(0) | channel(8) | channel(16) | channel(24);
        }
    } // namespace

    UiTextMeasure measureRichUiTextBlock(const World& world, const UiStyleSheet& styles, const UiFonts fonts,
                                         const Entity element, const std::string_view markup, const float width,
                                         UiTextMeasureCache& cache, const UiRichMeasureInputs& rich)
    {
        const UiStyle style = restingStyle(world, styles, element);
        char glyph[4];
        std::string_view shown = markup;
        if (iconText(world.tryGetConstComponent<UiLabelComponent>(element), style, glyph, shown))
        {
            return measureUiTextBlock(world, styles, fonts, element, markup, width, cache,
                                      UiCaseLocale::Default); // an icon is never markup, so no transform
        }
        FontId family = NO_FONT;
        const FontId fontId = styleFont(styles, fonts, style, false, &family);
        if (fontId == NO_FONT)
        {
            return {Vec2{0.0f, style.textSize}, false};
        }
        UiTextMeasureCache::Key key;
        key.text = std::hash<std::string_view>{}(markup);
        key.length = markup.size();
        key.font = fontId;
        key.size = std::bit_cast<u32>(style.textSize);
        key.width = std::bit_cast<u32>(width > 0.0f ? width : -1.0f);
        key.lineHeight = std::bit_cast<u32>(style.textLineHeight);
        key.letterSpacing = std::bit_cast<u32>(style.textLetterSpacing);
        key.maxLines = style.textMaxLines;
        key.wrap = static_cast<u8>(style.textWrap);
        key.overflow = static_cast<u8>(style.textOverflow);
        key.rich = true;
        if (const UiTextMeasure* known = cache.find(key))
        {
            ++cache.hits;
            return *known;
        }
        ++cache.layouts;
        const UiRichText& parsed = rich.markup.get(markup);
        thread_local std::vector<UiPaintSpan> spans;
        thread_local UiTextBlock block;
        const bool pending =
            buildSpans(parsed, style, styles, fonts, fontId, family, 1.0f, style.textColor, rich.textures, spans);
        layoutUiRichTextBlock(fonts, fontId, parsed.plain, style.textSize, spans,
                              {width, style.textWrap, style.textOverflow, style.textMaxLines, style.textLetterSpacing},
                              block);
        float height = 0.0f;
        for (const UiTextLine& line : block.lines)
        {
            height += (line.ascent + line.descent) * style.textLineHeight;
        }
        const UiTextMeasure measured{Vec2{block.width, height}, style.textWrap == UiTextWrap::Word};
        if (!pending)
        {
            cache.insert(key, measured);
            return measured;
        }
        // a sprite still to resolve is a square for now: not cached, and the label measures again when it lands
        for (const UiPaintSpan& span : spans)
        {
            if (span.sprite != NO_UI_TEXTURE && !rich.textures.entries[span.sprite].bindlessIndex &&
                !rich.textures.entries[span.sprite].failed)
            {
                rich.layoutWaiting.add(element.id(), UiWaitSet::pageKey(span.sprite));
            }
        }
        return measured;
    }

    static u32 prepareTexture(const UiTextureId id, const UiTextureTable& textures, UiPreparedPaint& prepared)
    {
        if (prepared.lastTexture == id)
            return prepared.lastTextureIndex;
        const auto [it, inserted] = prepared.textureLookup.try_emplace(id, static_cast<u32>(prepared.textures.size()));
        if (inserted)
        {
            const auto& image = textures.entries[id];
            prepared.textures.push_back(
                {image.bindlessIndex, image.sprite, !image.failed && (image.asset.isValid() || image.file.isValid())});
        }
        prepared.lastTexture = id;
        prepared.lastTextureIndex = it->second;
        return it->second;
    }

    static void prepareText(const UiPaintRecord& record, const UiTextureTable& textures, const UiFonts fonts,
                            std::vector<UiPlacedGlyph>& placed, UiPreparedPaint& prepared)
    {
        if (record.font == NO_FONT)
        {
            return;
        }
        const UiFont& font = *fonts[record.font];
        const float size = record.textSize;
        const float pixelRange = std::max(UiFont::PX_RANGE * size / UiFont::EM_PX, 1.0f); // screen px range
        // the block sits inside the fit padding, as the measure wraps it
        const float innerWidth = std::max(0.0f, record.rect.size.x - 2.0f * record.textInset.x);
        const auto alignedX = [&](const float lineWidth)
        {
            const float slack = (record.textBlock ? innerWidth : record.rect.size.x) - lineWidth;
            return record.rect.position.x + (record.textBlock ? record.textInset.x : 0.0f) +
                (record.textAlign == UiTextAlign::Center      ? slack * 0.5f
                     : record.textAlign == UiTextAlign::Right ? slack
                                                              : 0.0f);
        };
        // `spanOf` (RICH): the span of each glyph of the run, which sets its size and color; a plain run
        // pays for none of it
        const auto draw = [&]<bool RICH>(const std::span<const UiPlacedGlyph> run, const std::span<const u32> spanOf,
                                         const float startX, const float baseline)
        {
            for (sizet g = 0; g < run.size(); ++g)
            {
                const UiPlacedGlyph& glyph = run[g];
                const UiPaintSpan* span = RICH && spanOf[g] != NO_SPAN ? &record.spans[spanOf[g]] : nullptr;
                const float glyphSize = span ? span->size : size;
                if (RICH && !glyph.glyph)
                {
                    // an inline sprite: a box as high as its run's line, standing on the baseline
                    const UiTextureTable::Entry& image = textures.entries[span->sprite];
                    if (!image.bindlessIndex)
                    {
                        if (!image.failed)
                        {
                            prepared.textWaits.push_back({record.element.id(), UiWaitSet::pageKey(span->sprite)});
                        }
                        continue;
                    }
                    const UiFont& face = *fonts[span->font];
                    const float height = (face.ascender - face.descender) * glyphSize;
                    prepared.glyphs.push_back({UiRect{Vec2{startX + glyph.penX, baseline - face.ascender * glyphSize},
                                                      Vec2{span->aspect * height, height}},
                                               {},
                                               {},
                                               prepareTexture(span->sprite, textures, prepared),
                                               span->color,
                                               0.0f,
                                               true});
                    continue;
                }
                const float range = span ? std::max(UiFont::PX_RANGE * glyphSize / UiFont::EM_PX, 1.0f) : pixelRange;
                const UiGlyph& cell = *glyph.glyph;
                if (cell.state == UiGlyphState::Pending)
                {
                    prepared.textWaits.push_back({record.element.id(), UiWaitSet::glyphKey(&cell)});
                    continue;
                }
                if (cell.state != UiGlyphState::Ready)
                {
                    continue;
                }
                const UiTextureId pageTexture = glyph.font->pageTextures[cell.page];
                const std::optional<u32> atlas = textures.entries[pageTexture].bindlessIndex;
                if (!atlas)
                {
                    prepared.textWaits.push_back({record.element.id(), UiWaitSet::pageKey(pageTexture)});
                    continue;
                }
                const float x0 = startX + glyph.penX + cell.planeMin.x * glyphSize;
                const float x1 = startX + glyph.penX + cell.planeMax.x * glyphSize;
                const float glyphBaseline = baseline - glyph.offsetY;
                const float y0 = glyphBaseline - cell.planeMax.y * glyphSize;
                const float y1 = glyphBaseline - cell.planeMin.y * glyphSize;
                prepared.glyphs.push_back({UiRect{Vec2{x0, y0}, Vec2{x1 - x0, y1 - y0}}, cell.uvMin, cell.uvMax,
                                           prepareTexture(pageTexture, textures, prepared),
                                           span ? span->color : record.color, range, false});
            }
        };
        const auto drawPlain = [&](const std::span<const UiPlacedGlyph> run, const float startX, const float baseline)
        { draw.template operator()<false>(run, {}, startX, baseline); };
        if (!record.textBlock)
        {
            placed.clear();
            const float width =
                layoutUiText(fonts, record.font, record.text, size, &placed, nullptr, record.textLetterSpacing);
            // vertically centered line box: ascender above, descender below the baseline
            drawPlain(placed, alignedX(width),
                      record.rect.position.y + record.rect.size.y * 0.5f +
                          (font.ascender + font.descender) * 0.5f * size);
            return;
        }
        thread_local UiTextBlock block;
        if (!record.spans.empty())
        {
            layoutUiRichTextBlock(
                fonts, record.font, record.text, size, record.spans,
                {innerWidth, record.textWrap, record.textOverflow, record.textMaxLines, record.textLetterSpacing},
                block);
            // a line is as high as its tallest item times the line height, the items on one baseline
            float height = 0.0f;
            for (const UiTextLine& line : block.lines)
            {
                height += (line.ascent + line.descent) * record.textLineHeight;
            }
            float lineTop = record.rect.position.y + record.textInset.y +
                (record.rect.size.y - 2.0f * record.textInset.y - height) *
                    (record.textVAlign == UiTextVAlign::Top          ? 0.0f
                         : record.textVAlign == UiTextVAlign::Bottom ? 1.0f
                                                                     : 0.5f);
            for (const UiTextLine& line : block.lines)
            {
                const float content = line.ascent + line.descent;
                const float box = content * record.textLineHeight;
                if (lineTop + box >= record.clip.position.y && lineTop <= record.clip.position.y + record.clip.size.y)
                {
                    draw.template operator()<true>(
                        std::span<const UiPlacedGlyph>(block.glyphs).subspan(line.first, line.count),
                        std::span<const u32>(block.spanOf).subspan(line.first, line.count), alignedX(line.width),
                        lineTop + (box - content) * 0.5f + line.ascent);
                }
                lineTop += box;
            }
            return;
        }
        layoutUiTextBlock(
            fonts, record.font, record.text, size,
            {innerWidth, record.textWrap, record.textOverflow, record.textMaxLines, record.textLetterSpacing}, block);
        // lines are line-height boxes with the glyph line centered in each; the block sits in the rect
        const float fontLine = (font.ascender - font.descender) * size;
        const float lineAdvance = fontLine * record.textLineHeight;
        const float top = record.rect.position.y + record.textInset.y +
            (record.rect.size.y - 2.0f * record.textInset.y - lineAdvance * static_cast<float>(block.lines.size())) *
                (record.textVAlign == UiTextVAlign::Top          ? 0.0f
                     : record.textVAlign == UiTextVAlign::Bottom ? 1.0f
                                                                 : 0.5f);
        const float clipTop = record.clip.position.y - lineAdvance;
        const float clipBottom = record.clip.position.y + record.clip.size.y + lineAdvance;
        for (u32 k = 0; k < block.lines.size(); ++k)
        {
            const UiTextLine& line = block.lines[k];
            const float lineTop = top + static_cast<float>(k) * lineAdvance;
            if (lineTop < clipTop || lineTop > clipBottom)
            {
                continue; // wholly outside its clip
            }
            drawPlain(std::span<const UiPlacedGlyph>(block.glyphs).subspan(line.first, line.count),
                      alignedX(line.width), lineTop + (lineAdvance - fontLine) * 0.5f + font.ascender * size);
        }
        return;
    }

    void prepareUiPaint(std::span<const UiPaintRecord> records, const UiTextureTable& textures, const UiFonts fonts,
                        std::vector<UiPlacedGlyph>& placed, UiPreparedPaint& prepared)
    {
        prepared.textures.clear();
        prepared.textureLookup.clear();
        prepared.lastTexture = NO_UI_TEXTURE;
        prepared.imageTextures.clear();
        prepared.glyphs.clear();
        prepared.textRuns.clear();
        prepared.validSeries.clear();
        prepared.textWaits.clear();
        prepared.waits.clear();
        const auto text = [&](const UiPaintRecord& record)
        {
            const u32 first = static_cast<u32>(prepared.glyphs.size());
            const u32 waitFirst = static_cast<u32>(prepared.textWaits.size());
            prepareText(record, textures, fonts, placed, prepared);
            prepared.textRuns.push_back({first, static_cast<u32>(prepared.glyphs.size()), waitFirst,
                                         static_cast<u32>(prepared.textWaits.size())});
        };
        for (const UiPaintRecord& record : records)
        {
            if (record.kind == UiPaintKind::Text)
            {
                text(record);
            }
            else if (record.kind == UiPaintKind::Image)
            {
                prepared.imageTextures.push_back(prepareTexture(record.texture, textures, prepared));
            }
            else if (record.kind == UiPaintKind::Canvas)
            {
                const UiCanvasContent& content = *record.canvas;
                const Vec2 origin = record.rect.position;
                const Vec2 scale = record.canvasScale;
                const float uniform = 0.5f * (scale.x + scale.y);
                const Vec2 clipMin = record.clip.position;
                const Vec2 clipMax = clipMin + record.clip.size;
                const auto outside = [&](const Vec2 lo, const Vec2 hi)
                { return hi.x <= clipMin.x || hi.y <= clipMin.y || lo.x >= clipMax.x || lo.y >= clipMax.y; };
                const auto toTarget = [&](const Vec2 local)
                { return origin + Vec2{local.x * scale.x, local.y * scale.y}; };
                for (const UiCanvasPrimitive& primitive : content.primitives)
                {
                    if (primitive.kind == UiCanvasKind::Series)
                    {
                        const UiSeries* series = content.series
                            ? content.series->find({primitive.text, std::bit_cast<u32>(primitive.c.x)})
                            : nullptr;
                        prepared.validSeries.push_back(series && series->owner == record.element.id());
                    }
                    else if (primitive.kind == UiCanvasKind::Text)
                    {
                        UiPaintRecord run;
                        run.element = record.element;
                        run.kind = UiPaintKind::Text;
                        run.rect =
                            UiRect{toTarget(primitive.a), Vec2{primitive.b.x * scale.x, primitive.b.y * scale.y}};
                        run.clip = record.clip;
                        run.clipRadii = record.clipRadii;
                        run.color =
                            modulated(primitive.color != 0 ? primitive.color : record.gradientColor, record.color);
                        run.font = NO_FONT;
                        if (record.font != NO_FONT && primitive.text < content.texts.size() && run.rect.size.x > 0.0f &&
                            run.rect.size.y > 0.0f && !outside(run.rect.position, run.rect.position + run.rect.size))
                        {
                            run.text = content.texts[primitive.text];
                            run.font = record.font;
                            run.textSize = primitive.width > 0.0f ? primitive.width * uniform : record.textSize;
                            run.textLetterSpacing = record.textLetterSpacing;
                            run.textAlign = primitive.align == UiCanvasAlign::Center ? UiTextAlign::Center
                                : primitive.align == UiCanvasAlign::Right            ? UiTextAlign::Right
                                                                                     : UiTextAlign::Left;
                        }
                        text(run);
                    }
                }
            }
        }
    }
} // namespace spite
