#include "uiview/UiViewContext.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>
#include <unordered_set>

#include <msdfgen/msdfgen-ext.h>

#include "UiPainter.hpp"
#include "uiruntime/UiRetainedInstances.hpp"
#include "base/Assert.hpp"
#include "base/Logging.hpp"
#include "base/ServiceLocator.hpp"
#include "ecs/CommandBuffer.hpp"
#include "profiling/Zone.hpp"
#include "ecs/SystemContext.hpp"
#include "ecs/World.hpp"
#include "ui/UiComponents.hpp"
#include "ui/UiContext.hpp"
#include "ui/UiHierarchyManager.hpp"
#include "ui/UiRouter.hpp"
#include "ui/UiTween.hpp"
#include "ui/UiTextMeasurer.hpp"

namespace spite
{
    namespace
    {
        // Frame generations are unique across views: the render shim keys its
        // buffers by source position, and a source swapped for another view
        // (Play, Stop) must never match the generation its slot holds.
        std::atomic<u64> g_frameGeneration{0};

        struct UiStyleLayerRecord
        {
            std::string name;
            std::string path;
            u32 order = 0;
            u32 elementCount = 0;
            SPITE_OBSERVED_SCHEMA(UiStyleLayerRecord, name, path, order, elementCount)
        };

        // One class or kind a layer defines.
        struct UiStyleDefRecord
        {
            std::string layer;
            std::string level;
            std::string name;
            SPITE_OBSERVED_SCHEMA(UiStyleDefRecord, layer, level, name)
        };

        void updateUiView(SystemContext& ctx)
        {
            ServiceLocator* services = ctx.tryGetServices();
            if (!services || !services->contains<UiViewContext>())
            {
                return;
            }
            services->write<UiViewContext>().get().update(&ctx.getCommandBuffer());
        }
    } // namespace

    struct UiViewContext::State : UiTextMeasurer
    {
        World* world = nullptr;
        UiContext* ui = nullptr;
        UiStyleSheet styles;
        Entity stylesheetRoot = Entity::undefined();
        UiStyleSheet* subtreeStylesheet = nullptr;
        mutable UiTextureTable textures; // measuring a rich label requests its inline sprites
        msdfgen::FreetypeHandle* freetype = nullptr;
        std::vector<std::unique_ptr<UiFont>> fonts;
        // every font's atlas pages in creation order: the index UiViewContext::atlasPage takes
        struct PageRef
        {
            FontId font;
            u32 page;
        };
        std::vector<PageRef> atlasPages;
        double glyphBudgetMs = 2.0;
        u64 glyphsGenerated = 0;
        // elements whose text last lowered with a glyph still to generate or a page texture unresolved
        UiWaitSet waiting;
        // rich labels measured with an inline sprite not resolved yet, by that sprite: layout runs again for them
        mutable UiWaitSet layoutWaiting;
        mutable UiRichTextCache richCache;
        std::vector<const UiGlyph*> landed; // scratch: the glyphs one update generated
        UiRetainedInstances retained;
        Vec2 targetSize;
        // Per paint position, where each element's records start.
        // the roots bound to OS windows as this update finds them, and as the last repaint painted them
        std::vector<UiWindowSurface> windowSurfaces;
        std::vector<UiWindowSurface> paintedWindowSurfaces;
        u32 paintedCount = 0;
        u64 countedLayoutGeneration = 0; // the layout generation ui.resolvedElements last reported
        UiClipOwnerCache clipCache;
        UiPaintedTextArea textArea; // the last text input painted while editing
        UiVisualSweep visuals; // rebuilt by every repainting update that has visual overrides
        bool hasVisuals = false;

        // repaint scratch, kept to avoid per-repaint allocation
        std::vector<UiPaintRecord> newRecords;
        UiCustomPaint customPaint;
        std::vector<UiCustomPaintRecord> customRecords;
        std::vector<UiPaintRecord> paintScratch;
        bool inCustomPaint = false;
        std::vector<UiInstance> newInstances;
        std::vector<u32> newRecordEnds;
        std::vector<u32> newInstanceEnds;
        std::vector<UiPlacedGlyph> placed;
        UiPreparedPaint preparedPaint;
        std::vector<UiPendingTransition> pendingTransitions;
        // heap text buffers of replaced records, reused by the next repaint's text records
        std::vector<std::string> spareTexts;
        static constexpr sizet MAX_SPARE_TEXTS = 256;
        // A repaint that paints a long text takes a buffer here and its replaced record gives its own back for the
        // next one, so an idle refresh needs a few of slack: a text that grows past the small-string size (a counter
        // changing from "-" to its value) replaces a record that has none to give.
        static constexpr sizet MIN_SPARE_TEXTS = 16;
        static constexpr sizet SPARE_TEXT_CAPACITY = 63;

        // inputs of the last repaint
        bool inputsChanged = true;
        u64 paintGeneration = 0;
        u64 styleGeneration = 0;
        Entity hovered = Entity::undefined();
        Entity pressed = Entity::undefined();
        Entity focused = Entity::undefined();
        // inputs of the last layout resolve that measured text
        u64 measuredStyleGeneration = 0;
        u64 measuredSubtreeGeneration = 0;
        u64 paintedSubtreeGeneration = 0;
        sizet measuredFontCount = 0;
        // the sheet generation whose numeric tokens the layout token table holds
        u64 tokenGeneration = ~u64{0};
        u64 subtreeTokenGeneration = ~u64{0};

        // A theme's spacing reaches the bound layout fields before the layout that follows
        void syncLayoutTokens()
        {
            if (styles.generation() != tokenGeneration)
            {
                tokenGeneration = styles.generation();
                ui->layoutTokens().assign(styles.numericTokens());
                ui->colorTokens().assign(styles.colorTokens());
            }
            if (subtreeStylesheet && subtreeStylesheet->generation() != subtreeTokenGeneration)
            {
                subtreeTokenGeneration = subtreeStylesheet->generation();
                ui->setSubtreeLayoutTokens(stylesheetRoot, subtreeStylesheet->numericTokens());
            }
            ui->syncLayoutTokens();
        }

        // label blocks measured in this font and style state, by text and width (cleared with the fallback chains)
        mutable UiTextMeasureCache textCache;
        mutable UiTextMeasureCache subtreeTextCache;

        UiTextMeasureCache& textCacheFor(const UiStyleSheet& sheet) const
        {
            return &sheet == subtreeStylesheet ? subtreeTextCache : textCache;
        }

        const UiStyleSheet& stylesFor(const Entity element) const
        {
            return subtreeStylesFor(ui->hierarchy(), element, styles, stylesheetRoot, subtreeStylesheet);
        }

        u64 subtreeGeneration() const { return subtreeStylesheet ? subtreeStylesheet->generation() : 0; }

        // the case rules of the bound Localization's locale (default rules while none is bound)
        UiCaseLocale caseLocale() const { return uiCaseLocale(ui->hierarchy().textLocale()); }

        Vec2 measureText(const Entity element, const std::string_view text) const override
        {
            ensureFallbacks();
            return measureUiText(*world, stylesFor(element), fonts, element, text, caseLocale());
        }

        Vec2 measureTextSized(const Entity element, const std::string_view text, const float textSize) const override
        {
            ensureFallbacks();
            return measureUiText(*world, stylesFor(element), fonts, element, text, caseLocale(), textSize);
        }

        UiTextMeasure measureTextBlock(const Entity element, const std::string_view text,
                                       const float width) const override
        {
            ensureFallbacks();
            const UiStyleSheet& sheet = stylesFor(element);
            return measureUiTextBlock(*world, sheet, fonts, element, text, width, textCacheFor(sheet), caseLocale());
        }

        UiTextMeasure measureRichTextBlock(const Entity element, const std::string_view text,
                                           const float width) const override
        {
            ensureFallbacks();
            const UiStyleSheet& sheet = stylesFor(element);
            return measureRichUiTextBlock(*world, sheet, fonts, element, text, width, textCacheFor(sheet),
                                          {richCache, textures, layoutWaiting});
        }

        // Gives every page a font created a texture id (the resolver makes the texture), in creation order.
        void syncPages(const FontId id)
        {
            UiFont& font = *fonts[id];
            for (u32 page = static_cast<u32>(font.pageTextures.size()); page < font.pages().size(); ++page)
            {
                font.pageTextures.push_back(textures.forFont(id, page));
                atlasPages.push_back({id, page});
            }
        }

        // The fallback chains of the loaded stylesheet, by font name; rebuilt when the stylesheet or the fonts changed,
        // before anything lays out text.
        mutable u64 chainStyleGeneration = ~u64{0};
        mutable sizet chainFontCount = ~sizet{0};
        void ensureFallbacks() const
        {
            if (chainStyleGeneration == styles.generation() && chainFontCount == fonts.size())
            {
                return;
            }
            chainStyleGeneration = styles.generation();
            chainFontCount = fonts.size();
            textCache.clear();
            subtreeTextCache.clear();
            for (const std::unique_ptr<UiFont>& font : fonts)
            {
                font->fallbacks.clear();
                for (const InternedString name : styles.fontFallbacksFor(font->name))
                {
                    const auto it =
                        std::ranges::find_if(fonts, [name](const auto& other) { return other->name == name; });
                    if (it == fonts.end())
                    {
                        SWARNING_LOG("font '{}': fallback '{}' is not a registered font", font->name.toView(),
                                     name.toView());
                    }
                    else if (it->get() != font.get())
                    {
                        font->fallbacks.push_back(static_cast<FontId>(it - fonts.begin()));
                    }
                }
            }
        }

        // The elements waiting on one glyph or page texture repaint (and wait again if still short).
        void wake(const u64 key)
        {
            waiting.resolve(key, [this](const u64 id) { ui->hierarchy().markPaintDirty(Entity(id)); });
            layoutWaiting.resolve(key,
                                  [this](const u64 id)
                                  {
                                      if (world->isEntityAlive(Entity(id)))
                                      {
                                          ui->hierarchy().markLayoutDirty(Entity(id));
                                      }
                                  });
        }

        // Generates the requested cells until the budget is spent (at least one): fonts in creation order, each
        // oldest request first.
        void generateGlyphs()
        {
            const auto demanding = [](const auto& font) { return font->hasDemand(); };
            if (!std::ranges::any_of(fonts, demanding))
            {
                return;
            }
            SPITE_ZONE("UiGlyphGenerate");
            const auto deadline = std::chrono::steady_clock::now() +
                std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                      std::chrono::duration<double, std::milli>(glyphBudgetMs));
            // another cell starts only if the slowest so far would still end inside the budget
            std::chrono::steady_clock::duration slowest{};
            std::chrono::steady_clock::time_point now;
            landed.clear();
            do
            {
                const auto begin = std::chrono::steady_clock::now();
                landed.push_back((*std::ranges::find_if(fonts, demanding))->generateOne());
                now = std::chrono::steady_clock::now();
                slowest = std::max(slowest, now - begin);
            }
            while (std::ranges::any_of(fonts, demanding) && now + slowest <= deadline);
            glyphsGenerated += landed.size();
            for (FontId id = 0; id < fonts.size(); ++id)
            {
                syncPages(id);
            }
            SPITE_COUNTER("ui.glyphsGenerated", static_cast<u32>(landed.size()));
            for (const UiGlyph* glyph : landed)
            {
                wake(UiWaitSet::glyphKey(glyph));
            }
        }

        // The base walk from the text's aligned start, as an editing (focused) input paints it.
        u32 caretAt(const Entity element, const std::string_view text, const float x, const float width) const override
        {
            const auto* fit = world->tryGetConstComponent<UiContentSizeComponent>(element);
            const float inset = fit ? std::clamp(fit->padding.x, 0.0f, width * 0.5f) : 0.0f;
            const float innerWidth = width - 2.0f * inset;
            const auto* identity = world->tryGetConstComponent<UiIdentityComponent>(element);
            const UiTextAlign align =
                stylesFor(element)
                    .resolve(identity ? identity->id : InternedString{},
                             identity ? identity->kind : UiWidgetKind::Panel, UiInteractionState::Focused,
                             identity ? identity->styleClass : InternedString{})
                    .textAlign;
            const float slack = innerWidth - measureText(element, text).x;
            const float start = slack < 0.0f   ? (textArea.owner == element ? -textArea.scroll : 0.0f) // scrolled
                : align == UiTextAlign::Center ? slack * 0.5f
                : align == UiTextAlign::Right  ? slack
                                               : 0.0f;
            return UiTextMeasurer::caretAt(element, text, x - inset - start, innerWidth);
        }

        // Keeps the heap text buffers of records about to be dropped for the next text records.
        void keepTexts(const std::span<UiPaintRecord> dropped)
        {
            for (UiPaintRecord& record : dropped)
            {
                if (record.text.capacity() > std::string().capacity() && spareTexts.size() < MAX_SPARE_TEXTS)
                {
                    spareTexts.push_back(std::move(record.text));
                }
            }
        }

        void topUpTexts()
        {
            while (spareTexts.size() < MIN_SPARE_TEXTS)
            {
                spareTexts.emplace_back().reserve(SPARE_TEXT_CAPACITY);
            }
        }

        // Stages 1+2 for paint positions [first, last), spliced over their previous records. Each element's
        // instances overwrite its block, and only the instances whose bytes changed are logged; an element that
        // outgrew its block re-blocks the range (the outgrown blocks double), moving every later instance. exact:
        // blocks fit their instances exactly (a full repaint).
        void repaint(const UiPaintInputs& inputs, const u32 first, const u32 last, const u64 generation,
                     const bool exact)
        {
            const std::span<const Entity> paintOrder = inputs.layout.paintOrder();
            keepTexts(newRecords);
            newRecords.clear();
            newInstances.clear();
            newRecordEnds.clear();
            newInstanceEnds.clear();
            const auto paintRows = retained.paintRows(inputs.world, ui->hierarchy(), first, last);
            for (u32 element = first; element < last; ++element)
            {
                if (hasVisuals)
                {
                    visuals.seek(element);
                }
                paintElement(inputs, textures, clipCache, paintOrder[element], hasVisuals ? &visuals : nullptr,
                             element < paintRows.size() && paintRows[element].identity ? &paintRows[element] : nullptr,
                             newRecords, spareTexts);
                newRecordEnds.push_back(static_cast<u32>(newRecords.size()));
            }
            if (customPaint)
            {
                customRecords.clear();
                struct CallbackGuard
                {
                    bool& active;
                    explicit CallbackGuard(bool& flag) : active(flag) { active = true; }
                    ~CallbackGuard() { active = false; }
                } guard(inCustomPaint);
                customPaint(paintOrder.subspan(first, last - first), customRecords);
                std::stable_sort(customRecords.begin(), customRecords.end(),
                                 [](const auto& a, const auto& b) { return a.elementIndex < b.elementIndex; });
                paintScratch.clear();
                u32 from = 0;
                sizet custom = 0;
                for (u32 i = 0; i < last - first; ++i)
                {
                    const u32 end = newRecordEnds[i];
                    for (; from < end; ++from)
                        paintScratch.push_back(std::move(newRecords[from]));
                    while (custom < customRecords.size() && customRecords[custom].elementIndex == i)
                    {
                        customRecords[custom].record.element = paintOrder[first + i];
                        paintScratch.push_back(std::move(customRecords[custom++].record));
                    }
                    newRecordEnds[i] = static_cast<u32>(paintScratch.size());
                }
                newRecords.swap(paintScratch);
            }
            prepareUiPaint(newRecords, textures, fonts, placed, preparedPaint);
            lowerUiPaint(newRecords, newRecordEnds, preparedPaint, newInstances, newInstanceEnds);
            for (const UiPaintWait& wait : preparedPaint.waits)
                waiting.add(wait.element, wait.key);

            retained.appendRecords(first, last, newRecords, newRecordEnds, spareTexts);
            paintedCount += last - first;

            retained.append(first, last, generation, exact, newInstances, newInstanceEnds);
        }

        static Vec4 laneValue(const float value) { return Vec4{value, 0.0f, 0.0f, 0.0f}; }
        static Vec4 laneValue(const Vec2 value) { return Vec4{value.x, value.y, 0.0f, 0.0f}; }
        static Vec4 laneValue(const Vec4 value) { return value; }

        // Reconciles the elements the repaint found disagreeing with their animation component.
        void reconcileTransitions(CommandBuffer* deferred)
        {
            UiTweens& tweens = ui->tweens();
            // the tweened fields, resolved once: a theme switch starts one tween per changed property of every element
            // (per shadow entry for the shadow properties: shadowColor[0], shadowColor[1], ...)
            static const auto fields = []
            {
                std::array<std::array<UiTweenField, UI_MAX_SHADOWS>, static_cast<sizet>(UiTransitionProperty::Count)>
                    table;
                for (sizet i = 0; i < table.size(); ++i)
                {
                    const auto property = static_cast<UiTransitionProperty>(i);
                    const bool perShadow = property == UiTransitionProperty::ShadowColor ||
                        property == UiTransitionProperty::ShadowOffset ||
                        property == UiTransitionProperty::ShadowBlur || property == UiTransitionProperty::ShadowSpread;
                    for (u32 entry = 0; entry < (perShadow ? UI_MAX_SHADOWS : 1u); ++entry)
                    {
                        const auto resolved =
                            UiTweens::field("UiStyleAnim",
                                            perShadow ? std::format("{}[{}]", UI_TRANSITION_PROPERTY_NAMES[i], entry)
                                                      : std::string(UI_TRANSITION_PROPERTY_NAMES[i]));
                        SASSERTM(resolved.has_value(), "UiStyleAnim has a tweenable field per transition property.");
                        table[i][entry] = *resolved;
                    }
                }
                return table;
            }();
            for (const UiPendingTransition& item : pendingTransitions)
            {
                const Entity element = item.element;
                if (!world->isEntityAlive(element))
                {
                    continue;
                }
                auto* anim = world->tryGetComponent<UiStyleAnimComponent>(element);
                if (item.style.transition == 0)
                {
                    deferred ? deferred->removeComponent<UiStyleAnimComponent>(element)
                             : world->removeComponent<UiStyleAnimComponent>(element);
                    continue;
                }
                UiStyleAnimComponent next = animOf(item.style);
                if (!anim)
                {
                    deferred ? deferred->addComponent(element, std::move(next))
                             : world->addComponent<UiStyleAnimComponent>(element, next);
                    continue;
                }
                const UiTransitionSet set = stylesFor(element).transitionSet(item.style.transition);
                const auto retarget = [&](const UiTransitionProperty property, const u32 index, auto& shown,
                                          auto& target, const auto& wanted)
                {
                    if (target == wanted)
                    {
                        return;
                    }
                    target = wanted;
                    const UiTransitionSpec& spec = set[static_cast<sizet>(property)];
                    UiTweenDesc desc;
                    desc.element = element;
                    desc.to = laneValue(wanted);
                    desc.duration = spec.duration;
                    desc.delay = spec.delay;
                    desc.ease = spec.ease;
                    if (const auto started = tweens.start(fields[static_cast<sizet>(property)][index], desc); !started)
                    {
                        SWARNING_LOG("style transition: {}", started.error().message);
                        shown = wanted; // no tween will move it: show the new value, not a stale one
                    }
                };
                using Property = UiTransitionProperty;
                retarget(Property::BackgroundColor, 0, anim->backgroundColor, anim->targetBackgroundColor,
                         next.targetBackgroundColor);
                retarget(Property::TextColor, 0, anim->textColor, anim->targetTextColor, next.targetTextColor);
                retarget(Property::BorderColor, 0, anim->borderColor, anim->targetBorderColor, next.targetBorderColor);
                retarget(Property::BorderWidth, 0, anim->borderWidth, anim->targetBorderWidth, next.targetBorderWidth);
                retarget(Property::Opacity, 0, anim->opacity, anim->targetOpacity, next.targetOpacity);
                retarget(Property::OutlineColor, 0, anim->outlineColor, anim->targetOutlineColor,
                         next.targetOutlineColor);
                retarget(Property::OutlineWidth, 0, anim->outlineWidth, anim->targetOutlineWidth,
                         next.targetOutlineWidth);
                retarget(Property::OutlineOffset, 0, anim->outlineOffset, anim->targetOutlineOffset,
                         next.targetOutlineOffset);
                retarget(Property::CornerRadius, 0, anim->cornerRadius, anim->targetCornerRadius,
                         next.targetCornerRadius);
                // The shadow stack moves entry by entry. An entry the style no longer has fades its color out where it
                // is; one that appears out of nothing takes its shape at once and fades in.
                for (u32 i = 0; i < UI_MAX_SHADOWS; ++i)
                {
                    Vec4 color = next.targetShadowColor[i];
                    Vec2 offset = next.targetShadowOffset[i];
                    float blur = next.targetShadowBlur[i];
                    float spread = next.targetShadowSpread[i];
                    if ((item.style.shadows[i].color >> 24) == 0)
                    {
                        color = Vec4{anim->targetShadowColor[i].x, anim->targetShadowColor[i].y,
                                     anim->targetShadowColor[i].z, 0.0f};
                        offset = anim->targetShadowOffset[i];
                        blur = anim->targetShadowBlur[i];
                        spread = anim->targetShadowSpread[i];
                    }
                    else
                    {
                        if (anim->shadowColor[i].w <= 0.0f && anim->targetShadowColor[i].w <= 0.0f)
                        {
                            anim->shadowColor[i] = Vec4{color.x, color.y, color.z, 0.0f};
                            anim->shadowOffset[i] = offset;
                            anim->shadowBlur[i] = blur;
                            anim->shadowSpread[i] = spread;
                        }
                        const u32 bit = 1u << i;
                        const u32 inset = next.targetShadowInset & bit; // inset does not tween: it snaps
                        anim->shadowInset = (anim->shadowInset & ~bit) | inset;
                        anim->targetShadowInset = (anim->targetShadowInset & ~bit) | inset;
                    }
                    retarget(Property::ShadowColor, i, anim->shadowColor[i], anim->targetShadowColor[i], color);
                    retarget(Property::ShadowOffset, i, anim->shadowOffset[i], anim->targetShadowOffset[i], offset);
                    retarget(Property::ShadowBlur, i, anim->shadowBlur[i], anim->targetShadowBlur[i], blur);
                    retarget(Property::ShadowSpread, i, anim->shadowSpread[i], anim->targetShadowSpread[i], spread);
                }
            }
            pendingTransitions.clear();
        }

        // The paint ranges whose records changed: the hierarchy's damage plus
    };

    UiViewContext::UiViewContext(World& world, UiContext& ui) : m_state(std::make_unique<State>())
    {
        // Invariant: one view reads a hierarchy's damage log; a second reader would trim it from under the first
        SASSERTM(!ui.hierarchy().textMeasurer(), "a UiViewContext already measures this hierarchy's text.");
        m_state->world = &world;
        m_state->spareTexts.reserve(State::MAX_SPARE_TEXTS);
        m_state->ui = &ui;
        ui.hierarchy().setTextMeasurer(m_state.get());
        world.services().registerService(this);
        world.services().registerTable<UiViewContext>("uiPaint", UiPaintRecord::spiteSchema(),
                                                      [](const UiViewContext& view, const TableRowSink& sink)
                                                      {
                                                          for (const UiPaintRecord& record : view.paintRecords())
                                                          {
                                                              sink(&record);
                                                          }
                                                      });
        world.services().registerTable<UiViewContext>(
            "styleLayers", UiStyleLayerRecord::spiteSchema(),
            [](const UiViewContext& view, const TableRowSink& sink)
            {
                u32 order = 0;
                for (const UiStyleLayerInfo& layer : view.m_state->styles.layers())
                {
                    const UiStyleLayerRecord record{std::string(layer.name.toView()), layer.path.generic_string(),
                                                    order++, layer.elementCount};
                    sink(&record);
                }
            });
        world.services().registerTable<UiViewContext>(
            "styleDefs", UiStyleDefRecord::spiteSchema(),
            [](const UiViewContext& view, const TableRowSink& sink)
            {
                for (const UiStyleLayerInfo& layer : view.m_state->styles.layers())
                {
                    const std::string layerName(layer.name.toView());
                    for (const InternedString name : layer.classes)
                    {
                        const UiStyleDefRecord record{layerName, "class", std::string(name.toView())};
                        sink(&record);
                    }
                    for (const std::string_view name : layer.kinds)
                    {
                        const UiStyleDefRecord record{layerName, "kind", std::string(name)};
                        sink(&record);
                    }
                }
            });
        world.registerSystem(
            SystemDescriptor{.name = "UiViewPaint",
                             .fn = &updateUiView,
                             .update = SystemPhaseDescriptor{
                                 .reads = {getComponentId<UiRectComponent>(), getComponentId<UiStateComponent>(),
                                           getComponentId<UiLabelComponent>(), getComponentId<UiValueComponent>(),
                                           getComponentId<UiImageComponent>(), getComponentId<UiIdentityComponent>(),
                                           getComponentId<UiTextEditComponent>(), getComponentId<UiVisualComponent>()},
                                 .stage = CoreExecutionStages::PRE_RENDER}});
    }

    UiViewContext::~UiViewContext()
    {
        m_state->ui->setSubtreeLayoutTokens(Entity::undefined(), {});
        m_state->world->services().unregisterService<UiViewContext>();
        m_state->ui->hierarchy().setTextMeasurer(nullptr);
        m_state->fonts.clear();
        if (m_state->freetype)
        {
            msdfgen::deinitializeFreetype(m_state->freetype);
        }
    }

    UiStyleSheet& UiViewContext::stylesheet() { return m_state->styles; }

    void UiViewContext::setSubtreeStylesheet(const Entity root, UiStyleSheet* sheet)
    {
        State& state = *m_state;
        if (state.stylesheetRoot == root && state.subtreeStylesheet == sheet)
        {
            return;
        }
        state.stylesheetRoot = root;
        state.subtreeStylesheet = sheet;
        if (sheet)
        {
            sheet->setTextScale(state.styles.textScale());
            state.subtreeTokenGeneration = sheet->generation();
            state.ui->setSubtreeLayoutTokens(root, sheet->numericTokens());
        }
        else
        {
            state.subtreeTokenGeneration = ~u64{0};
            state.ui->setSubtreeLayoutTokens(Entity::undefined(), {});
        }
        state.inputsChanged = true;
        state.measuredSubtreeGeneration = ~u64{0};
        state.textCache.clear();
        state.subtreeTextCache.clear();
    }

    const UiStyleSheet& UiViewContext::stylesheetFor(const Entity element) const { return m_state->stylesFor(element); }

    void UiViewContext::setTargetSize(const Vec2 size)
    {
        if (size != m_state->targetSize)
        {
            m_state->targetSize = size;
            m_state->inputsChanged = true;
        }
    }

    Expected<FontId> UiViewContext::registerFont(const InternedString name, const std::filesystem::path& ttf)
    {
        if (const FontId existing = findFont(name); existing != NO_FONT)
        {
            return existing;
        }
        State& state = *m_state;
        if (!state.freetype)
        {
            state.freetype = msdfgen::initializeFreetype();
        }
        auto font = UiFont::load(state.freetype, name, ttf);
        if (!font)
        {
            return std::unexpected(font.error());
        }
        const auto id = static_cast<FontId>(state.fonts.size());
        state.fonts.push_back(std::move(*font));
        state.syncPages(id);
        state.inputsChanged = true;
        return id;
    }

    FontId UiViewContext::findFont(const InternedString name) const
    {
        const auto& fonts = m_state->fonts;
        const auto it = std::ranges::find_if(fonts, [name](const auto& font) { return font->name == name; });
        return it == fonts.end() ? NO_FONT : static_cast<FontId>(it - fonts.begin());
    }

    float UiViewContext::textAdvance(const FontId font, const std::string_view text, const float sizePx) const
    {
        SASSERTM(font < m_state->fonts.size(), "textAdvance: unknown font.");
        m_state->ensureFallbacks();
        return layoutUiText(m_state->fonts, font, text, sizePx, nullptr);
    }

    std::vector<UiTextGlyph> UiViewContext::textGlyphs(const FontId font, const std::string_view text,
                                                       const float sizePx) const
    {
        SASSERTM(font < m_state->fonts.size(), "textGlyphs: unknown font.");
        m_state->ensureFallbacks();
        std::vector<UiPlacedGlyph> placed;
        UiPreparedPaint preparedPaint;
        std::vector<u32> clusters;
        layoutUiText(m_state->fonts, font, text, sizePx, &placed, &clusters);
        std::vector<UiTextGlyph> glyphs;
        glyphs.reserve(placed.size());
        for (sizet i = 0; i < placed.size(); ++i)
        {
            const UiPlacedGlyph& glyph = placed[i];
            const auto face = std::ranges::find_if(m_state->fonts, [&](const auto& candidate)
                                                   { return candidate.get() == glyph.font; });
            glyphs.push_back({static_cast<FontId>(face - m_state->fonts.begin()), glyph.glyph->glyphIndex, clusters[i],
                              glyph.penX, glyph.offsetY});
        }
        return glyphs;
    }

    u64 UiViewContext::textShapeCount() const
    {
        u64 count = 0;
        for (const auto& font : m_state->fonts)
        {
            count += font->shapeCount();
        }
        return count;
    }

    UiFontAtlasView UiViewContext::fontAtlas(const FontId font, const u32 page) const
    {
        SASSERTM(font < m_state->fonts.size() && page < m_state->fonts[font]->pages().size(),
                 "fontAtlas: unknown font or page.");
        return {UiFont::PAGE_SIZE, UiFont::PAGE_SIZE, m_state->fonts[font]->pages()[page].rgba8};
    }

    void UiViewContext::setGlyphBudgetMs(const double milliseconds) { m_state->glyphBudgetMs = milliseconds; }

    double UiViewContext::glyphBudgetMs() const { return m_state->glyphBudgetMs; }

    u64 UiViewContext::textLayoutCount() const { return m_state->textCache.layouts; }

    u64 UiViewContext::richTextParseCount() const { return m_state->richCache.parses; }
    sizet UiViewContext::richTextCachedCount() const { return m_state->richCache.size(); }


    u64 UiViewContext::glyphsGenerated() const { return m_state->glyphsGenerated; }

    u32 UiViewContext::glyphWaitingCount() const { return static_cast<u32>(m_state->waiting.elementCount()); }

    std::vector<UiFontInfo> UiViewContext::fontInfo() const
    {
        const State& state = *m_state;
        std::vector<UiFontInfo> infos;
        for (FontId id = 0; id < state.fonts.size(); ++id)
        {
            const UiFont& font = *state.fonts[id];
            UiFontInfo& info = infos.emplace_back();
            info.name = std::string(font.name.toView());
            for (const FontId fallback : font.fallbacks)
            {
                info.fallbacks.emplace_back(state.fonts[fallback]->name.toView());
            }
            const UiFontCounts& counts = font.counts();
            info.ready = counts.ready;
            info.blank = counts.blank;
            info.pending = counts.pending;
            info.failed = counts.failed;
            info.absent = counts.absent;
            info.copied = counts.copied;
            for (u32 page = 0; page < font.pages().size(); ++page)
            {
                const UiFontPage& source = font.pages()[page];
                const auto ref = std::ranges::find_if(state.atlasPages, [&](const State::PageRef& candidate)
                                                      { return candidate.font == id && candidate.page == page; });
                info.pages.push_back({static_cast<u32>(ref - state.atlasPages.begin()), source.penY + source.rowHeight,
                                      source.version,
                                      state.textures.entries[font.pageTextures[page]].bindlessIndex.has_value()});
            }
        }
        return infos;
    }

    u32 UiViewContext::atlasPageCount() const { return static_cast<u32>(m_state->atlasPages.size()); }

    UiAtlasPage UiViewContext::atlasPage(const u32 index) const
    {
        const State& state = *m_state;
        SASSERTM(index < state.atlasPages.size(), "atlasPage: unknown page.");
        const UiFont& font = *state.fonts[state.atlasPages[index].font];
        const UiFontPage& page = font.pages()[state.atlasPages[index].page];
        const UiTextureId texture = font.pageTextures[state.atlasPages[index].page];
        return {texture, UiFont::PAGE_SIZE, page.version, state.textures.entries[texture].backing, page.rgba8};
    }

    bool UiViewContext::atlasDamageSince(const u32 index, const u64 held, std::vector<UiAtlasRect>& rects) const
    {
        const State& state = *m_state;
        SASSERTM(index < state.atlasPages.size(), "atlasDamageSince: unknown page.");
        rects.clear();
        const UiFontPage& page = state.fonts[state.atlasPages[index].font]->pages()[state.atlasPages[index].page];
        if (held < page.baseVersion)
        {
            return false;
        }
        for (const UiPageDamage& cell : page.damage)
        {
            if (cell.version > held)
            {
                rects.push_back({cell.x, cell.y, cell.width, cell.height});
            }
        }
        return true;
    }

    namespace
    {
        UiTextureId shownBy(const UiTextureTable& textures, const u64 imageSlot)
        {
            const auto at = textures.imageSlots.find(imageSlot);
            return at == textures.imageSlots.end() ? NO_UI_TEXTURE : at->second;
        }
    } // namespace

    void UiViewContext::registerImage(const u64 imageSlot, const InternedString assetName)
    {
        UiTextureTable& textures = m_state->textures;
        const UiTextureId previous = shownBy(textures, imageSlot);
        textures.imageSlots[imageSlot] = textures.forAsset(assetName);
        textures.releaseIfUnused(previous);
        m_state->inputsChanged = true;
    }

    void UiViewContext::registerImage(const u64 imageSlot, const u32 bindlessIndex)
    {
        UiTextureTable& textures = m_state->textures;
        const UiTextureId previous = shownBy(textures, imageSlot);
        textures.imageSlots[imageSlot] = textures.forBindless(bindlessIndex);
        textures.releaseIfUnused(previous);
        m_state->inputsChanged = true;
    }

    void UiViewContext::registerImageFile(const u64 imageSlot, const std::string_view path)
    {
        UiTextureTable& textures = m_state->textures;
        const UiTextureId previous = shownBy(textures, imageSlot);
        const UiTextureId shown = textures.forFile(InternedString(path));
        if (previous == shown)
        {
            return; // the same file again changes nothing
        }
        textures.imageSlots[imageSlot] = shown;
        textures.releaseIfUnused(previous);
        m_state->inputsChanged = true;
    }

    void UiViewContext::clearImage(const u64 imageSlot)
    {
        UiTextureTable& textures = m_state->textures;
        const auto at = textures.imageSlots.find(imageSlot);
        if (at == textures.imageSlots.end())
        {
            return;
        }
        const UiTextureId previous = at->second;
        textures.imageSlots.erase(at);
        textures.releaseIfUnused(previous);
        m_state->inputsChanged = true;
    }

    std::optional<UiImageInfo> UiViewContext::imageInfo(const u64 imageSlot) const
    {
        const UiTextureTable& textures = m_state->textures;
        const auto at = textures.imageSlots.find(imageSlot);
        if (at == textures.imageSlots.end() || !textures.entries[at->second].file.isValid())
        {
            return std::nullopt;
        }
        const UiTextureTable::Entry& entry = textures.entries[at->second];
        return UiImageInfo{entry.bindlessIndex ? UiImageState::Ready
                               : entry.failed  ? UiImageState::Failed
                                               : UiImageState::Pending,
                           entry.width, entry.height};
    }

    std::span<const UiTextureId> UiViewContext::pendingTextures() const { return m_state->textures.pending; }

    bool UiViewContext::canResetGpuTextures() const
    {
        const UiTextureTable& textures = m_state->textures;
        return std::ranges::none_of(textures.imageSlots,
                                    [&textures](const auto& slot)
                                    {
                                        const UiTextureTable::Entry& entry = textures.entries[slot.second];
                                        return !entry.asset.isValid() && !entry.file.isValid() &&
                                            entry.font == NO_FONT && entry.bindlessIndex &&
                                            (*entry.bindlessIndex & UI_VIEW_TEXTURE_FLAG) == 0;
                                    });
    }

    void UiViewContext::resetGpuTextures()
    {
        UiTextureTable& textures = m_state->textures;
        textures.pending.clear();
        for (UiTextureId id = 0; id < textures.entries.size(); ++id)
        {
            UiTextureTable::Entry& entry = textures.entries[id];
            if (entry.released || (!entry.asset.isValid() && !entry.file.isValid() && entry.font == NO_FONT))
                continue;
            entry.bindlessIndex.reset();
            entry.backing = 0;
            entry.sprite.reset();
            entry.width = entry.height = 0;
            entry.failed = false;
            textures.pending.push_back(id);
        }
        m_state->inputsChanged = true;
    }

    UiTextureSource UiViewContext::textureSource(const UiTextureId texture) const
    {
        const UiTextureTable::Entry& entry = m_state->textures.entries[texture];
        return {entry.asset, entry.file, entry.font, entry.page};
    }

    void UiViewContext::resolveTexture(const UiTextureId texture, const u32 bindlessIndex, const u64 backing)
    {
        UiTextureTable& textures = m_state->textures;
        UiTextureTable::Entry& entry = textures.entries[texture];
        entry.bindlessIndex = bindlessIndex;
        entry.backing = backing;
        std::erase(textures.pending, texture);
        // the elements waiting on the page or image repaint alone
        m_state->wake(UiWaitSet::pageKey(texture));
    }

    void UiViewContext::resolveSprite(const UiTextureId texture, const u32 bindlessIndex, const UiSpriteRect& sprite)
    {
        UiTextureTable::Entry& entry = m_state->textures.entries[texture];
        entry.sprite = sprite;
        resolveTexture(texture, bindlessIndex);
    }

    void UiViewContext::resolveImageFile(const UiTextureId texture, const u32 bindlessIndex, const u32 width,
                                         const u32 height)
    {
        UiTextureTable::Entry& entry = m_state->textures.entries[texture];
        entry.width = width;
        entry.height = height;
        resolveTexture(texture, bindlessIndex);
    }

    std::vector<UiTextureId> UiViewContext::takeReleasedTextures()
    {
        std::vector<UiTextureId> taken;
        taken.swap(m_state->textures.released);
        return taken;
    }

    void UiViewContext::failTexture(const UiTextureId texture)
    {
        UiTextureTable& textures = m_state->textures;
        textures.entries[texture].failed = true;
        std::erase(textures.pending, texture); // a failure is final: nothing left for the resolver to poll
        m_state->wake(UiWaitSet::pageKey(texture));
    }

    InternedString UiViewContext::styleIconOf(const Entity element, const UiInteractionState interaction) const
    {
        const auto* identity = m_state->world->tryGetConstComponent<UiIdentityComponent>(element);
        return m_state->stylesFor(element)
            .resolve(identity ? identity->id : InternedString{}, identity ? identity->kind : UiWidgetKind::Panel,
                     interaction, identity ? identity->styleClass : InternedString{})
            .icon;
    }

    std::optional<UiShownImage> UiViewContext::shownImage(const Entity element) const
    {
        const State& state = *m_state;
        const u32 position = state.ui->hierarchy().layout().paintIndex(element);
        if (position == UiHierarchyManager::NO_INDEX)
        {
            return std::nullopt;
        }
        const auto records = state.retained.recordsOf(position);
        for (sizet i = records.size(); i-- > 0;)
        {
            const UiPaintRecord& record = records[i];
            if (record.kind != UiPaintKind::Image)
            {
                continue;
            }
            const UiTextureTable::Entry& entry = state.textures.entries[record.texture];
            return UiShownImage{entry.asset,
                                entry.bindlessIndex ? UiImageState::Ready
                                    : entry.failed  ? UiImageState::Failed
                                                    : UiImageState::Pending,
                                entry.sprite.has_value()};
        }
        return std::nullopt;
    }

    void UiViewContext::settleStyle()
    {
        State& state = *m_state;
        state.syncLayoutTokens();
        if (state.styles.generation() != state.measuredStyleGeneration ||
            state.subtreeGeneration() != state.measuredSubtreeGeneration)
        {
            state.measuredStyleGeneration = state.styles.generation();
            state.measuredSubtreeGeneration = state.subtreeGeneration();
            state.textCache.clear();
            state.subtreeTextCache.clear();
            state.ui->hierarchy().requeueObservedSizes();
            state.ui->hierarchy().resolveLayout(state.ui->hierarchy().layoutScale());
        }
    }

    bool UiViewContext::applyDisplay(const float layoutScale, const float textScale)
    {
        State& state = *m_state;
        UiHierarchyManager& hierarchy = state.ui->hierarchy();
        const bool scaleChanged =
            std::isfinite(layoutScale) && layoutScale > 0.0f && layoutScale != hierarchy.layoutScale();
        const bool textChanged = state.styles.setTextScale(textScale);
        if (state.subtreeStylesheet)
        {
            state.subtreeStylesheet->setTextScale(textScale);
        }
        hierarchy.setTextScale(state.styles.textScale());
        if (scaleChanged || textChanged)
        {
            // the text scale's generation is measured here: the next update would resolve a second time
            hierarchy.requeueObservedSizes();
            hierarchy.resolveLayout(scaleChanged ? layoutScale : hierarchy.layoutScale());
            state.measuredStyleGeneration = state.styles.generation();
            state.measuredSubtreeGeneration = state.subtreeGeneration();
            state.measuredFontCount = state.fonts.size();
        }
        return scaleChanged || textChanged;
    }

    void UiViewContext::setCustomPaint(UiCustomPaint callback)
    {
        SASSERTM(!m_state->inCustomPaint, "Custom paint cannot change registration during repaint.");
        if (m_state->inCustomPaint)
            return;
        m_state->customPaint = std::move(callback);
        m_state->inputsChanged = true;
    }

    void UiViewContext::invalidateCustomPaint(const Entity element)
    {
        SASSERTM(!m_state->inCustomPaint, "Custom paint cannot invalidate during repaint.");
        if (m_state->inCustomPaint)
            return;
        m_state->ui->hierarchy().markPaintDirty(element);
    }

    bool UiViewContext::update(CommandBuffer* deferred)
    {
        State& state = *m_state;
        SASSERTM(!state.inCustomPaint, "Custom paint cannot repaint recursively.");
        if (state.inCustomPaint)
            return false;
        UiHierarchyManager& hierarchy = state.ui->hierarchy();
        state.styles.pollWatch();
        state.ensureFallbacks();
        state.syncLayoutTokens();
        // Content sizes measure with styles and fonts: re-resolve before
        // painting rather than show a frame of stale sizes.
        if (state.styles.generation() != state.measuredStyleGeneration ||
            state.subtreeGeneration() != state.measuredSubtreeGeneration ||
            state.fonts.size() != state.measuredFontCount)
        {
            state.measuredStyleGeneration = state.styles.generation();
            state.measuredSubtreeGeneration = state.subtreeGeneration();
            state.measuredFontCount = state.fonts.size();
            state.textCache.clear();
            state.subtreeTextCache.clear();
            hierarchy.requeueObservedSizes(); // a dock's tab strip and a table's columns measure text
            hierarchy.resolveLayout(hierarchy.layoutScale());
        }
        state.generateGlyphs();
        const UiRouter& router = state.ui->router();
        // The focused look shows where the keys go: after a key moved the focus, or on a field that takes text. A
        // press focuses a button or a checkbox without leaving a ring on it.
        const Entity focusShown = router.focused() != Entity::undefined() &&
                (router.focusedByKeys() ||
                 state.world->tryGetConstComponent<UiTextEditComponent>(router.focused()) != nullptr)
            ? router.focused()
            : Entity::undefined();
        const auto elementCount = static_cast<u32>(hierarchy.paintOrderOf().size());
        // a bound root paints clipped to its window; a window that moved or a binding that changed repaints
        // everything, so the clips and the windows' instance runs follow
        state.windowSurfaces.clear();
        for (const auto& [root, window] : hierarchy.windowRoots())
        {
            const std::optional<UiPaintRange> range = hierarchy.paintRangeOf(root);
            const std::optional<UiRect> rect = hierarchy.windowRect(window);
            if (range && rect && range->count > 0)
            {
                state.windowSurfaces.push_back({range->start, range->start + range->count, *rect, window});
            }
        }
        if (!std::ranges::equal(state.windowSurfaces, state.paintedWindowSurfaces,
                                [](const UiWindowSurface& a, const UiWindowSurface& b)
                                {
                                    return a.start == b.start && a.end == b.end && a.window == b.window &&
                                        a.rect.position == b.rect.position && a.rect.size == b.rect.size;
                                }))
        {
            state.inputsChanged = true;
        }
        // nullopt repaints everything: a full resolve, an overflowing damage
        // log, or an input every element paints with
        const std::optional<std::span<const UiPaintRange>> damage = state.inputsChanged ||
                state.styles.generation() != state.styleGeneration ||
                state.subtreeGeneration() != state.paintedSubtreeGeneration || !state.retained.hasElements(elementCount)
            ? std::nullopt
            : hierarchy.paintDamageSince(state.paintGeneration);
        std::array<Entity, 6> changedStates;
        u32 changedStateCount = 0;
        for (const auto& [last, now] :
             {std::pair{state.hovered, router.hovered()}, std::pair{state.pressed, router.pressed()},
              std::pair{state.focused, focusShown}})
        {
            if (last != now)
            {
                changedStates[changedStateCount++] = last;
                changedStates[changedStateCount++] = now;
            }
        }
        // A canvas whose style reads the same in its old and new interaction state is not repainted for the change:
        // lowering its primitives again would cost as much as a content update.
        bool onlySkippable = changedStateCount != 0;
        if (changedStateCount != 0 && state.ui->canvasCount() != 0)
        {
            const auto interactionOf =
                [&](const Entity element, const Entity hovered, const Entity pressed, const Entity focused)
            {
                const auto* ui = state.world->tryGetConstComponent<UiStateComponent>(element);
                return uiInteractionState(!ui || ui->interactable, element == pressed, element == hovered,
                                          element == focused);
            };
            for (u32 i = 0; i < changedStateCount; ++i)
            {
                const Entity element = changedStates[i];
                if (element == Entity::undefined() || !state.ui->canvasContent(element))
                {
                    continue;
                }
                const auto* identity = state.world->tryGetConstComponent<UiIdentityComponent>(element);
                const auto resolveIn = [&](const UiInteractionState interaction)
                {
                    return state.stylesFor(element).resolve(
                        identity ? identity->id : InternedString{}, identity ? identity->kind : UiWidgetKind::Panel,
                        interaction, identity ? identity->styleClass : InternedString{});
                };
                if (resolveIn(interactionOf(element, state.hovered, state.pressed, state.focused)) ==
                    resolveIn(interactionOf(element, router.hovered(), router.pressed(), focusShown)))
                {
                    changedStates[i] = Entity::undefined();
                }
            }
        }
        if (onlySkippable)
        {
            onlySkippable = std::all_of(changedStates.begin(), changedStates.begin() + changedStateCount,
                                        [](const Entity element) { return element == Entity::undefined(); });
        }
        if (hierarchy.layoutGeneration() != state.countedLayoutGeneration)
        {
            state.countedLayoutGeneration = hierarchy.layoutGeneration();
            SPITE_COUNTER("ui.resolvedElements", hierarchy.resolvedElementCount());
        }
        if (damage && damage->empty() && (changedStateCount == 0 || onlySkippable))
        {
            if (onlySkippable)
            {
                // the state change is noted without a repaint: a later one compares against these
                state.hovered = router.hovered();
                state.pressed = router.pressed();
                state.focused = focusShown;
            }
            return false;
        }

        SPITE_ZONE("UiViewRepaint");
        state.inputsChanged = false;
        state.paintGeneration = hierarchy.paintGeneration();
        state.styleGeneration = state.styles.generation();
        state.paintedSubtreeGeneration = state.subtreeGeneration();
        state.hovered = router.hovered();
        state.pressed = router.pressed();
        state.focused = focusShown;
        state.paintedCount = 0;
        state.clipCache = {};
        if (!hierarchy.visualOwners().empty())
        {
            state.visuals.build(hierarchy, *state.world);
        }
        else
        {
            state.visuals = {};
        }
        state.hasVisuals = !state.visuals.empty();

        const UiPaintInputs inputs{*state.world,
                                   hierarchy.layout(),
                                   state.styles,
                                   state.fonts,
                                   state.hovered,
                                   state.pressed,
                                   state.focused,
                                   hierarchy.layoutScale(),
                                   state.targetSize,
                                   state.windowSurfaces,
                                   &state.pendingTransitions,
                                   &state.richCache,
                                   &state.textArea,
                                   state.ui,
                                   state.caseLocale(),
                                   state.stylesheetRoot,
                                   state.subtreeStylesheet};
        UiFrameGeometry& frame = state.retained.frame();
        const u64 generation = ++g_frameGeneration;
        state.retained.begin();
        if (damage)
        {
            for (const UiPaintRange range :
                 state.retained.collectDirty(hierarchy, *damage, std::span(changedStates.data(), changedStateCount)))
            {
                state.repaint(inputs, range.start, range.start + range.count, generation, false);
            }
        }
        else
        {
            state.retained.resetRecords(elementCount, state.spareTexts);
            state.retained.reset(elementCount);
            state.repaint(inputs, 0, elementCount, generation, true);
            state.topUpTexts();
        }
        hierarchy.consumePaintDamage(state.paintGeneration);
        // the area is the focused input's while it is laid out and shown; its paint reports it again on any change
        state.ui->setTextInputArea(state.textArea.owner != Entity::undefined() &&
                                           state.textArea.owner == state.focused &&
                                           hierarchy.isVisibleOf(state.textArea.owner)
                                       ? std::optional(state.textArea.area)
                                       : std::nullopt);
        if (!state.pendingTransitions.empty())
        {
            state.reconcileTransitions(deferred);
        }
        SPITE_COUNTER("ui.repaintedElements", state.paintedCount);
        frame.targetSize = state.targetSize;
        state.paintedWindowSurfaces = state.windowSurfaces;
        frame.windowRuns.clear();
        for (const UiWindowSurface& surface : state.windowSurfaces)
        {
            frame.windowRuns.push_back({surface.window, state.retained.offset(surface.start),
                                        state.retained.offset(surface.end), surface.rect.position});
        }
        std::ranges::sort(frame.windowRuns, {}, &UiWindowRun::first);
        state.retained.finish(generation, damage.has_value());
        return true;
    }

    std::span<const UiPaintRecord> UiViewContext::paintRecords() const { return m_state->retained.records(); }

    u32 UiViewContext::paintedElementCount() const { return m_state->paintedCount; }

    const UiFrameGeometry& UiViewContext::frame() const { return m_state->retained.frame(); }

    const UiSeriesTable& UiViewContext::series() const { return m_state->ui->series(); }
} // namespace spite
