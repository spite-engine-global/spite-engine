#pragma once

#include <string>
#include <string_view>

#include "base/InternedString.hpp"

namespace spite
{
    // The class of a composite widget's part under the root's class family: "<family>.<part> <own>" where <own>
    // is the part's default class ("dropdown.label") and <part> its name after the first dot ("label"); the
    // default alone while the root names no class. The style sheet reads the list most specific first.
    inline InternedString partClass(const InternedString family, const std::string_view own)
    {
        if (!family.isValid())
        {
            return InternedString(own);
        }
        const std::string_view suffix = own.substr(own.find('.') + 1);
        std::string list(family.toView());
        list.append(1, '.').append(suffix).append(1, ' ').append(own);
        return InternedString(list);
    }
} // namespace spite
