#pragma once
#include <new>
#include "base/Platform.hpp"
#include "base/BaseExport.hpp"

namespace spite
{
    inline constexpr sizet BYTE = 1;
    inline constexpr sizet KB = 1024 * BYTE;
    inline constexpr sizet MB = 1024 * KB;
    inline constexpr sizet GB = 1024 * MB;

    [[nodiscard]] SPITE_BASE_API void* tryAllocate(sizet size, sizet alignment) noexcept;
    [[noreturn]] SPITE_BASE_API void allocationFailed(sizet size, sizet alignment) noexcept;
} // namespace spite

// The process allocator. SpiteBase.dll defines these over mimalloc and exports
// them by mangled name through SpiteBase.def — MSVC owns the declarations of
// the replaceable operators (vcruntime_new.h) and forbids adding
// dllexport/dllimport by redeclaration, so the dll attributes are not an
// option. Consumers link SpiteBase.lib; the linker binds these well-known
// mangled names to the DLL's exports ahead of the static CRT's local copies,
// so allocation and release always pair against one heap no matter which
// module boundary they cross. Every TU that consumes SPITE modules should see
// these declarations — the supported way is including Spite.hpp first.
[[nodiscard]] void* operator new(std::size_t size);
[[nodiscard]] void* operator new[](std::size_t size);
[[nodiscard]] void* operator new(std::size_t size, std::align_val_t alignment);
[[nodiscard]] void* operator new[](std::size_t size, std::align_val_t alignment);
[[nodiscard]] void* operator new(std::size_t size, const std::nothrow_t&) noexcept;
[[nodiscard]] void* operator new[](std::size_t size, const std::nothrow_t&) noexcept;
[[nodiscard]] void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept;
[[nodiscard]] void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept;

void operator delete(void* ptr) noexcept;
void operator delete[](void* ptr) noexcept;
void operator delete(void* ptr, std::size_t size) noexcept;
void operator delete[](void* ptr, std::size_t size) noexcept;
void operator delete(void* ptr, std::align_val_t alignment) noexcept;
void operator delete[](void* ptr, std::align_val_t alignment) noexcept;
void operator delete(void* ptr, std::size_t size, std::align_val_t alignment) noexcept;
void operator delete[](void* ptr, std::size_t size, std::align_val_t alignment) noexcept;
void operator delete(void* ptr, const std::nothrow_t&) noexcept;
void operator delete[](void* ptr, const std::nothrow_t&) noexcept;
void operator delete(void* ptr, std::align_val_t alignment, const std::nothrow_t&) noexcept;
void operator delete[](void* ptr, std::align_val_t alignment, const std::nothrow_t&) noexcept;
