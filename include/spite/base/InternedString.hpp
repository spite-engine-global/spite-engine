#pragma once
#include <compare>
#include <functional>
#include <string_view>

#include "base/Platform.hpp"
#include "BaseExport.hpp"

namespace spite
{
    class StringInterner;

    struct SPITE_BASE_API InternedString
    {
    private:
        friend class StringInterner;
        u64 m_id = 0;

        explicit constexpr InternedString(const u64 id) noexcept : m_id(id) {}

    public:
        constexpr InternedString() noexcept = default;

        InternedString(const char* str);

        InternedString(std::string_view str);

        [[nodiscard]] static constexpr InternedString fromId(const u64 id) noexcept { return InternedString(id); }

        // string_view returned lives as long as StringInterner, so it is safe to use it anywhere without actual string
        // allocation
        [[nodiscard]] std::string_view toView() const;

        [[nodiscard]] constexpr u64 id() const noexcept { return m_id; }

        [[nodiscard]] constexpr bool isValid() const noexcept { return m_id != 0; }

        [[nodiscard]] static constexpr InternedString undefined() noexcept { return {}; }

        constexpr bool operator==(const InternedString& other) const noexcept = default;
        constexpr auto operator<=>(const InternedString& other) const noexcept = default;

        struct hash
        {
            [[nodiscard]] sizet operator()(const InternedString& hs) const noexcept
            {
                return std::hash<u64>{}(hs.m_id);
            }
        };
    };
} // namespace spite


template <>
struct std::hash<spite::InternedString>
{
    spite::sizet operator()(const spite::InternedString& value) const noexcept
    {
        return spite::InternedString::hash{}(value);
    }
};
