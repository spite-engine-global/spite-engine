#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "base/Platform.hpp"
#include "localization/LocalizationExport.hpp"

namespace spite
{
    // CLDR cardinal plural categories.
    enum class PluralCategory : u8
    {
        Zero,
        One,
        Two,
        Few,
        Many,
        Other
    };

    // canonicalLocale rejects ids longer than this (the BCP 47 length of a well-formed tag) or with more parts
    constexpr sizet MAX_LOCALE_ID_LENGTH = 35;
    constexpr sizet MAX_LOCALE_PARTS = 8;

    // A locale id in canonical BCP 47 case ("de_at" -> "de-AT", "zh_hans_cn" -> "zh-Hans-CN"); empty when the id is
    // not well formed (a 2-3 letter language, then script, region or variant subtags).
    [[nodiscard]] SPITE_LOCALIZATION_API std::string canonicalLocale(std::string_view locale);

    // The locales a lookup tries, nearest first: the locale, its parents with trailing subtags dropped, then the
    // default locale's chain ("de-AT" with default "en": de-AT, de, en). Duplicates and malformed ids are skipped.
    [[nodiscard]] SPITE_LOCALIZATION_API std::vector<std::string> localeFallbackChain(std::string_view locale,
                                                                                      std::string_view defaultLocale);

    // Plural rules and number symbols are built in per language (the locale's first subtag): en de nl sv it es fr pt
    // ru uk pl cs tr ar ja zh ko. "pt" is pt-BR (zero and one for 0 and 1); the pt-PT rule is not modelled. Any other
    // language selects Other for every number and formats like en.
    [[nodiscard]] SPITE_LOCALIZATION_API PluralCategory pluralCategory(std::string_view locale, std::int64_t n);
    // decimal: [-]digits[.digits]; visible fraction digits count (CLDR operand v), so "1.0" is Other in en while
    // "1" is One. Malformed input selects Other.
    [[nodiscard]] SPITE_LOCALIZATION_API PluralCategory pluralCategory(std::string_view locale,
                                                                       std::string_view decimal);

    // Appends n with the locale's decimal and grouping separators (UTF-8). A double keeps at most three fraction
    // digits, trailing zeros dropped. Digits are always Latin; the sign is '-'.
    SPITE_LOCALIZATION_API void appendNumber(std::string_view locale, std::int64_t n, std::string& out);
    SPITE_LOCALIZATION_API void appendNumber(std::string_view locale, double n, std::string& out);
} // namespace spite
