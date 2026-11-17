#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <string_view>

#include "editor/EditorExport.hpp"

namespace spite
{
    class Engine;

    class SPITE_EDITOR_API EditorPreviewJobs
    {
    public:
        explicit EditorPreviewJobs(Engine& engine);
        ~EditorPreviewJobs();
        void update();
        [[nodiscard]] std::uint64_t revision() const;
        void enqueue(std::string_view type, std::string_view name);
        [[nodiscard]] bool busy() const;
        [[nodiscard]] int report(std::ostream& out) const;

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
