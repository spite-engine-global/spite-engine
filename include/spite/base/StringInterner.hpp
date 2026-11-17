#pragma once

#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/InternedString.hpp"
#include "BaseExport.hpp"


namespace spite
{
    /**
     * @class StringInterner
     * @brief A thread-safe singleton registry that maps strings to unique HashedString IDs.
     *
     * This avoids storing duplicate strings in memory. All HashedString objects are created
     * through this registry. It must be initialized at startup with `init()` and cleaned up
     * at shutdown with `destroy()`.
     */
    class SPITE_BASE_API StringInterner
    {
    private:
        // Transparent hasher for heterogeneous lookups (e.g., char* vs spite::string)
        struct StringHasher
        {
            using is_transparent = void;
            [[nodiscard]] sizet operator()(std::string_view value) const noexcept;
        };

        struct StringEqual
        {
            using is_transparent = void;

            [[nodiscard]] bool operator()(std::string_view lhs, std::string_view rhs) const noexcept;
        };

        using StringIdMap = std::unordered_map<std::string, u64, StringHasher, StringEqual>;

        static StringInterner* m_instance;

        StringIdMap m_stringToId;
        std::vector<const std::string*> m_idToStrings;

        u64 m_nextId = 1;
        mutable std::shared_mutex m_mutex;

        StringInterner();

    public:
        StringInterner(const StringInterner& other) = delete;
        StringInterner(StringInterner&& other) noexcept = delete;
        StringInterner& operator=(const StringInterner& other) = delete;
        StringInterner& operator=(StringInterner&& other) noexcept = delete;

        ~StringInterner() = default;

        static void init();
        static void destroy();
        [[nodiscard]] static bool isInitialized() noexcept;
        [[nodiscard]] static StringInterner* get();

        [[nodiscard]] InternedString getOrCreate(const char* str);
        [[nodiscard]] InternedString getOrCreate(std::string_view str);
        // number of distinct interned strings (a growth counter for leak tests)
        [[nodiscard]] sizet size() const;
        // undefined when str was never interned; interns nothing
        [[nodiscard]] InternedString find(std::string_view str) const;

        [[nodiscard]] std::string_view resolveView(InternedString id) const;
        // Empty instead of waiting when the interner is being written (a crash report must not block on a lock the
        // dying thread may hold).
        [[nodiscard]] std::string_view tryResolveView(InternedString id) const;
    };

    [[nodiscard]] inline InternedString toHashedString(const char* str)
    {
        return StringInterner::get()->getOrCreate(str);
    }
} // namespace spite
