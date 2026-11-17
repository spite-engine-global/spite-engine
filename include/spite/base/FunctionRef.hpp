#pragma once

#include <memory>
#include <type_traits>
#include <utility>

namespace spite
{
    template <typename Signature>
    class function_ref;

    // Non-owning, non-allocating view of a callable: valid only while the
    // callable it refers to lives, so it suits arguments consumed during the
    // call (never stored members).
    template <typename R, typename... Args>
    class function_ref<R(Args...)>
    {
    public:
        template <typename F>
            requires(!std::is_same_v<std::remove_cvref_t<F>, function_ref> && std::is_invocable_r_v<R, F&, Args...>)
        function_ref(F&& callable) noexcept :
            m_object(const_cast<void*>(static_cast<const void*>(std::addressof(callable)))),
            m_invoke([](void* object, Args... args) -> R
                     { return (*static_cast<std::remove_reference_t<F>*>(object))(std::forward<Args>(args)...); })
        {
        }

        R operator()(Args... args) const { return m_invoke(m_object, std::forward<Args>(args)...); }

    private:
        void* m_object;
        R (*m_invoke)(void*, Args...);
    };
} // namespace spite
