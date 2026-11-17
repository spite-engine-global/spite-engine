#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace spite
{
    class Engine;
    class EditorDialogs;
    class EditorFeedback;
    class Widget;

    // The Capture panel (inventory row 6): the engine's capture commands as buttons and the files
    // they leave in the `captures` root as a list (`editor.capture`; code-owned slots capture.toolbar, capture.rows
    // and capture.detail).
    //  - Frame Capture, Snapshot, Screenshot (with a view choice: the presented frame or render view k) and RenderDoc
    //    run `capture`, `snapshot`, `screenshot` and `renderdocCapture`. The deferred ones
    //    (capture, screenshot, renderdocCapture answer after the frame's readbacks, which the engine bounds by its
    //    frame counts) are sent with a reply sink, so the frame loop never waits: the button stays off and the line
    //    under the list says "In progress" until the engine answers, then a toast names the file or carries the
    //    engine's reason (a minimized window, an inactive view, no RenderDoc). Snapshot writes at once, on the frame
    //    thread, as the command does for a client. Enable or Disable RenderDoc uses `restartRenderer` when Play
    //    is inactive or paused. The .rdc capture action is available once the renderer has the capture layer enabled.
    //  - The list is the files at the top of the captures root (the `listDirectory` query), newest first, with kind,
    //    size and local time; the picked row's path shows under it. Reveal shows it in the file manager, Copy Path
    //    puts the path on the clipboard, Delete asks first (EditorDialogs, destructive); each acts on a name from the
    //    listing through ControlServer::resolveFile, so a path never leaves the root.
    //
    // Query `captureView` (owner editorCapture) answers from a fresh read of the root, shown or hidden: rows (name,
    // kind, size, time, path, bytes, modified), selected, detail, the operations in progress, truncated, renderdoc,
    // restartQueued,
    // and the reads made (scans). View commands captureSelect name=<file> or row=N [expect=<name>], captureRefresh,
    // captureReveal, captureCopy and captureDelete. The directory is read when the panel is first shown, after a
    // capture ends or a file is deleted, on Refresh and by the query; a quiescent shown panel reads nothing.
    class EditorCapture
    {
    public:
        EditorCapture(Engine& engine, EditorFeedback& feedback, EditorDialogs& dialogs);
        ~EditorCapture();
        EditorCapture(const EditorCapture&) = delete;
        EditorCapture& operator=(const EditorCapture&) = delete;

        [[nodiscard]] Widget content() const;
        void update();
        // What Reveal calls with the file, and the monotonic milliseconds its rate limit reads (test seam).
        void setReveal(std::function<bool(const std::filesystem::path&)> reveal, std::function<std::uint64_t()> nowMs);

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
