#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class EditorDialogs;
    class EditorFeedback;
    class Widget;

    // The engine log (LogHistory.entries) as a virtual table in a document view, over the panel list
    // pattern's streaming model (EditorPanelList.hpp): columns time, level, category (a row's [Tag] prefix, else its
    // source file) and message; level toggles (default info and above), a category popup, a search with its match
    // count, follow of the newest row, Clear (hides the rows so far in this panel), Save (the shown rows to a file
    // under the `logs` root, chosen in a file dialog), and Copy (the selected rows to the clipboard); a selected
    // row's full text shows in a wrapping detail pane.
    // Commands logLevel, logCategory, logFilter (search), logFollow, logClear, logSelect, logCopy, logSave, logTable;
    // query `logView`. Per frame while idle: one atomic load of the log's recorded count and the chips' three compares.
    class EditorLog
    {
    public:
        EditorLog(Engine& engine, EditorFeedback& feedback, EditorDialogs& dialogs);
        ~EditorLog();
        EditorLog(const EditorLog&) = delete;
        EditorLog& operator=(const EditorLog&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
