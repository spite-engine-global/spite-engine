#include "LocaleRules.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iterator>
#include <limits>

namespace spite::loc
{
    namespace
    {
        constexpr std::string_view NBSP = "\xC2\xA0"; // U+00A0
        constexpr std::string_view NARROW_NBSP = "\xE2\x80\xAF"; // U+202F

        constexpr bool in(const u64 value, const u64 low, const u64 high) { return value >= low && value <= high; }

        // n = value: integer-valued with that integer part
        constexpr bool nIs(const PluralOperands& o, const u64 value) { return o.f == 0 && o.i == value; }

        PluralCategory otherOnly(const PluralOperands&) { return PluralCategory::Other; }

        // en de nl sv: one: i = 1 and v = 0
        PluralCategory oneIntegerOne(const PluralOperands& o)
        {
            return o.i == 1 && o.v == 0 ? PluralCategory::One : PluralCategory::Other;
        }

        // it es fr pt: many: e = 0 and i != 0 and i % 1000000 = 0 and v = 0 (no compact notation, so e = 0)
        constexpr bool millions(const PluralOperands& o) { return o.i != 0 && o.i % 1000000 == 0 && o.v == 0; }

        PluralCategory italian(const PluralOperands& o)
        {
            if (o.i == 1 && o.v == 0)
                return PluralCategory::One;
            return millions(o) ? PluralCategory::Many : PluralCategory::Other;
        }

        // es: one: n = 1
        PluralCategory spanish(const PluralOperands& o)
        {
            if (nIs(o, 1))
                return PluralCategory::One;
            return millions(o) ? PluralCategory::Many : PluralCategory::Other;
        }

        // fr: one: i = 0,1; pt: one: i = 0..1 (the same set)
        PluralCategory french(const PluralOperands& o)
        {
            if (o.i <= 1)
                return PluralCategory::One;
            return millions(o) ? PluralCategory::Many : PluralCategory::Other;
        }

        // ru uk
        PluralCategory eastSlavic(const PluralOperands& o)
        {
            if (o.v != 0)
                return PluralCategory::Other;
            const u64 i10 = o.i % 10;
            const u64 i100 = o.i % 100;
            if (i10 == 1 && i100 != 11)
                return PluralCategory::One;
            if (in(i10, 2, 4) && !in(i100, 12, 14))
                return PluralCategory::Few;
            return PluralCategory::Many; // i % 10 = 0 or 5..9, or i % 100 = 11..14
        }

        PluralCategory polish(const PluralOperands& o)
        {
            if (o.v != 0)
                return PluralCategory::Other;
            if (o.i == 1)
                return PluralCategory::One;
            const u64 i10 = o.i % 10;
            const u64 i100 = o.i % 100;
            if (in(i10, 2, 4) && !in(i100, 12, 14))
                return PluralCategory::Few;
            return PluralCategory::Many; // i != 1 and i % 10 = 0..1, or i % 10 = 5..9, or i % 100 = 12..14
        }

        PluralCategory czech(const PluralOperands& o)
        {
            if (o.v != 0)
                return PluralCategory::Many;
            if (o.i == 1)
                return PluralCategory::One;
            return in(o.i, 2, 4) ? PluralCategory::Few : PluralCategory::Other;
        }

        // tr: one: n = 1
        PluralCategory turkish(const PluralOperands& o)
        {
            return nIs(o, 1) ? PluralCategory::One : PluralCategory::Other;
        }

        PluralCategory arabic(const PluralOperands& o)
        {
            if (o.f != 0)
                return PluralCategory::Other;
            if (o.i == 0)
                return PluralCategory::Zero;
            if (o.i == 1)
                return PluralCategory::One;
            if (o.i == 2)
                return PluralCategory::Two;
            const u64 i100 = o.i % 100;
            if (in(i100, 3, 10))
                return PluralCategory::Few;
            if (in(i100, 11, 99))
                return PluralCategory::Many;
            return PluralCategory::Other;
        }

        constexpr LocaleRules ROOT{"", otherOnly, ".", ",", 1};

        // CLDR 46 cardinal rules and latn number symbols
        constexpr std::array RULES{
            LocaleRules{"en", oneIntegerOne, ".", ",", 1},  LocaleRules{"de", oneIntegerOne, ",", ".", 1},
            LocaleRules{"nl", oneIntegerOne, ",", ".", 1},  LocaleRules{"sv", oneIntegerOne, ",", NBSP, 1},
            LocaleRules{"it", italian, ",", ".", 1},        LocaleRules{"es", spanish, ",", ".", 2},
            LocaleRules{"fr", french, ",", NARROW_NBSP, 1}, LocaleRules{"pt", french, ",", ".", 1},
            LocaleRules{"ru", eastSlavic, ",", NBSP, 1},    LocaleRules{"uk", eastSlavic, ",", NBSP, 1},
            LocaleRules{"pl", polish, ",", NBSP, 2},        LocaleRules{"cs", czech, ",", NBSP, 1},
            LocaleRules{"tr", turkish, ",", ".", 1},        LocaleRules{"ar", arabic, ".", ",", 1},
            LocaleRules{"ja", otherOnly, ".", ",", 1},      LocaleRules{"zh", otherOnly, ".", ",", 1},
            LocaleRules{"ko", otherOnly, ".", ",", 1},
        };

        char lower(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

        // writes the integer digits of magnitude with grouping
        void appendGrouped(const LocaleRules& rules, const std::string_view digits, std::string& out)
        {
            const sizet count = digits.size();
            if (count < 3u + rules.minimumGroupingDigits)
            {
                out.append(digits);
                return;
            }
            for (sizet index = 0; index < count; ++index)
            {
                if (index != 0 && (count - index) % 3 == 0)
                {
                    out.append(rules.group);
                }
                out.push_back(digits[index]);
            }
        }

        void appendDecimal(const LocaleRules& rules, const std::string_view decimal, std::string& out)
        {
            const sizet point = decimal.find('.');
            appendGrouped(rules, decimal.substr(0, point), out);
            if (point != std::string_view::npos)
            {
                out.append(rules.decimal);
                out.append(decimal.substr(point + 1));
            }
        }
    } // namespace

    const LocaleRules& localeRules(const std::string_view locale)
    {
        const sizet end = locale.find_first_of("-_");
        const std::string_view language = locale.substr(0, end);
        for (const LocaleRules& rules : RULES)
        {
            if (rules.language.size() == language.size() &&
                std::equal(language.begin(), language.end(), rules.language.begin(),
                           [](const char a, const char b) { return lower(a) == b; }))
            {
                return rules;
            }
        }
        return ROOT;
    }

    bool isRootRules(const LocaleRules& rules) { return &rules == &ROOT; }

    bool pluralOperands(std::string_view decimal, PluralOperands& out)
    {
        out = {};
        if (decimal.starts_with('-'))
        {
            decimal.remove_prefix(1);
        }
        const sizet point = decimal.find('.');
        const std::string_view integer = decimal.substr(0, point);
        const std::string_view fraction =
            point == std::string_view::npos ? std::string_view{} : decimal.substr(point + 1);
        if (integer.empty() || (point != std::string_view::npos && fraction.empty()))
        {
            return false;
        }
        constexpr u64 MODULUS = 1'000'000'000'000'000'000ull;
        constexpr u64 BASE = 1'000'000ull; // the largest i % 10^k a rule asks for is k = 6
        u64 low = 0; // i % BASE
        bool exact = true;
        for (const char c : integer)
        {
            if (c < '0' || c > '9')
                return false;
            const u64 digit = static_cast<u64>(c - '0');
            low = (low * 10 + digit) % BASE;
            if (exact && out.i > (std::numeric_limits<u64>::max() - digit) / 10)
            {
                exact = false;
            }
            out.i = out.i * 10 + digit;
        }
        if (!exact)
        {
            out.i = MODULUS + low; // past u64: above every literal a rule compares i with, i % 10^k kept
        }
        for (const char c : fraction)
        {
            if (c < '0' || c > '9')
                return false;
            out.f = (out.f * 10 + static_cast<u64>(c - '0')) % MODULUS;
        }
        out.v = static_cast<u32>(fraction.size());
        return true;
    }

    sizet decimalDigits(const double n, char (&buffer)[DECIMAL_BUFFER_SIZE])
    {
        if (!std::isfinite(n))
        {
            return 0;
        }
        const auto result =
            std::to_chars(buffer, buffer + DECIMAL_BUFFER_SIZE, std::fabs(n), std::chars_format::fixed, 3);
        sizet length = static_cast<sizet>(result.ptr - buffer);
        while (buffer[length - 1] == '0')
        {
            --length;
        }
        if (buffer[length - 1] == '.')
        {
            --length;
        }
        return length;
    }

    void appendNumber(const LocaleRules& rules, const std::int64_t n, std::string& out)
    {
        const u64 magnitude = n < 0 ? 0ull - static_cast<u64>(n) : static_cast<u64>(n);
        char buffer[24];
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), magnitude);
        if (n < 0)
        {
            out.push_back('-');
        }
        appendGrouped(rules, std::string_view(buffer, result.ptr), out);
    }

    void appendNumber(const LocaleRules& rules, const double n, std::string& out)
    {
        if (std::isnan(n))
        {
            out.append("NaN");
            return;
        }
        if (std::signbit(n) && n != 0.0)
        {
            out.push_back('-');
        }
        if (std::isinf(n))
        {
            out.append("\xE2\x88\x9E"); // U+221E
            return;
        }
        char buffer[DECIMAL_BUFFER_SIZE];
        const sizet length = decimalDigits(n, buffer);
        const std::string_view digits(buffer, length);
        if (digits == "0" && std::signbit(n))
        {
            out.pop_back(); // -0.0001 rounds to 0, shown unsigned
        }
        appendDecimal(rules, digits, out);
    }

    PluralCategory pluralCategory(const LocaleRules& rules, const std::int64_t n)
    {
        PluralOperands operands;
        operands.i = n < 0 ? 0ull - static_cast<u64>(n) : static_cast<u64>(n);
        return rules.plural(operands);
    }

    PluralCategory pluralCategory(const LocaleRules& rules, const double n)
    {
        char buffer[DECIMAL_BUFFER_SIZE];
        const sizet length = decimalDigits(n, buffer);
        PluralOperands operands;
        if (length == 0 || !pluralOperands(std::string_view(buffer, length), operands))
        {
            return PluralCategory::Other;
        }
        return rules.plural(operands);
    }
} // namespace spite::loc

namespace spite
{
    namespace
    {
        bool allOf(const std::string_view text, bool (*predicate)(char))
        {
            return !text.empty() && std::ranges::all_of(text, predicate);
        }

        bool isAlpha(const char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
        bool isDigit(const char c) { return c >= '0' && c <= '9'; }
        bool isAlnum(const char c) { return isAlpha(c) || isDigit(c); }
        char upper(const char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }
        char lowerChar(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
    } // namespace

    std::string canonicalLocale(const std::string_view locale)
    {
        std::string result;
        if (locale.size() > MAX_LOCALE_ID_LENGTH)
        {
            return {};
        }
        sizet start = 0;
        sizet parts = 0;
        bool first = true;
        while (start <= locale.size())
        {
            const sizet end = std::min(locale.find_first_of("-_", start), locale.size());
            const std::string_view part = locale.substr(start, end - start);
            if (++parts > MAX_LOCALE_PARTS)
                return {};
            if (first)
            {
                if (part.size() < 2 || part.size() > 3 || !allOf(part, isAlpha))
                    return {};
                std::ranges::transform(part, std::back_inserter(result), lowerChar);
                first = false;
            }
            else
            {
                result.push_back('-');
                if (part.size() == 4 && allOf(part, isAlpha)) // script
                {
                    result.push_back(upper(part[0]));
                    std::ranges::transform(part.substr(1), std::back_inserter(result), lowerChar);
                }
                else if ((part.size() == 2 && allOf(part, isAlpha)) || (part.size() == 3 && allOf(part, isDigit)))
                {
                    std::ranges::transform(part, std::back_inserter(result), upper); // region
                }
                else if (part.size() >= 5 && part.size() <= 8 && allOf(part, isAlnum)) // variant
                {
                    std::ranges::transform(part, std::back_inserter(result), lowerChar);
                }
                else
                {
                    return {};
                }
            }
            start = end + 1;
        }
        return result;
    }

    std::vector<std::string> localeFallbackChain(const std::string_view locale, const std::string_view defaultLocale)
    {
        std::vector<std::string> chain;
        for (const std::string_view id : {locale, defaultLocale})
        {
            std::string current = canonicalLocale(id);
            while (!current.empty())
            {
                if (std::ranges::find(chain, current) == chain.end())
                {
                    chain.push_back(current);
                }
                const sizet dash = current.rfind('-');
                current.resize(dash == std::string::npos ? 0 : dash);
            }
        }
        return chain;
    }

    PluralCategory pluralCategory(const std::string_view locale, const std::int64_t n)
    {
        return loc::pluralCategory(loc::localeRules(locale), n);
    }

    PluralCategory pluralCategory(const std::string_view locale, const std::string_view decimal)
    {
        loc::PluralOperands operands;
        if (!loc::pluralOperands(decimal, operands))
        {
            return PluralCategory::Other;
        }
        return loc::localeRules(locale).plural(operands);
    }

    void appendNumber(const std::string_view locale, const std::int64_t n, std::string& out)
    {
        loc::appendNumber(loc::localeRules(locale), n, out);
    }

    void appendNumber(const std::string_view locale, const double n, std::string& out)
    {
        loc::appendNumber(loc::localeRules(locale), n, out);
    }
} // namespace spite
