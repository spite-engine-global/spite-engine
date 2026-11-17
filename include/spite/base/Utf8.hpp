#pragma once

#include <cstddef>
#include <string_view>

namespace spite
{
    // Strict UTF-8 (no overlongs, surrogates or code points past U+10FFFF): text from an untrusted file that goes into
    // a reply.
    inline bool validUtf8(const std::string_view text)
    {
        const size_t size = text.size();
        for (size_t i = 0; i < size;)
        {
            const auto c = static_cast<unsigned char>(text[i]);
            const size_t length = c < 0x80 ? 1
                : c >= 0xC2 && c <= 0xDF   ? 2
                : c >= 0xE0 && c <= 0xEF   ? 3
                : c >= 0xF0 && c <= 0xF4   ? 4
                                           : 0;
            if (length == 0 || i + length > size)
            {
                return false;
            }
            for (size_t k = 1; k < length; ++k)
            {
                if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80)
                {
                    return false;
                }
            }
            if (length > 2)
            {
                const auto second = static_cast<unsigned char>(text[i + 1]);
                if ((c == 0xE0 && second < 0xA0) || (c == 0xED && second > 0x9F) || (c == 0xF0 && second < 0x90) ||
                    (c == 0xF4 && second > 0x8F))
                {
                    return false;
                }
            }
            i += length;
        }
        return true;
    }
} // namespace spite
