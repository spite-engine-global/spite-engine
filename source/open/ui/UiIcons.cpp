#include "ui/UiIcons.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace spite
{
    namespace
    {
        constexpr UiIconEntry TABLE[] = {
#include "UiIconTable.inc"
        };

        constexpr bool sorted()
        {
            for (sizet i = 1; i < std::size(TABLE); ++i)
            {
                if (!(TABLE[i - 1].name < TABLE[i].name))
                {
                    return false;
                }
            }
            return true;
        }
        static_assert(sorted(), "UiIconTable.inc is sorted by name and holds no duplicate");

        u32 editDistance(const std::string_view a, const std::string_view b)
        {
            std::vector<u32> row(b.size() + 1);
            for (u32 j = 0; j <= b.size(); ++j)
            {
                row[j] = j;
            }
            for (sizet i = 1; i <= a.size(); ++i)
            {
                u32 diagonal = row[0];
                row[0] = static_cast<u32>(i);
                for (sizet j = 1; j <= b.size(); ++j)
                {
                    const u32 above = row[j];
                    row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0u : 1u)});
                    diagonal = above;
                }
            }
            return row[b.size()];
        }
    } // namespace

    u32 uiIconCodepoint(const std::string_view name)
    {
        const auto it = std::ranges::lower_bound(TABLE, name, {}, &UiIconEntry::name);
        return it != std::end(TABLE) && it->name == name ? it->codepoint : 0;
    }

    std::string_view uiIconClosestName(const std::string_view name)
    {
        constexpr sizet MAX_COMPARED = 64; // a hostile document's long string stays cheap
        const std::string_view probe = name.substr(0, MAX_COMPARED);
        std::string_view best;
        u32 bestDistance = ~0u;
        for (const UiIconEntry& entry : TABLE)
        {
            if (const u32 distance = editDistance(probe, entry.name); distance < bestDistance)
            {
                best = entry.name;
                bestDistance = distance;
            }
        }
        return best;
    }

    std::span<const UiIconEntry> uiIcons() { return TABLE; }
} // namespace spite
