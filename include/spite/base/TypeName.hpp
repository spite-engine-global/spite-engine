#pragma once

#include <string>
#include <string_view>
#include <typeinfo>

#include "base/Assert.hpp"
#include "base/BaseExport.hpp"
#include "base/Platform.hpp"

namespace spite::detail
{
    // The stable runtime name of a C++ type, shared by components and services.
    // MSVC type names look like "struct spite::ns::TransformComponent"; the
    // name is the last scope segment. It runs to the end of the null-terminated
    // type_info buffer, so data() stays null-terminated.
    inline std::string_view typeNameSegment(const char* typeName)
    {
        std::string_view name = typeName;
        constexpr std::string_view typePrefixes[] = {"struct ", "class ", "union ", "enum "};
        for (const std::string_view prefix : typePrefixes)
        {
            if (name.starts_with(prefix))
            {
                name.remove_prefix(prefix.size());
                break;
            }
        }
        const sizet lastScopeSeparator = name.rfind("::");
        if (lastScopeSeparator != std::string_view::npos)
        {
            name.remove_prefix(lastScopeSeparator + 2);
        }
        SASSERTM(name.find('<') == std::string_view::npos,
                 "Template instantiations need an explicit name (SPITE_COMPONENT_NAME / SPITE_SERVICE_NAME): {}", name);
        return name;
    }

#if !defined(_WIN32)
    SPITE_BASE_API std::string demangledTypeName(const char* name);
#endif

    template <class T>
    std::string_view shortTypeName()
    {
#if defined(_WIN32)
        static const std::string_view name = typeNameSegment(typeid(T).name());
#else
        static const std::string name = demangledTypeName(typeid(T).name());
#endif
        return name;
    }
} // namespace spite::detail
