#pragma once
#include <memory>
namespace spite
{
    class Engine;
    struct Widget;
    struct DockSpace;
    class EditorModelPreview
    {
    public:
        EditorModelPreview(Engine& engine, const DockSpace& dock);
        ~EditorModelPreview();
        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
