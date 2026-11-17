#pragma once

#include <functional>
#include <memory>
#include <string_view>

namespace spite
{
    class Engine;
    class EditorModel;
    class EditorFeedback;
    class Widget;

    // The Scenes panel lists the workspace, recent scene files and entity counts.
    // Its file actions share the editor model's flows with the File menu and Hierarchy.
    class EditorScenes
    {
    public:
        EditorScenes(Engine& engine, const EditorModel& model, EditorFeedback& feedback,
                     std::function<void(std::string_view path)> openScene);
        ~EditorScenes();
        EditorScenes(const EditorScenes&) = delete;
        EditorScenes& operator=(const EditorScenes&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
