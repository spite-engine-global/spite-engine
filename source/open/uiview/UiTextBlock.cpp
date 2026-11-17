#include "UiTextBlock.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace spite
{
    namespace
    {
        struct Item
        {
            const UiGlyph* glyph = nullptr;
            const UiFont* font = nullptr;
            float kern = 0.0f; // px added to the pen before this glyph when it follows one of the same face
            float advance = 0.0f;
            float offsetX = 0.0f; // shaped glyphs: px from the pen
            float offsetY = 0.0f;
            u32 codepoint = 0; // a shaped glyph: the first codepoint of its cluster
            u32 cluster = 0; // byte offset in the text
            bool space = false; // takes no visible width at a line end
            bool newline = false;
            bool breakAfter = false;
            bool clusterStart = true; // lines break and cut only before a cluster's first glyph
            bool rtl = false; // of a right-to-left shaped run: drawn in reverse within its line
            bool ltrInRtl = false; // an Arabic-Indic digit: keeps its order inside the reversed run (UAX #9 EN)
            bool joins = false; // of a cursive run (Arabic, Syriac, Thaana...): its letters take no letter spacing
            u32 span = NO_SPAN; // rich text: the span of the item
            float ascent = 0.0f; // rich text: px above and below the baseline
            float descent = 0.0f;
        };

        // per thread scratch: layout allocates nothing in steady state
        thread_local std::vector<Item> items;
        thread_local std::vector<float> startX;
        thread_local std::vector<float> endX;
        thread_local bool anyRtl = false;
        thread_local u32 clusterBase = 0; // byte offset of the text buildItems reads, in the whole text

        bool isSpace(const u32 cp) { return cp == ' ' || cp == '\t' || cp == 0x3000; }

        bool isArabicDigit(const u32 cp) { return (cp >= 0x0660 && cp <= 0x0669) || (cp >= 0x06F0 && cp <= 0x06F9); }

        bool isRtl(const u32 cp)
        {
            return (cp >= 0x0590 && cp <= 0x08FF) || (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFE) ||
                (cp >= 0x10800 && cp <= 0x10FFF) || (cp >= 0x1E800 && cp <= 0x1EFFF);
        }

        // a letter of a script that joins its neighbours (Arabic, Syriac, Thaana, N'Ko, Mandaic...): letter spacing
        // would break the word, so a run of them takes none; Hebrew and Samaritan letters stand alone like Latin
        bool uiLettersJoin(const u32 cp)
        {
            const bool hebrew = (cp >= 0x0590 && cp <= 0x05FF) || (cp >= 0xFB1D && cp <= 0xFB4F);
            const bool samaritan = cp >= 0x0800 && cp <= 0x083F;
            return isRtl(cp) && !hebrew && !samaritan;
        }

        // combining marks of the blocks every script uses and variation selectors: shaped with the character before
        bool attachesToPrevious(const u32 cp)
        {
            return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
                (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xFE20 && cp <= 0xFE2F) || (cp >= 0xE0100 && cp <= 0xE01EF);
        }

        // CJK ideographs, kana, bopomofo and fullwidth forms: a break opportunity sits beside them
        bool isIdeographic(const u32 cp)
        {
            return (cp >= 0x2E80 && cp <= 0x312F && cp != 0x3000) || (cp >= 0x3400 && cp <= 0x4DBF) ||
                (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFFEF) ||
                (cp >= 0x20000 && cp <= 0x2FA1F);
        }

        // closing punctuation, small kana and the prolonged sound mark: never start a line
        bool noLineStart(const u32 cp)
        {
            switch (cp)
            {
            case ',':
            case '.':
            case ';':
            case ':':
            case '!':
            case '?':
            case ')':
            case ']':
            case '}':
            case 0x3001:
            case 0x3002:
            case 0x3005:
            case 0x3009:
            case 0x300B:
            case 0x300D:
            case 0x300F:
            case 0x3011:
            case 0x3015:
            case 0x30FC:
            case 0x2025:
            case 0x2026:
            case 0xFF01:
            case 0xFF09:
            case 0xFF0C:
            case 0xFF0E:
            case 0xFF1A:
            case 0xFF1B:
            case 0xFF1F:
            case 0xFF3D:
            case 0xFF5D:
                return true;
            default:
                return (cp >= 0x3041 && cp <= 0x3049 && (cp & 1) == 1) || cp == 0x3063 || cp == 0x3083 ||
                    cp == 0x3085 || cp == 0x3087 || cp == 0x308E || (cp >= 0x30A1 && cp <= 0x30A9 && (cp & 1) == 1) ||
                    cp == 0x30C3 || cp == 0x30E3 || cp == 0x30E5 || cp == 0x30E7 || cp == 0x30EE;
            }
        }

        // opening punctuation: never ends a line
        bool noLineEnd(const u32 cp)
        {
            switch (cp)
            {
            case '(':
            case '[':
            case '{':
            case 0x3008:
            case 0x300A:
            case 0x300C:
            case 0x300E:
            case 0x3010:
            case 0x3014:
            case 0xFF08:
            case 0xFF3B:
            case 0xFF5B:
                return true;
            default:
                return false;
            }
        }

        bool isDigit(const u32 cp) { return cp >= '0' && cp <= '9'; }

        bool isLetter(const u32 cp)
        {
            return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || (cp >= 0xC0 && !isIdeographic(cp));
        }

        // Whether a line may break between two adjacent glyphs (UAX #14-lite).
        bool canBreakBetween(const Item* before, const Item& a, const Item& b)
        {
            if (a.space)
            {
                return true;
            }
            if (b.space || b.newline)
            {
                return false;
            }
            if (a.codepoint == '-' || a.codepoint == 0x2010)
            {
                // a hyphen joins two words: a leading or doubled one leaves no line of only hyphens
                return isLetter(b.codepoint) && before && (isLetter(before->codepoint) || isDigit(before->codepoint));
            }
            return (isIdeographic(a.codepoint) || isIdeographic(b.codepoint)) && !noLineStart(b.codepoint) &&
                !noLineEnd(a.codepoint);
        }

        void pushItem(const Item& item)
        {
            if (!items.empty())
            {
                Item& before = items.back();
                const Item* beforeThat = items.size() >= 2 ? &items[items.size() - 2] : nullptr;
                before.breakAfter = item.clusterStart && !before.newline && canBreakBetween(beforeThat, before, item);
            }
            items.push_back(item);
        }

        // The face a codepoint of a shaped run takes: the run's face while it has the codepoint (a run stays in one
        // face where it can), else the first of the chain that has it, else the primary (its .notdef: missing).
        const UiFont* runFaceOf(const UiFonts fonts, const UiFont& base, const UiFont* current, const u32 codepoint)
        {
            if (current && current->hasCodepoint(codepoint))
            {
                return current;
            }
            if (base.hasCodepoint(codepoint))
            {
                return &base;
            }
            for (const FontId id : base.fallbacks)
            {
                if (fonts[id]->hasCodepoint(codepoint))
                {
                    return fonts[id].get();
                }
            }
            return &base;
        }

        void pushShaped(const UiFont& base, const UiFont& face, const std::string_view text, const sizet first,
                        const sizet last, const float sizePx, const bool digits)
        {
            const UiShapedRun& run = face.shape(text.substr(first, last - first));
            const bool rtl = run.rtl || digits;
            anyRtl = anyRtl || rtl;
            for (const UiShapedGlyph& shaped : run.glyphs)
            {
                const auto cluster = clusterBase + static_cast<u32>(first + shaped.cluster);
                sizet at = first + shaped.cluster;
                const u32 codepoint = nextCodepoint(text, at);
                const UiGlyph* glyph = face.glyphByIndex(shaped.glyph);
                if (glyph->state == UiGlyphState::Absent)
                {
                    base.reportMissing(codepoint);
                    continue;
                }
                Item item;
                item.glyph = glyph;
                item.font = &face;
                item.advance = shaped.advance * sizePx;
                item.offsetX = shaped.offsetX * sizePx;
                item.offsetY = shaped.offsetY * sizePx;
                item.codepoint = codepoint;
                item.cluster = cluster;
                item.space = isSpace(codepoint);
                item.clusterStart = items.empty() || items.back().cluster != cluster;
                item.rtl = rtl;
                item.ltrInRtl = digits && !item.space;
                item.joins = rtl && !item.ltrInRtl &&
                    (item.space ? !items.empty() && items.back().joins : uiLettersJoin(codepoint));
                pushItem(item);
            }
        }

        // Shapes the run of text from `first` (a codepoint that needs shaping, or the character a mark attaches
        // to): it takes every following codepoint that needs shaping and the spaces between them, split where the
        // face or the direction changes. Returns where the run ends.
        sizet pushShapedRun(const UiFonts fonts, const UiFont& base, const std::string_view text, const sizet first,
                            const sizet complexAt, const float sizePx)
        {
            sizet end = complexAt;
            for (sizet scan = complexAt; scan < text.size();)
            {
                sizet next = scan;
                const u32 codepoint = nextCodepoint(text, next);
                if (uiNeedsShaping(codepoint))
                {
                    end = next;
                }
                else if (!isSpace(codepoint))
                {
                    break;
                }
                scan = next;
            }
            const UiFont* face = nullptr;
            bool rtl = false;
            bool digits = false; // Arabic-Indic digits: their own left-to-right runs inside the right-to-left text
            sizet runStart = first;
            for (sizet cursor = first; cursor < end;)
            {
                const sizet at = cursor;
                const u32 codepoint = nextCodepoint(text, cursor);
                if (isSpace(codepoint) && face)
                {
                    continue;
                }
                const UiFont* codepointFace = runFaceOf(fonts, base, face, codepoint);
                const bool codepointDigit = isArabicDigit(codepoint);
                const bool codepointRtl = isRtl(codepoint) && !codepointDigit;
                if (face && (codepointFace != face || codepointRtl != rtl || codepointDigit != digits))
                {
                    pushShaped(base, *face, text, runStart, at, sizePx, digits);
                    runStart = at;
                }
                face = codepointFace;
                rtl = codepointRtl;
                digits = codepointDigit;
            }
            pushShaped(base, *face, text, runStart, end, sizePx, digits);
            return end;
        }

        // appends the items of `text` (the caller clears items and sets clusterBase)
        void buildItems(const UiFonts fonts, const FontId primary, const std::string_view text, const float sizePx,
                        const bool newlines)
        {
            const UiFont& base = *fonts[primary];
            const UiGlyph* previous = nullptr;
            const UiFont* previousFont = nullptr;
            sizet simpleAt = 0; // the last item from the per-codepoint path, while it is items.back(): where it starts
            sizet simpleEnd = std::string_view::npos; // and ends
            for (sizet cursor = 0; cursor < text.size();)
            {
                const sizet at = cursor;
                const u32 codepoint = nextCodepoint(text, cursor);
                if (codepoint == '\r')
                {
                    continue;
                }
                if (uiNeedsShaping(codepoint))
                {
                    sizet first = at;
                    if (simpleEnd == at && attachesToPrevious(codepoint))
                    {
                        items.pop_back(); // shaped with its mark
                        first = simpleAt;
                    }
                    cursor = pushShapedRun(fonts, base, text, first, at, sizePx);
                    previous = nullptr;
                    simpleEnd = std::string_view::npos;
                    continue;
                }
                Item item;
                item.codepoint = codepoint;
                item.cluster = clusterBase + static_cast<u32>(at);
                if (codepoint == '\n' && newlines)
                {
                    item.newline = true;
                    items.push_back(item);
                    previous = nullptr;
                    simpleEnd = std::string_view::npos;
                    continue;
                }
                const UiFont* owner = nullptr;
                const UiGlyph* glyph = resolveUiGlyph(fonts, base, codepoint, owner);
                if (glyph->state == UiGlyphState::Absent)
                {
                    base.reportMissing(codepoint);
                    previous = nullptr;
                    simpleEnd = std::string_view::npos;
                    continue;
                }
                item.glyph = glyph;
                item.font = owner;
                item.kern = previous && previousFont == owner ? owner->kerning(*previous, *glyph) * sizePx : 0.0f;
                item.advance = glyph->advance * sizePx;
                item.space = isSpace(codepoint);
                pushItem(item);
                previous = glyph;
                previousFont = owner;
                simpleAt = at;
                simpleEnd = cursor;
            }
        }

        // Adds `spacing` px after each cluster, never taking the cluster's total advance below 0 (a cluster ending in a
        // zero-advance mark tightens like its precomposed form). A joining run takes none, nor a sprite or a newline.
        // The spacing sits on the cluster's last glyph, or its first in a right-to-left run (drawn reversed, so that
        // glyph is the last one on screen and the marks stay with their letter).
        void applyLetterSpacing(const float spacing)
        {
            if (spacing == 0.0f)
            {
                return;
            }
            for (sizet first = 0; first < items.size();)
            {
                sizet end = first + 1;
                while (end < items.size() && !items[end].clusterStart)
                {
                    ++end;
                }
                const Item& tail = items[end - 1];
                if (tail.glyph && !tail.joins)
                {
                    Item& target = tail.rtl && !tail.ltrInRtl ? items[first] : items[end - 1];
                    float others = 0.0f;
                    for (sizet i = first; i < end; ++i)
                    {
                        others += &items[i] == &target ? 0.0f : items[i].advance;
                    }
                    target.advance = std::max(-others, target.advance + spacing);
                }
                first = end;
            }
        }

        // The pen after the cluster that item i starts, given the pen after item i (a shaped cluster's later
        // glyphs carry no kerning).
        float clusterEndX(const u32 i, float x)
        {
            for (u32 j = i + 1; j < items.size() && !items[j].clusterStart; ++j)
            {
                x += items[j].advance;
            }
            return x;
        }

        // Appends items [begin, end) of one line in visual order: a right-to-left run reversed in place, pens
        // recomputed; without one, the pens of the logical pass. clusters (null: not wanted) gets their clusters.
        void emitLine(const u32 begin, const u32 end, std::vector<UiPlacedGlyph>& out, std::vector<u32>* clusters,
                      std::vector<u32>* richSpans = nullptr)
        {
            const auto place = [&](const Item& item, const float pen)
            {
                out.push_back({item.glyph, item.font, pen + item.offsetX, item.offsetY});
                if (clusters)
                {
                    clusters->push_back(item.cluster);
                }
                if (richSpans)
                {
                    richSpans->push_back(item.span);
                }
            };
            if (!anyRtl)
            {
                for (u32 i = begin; i < end; ++i)
                {
                    place(items[i], startX[i]);
                }
                return;
            }
            float pen = 0.0f;
            for (u32 i = begin; i < end;)
            {
                if (!items[i].rtl)
                {
                    const float x = pen + (i == begin ? 0.0f : items[i].kern);
                    place(items[i], x);
                    pen = x + items[i].advance;
                    ++i;
                    continue;
                }
                u32 runEnd = i;
                while (runEnd < end && items[runEnd].rtl)
                {
                    ++runEnd;
                }
                for (u32 k = runEnd; k > i;)
                {
                    u32 block = k - 1; // a digit block keeps its logical order
                    while (items[k - 1].ltrInRtl && block > i && items[block - 1].ltrInRtl)
                    {
                        --block;
                    }
                    for (u32 j = block; j < k; ++j)
                    {
                        place(items[j], pen);
                        pen += items[j].advance;
                    }
                    k = block;
                }
                i = runEnd;
            }
        }

        // "…" from the face chain, else three periods; empty when no face has either
        struct Ellipsis
        {
            std::array<Item, 3> glyphs;
            u32 count = 0;
            float width = 0.0f;
        };

        Ellipsis ellipsisOf(const UiFonts fonts, const FontId primary, const float sizePx, const float letterSpacing)
        {
            Ellipsis result;
            const UiFont& base = *fonts[primary];
            const UiFont* owner = nullptr;
            const UiGlyph* glyph = resolveUiGlyph(fonts, base, 0x2026, owner);
            u32 repeat = 1;
            if (glyph->state == UiGlyphState::Absent)
            {
                glyph = resolveUiGlyph(fonts, base, '.', owner);
                repeat = 3;
            }
            if (glyph->state == UiGlyphState::Absent)
            {
                return result;
            }
            for (u32 i = 0; i < repeat; ++i)
            {
                result.glyphs[i].glyph = glyph;
                result.glyphs[i].font = owner;
                result.glyphs[i].advance = std::max(0.0f, glyph->advance * sizePx + letterSpacing);
                result.width += result.glyphs[i].advance;
            }
            result.count = repeat;
            return result;
        }
    } // namespace

    // Breaks the built items into lines and emits them; `rich`: also each glyph's span and each line's metrics.
    static void breakLines(const UiFonts fonts, const FontId primary, const float sizePx,
                           const UiTextBlockParams& params, UiTextBlock& out, const bool rich)
    {
        out.glyphs.clear();
        out.lines.clear();
        out.spanOf.clear();
        out.width = 0.0f;
        applyLetterSpacing(params.letterSpacing);
        const auto count = static_cast<u32>(items.size());
        startX.resize(count);
        endX.resize(count);

        const bool bounded = params.width > 0.0f;
        const bool softWrap = params.wrap == UiTextWrap::Word && bounded;
        const bool ellipsisMode = params.overflow != UiTextOverflow::Clip;
        const bool middleMode = params.overflow == UiTextOverflow::MiddleEllipsis && !anyRtl;
        const float limit = bounded ? params.width : std::numeric_limits<float>::infinity();
        // the same tolerance in measure (unscaled) and paint (scaled) breaks lines at the same glyph
        const auto fits = [limit](const float x) { return x <= limit + 1e-4f * limit + 1e-3f; };
        Ellipsis ellipsis;
        bool ellipsisReady = false;

        u32 begin = 0;
        bool softBreak = false;
        bool first = true;
        while (first || begin < count)
        {
            first = false;
            if (softBreak)
            {
                while (begin < count && items[begin].space)
                {
                    ++begin;
                }
                if (begin >= count)
                {
                    break;
                }
            }
            const bool lastLine = params.maxLines != 0 && out.lines.size() + 1 == params.maxLines;
            const bool breakOnWidth = softWrap && !(lastLine && ellipsisMode);

            u32 segEnd = count;
            u32 next = count;
            bool soft = false;
            bool hasMore = false;
            i32 lastBreak = -1;
            float x = 0.0f;
            for (u32 i = begin;; ++i)
            {
                if (i == count)
                {
                    break;
                }
                const Item& item = items[i];
                if (item.newline)
                {
                    segEnd = i;
                    next = i + 1;
                    hasMore = next < count;
                    break;
                }
                const float xi = x + (i == begin ? 0.0f : item.kern);
                const float xe = xi + item.advance;
                if (breakOnWidth && !item.space && item.clusterStart && i > begin && !fits(clusterEndX(i, xe)))
                {
                    segEnd = lastBreak >= static_cast<i32>(begin) ? static_cast<u32>(lastBreak) + 1 : i;
                    next = segEnd;
                    soft = true;
                    break;
                }
                startX[i] = xi;
                endX[i] = xe;
                x = xe;
                if (item.breakAfter)
                {
                    lastBreak = static_cast<i32>(i);
                }
            }

            u32 trimmed = segEnd;
            while (trimmed > begin && items[trimmed - 1].space)
            {
                --trimmed;
            }
            const float visible =
                trimmed > begin ? std::max(0.0f, endX[trimmed - 1]) : 0.0f; // kerning can lead below 0
            UiTextLine line;
            line.first = static_cast<u32>(out.glyphs.size());
            // a wrapped line before the last never cuts: it is only wider than the line when one glyph is
            const bool cut =
                ellipsisMode && !soft && ((lastLine && hasMore) || (!fits(visible) && (!softWrap || lastLine)));
            u32 shown = trimmed;
            u32 tail = trimmed; // middle mode: items [tail, trimmed) follow the ellipsis
            float lineWidth = visible;
            if (cut)
            {
                if (!ellipsisReady)
                {
                    ellipsis = ellipsisOf(fonts, primary, sizePx, params.letterSpacing);
                    ellipsisReady = true;
                }
                if (middleMode && !fits(visible))
                {
                    // the head takes up to half the room around the ellipsis, the tail the rest the head left
                    const float half = (limit - ellipsis.width) * 0.5f;
                    shown = begin;
                    for (u32 i = begin + 1; i < trimmed && endX[i - 1] <= half; ++i)
                    {
                        if (items[i].clusterStart)
                        {
                            shown = i;
                        }
                    }
                    while (shown > begin && items[shown - 1].space) // no gap before the dots, as the end ellipsis
                    {
                        --shown;
                    }
                    lineWidth = shown > begin ? endX[shown - 1] : 0.0f;
                    for (u32 i = trimmed; i-- > shown;)
                    {
                        if (!items[i].clusterStart)
                        {
                            continue;
                        }
                        if (!fits(lineWidth + ellipsis.width + endX[trimmed - 1] - startX[i]))
                        {
                            break;
                        }
                        tail = items[i].space ? tail : i;
                    }
                }
                else
                {
                    // the longest prefix that leaves room for the ellipsis
                    for (shown = trimmed;; --shown)
                    {
                        u32 end = shown;
                        while (end > begin && items[end - 1].space)
                        {
                            --end;
                        }
                        const float room = end > begin ? endX[end - 1] : 0.0f;
                        if (shown == begin ||
                            ((shown == count || items[shown].clusterStart) && fits(room + ellipsis.width)))
                        {
                            shown = end;
                            lineWidth = room;
                            break;
                        }
                    }
                }
            }
            emitLine(begin, shown, out.glyphs, nullptr, rich ? &out.spanOf : nullptr);
            if (rich)
            {
                for (u32 i = begin; i < shown; ++i)
                {
                    line.ascent = std::max(line.ascent, items[i].ascent);
                    line.descent = std::max(line.descent, items[i].descent);
                }
                if (shown == begin)
                {
                    line.ascent = fonts[primary]->ascender * sizePx;
                    line.descent = -fonts[primary]->descender * sizePx;
                }
            }
            if (cut)
            {
                float pen = lineWidth;
                for (u32 i = 0; i < ellipsis.count; ++i)
                {
                    if (rich)
                    {
                        out.spanOf.push_back(NO_SPAN);
                    }
                    out.glyphs.push_back({ellipsis.glyphs[i].glyph, ellipsis.glyphs[i].font, pen});
                    pen += ellipsis.glyphs[i].advance;
                }
                lineWidth = pen;
                if (tail < trimmed)
                {
                    const sizet from = out.glyphs.size();
                    emitLine(tail, trimmed, out.glyphs, nullptr, rich ? &out.spanOf : nullptr);
                    const float shift = pen - startX[tail];
                    for (sizet g = from; g < out.glyphs.size(); ++g)
                    {
                        out.glyphs[g].penX += shift;
                    }
                    if (rich)
                    {
                        for (u32 i = tail; i < trimmed; ++i)
                        {
                            line.ascent = std::max(line.ascent, items[i].ascent);
                            line.descent = std::max(line.descent, items[i].descent);
                        }
                    }
                    lineWidth = pen + endX[trimmed - 1] - startX[tail];
                }
            }
            line.count = static_cast<u32>(out.glyphs.size()) - line.first;
            line.width = lineWidth;
            out.lines.push_back(line);
            out.width = std::max(out.width, lineWidth);
            if (lastLine)
            {
                break;
            }
            begin = next;
            softBreak = soft;
        }
    }

    void layoutUiTextBlock(const UiFonts fonts, const FontId primary, const std::string_view text, const float sizePx,
                           const UiTextBlockParams& params, UiTextBlock& out)
    {
        items.clear();
        anyRtl = false;
        clusterBase = 0;
        buildItems(fonts, primary, text, sizePx, params.wrap != UiTextWrap::None);
        breakLines(fonts, primary, sizePx, params, out, false);
    }

    void layoutUiRichTextBlock(const UiFonts fonts, const FontId primary, const std::string_view plain,
                               const float sizePx, const std::span<const UiPaintSpan> spans,
                               const UiTextBlockParams& params, UiTextBlock& out)
    {
        items.clear();
        anyRtl = false;
        const bool newlines = params.wrap != UiTextWrap::None;
        for (sizet s = 0; s < spans.size();)
        {
            const UiPaintSpan& span = spans[s];
            const UiFont& face = *fonts[span.font];
            if (span.sprite != NO_UI_TEXTURE)
            {
                Item item;
                item.cluster = span.begin;
                item.codepoint = 0xFFFC;
                item.advance = span.aspect * (face.ascender - face.descender) * span.size;
                item.span = static_cast<u32>(s);
                item.ascent = face.ascender * span.size;
                item.descent = -face.descender * span.size;
                pushItem(item);
                ++s;
                continue;
            }
            // consecutive text spans in one face and size shape and kern as one text
            sizet end = s + 1;
            while (end < spans.size() && spans[end].sprite == NO_UI_TEXTURE && spans[end].font == span.font &&
                   spans[end].size == span.size)
            {
                ++end;
            }
            const sizet first = items.size();
            clusterBase = span.begin;
            buildItems(fonts, span.font, plain.substr(span.begin, spans[end - 1].end - span.begin), span.size,
                       newlines);
            for (sizet i = first; i < items.size(); ++i)
            {
                // the last span of the group that starts at or before the item's byte
                const auto owner = std::upper_bound(spans.begin() + static_cast<std::ptrdiff_t>(s),
                                                    spans.begin() + static_cast<std::ptrdiff_t>(end), items[i].cluster,
                                                    [](const u32 cluster, const UiPaintSpan& other)
                                                    { return cluster < other.begin; }) -
                    1;
                items[i].span = static_cast<u32>(owner - spans.begin());
                items[i].ascent = face.ascender * span.size;
                items[i].descent = -face.descender * span.size;
            }
            s = end;
        }
        breakLines(fonts, primary, sizePx, params, out, true);
    }

    float layoutShapedUiLine(const UiFonts fonts, const FontId primary, const std::string_view text, const float sizePx,
                             std::vector<UiPlacedGlyph>* placed, std::vector<u32>* clusters, const float letterSpacing)
    {
        items.clear();
        anyRtl = false;
        clusterBase = 0;
        buildItems(fonts, primary, text, sizePx, false);
        applyLetterSpacing(letterSpacing);
        const auto count = static_cast<u32>(items.size());
        startX.resize(count);
        float pen = 0.0f;
        for (u32 i = 0; i < count; ++i)
        {
            startX[i] = pen + (i == 0 ? 0.0f : items[i].kern);
            pen = startX[i] + items[i].advance;
        }
        if (placed)
        {
            emitLine(0, count, *placed, clusters);
        }
        return std::max(0.0f, pen);
    }
} // namespace spite
