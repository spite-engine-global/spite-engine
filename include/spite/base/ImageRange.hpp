#pragma once

#include <cstdint>
#include <functional>
#include <typeinfo>

namespace spite
{
    // Where a std::function's code lives, kept beside a wrapper that hides
    // the function inside its own callable: the plain function pointer it
    // targets, if any, and its target type descriptor.
    struct CodeOrigin
    {
        const void* function = nullptr;
        const std::type_info* type = nullptr;

        template <typename R, typename... Args>
        [[nodiscard]] static CodeOrigin of(const std::function<R(Args...)>& fn)
        {
            if (!fn)
            {
                return {};
            }
            const auto* target = fn.template target<R (*)(Args...)>();
            return {target ? reinterpret_cast<const void*>(*target) : nullptr, &fn.target_type()};
        }
    };

    // The address range of a loaded executable image (a runtime module's DLL):
    // what registries must no longer reference once the module unloads.
    struct ImageRange
    {
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;

        [[nodiscard]] bool contains(const void* address) const
        {
            const auto value = reinterpret_cast<std::uintptr_t>(address);
            return value >= begin && value < end;
        }

        // A std::function made inside the image: its call thunk and target
        // type descriptor live there, so it dies with the image even when it
        // wraps code from elsewhere. A plain function pointer target is
        // checked as well.
        [[nodiscard]] bool contains(const CodeOrigin& origin) const
        {
            return (origin.function && contains(origin.function)) || (origin.type && contains(origin.type));
        }

        template <typename R, typename... Args>
        [[nodiscard]] bool contains(const std::function<R(Args...)>& fn) const
        {
            return contains(CodeOrigin::of(fn));
        }
    };
} // namespace spite
