#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class EditorDialogs;
    class EditorFeedback;
    class Widget;

    // The UI Documents panel: the documents of the editor's UI world (name, file, state, the slots of
    // each view's contract), the stylesheet layers (name, file, counts, theme or loaded) and the fonts (fallbacks,
    // glyph states), with the actions Reload (uiImport re-reading the view's file), Export (uiExport to a file under
    // assets/ui picked in the file dialog), Import (uiImport from a picked file; a refusal shows inline and as an
    // error toast), Load Layer / Unload Layer (loadLayer, unloadLayer) and Pick Element.
    //
    // Pick Element opens a modal over the editor (so the click reaches nothing below it: the picked element is not
    // activated): a click on the backdrop picks the element under it (uiHit, uiStyle), Esc closes it, and the modal's
    // field takes an element id for a keyboard or gamepad pick. The picked element's id, kind, rect, class and
    // resolved style fields (with the layer each came from) fill the Style table, and a border-only overlay panel is
    // moved over its rect; the overlay is hidden, so costs nothing, while nothing is picked.
    //
    // The panel edits the documents the editor runs on, so two fences exist (GUARDED_VIEWS, REQUIRED_LAYERS in
    // EditorUiDocuments.cpp): Import into a guarded document asks first (the control command needs force=true),
    // and the base sheet and the active theme's layer are never unloaded.
    //
    // Reads (uiDocuments, uiLayers, uiFonts) run when the panel is shown and a command ran since the last read (at
    // most every 20 frames for commands that are not its own); an idle or hidden panel costs two counter compares. A
    // document's file reloading by itself shows on the next read. Query `uiDocumentsView` (owner editorUiDocuments);
    // view commands uidocumentsSelect, uidocumentsLayer, uidocumentsReload, uidocumentsExport, uidocumentsImport,
    // uidocumentsLoadLayer, uidocumentsUnloadLayer and uidocumentsPick.
    class EditorUiDocuments
    {
    public:
        EditorUiDocuments(Engine& engine, EditorDialogs& dialogs, EditorFeedback& feedback);
        ~EditorUiDocuments();
        EditorUiDocuments(const EditorUiDocuments&) = delete;
        EditorUiDocuments& operator=(const EditorUiDocuments&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
