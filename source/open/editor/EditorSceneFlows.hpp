#pragma once

#include <functional>
#include <memory>
#include <string>

#include "base/MathTypes.hpp"
#include "ecs/EcsTypes.hpp"
#include "editor/EditorExport.hpp"

namespace spite
{
    class Engine;
    class EditorDialogRequests;
    class EditorFeedback;
    class EditorModel;

    // The editor model owns scene workspace operations and their file and confirmation dialogs.
    // Requests are queued on the main thread. Only one flow runs at a time.
    class SPITE_EDITOR_API EditorSceneFlows
    {
    public:
        enum class Op : u8
        {
            New,
            Open,
            OpenRecent,
            Replace,
            Reset,
            Save,
            SaveAs,
            SaveAll,
            SaveWorkspace,
            OpenWorkspace,
            Close,
            SaveEntity,
            Import,
            Quit,
        };

        EditorSceneFlows(Engine& engine, EditorModel& model, EditorDialogRequests& dialogs, EditorFeedback& feedback);
        ~EditorSceneFlows();
        EditorSceneFlows(const EditorSceneFlows&) = delete;
        EditorSceneFlows& operator=(const EditorSceneFlows&) = delete;

        // Queues a flow. `path` is OpenRecent's scene; done(saved) is Save's: true when the scene was written, false
        // when the person cancelled, the write failed or the request was dropped.
        void request(Op op, std::string path = {}, std::function<void(bool saved)> done = {});
        void requestLayer(Op op, u32 layer);
        void saveEntity(Entity entity);
        // Starts the queued flow when none runs; drops the rest.
        void update();
        // A flow is between its first dialog and its command's reply.
        [[nodiscard]] bool busy() const;

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
