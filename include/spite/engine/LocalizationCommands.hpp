#pragma once

#include "engine/EngineToolsExport.hpp"

namespace spite
{
    class ControlServer;
    class Localization;
    class LocalizationContent;

    // Registers on control (owner "engine"); locales are canonical BCP 47 ids, a key is namespace.key:
    //   locales -> locale, defaultLocale, fallbackChain, revision, root, missingLookups, locales
    //     [{locale, messages, tables [{namespace, messages}]}] and errors [{file, line, key, offset, reason}] of
    //     files that failed their last load; "en-XA" is the pseudo-locale derived from the default locale
    //   setLocale locale -> locale, loaded, revision, fallbackChain: one revision bump, every consumer re-resolves
    //     in that frame; loaded false when no table of the locale (or its language) exists
    //   locStrings [locale] [namespace] [prefix] [limit] [offset] -> locale, total, messages [{key, namespace, text}]
    //     the locale's own tables, sorted by key
    //   locMissing [locale] -> locales [{locale, count, keys, truncated}]: keys another loaded locale defines and this
    //     one's own tables do not (authors' translation gaps; the pseudo-locale is excluded), and lookups
    //     {total, keys}: keys the running game looked up that no table answers
    //   locResolve key [args] -> key, found, text: the active locale's message formatted with args (a JSON object of
    //     names to integers, numbers or strings); does not count as a missing lookup
    //   locReload [force] -> applied, removed, errors: rescans assets/loc now (files are also watched)
    //   setLocString locale key text -> file: writes the message into assets/loc/<locale>/<namespace>.json (flat
    //     JSON), validated first; the previous file is restored when the result does not load. Undoable (a view
    //     step, never a scene edit): undo writes the previous text back, or removes a message the locale lacked
    //   unsetLocString locale key -> locale, key: removes the message from the locale's own table file; undoable
    SPITE_ENGINE_TOOLS_API void registerLocalizationCommands(ControlServer& control, Localization& localization,
                                                             LocalizationContent& content);
} // namespace spite
