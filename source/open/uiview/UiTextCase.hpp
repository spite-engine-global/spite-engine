#pragma once

#include <string>
#include <string_view>

#include "base/Platform.hpp"
#include "uiview/UiText.hpp"

namespace spite
{
    // The language rules a case transform follows. Turkic: i uppercases to İ (U+0130) and I lowercases to ı
    // (U+0131), the dotted and dotless pairs of Turkish and Azeri. Every other locale, the pseudo-locale en-XA
    // included, takes the default rules.
    enum class UiCaseLocale : u8
    {
        Default,
        Turkic,
    };

    // tr and az, with or without a region or script subtag ("tr-TR"); anything else, and the empty id: Default.
    [[nodiscard]] UiCaseLocale uiCaseLocale(std::string_view localeId);

    // `text` converted by `transform`: ASCII, Latin-1, Latin Extended-A, the Latin Extended-B and Additional letters
    // the pseudo-locale uses, Greek and Cyrillic: simple one-to-one pairs, plus ß -> SS, U+1E9E -> ß and İ -> i +
    // U+0307 outside Turkic. No Greek final sigma or tonos removal and no Lithuanian rules: they need context or a
    // locale the project does not ship, and a caret measures prefixes of the text, so a rule may not look ahead.
    // Returns `text` itself when nothing changes (or transform is None), else `scratch`, which it overwrites. Bytes
    // that are not valid UTF-8 pass through unchanged.
    [[nodiscard]] std::string_view applyUiTextTransform(UiTextTransform transform, UiCaseLocale locale,
                                                        std::string_view text, std::string& scratch);
} // namespace spite
