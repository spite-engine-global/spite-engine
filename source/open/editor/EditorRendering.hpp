#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class EditorFeedback;
    class Widget;

    // The Rendering panel (inventory row 5): the renderer's reloads, views and shadows, in three
    // tabs of one document view (`editor.rendering`; code-owned slots rendering.toolbar, rendering.rows and
    // rendering.detail).
    //  - Pipeline: one table of the SpiteRenderPipeline build (DLL, reloads, auto-reload, why the last reload was
    //    refused: the `renderPipeline` query) and of shader hot reload (the `shaderStatus` query: whether it is on and
    //    watching, the reloads, the last one's summary and its failed, refused and compiled files, an error one row
    //    per line, to 60 rows and then "and N more lines"; a column cuts a long path or error, so the picked row is
    //    read in full under the table). Reload Pipeline runs reloadRenderPipeline, Reload Shaders
    //    runs reloadShaders; each answers with a toast (a refusal carries the engine's reason) and the rows, which
    //    show a watcher's reload too. A build or shader that fails keeps the running one: the engine guarantees it,
    //    the panel only shows the reason.
    //  - Views: the render views (the `views` query: world, camera, extent, swapchain, follows game). Selecting a row
    //    enables Set Swapchain View (setSwapchainView; the editor refuses it, its views render offscreen into the
    //    panels, and the toast says so) and the camera chooser (bindCamera with a camera of the view's world).
    //  - Shadows: every view's sun cascades (the `shadowCascades` query: range, radius, texel, dirty reasons, frames
    //    rendered, visible and dropped meshlets), the live shadow settings below them (config graphics.shadows,
    //    edited in Project Settings) and Invalidate (shadowInvalidate). Reading the cascades asks the renderer for
    //    their statistics: only the shown tab does, every REFRESH_FRAMES.
    //
    // Query `renderingView` (owner editorRendering) answers the current tab from the engine, shown or hidden; view
    // commands `renderingTab tab=pipeline|views|shadows` and `renderingSelect row=N`. A hidden panel reads nothing;
    // a shown one compares counters per frame, reads again on a state command (at most every 20 frames), a reload, a
    // locale change and, on the Views and Shadows tabs, every 30 frames, and an equal read writes nothing.
    class EditorRendering
    {
    public:
        EditorRendering(Engine& engine, EditorFeedback& feedback);
        ~EditorRendering();
        EditorRendering(const EditorRendering&) = delete;
        EditorRendering& operator=(const EditorRendering&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
