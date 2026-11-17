#pragma once

#include <memory>
#include <string>

#include "control/ControlServer.hpp"

namespace spite
{
    class Engine;
    class Widget;

    // The Activity tab (Agent layer): the displayed world's commands, newest last, each with its
    // source (YOU in the accent colour for the editor, AGENT in the agent colour for a control client, HOST for other
    // in-process callers), the client's name, the time, the command and its arguments, and the result (a refused edit
    // shows its reason). The source is what the request REPORTED: a label, not a guarantee.
    //
    // The rows are the command log read by the shared adapter (EditorCommandLog), the one source the History panel
    // also reads; the panel holds the newest EditorCommandLog::MAX_ENTRIES and follows the
    // newest row while it is in view. Reads happen only while the panel is shown and a command ran, never per frame.
    // Query `activityView` (owner editorActivity): the world, the rows with their cells and the read count.
    class EditorActivity
    {
    public:
        explicit EditorActivity(Engine& engine);
        ~EditorActivity();
        EditorActivity(const EditorActivity&) = delete;
        EditorActivity& operator=(const EditorActivity&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
