#pragma once

#include <concepts>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "base/Assert.hpp"
#include "base/Platform.hpp"

namespace spite
{
    template <typename TEvent>
    class EventBus final
    {
        static_assert(std::variant_size_v<TEvent> > 0, "EventBus requires a non-empty std::variant.");

        struct Listener
        {
            u64 id = 0;
            std::function<void(const TEvent&)> callback;
        };

    public:
        class Subscription
        {
        public:
            Subscription() = default;
            ~Subscription() { reset(); }
            Subscription(const Subscription&) = delete;
            Subscription& operator=(const Subscription&) = delete;
            Subscription(Subscription&& other) noexcept : m_bus(other.m_bus), m_id(other.m_id)
            {
                other.m_bus = nullptr;
                other.m_id = 0;
            }
            Subscription& operator=(Subscription&& other) noexcept
            {
                if (this != &other)
                {
                    reset();
                    m_bus = other.m_bus;
                    m_id = other.m_id;
                    other.m_bus = nullptr;
                    other.m_id = 0;
                }
                return *this;
            }
            void reset()
            {
                if (m_bus)
                {
                    m_bus->unsubscribe(m_id);
                    m_bus = nullptr;
                    m_id = 0;
                }
            }
            [[nodiscard]] bool isValid() const { return m_bus != nullptr; }

        private:
            Subscription(EventBus* bus, const u64 id) : m_bus(bus), m_id(id) {}
            EventBus* m_bus = nullptr;
            u64 m_id = 0;
            friend class EventBus;
        };

        EventBus() = default;
        ~EventBus() = default;
        EventBus(const EventBus&) = delete;
        EventBus& operator=(const EventBus&) = delete;

        template <typename TAlternative, typename THandler>
            requires std::constructible_from<TEvent, TAlternative> && std::invocable<THandler, const TAlternative&>
        [[nodiscard]] Subscription subscribe(THandler&& handler)
        {
            std::lock_guard lock(m_listenerMutex);
            const u64 id = m_nextListenerId++;
            m_listeners.emplace(id,
                                Listener{.id = id,
                                         .callback = [handler = std::forward<THandler>(handler)](const TEvent& event)
                                         {
                                             if (const auto* typedEvent = std::get_if<TAlternative>(&event))
                                             {
                                                 handler(*typedEvent);
                                             }
                                         }});
            return Subscription(this, id);
        }

        template <typename TAlternative>
            requires std::constructible_from<TEvent, TAlternative>
        bool enqueue(TAlternative&& event)
        {
            if (s_drainingBus == this)
            {
                SASSERTM(false, "Events cannot be enqueued from an event callback.");
                return false;
            }

            std::lock_guard lock(m_enqueueMutex);
            m_pendingEvents.emplace_back(std::forward<TAlternative>(event));
            return true;
        }

        void drain()
        {
            SASSERTM(s_drainingBus == nullptr, "Nested event draining is not supported.");
            std::lock_guard enqueueLock(m_enqueueMutex);
            DrainScope scope(*this);

            for (const TEvent& event : m_pendingEvents)
            {
                std::vector<std::function<void(const TEvent&)>> callbacks;
                {
                    std::lock_guard listenerLock(m_listenerMutex);
                    callbacks.reserve(m_listeners.size());
                    for (const auto& [_, listener] : m_listeners)
                    {
                        callbacks.push_back(listener.callback);
                    }
                }

                for (const auto& callback : callbacks)
                {
                    callback(event);
                }
            }
            m_pendingEvents.clear();
        }

    private:
        class DrainScope
        {
        public:
            explicit DrainScope(EventBus& bus) : m_previous(s_drainingBus) { s_drainingBus = &bus; }
            ~DrainScope() { s_drainingBus = m_previous; }

        private:
            EventBus* m_previous;
        };

        void unsubscribe(const u64 id)
        {
            std::lock_guard lock(m_listenerMutex);
            m_listeners.erase(id);
        }

        inline static thread_local EventBus* s_drainingBus = nullptr;
        std::mutex m_enqueueMutex;
        std::mutex m_listenerMutex;
        std::vector<TEvent> m_pendingEvents;
        std::unordered_map<u64, Listener> m_listeners;
        u64 m_nextListenerId = 1;
    };
} // namespace spite
