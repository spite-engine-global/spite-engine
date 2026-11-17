#pragma once

#include <string>
#include <string_view>

#include "localization/Locale.hpp"

namespace spite::loc
{
    // CLDR plural operands of the absolute value: integer digits i (exact up to u64; past it 10^18 plus the last six
    // digits), visible fraction digit count v and the fraction digits as an integer f ("1.50": i 1, v 2, f 50).
    struct PluralOperands
    {
        u64 i = 0;
        u32 v = 0;
        u64 f = 0;
    };

    struct LocaleRules
    {
        std::string_view language;
        PluralCategory (*plural)(const PluralOperands&);
        std::string_view decimal;
        std::string_view group;
        u8 minimumGroupingDigits; // CLDR minimumGroupingDigits: 2 leaves four-digit numbers ungrouped
    };

    // the rules of the locale's language; the root rules (Other only, en symbols) for an unknown one
    [[nodiscard]] const LocaleRules& localeRules(std::string_view locale);
    [[nodiscard]] bool isRootRules(const LocaleRules& rules);

    // false for malformed input
    [[nodiscard]] bool pluralOperands(std::string_view decimal, PluralOperands& out);

    // Writes |n| rounded to at most three fraction digits, trailing zeros dropped, into buffer (plain ASCII, no
    // sign); returns the length, 0 for a non-finite n. The buffer fits every finite double.
    constexpr sizet DECIMAL_BUFFER_SIZE = 352;
    [[nodiscard]] sizet decimalDigits(double n, char (&buffer)[DECIMAL_BUFFER_SIZE]);

    void appendNumber(const LocaleRules& rules, std::int64_t n, std::string& out);
    void appendNumber(const LocaleRules& rules, double n, std::string& out);
    [[nodiscard]] PluralCategory pluralCategory(const LocaleRules& rules, std::int64_t n);
    [[nodiscard]] PluralCategory pluralCategory(const LocaleRules& rules, double n);
} // namespace spite::loc
