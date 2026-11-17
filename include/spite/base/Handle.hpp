#pragma once

#include <compare>
#include <cstdint>

namespace spite
{
    // The engine's recycled-slot identity: a u32 index plus a u32 generation
    // packed into one u64. This is Entity's wire format, shared by every handle
    // wrapper so serialization and C-API boundaries move a single u64.
    //
    // Handle is storage and semantics only and must never appear in an API
    // signature: domain wrapper types (Entity, resource handles) are the
    // identity callers see; they embed a Handle and forward to it.
    struct Handle
    {
        uint64_t id = 0;

        constexpr Handle() = default;
        explicit constexpr Handle(const uint64_t handleId) : id(handleId) {}
        constexpr Handle(const uint32_t index, const uint32_t generation) :
            id(static_cast<uint64_t>(generation) << 32 | index)
        {
        }

        [[nodiscard]] constexpr uint32_t index() const { return static_cast<uint32_t>(id & 0xFFFFFFFF); }
        [[nodiscard]] constexpr uint32_t generation() const { return static_cast<uint32_t>(id >> 32); }
        [[nodiscard]] constexpr bool isValid() const { return id != 0; }

        auto operator<=>(const Handle&) const = default;

        // index 0 is reserved: issuers never hand it out, so undefined() cannot
        // collide with a live handle
        static constexpr Handle undefined() { return Handle(static_cast<uint64_t>(0)); }
    };
} // namespace spite

template <>
struct std::hash<spite::Handle>
{
    size_t operator()(const spite::Handle& handle) const noexcept { return std::hash<uint64_t>{}(handle.id); }
};
