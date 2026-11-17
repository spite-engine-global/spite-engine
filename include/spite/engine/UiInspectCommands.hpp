#pragma once

#include <functional>
#include <string_view>

#include "engine/EngineToolsExport.hpp"

namespace spite
{
    class ControlServer;
    class UiContext;

    // The UI of a world by name (empty: the default); null when the world has none.
    using UiOfFn = std::function<UiContext*(std::string_view world)>;

    // Registers on control (owner "engine"), acting on the UI of the "world" arg (a UI world, scene, or play while
    // playing; default: the first UI world, else scene). An element is named by `entity`, or by `id`: an element id,
    // or a "/"-separated id path whose segments are ancestors in order (not necessarily direct parents) ending at
    // the element; an id that matches several elements is refused with their entities.
    //   uiTree [root] [depth] [visibleOnly] [maxNodes] [world] -> nodes, truncated, tree: {entity, id, kind,
    // styleClass, role and name (UiAccessibility.hpp; nameKey when from a localization key),
    //     rect {x,y,w,h in UI pixels}, visible, interactable, state (normal, hover, pressed, focused,
    //     disabled), text, number, scroll, neighbors (explicit navigation neighbours), childCount, children}. Without
    //     `root`, children of the surfaces.
    //   uiHit x y [world] -> the frontmost element under a UI-pixel point: entity, id, kind, path (ids from its
    //     surface), rect; entity 0 when none
    //   uiActivate (entity|id) action [value] [world] -> click (queued: fires at the next frame), focus,
    //     setValue (text or number field, through select-all, insert and commit), scrollIntoView
    //   UI documents (a view is a document view by `view` name, or the code-built tree under `root`/`id`):
    //   uiDocuments [view] [world] -> views with stats, contract, problems, codeOnly and documentOnly ids
    //   uiExport (view|root|id) [path] [world] -> the live tree as a document (saved under assets/ui with path)
    //   uiImport (view|root|id) [document|path] [world] -> applied by id-path diff; undoable (the previous document)
    SPITE_ENGINE_TOOLS_API void registerUiInspectCommands(ControlServer& control, UiOfFn uiOf);
} // namespace spite
