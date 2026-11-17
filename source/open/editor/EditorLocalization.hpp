#pragma once

#include <memory>

namespace spite
{
    class Engine;
    class Widget;

    // The Localization panel: the string table of one locale as a list of keys (namespace, value, plural
    // forms, translated or missing) with a locale selector, a search, a "missing only" filter, a value editor, a
    // preview and the actions locReload and setLocale ("Show in UI").
    //
    // Save is setLocString (undoable: one undo step puts the file back; unsetLocString is its inverse for a key the
    // locale did not define): the engine writes the table file at once and reloads it, so the string shows live in
    // every consumer and there is no second, unsaved state to keep. A missing key shows the default locale's text and
    // is saved as the locale's own. The pseudo-locale en-XA is derived and read-only, so it is not offered.
    //
    // The preview resolves the selected key in the ACTIVE locale (locResolve) with sample arguments typed as a JSON
    // object; its label names that locale, "Show in UI" makes the edited locale the active one.
    //
    // Reads (locales, locStrings, locMissing) run when the panel is shown, the locale revision moved (a save, a file
    // change on disk, setLocale) or Reload ran: an idle or hidden panel costs two counter compares. At most 5000 keys
    // are listed ("first N of M" in the note, and the missing list's own cap of 500). Query `localizationView` (owner
    // editorLocalization); view commands localizationLocale, localizationSelect, localizationMissingOnly,
    // localizationEdit, localizationSave, localizationPreview.
    class EditorLocalization
    {
    public:
        explicit EditorLocalization(Engine& engine);
        ~EditorLocalization();
        EditorLocalization(const EditorLocalization&) = delete;
        EditorLocalization& operator=(const EditorLocalization&) = delete;

        [[nodiscard]] Widget content() const;
        void update();

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
