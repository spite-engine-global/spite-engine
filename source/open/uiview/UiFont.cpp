#include "UiFont.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <harfbuzz/hb.h>
#include <msdfgen/msdfgen-ext.h>
#include <msdfgen/msdfgen.h>

#include "base/Logging.hpp"
#include "profiling/Zone.hpp"

namespace spite
{
    namespace
    {
        // The baked ASCII page is a pure function of the face file; baking it dominates a font's load, so every
        // later UiFont of the same file in the process copies it.
        struct Baked
        {
            float ascender;
            float descender;
            UiFontPage page;
            std::array<UiGlyph, UiFont::LAST_CHAR - UiFont::FIRST_CHAR + 1> glyphs;
            UiFontCounts counts;
        };
        std::mutex bakedMutex;
        std::map<std::string, Baked> bakedFaces;

        // A glyph's MSDF cell is a pure function of the face file and the glyph index; generating one is the slowest
        // part of showing text, so every later UiFont of the same file in the process copies the cell it made.
        struct GeneratedCell
        {
            u32 width = 0;
            u32 height = 0;
            std::vector<std::byte> rgba8; // rows top to bottom, as the page holds them
        };
        constexpr sizet MAX_CACHED_CELL_BYTES = 64u << 20;
        std::map<std::pair<std::string, u32>, GeneratedCell> generatedCells;
        sizet generatedCellBytes = 0;

        std::string bakedKey(const std::filesystem::path& path)
        {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            const auto written = std::filesystem::last_write_time(path, error).time_since_epoch().count();
            return std::format("{}|{}|{}", path.lexically_normal().string(), size, written);
        }

        // The cell of a glyph shape: its padded bounds in em and in atlas pixels.
        struct CellGeometry
        {
            double left;
            double bottom;
            u32 width;
            u32 height;
        };

        std::optional<CellGeometry> cellOf(const msdfgen::Shape& shape)
        {
            const msdfgen::Shape::Bounds bounds = shape.getBounds();
            if (shape.contours.empty() || bounds.l >= bounds.r || bounds.b >= bounds.t)
            {
                return std::nullopt;
            }
            const double pad = UiFont::PX_RANGE * 0.5 / UiFont::EM_PX;
            const double left = bounds.l - pad;
            const double bottom = bounds.b - pad;
            return CellGeometry{left, bottom, static_cast<u32>(std::ceil((bounds.r + pad - left) * UiFont::EM_PX)),
                                static_cast<u32>(std::ceil((bounds.t + pad - bottom) * UiFont::EM_PX))};
        }
    } // namespace

    struct UiFont::Shaper
    {
        std::vector<char> bytes; // the face file as read at load; HarfBuzz reads it in place
        hb_blob_t* blob = nullptr;
        hb_face_t* face = nullptr;
        hb_font_t* font = nullptr;
        hb_buffer_t* buffer = nullptr;
        hb_language_t language = HB_LANGUAGE_INVALID;
        float emPerUnit = 0.0f;

        ~Shaper()
        {
            hb_buffer_destroy(buffer);
            hb_font_destroy(font);
            hb_face_destroy(face);
            hb_blob_destroy(blob);
        }
    };

    // The bytes are read with the FreeType face (load), so both see the same file whatever happens to it later.
    std::unique_ptr<UiFont::Shaper> UiFont::makeShaper(const std::filesystem::path& path) const
    {
        auto shaper = std::make_unique<Shaper>();
        if (std::ifstream file(path, std::ios::binary | std::ios::ate); file)
        {
            shaper->bytes.resize(static_cast<sizet>(file.tellg()));
            file.seekg(0);
            file.read(shaper->bytes.data(), static_cast<std::streamsize>(shaper->bytes.size()));
            shaper->bytes.resize(static_cast<sizet>(std::max<std::streamsize>(file.gcount(), 0)));
        }
        if (shaper->bytes.empty())
        {
            SERROR_LOG("font '{}': cannot read '{}' for shaping; shaped text will show missing glyphs", name.toView(),
                       path.string());
        }
        shaper->blob = hb_blob_create(shaper->bytes.data(), static_cast<unsigned>(shaper->bytes.size()),
                                      HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        shaper->face = hb_face_create(shaper->blob, 0);
        shaper->font = hb_font_create(shaper->face);
        shaper->buffer = hb_buffer_create();
        // no language: the process locale must not change the glyphs (localized forms follow the locale later)
        shaper->language = hb_language_from_string("und", -1);
        const unsigned upem = std::max(hb_face_get_upem(shaper->face), 1u);
        hb_font_set_scale(shaper->font, static_cast<int>(upem), static_cast<int>(upem));
        shaper->emPerUnit = 1.0f / static_cast<float>(upem);
        return shaper;
    }

    Expected<std::unique_ptr<UiFont>> UiFont::load(msdfgen::FreetypeHandle* library, const InternedString name,
                                                   const std::filesystem::path& path)
    {
        std::unique_ptr<UiFont> font(new UiFont());
        font->name = name;
        font->m_path = path;
        font->m_handle = msdfgen::loadFont(library, path.string().c_str());
        if (!font->m_handle)
        {
            return std::unexpected(makeError(ErrorCode::AssetLoadFailed,
                                             std::format("font '{}': cannot load '{}'", name.toView(), path.string())));
        }
        msdfgen::FontMetrics metrics{};
        msdfgen::getFontMetrics(metrics, font->m_handle, msdfgen::FONT_SCALING_EM_NORMALIZED);
        font->ascender = static_cast<float>(metrics.ascenderY);
        font->descender = static_cast<float>(metrics.descenderY);
        font->m_shaper = font->makeShaper(path);

        const std::string key = bakedKey(path);
        font->m_cellKey = key;
        {
            const std::scoped_lock lock(bakedMutex);
            if (const auto it = bakedFaces.find(key); it != bakedFaces.end())
            {
                font->m_pages.push_back(it->second.page);
                font->m_ascii = it->second.glyphs;
                font->m_counts = it->second.counts;
                return font;
            }
        }

        font->m_pages.emplace_back().rgba8.assign(sizet{PAGE_SIZE} * PAGE_SIZE * 4, std::byte{0});
        font->m_baking = true;
        for (u32 codepoint = FIRST_CHAR; codepoint <= LAST_CHAR; ++codepoint)
        {
            UiGlyph& glyph = font->m_ascii[codepoint - FIRST_CHAR];
            font->loadMetrics(codepoint, glyph);
            if (glyph.state == UiGlyphState::Pending)
            {
                font->rasterize(glyph);
            }
        }
        font->m_baking = false;
        // the bake is the page's base: a consumer holding nothing copies it whole
        UiFontPage& page = font->m_pages[0];
        page.version = page.baseVersion = 1;
        page.damage.clear();
        {
            const std::scoped_lock lock(bakedMutex);
            bakedFaces.try_emplace(key, Baked{font->ascender, font->descender, page, font->m_ascii, font->m_counts});
        }
        return font;
    }

    UiFont::~UiFont()
    {
        if (m_handle)
        {
            msdfgen::destroyFont(m_handle);
        }
    }

    void UiFont::count(const UiGlyphState state, const int delta) const
    {
        u32& slot = state == UiGlyphState::Ready ? m_counts.ready
            : state == UiGlyphState::Blank       ? m_counts.blank
            : state == UiGlyphState::Pending     ? m_counts.pending
            : state == UiGlyphState::Failed      ? m_counts.failed
                                                 : m_counts.absent;
        slot = static_cast<u32>(static_cast<int>(slot) + delta);
    }

    void UiFont::loadMetrics(const u32 codepoint, UiGlyph& glyph) const
    {
        msdfgen::GlyphIndex index;
        loadIndexMetrics(msdfgen::getGlyphIndex(index, m_handle, codepoint) ? index.getIndex() : 0, glyph);
    }

    void UiFont::loadIndexMetrics(const u32 glyphIndex, UiGlyph& glyph) const
    {
        glyph.state = UiGlyphState::Absent;
        const msdfgen::GlyphIndex index(glyphIndex);
        msdfgen::Shape shape;
        double advance = 0.0;
        if (glyphIndex != 0 &&
            msdfgen::loadGlyph(shape, m_handle, index, msdfgen::FONT_SCALING_EM_NORMALIZED, &advance))
        {
            glyph.advance = static_cast<float>(advance);
            glyph.glyphIndex = index.getIndex();
            shape.normalize();
            if (const auto cell = cellOf(shape))
            {
                glyph.state = UiGlyphState::Pending;
                glyph.planeMin = Vec2{static_cast<float>(cell->left), static_cast<float>(cell->bottom)};
                glyph.planeMax = Vec2{static_cast<float>(cell->left + cell->width / EM_PX),
                                      static_cast<float>(cell->bottom + cell->height / EM_PX)};
            }
            else
            {
                glyph.state = UiGlyphState::Blank;
            }
        }
        count(glyph.state, 1);
    }

    bool UiFont::allocate(const u32 width, const u32 height, u16& page, u32& x, u32& y)
    {
        // Shelf packing into the newest page: deterministic for a given face and request order.
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            UiFontPage& current = m_pages.back();
            if (current.penX + width > PAGE_SIZE)
            {
                current.penX = 0;
                current.penY += current.rowHeight + 1;
                current.rowHeight = 0;
            }
            if (current.penY + height <= PAGE_SIZE)
            {
                page = static_cast<u16>(m_pages.size() - 1);
                x = current.penX;
                y = current.penY;
                current.penX += width + 1;
                current.rowHeight = std::max(current.rowHeight, height);
                return true;
            }
            if (m_pages.size() == MAX_PAGES)
            {
                return false;
            }
            m_pages.emplace_back().rgba8.assign(sizet{PAGE_SIZE} * PAGE_SIZE * 4, std::byte{0});
        }
        return false;
    }

    void UiFont::rasterize(UiGlyph& glyph)
    {
        msdfgen::Shape shape;
        msdfgen::loadGlyph(shape, m_handle, msdfgen::GlyphIndex(glyph.glyphIndex), msdfgen::FONT_SCALING_EM_NORMALIZED);
        shape.normalize();
        const std::optional<CellGeometry> cell = cellOf(shape);
        count(UiGlyphState::Pending, -1);
        u32 x = 0;
        u32 y = 0;
        if (!cell || !allocate(cell->width, cell->height, glyph.page, x, y))
        {
            glyph.state = UiGlyphState::Failed;
            count(glyph.state, 1);
            if (!std::exchange(m_fullReported, true))
            {
                SWARNING_LOG("font '{}': glyph atlas full ({} pages of {}x{}); further glyphs are not drawn",
                             name.toView(), MAX_PAGES, PAGE_SIZE, PAGE_SIZE);
            }
            return;
        }
        UiFontPage& page = m_pages[glyph.page];
        const std::pair<std::string, u32> cellKey{m_cellKey, glyph.glyphIndex};
        const auto rowAt = [&](const u32 row) { return &page.rgba8[((y + row) * PAGE_SIZE + x) * 4]; };
        bool copied = false;
        if (!m_baking)
        {
            const std::scoped_lock lock(bakedMutex);
            if (const auto it = generatedCells.find(cellKey);
                it != generatedCells.end() && it->second.width == cell->width && it->second.height == cell->height)
            {
                for (u32 row = 0; row < cell->height; ++row)
                {
                    std::memcpy(rowAt(row), &it->second.rgba8[sizet{row} * cell->width * 4], sizet{cell->width} * 4);
                }
                copied = true;
                ++m_counts.copied;
            }
        }
        if (!copied)
        {
            msdfgen::edgeColoringSimple(shape, 3.0);
            msdfgen::Bitmap<float, 3> bitmap(static_cast<int>(cell->width), static_cast<int>(cell->height));
            // Static instances of variable faces keep overlapping contours, which leave clashes at the
            // junctions; without geometry preprocessing (Skia) a scanline pass fixes the signs, then the
            // error correction runs on the corrected field (msdfgen's -scanline mode).
            const msdfgen::Projection projection(msdfgen::Vector2(EM_PX), msdfgen::Vector2(-cell->left, -cell->bottom));
            const msdfgen::Range range(PX_RANGE / EM_PX);
            msdfgen::generateMSDF(bitmap, shape, projection, range,
                                  msdfgen::MSDFGeneratorConfig(
                                      false, msdfgen::ErrorCorrectionConfig(msdfgen::ErrorCorrectionConfig::DISABLED)));
            msdfgen::distanceSignCorrection(bitmap, shape, projection, msdfgen::FILL_NONZERO);
            msdfgen::msdfErrorCorrection(bitmap, shape, projection, range, msdfgen::MSDFGeneratorConfig(false));

            const auto toByte = [](const float value)
            { return static_cast<std::byte>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f)); };
            for (u32 row = 0; row < cell->height; ++row)
            {
                // msdfgen rows run bottom-up; the page runs top-down
                std::byte* out = rowAt(cell->height - 1 - row);
                for (u32 column = 0; column < cell->width; ++column)
                {
                    const float* pixel = bitmap(static_cast<int>(column), static_cast<int>(row));
                    out[column * 4 + 0] = toByte(pixel[0]);
                    out[column * 4 + 1] = toByte(pixel[1]);
                    out[column * 4 + 2] = toByte(pixel[2]);
                    out[column * 4 + 3] = std::byte{255};
                }
            }
            const sizet bytes = sizet{cell->width} * cell->height * 4;
            const std::scoped_lock lock(bakedMutex);
            if (!m_baking && generatedCellBytes + bytes <= MAX_CACHED_CELL_BYTES)
            {
                GeneratedCell& entry = generatedCells[cellKey];
                entry.width = cell->width;
                entry.height = cell->height;
                entry.rgba8.resize(bytes);
                for (u32 row = 0; row < cell->height; ++row)
                {
                    std::memcpy(&entry.rgba8[sizet{row} * cell->width * 4], rowAt(row), sizet{cell->width} * 4);
                }
                generatedCellBytes += bytes;
            }
        }
        constexpr float SIZE = static_cast<float>(PAGE_SIZE);
        glyph.uvMin = Vec2{x / SIZE, y / SIZE};
        glyph.uvMax = Vec2{(x + cell->width) / SIZE, (y + cell->height) / SIZE};
        glyph.state = UiGlyphState::Ready;
        count(glyph.state, 1);
        page.damage.push_back({++page.version, static_cast<u16>(x), static_cast<u16>(y), static_cast<u16>(cell->width),
                               static_cast<u16>(cell->height)});
        if (page.damage.size() > MAX_PAGE_DAMAGE)
        {
            const auto half = page.damage.begin() + static_cast<std::ptrdiff_t>(MAX_PAGE_DAMAGE / 2);
            page.baseVersion = (half - 1)->version;
            page.damage.erase(page.damage.begin(), half);
        }
    }

    const UiGlyph* UiFont::generateOne()
    {
        if (!hasDemand())
        {
            return nullptr;
        }
        UiGlyph* glyph = m_demand[m_demandHead++];
        rasterize(*glyph);
        if (!hasDemand())
        {
            m_demand.clear();
            m_demandHead = 0;
        }
        return glyph;
    }

    const UiGlyph* UiFont::glyph(const u32 codepoint) const
    {
        if (codepoint >= FIRST_CHAR && codepoint <= LAST_CHAR)
        {
            return &m_ascii[codepoint - FIRST_CHAR];
        }
        const auto [it, inserted] = m_other.try_emplace(codepoint);
        if (inserted)
        {
            SPITE_ZONE("UiGlyphMetrics");
            loadMetrics(codepoint, it->second);
            if (it->second.state == UiGlyphState::Pending)
            {
                m_demand.push_back(&it->second);
            }
        }
        return &it->second;
    }

    const UiGlyph* UiFont::glyphByIndex(const u32 glyphIndex) const
    {
        const auto [it, inserted] = m_byIndex.try_emplace(glyphIndex);
        if (inserted)
        {
            SPITE_ZONE("UiGlyphMetrics");
            loadIndexMetrics(glyphIndex, it->second);
            if (it->second.state == UiGlyphState::Pending)
            {
                m_demand.push_back(&it->second);
            }
        }
        return &it->second;
    }

    bool UiFont::hasCodepoint(const u32 codepoint) const
    {
        if (codepoint >= FIRST_CHAR && codepoint <= LAST_CHAR)
        {
            return m_ascii[codepoint - FIRST_CHAR].state != UiGlyphState::Absent;
        }
        msdfgen::GlyphIndex index;
        return msdfgen::getGlyphIndex(index, m_handle, codepoint) && index.getIndex() != 0;
    }

    namespace
    {
        constexpr sizet SHAPED_ENTRY_OVERHEAD = 96; // hash node, key header, vector header
        sizet shapedRunBytes(const sizet textBytes, const UiShapedRun& run)
        {
            return SHAPED_ENTRY_OVERHEAD + textBytes + run.glyphs.capacity() * sizeof(UiShapedGlyph);
        }
    } // namespace

    const UiShapedRun& UiFont::shape(const std::string_view run) const
    {
        if (const auto it = m_shaped.find(run); it != m_shaped.end())
        {
            return it->second;
        }
        SPITE_ZONE("UiShape");
        ++m_shapeCount;
        // full by count or bytes: cleared (the measure cache's policy); the entry being added is made after
        if (m_shaped.size() >= MAX_SHAPED_RUNS || m_shapedBytes >= MAX_SHAPED_BYTES)
        {
            m_shaped.clear();
            m_shapedBytes = 0;
        }
        const Shaper& shaper = *m_shaper;
        hb_buffer_clear_contents(shaper.buffer);
        hb_buffer_add_utf8(shaper.buffer, run.data(), static_cast<int>(run.size()), 0, static_cast<int>(run.size()));
        hb_buffer_set_language(shaper.buffer, shaper.language);
        hb_buffer_guess_segment_properties(shaper.buffer);
        hb_shape(shaper.font, shaper.buffer, nullptr, 0);
        unsigned count = 0;
        const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(shaper.buffer, &count);
        const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(shaper.buffer, &count);

        // a run above the threshold is not kept (one huge paragraph must not evict or fill the cache)
        const bool keep = run.size() <= MAX_CACHED_RUN_BYTES;
        UiShapedRun& shaped = keep ? m_shaped.try_emplace(std::string(run)).first->second : m_uncached;
        shaped.glyphs.clear();
        shaped.rtl = HB_DIRECTION_IS_BACKWARD(hb_buffer_get_direction(shaper.buffer));
        shaped.glyphs.reserve(count);
        const float em = shaper.emPerUnit;
        for (unsigned i = 0; i < count; ++i)
        {
            shaped.glyphs.push_back(
                {infos[i].codepoint, infos[i].cluster, static_cast<float>(positions[i].x_advance) * em,
                 static_cast<float>(positions[i].x_offset) * em, static_cast<float>(positions[i].y_offset) * em});
        }
        // HarfBuzz returns a backward run in visual order; lines break in logical order
        if (shaped.rtl)
        {
            std::ranges::reverse(shaped.glyphs);
        }
        if (keep)
        {
            m_shapedBytes += shapedRunBytes(run.size(), shaped);
        }
        return shaped;
    }

    float UiFont::kerning(const UiGlyph& left, const UiGlyph& right) const
    {
        double kerning = 0.0;
        return msdfgen::getKerning(kerning, m_handle, msdfgen::GlyphIndex(left.glyphIndex),
                                   msdfgen::GlyphIndex(right.glyphIndex), msdfgen::FONT_SCALING_EM_NORMALIZED)
            ? static_cast<float>(kerning)
            : 0.0f;
    }

    void UiFont::reportMissing(const u32 codepoint) const
    {
        if (m_reportedMissing.insert(codepoint).second)
        {
            SWARNING_LOG("font '{}': no glyph for U+{:04X}", name.toView(), codepoint);
        }
    }

    u32 nextCodepoint(std::string_view text, sizet& cursor)
    {
        const auto lead = static_cast<u8>(text[cursor++]);
        const u32 length = lead < 0x80 ? 1
            : (lead >> 5) == 0x6       ? 2
            : (lead >> 4) == 0xE       ? 3
            : (lead >> 3) == 0x1E      ? 4
                                       : 0;
        if (length == 0 || cursor + length - 1 > text.size())
        {
            return 0xFFFD;
        }
        u32 codepoint = length == 1 ? lead : lead & (0x7Fu >> length);
        for (u32 i = 1; i < length; ++i)
        {
            codepoint = (codepoint << 6) | (static_cast<u8>(text[cursor++]) & 0x3Fu);
        }
        return codepoint;
    }

    const UiGlyph* resolveUiGlyph(const UiFonts fonts, const UiFont& base, const u32 codepoint, const UiFont*& owner)
    {
        owner = &base;
        const UiGlyph* glyph = base.glyph(codepoint);
        for (const FontId id : base.fallbacks)
        {
            if (glyph->state != UiGlyphState::Absent)
            {
                break;
            }
            owner = fonts[id].get();
            glyph = owner->glyph(codepoint);
        }
        return glyph;
    }

    float layoutUiText(const UiFonts fonts, const FontId primary, const std::string_view text, const float sizePx,
                       std::vector<UiPlacedGlyph>* placed, std::vector<u32>* clusters, const float letterSpacing)
    {
        const UiFont& base = *fonts[primary];
        float pen = 0.0f;
        const UiGlyph* previous = nullptr;
        const UiFont* previousFont = nullptr;
        const sizet firstPlaced = placed ? placed->size() : 0;
        const sizet firstCluster = clusters ? clusters->size() : 0;
        for (sizet cursor = 0; cursor < text.size();)
        {
            const sizet at = cursor;
            const u32 codepoint = nextCodepoint(text, cursor);
            if (uiNeedsShaping(codepoint))
            {
                if (placed)
                {
                    placed->resize(firstPlaced);
                }
                if (clusters)
                {
                    clusters->resize(firstCluster);
                }
                return layoutShapedUiLine(fonts, primary, text, sizePx, placed, clusters, letterSpacing);
            }
            const UiFont* owner = nullptr;
            const UiGlyph* glyph = resolveUiGlyph(fonts, base, codepoint, owner);
            if (glyph->state == UiGlyphState::Absent)
            {
                base.reportMissing(codepoint);
                previous = nullptr;
                continue;
            }
            if (previous && previousFont == owner)
            {
                pen += owner->kerning(*previous, *glyph) * sizePx;
            }
            if (placed)
            {
                placed->push_back({glyph, owner, pen});
            }
            if (clusters)
            {
                clusters->push_back(static_cast<u32>(at));
            }
            pen += std::max(0.0f, glyph->advance * sizePx + letterSpacing);
            previous = glyph;
            previousFont = owner;
        }
        return std::max(0.0f, pen); // a kerning before a zero advance can end below 0
    }
} // namespace spite
