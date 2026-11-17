#pragma once

#include <memory>

#include "base/MathTypes.hpp"
#include "rendering/GraphicsDescs.hpp"
#include "ui/UiWidgets.hpp"

namespace spite
{
    class Engine;
    class EditorFeedback;

    // The Game panel's toolbar, view room and status line (SV5): a document view (`editor.game`,
    // assets/ui/editor/game.json) whose slots game.toolbar (the camera chooser, the aspect and resolution chooser and
    // the Stats box), game.view (the shell's view host, which the target is sized to) and game.status (the input
    // state chip with how to release it, and the note of a camera that was removed) code fills.
    //
    // Every control is a control command: the camera chooser runs bindCamera on the game view with a camera of the
    // world it draws (the cameras are the CameraComponent entities of Engine::gameWorld; a camera removed meanwhile
    // leaves the view on the fallback camera, and the chooser and the status line say so); the aspect and resolution
    // chooser runs gameViewFit (free | W:H | WxH, refused outside 1..64 per aspect side, 16..8192 pixels per side and
    // 3840x2160 pixels in all); the Stats box runs gameViewStats (the frame time over the last half second, the
    // entities of the drawn world and the target's size over the picture). The query gamePanel (owner editorGameView)
    // answers all of it from the engine and writes nothing, so it is current while the panel is hidden, except the
    // view's width, height and windowRect: a hidden panel does not size its view, so they follow a fit when the panel
    // is next shown. The commands act while it is hidden and the panel shows them when it is next shown.
    //
    // The shell asks targetIn for the extent the game view's target takes in the host's room: the room (Free), the
    // largest rect of the aspect that fits it, or the fixed resolution; the picture is letterboxed in the room.
    //
    // Idle cost: a shown, quiescent panel compares a handful of counters per frame and writes nothing; the stats
    // label is written twice a second while the box is checked; a hidden panel does nothing.
    class EditorGameToolbar
    {
    public:
        EditorGameToolbar(Engine& engine, EditorFeedback& feedback, u32 gameView, const Widget& host,
                          const Widget& image);
        ~EditorGameToolbar();
        EditorGameToolbar(const EditorGameToolbar&) = delete;
        EditorGameToolbar& operator=(const EditorGameToolbar&) = delete;

        [[nodiscard]] Widget content() const;
        void update();
        [[nodiscard]] Extent2D targetIn(Vec2 room) const;

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
