#pragma once

#include <algorithm>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "base/memory/Memory.hpp"

namespace spite
{
    template <typename T, sizet InlineCapacity = 8>
    class sbo_vector
    {
        static_assert(InlineCapacity > 0, "InlineCapacity must be greater than 0");
        static_assert(std::is_nothrow_copy_constructible_v<T>);
        static_assert(std::is_nothrow_default_constructible_v<T>);
        static_assert(std::is_nothrow_move_constructible_v<T>);
        static_assert(std::is_nothrow_move_assignable_v<T>);
        static_assert(std::is_nothrow_destructible_v<T>);

        union Storage
        {
            alignas(T) std::byte inlineBuffer[sizeof(T) * InlineCapacity];
            T* heapPtr;

            constexpr Storage() noexcept : inlineBuffer{} {}
            ~Storage() {}
        };

        Storage m_storage;
        sizet m_size = 0;
        sizet m_capacity = InlineCapacity;
        bool m_isInline = true;

        [[nodiscard]] T* inlineData() noexcept { return reinterpret_cast<T*>(m_storage.inlineBuffer); }

        [[nodiscard]] const T* inlineData() const noexcept
        {
            return reinterpret_cast<const T*>(m_storage.inlineBuffer);
        }

        [[nodiscard]] bool referencesElement(const T* value) const noexcept
        {
            const T* first = data();
            const auto less = std::less<const T*>{};
            return !less(value, first) && less(value, first + m_size);
        }

        [[nodiscard]] static T* allocate(sizet capacity)
        {
            if (capacity > std::numeric_limits<sizet>::max() / sizeof(T))
            {
                allocationFailed(capacity, alignof(T));
            }

            void* storage = tryAllocate(capacity * sizeof(T), alignof(T));
            if (!storage)
            {
                allocationFailed(capacity * sizeof(T), alignof(T));
            }

            return static_cast<T*>(storage);
        }

        static void deallocate(T* ptr) noexcept { ::operator delete(ptr, static_cast<std::align_val_t>(alignof(T))); }

        void destroyRange(T* first, sizet count) noexcept
        {
            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                for (sizet i = count; i > 0; --i)
                {
                    std::destroy_at(first + (i - 1));
                }
            }
        }

        void destroyElements() noexcept { destroyRange(data(), m_size); }

        void releaseStorage() noexcept
        {
            destroyElements();
            if (!m_isInline)
            {
                deallocate(m_storage.heapPtr);
            }

            m_size = 0;
            m_capacity = InlineCapacity;
            m_isInline = true;
        }

        void growToAccommodate(sizet newSize)
        {
            if (newSize <= m_capacity)
            {
                return;
            }

            sizet newCapacity = m_capacity;
            while (newCapacity < newSize)
            {
                if (newCapacity > std::numeric_limits<sizet>::max() / 2)
                {
                    newCapacity = newSize;
                    break;
                }
                newCapacity *= 2;
            }

            T* newBuffer = allocate(newCapacity);
            T* oldData = data();
            for (sizet i = 0; i < m_size; ++i)
            {
                std::construct_at(newBuffer + i, std::move(oldData[i]));
            }

            destroyElements();
            if (!m_isInline)
            {
                deallocate(m_storage.heapPtr);
            }

            m_storage.heapPtr = newBuffer;
            m_capacity = newCapacity;
            m_isInline = false;
        }

        void moveFrom(sbo_vector&& other)
        {
            if (!other.m_isInline)
            {
                m_storage.heapPtr = other.m_storage.heapPtr;
                m_size = other.m_size;
                m_capacity = other.m_capacity;
                m_isInline = false;

                other.m_size = 0;
                other.m_capacity = InlineCapacity;
                other.m_isInline = true;
                return;
            }

            for (sizet i = 0; i < other.m_size; ++i)
            {
                std::construct_at(inlineData() + i, std::move(other.data()[i]));
            }

            m_size = other.m_size;
            other.clear();
        }

    public:
        using value_type = T;
        using size_type = sizet;
        using iterator = T*;
        using const_iterator = const T*;

        sbo_vector() = default;

        sbo_vector(sizet count, const T& value) { resize(count, value); }

        explicit sbo_vector(sizet count) { resize(count); }

        sbo_vector(std::initializer_list<T> init)
        {
            reserve(init.size());
            for (const T& item : init)
            {
                emplace_back(item);
            }
        }

        ~sbo_vector() { releaseStorage(); }

        sbo_vector(const sbo_vector& other)
        {
            reserve(other.m_size);
            for (const T& item : other)
            {
                emplace_back(item);
            }
        }

        sbo_vector(sbo_vector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) { moveFrom(std::move(other)); }

        sbo_vector& operator=(const sbo_vector& other)
        {
            if (this == &other)
            {
                return *this;
            }

            sbo_vector copy(other);
            *this = std::move(copy);
            return *this;
        }

        sbo_vector& operator=(sbo_vector&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        {
            if (this != &other)
            {
                releaseStorage();
                moveFrom(std::move(other));
            }
            return *this;
        }

        T& operator[](sizet index) noexcept { return data()[index]; }

        const T& operator[](sizet index) const noexcept { return data()[index]; }

        T* data() noexcept { return m_isInline ? inlineData() : m_storage.heapPtr; }

        const T* data() const noexcept { return m_isInline ? inlineData() : m_storage.heapPtr; }

        bool operator==(const sbo_vector& other) const
        {
            return m_size == other.m_size && std::equal(begin(), end(), other.begin());
        }

        iterator begin() noexcept { return data(); }
        const_iterator begin() const noexcept { return data(); }
        const_iterator cbegin() const noexcept { return data(); }
        iterator end() noexcept { return data() + m_size; }
        const_iterator end() const noexcept { return data() + m_size; }
        const_iterator cend() const noexcept { return data() + m_size; }

        [[nodiscard]] bool empty() const noexcept { return m_size == 0; }
        [[nodiscard]] sizet size() const noexcept { return m_size; }
        [[nodiscard]] sizet capacity() const noexcept { return m_capacity; }

        void reserve(sizet newCapacity) { growToAccommodate(newCapacity); }

        void clear() noexcept
        {
            destroyElements();
            m_size = 0;
        }

        void push_back(const T& value)
        {
            if (m_size == m_capacity && referencesElement(std::addressof(value)))
            {
                T copy(value);
                growToAccommodate(m_size + 1);
                std::construct_at(data() + m_size, std::move(copy));
            }
            else
            {
                growToAccommodate(m_size + 1);
                std::construct_at(data() + m_size, value);
            }
            ++m_size;
        }

        void push_back(T&& value)
        {
            if (m_size == m_capacity && referencesElement(std::addressof(value)))
            {
                T movedValue(std::move(value));
                growToAccommodate(m_size + 1);
                std::construct_at(data() + m_size, std::move(movedValue));
            }
            else
            {
                growToAccommodate(m_size + 1);
                std::construct_at(data() + m_size, std::move(value));
            }
            ++m_size;
        }

        template <typename... Args>
        T& emplace_back(Args&&... args)
        {
            growToAccommodate(m_size + 1);
            T* ptr = data() + m_size;
            std::construct_at(ptr, std::forward<Args>(args)...);
            ++m_size;
            return *ptr;
        }

        void pop_back() noexcept
        {
            if (m_size != 0)
            {
                --m_size;
                std::destroy_at(data() + m_size);
            }
        }

        iterator erase(const_iterator pos)
        {
            const sizet index = static_cast<sizet>(pos - cbegin());
            T* destination = begin() + index;
            std::move(destination + 1, end(), destination);
            pop_back();
            return destination;
        }

        iterator erase(const_iterator first, const_iterator last)
        {
            const sizet firstIndex = static_cast<sizet>(first - cbegin());
            const sizet removedCount = static_cast<sizet>(last - first);
            if (removedCount == 0)
            {
                return begin() + firstIndex;
            }

            T* destination = begin() + firstIndex;
            T* newEnd = std::move(begin() + firstIndex + removedCount, end(), destination);
            while (end() != newEnd)
            {
                pop_back();
            }
            return destination;
        }

        void resize(sizet newSize)
        {
            if (newSize < m_size)
            {
                destroyRange(data() + newSize, m_size - newSize);
                m_size = newSize;
                return;
            }

            growToAccommodate(newSize);
            while (m_size < newSize)
            {
                std::construct_at(data() + m_size);
                ++m_size;
            }
        }

        void resize(sizet newSize, const T& value)
        {
            if (newSize < m_size)
            {
                destroyRange(data() + newSize, m_size - newSize);
                m_size = newSize;
                return;
            }

            growToAccommodate(newSize);
            while (m_size < newSize)
            {
                std::construct_at(data() + m_size, value);
                ++m_size;
            }
        }

        T& back() { return data()[m_size - 1]; }
        const T& back() const { return data()[m_size - 1]; }
    };
} // namespace spite
