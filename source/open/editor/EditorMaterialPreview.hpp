#pragma once

#include <memory>
#include "ui/UiWidgets.hpp"

namespace spite
{
    class Engine;
    class EditorFeedback;

    class EditorMaterialPreview
    {
    public:
        EditorMaterialPreview(Engine& engine, EditorFeedback& feedback);
        ~EditorMaterialPreview();
        Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
