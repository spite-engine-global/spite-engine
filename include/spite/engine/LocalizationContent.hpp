#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/Platform.hpp"
#include "engine/EngineToolsExport.hpp"

namespace spite
{
    class Localization;

    // A file that failed to load: the line (1-based, 0 when unknown) of the JSON error or of the key, and for a
    // message the key and the byte offset into it.
    struct LocFileError
    {
        std::string file;
        u32 line = 0;
        std::string key;
        u32 offset = 0;
        std::string reason;

        [[nodiscard]] std::string text() const;
    };

    struct LocReloadReport
    {
        u32 applied = 0; // tables set
        u32 removed = 0; // tables dropped because their file is gone
        std::vector<LocFileError> errors;
    };

    // Loads the string tables <root>/<locale>/<namespace>.json (one flat JSON object per file: table key to message
    // source) into a Localization, derives the pseudo-locale (PSEUDO_LOCALE) from the default locale's tables and
    // reloads what changed on disk. A feature's own table sits one directory down: <root>/<locale>/<group>/<name>.json
    // is the namespace <group>-<name> (assets/loc/en/editor/console.json is "editor-console", its key "title" is
    // "editor-console.title"); a flat file naming the same namespace wins. Everything runs on the thread that calls it,
    // which must be the frame point of the thread holding resolved views (Localization's writers): poll() once per
    // frame there.
    //
    // A file that cannot be read, parsed or whose messages are invalid keeps the previous table and reports the file,
    // line and key; the others still apply. Limits: MAX_FILE_BYTES per file, MAX_FILES_PER_LOCALE and MAX_LOCALES;
    // directory and file names must be canonical locale ids and valid namespaces, and a directory or file that is a
    // link or junction is ignored. A root that does not exist (at construction, or deleted later) is checked every 100
    // ms and watched and reloaded once it exists.
    class SPITE_ENGINE_TOOLS_API LocalizationContent
    {
    public:
        static constexpr sizet MAX_FILE_BYTES = 1024 * 1024;
        static constexpr sizet MAX_FILES_PER_LOCALE = 512;
        static constexpr sizet MAX_LOCALES = 64;

        // loads every table under root at once (one revision bump), then watches root
        LocalizationContent(Localization& localization, std::filesystem::path root);
        ~LocalizationContent();
        LocalizationContent(const LocalizationContent&) = delete;
        LocalizationContent& operator=(const LocalizationContent&) = delete;

        // Per frame: applies settled file changes, then notifies the locale-changed listeners when the
        // Localization revision moved. Idle cost: one clock read and an atomic load, plus a change-notification
        // handle check every 100 ms.
        void poll();

        // Rescans the tree now; force re-reads files whose stamp is unchanged.
        LocReloadReport reload(bool force);

        // Sets one message in loc/<locale>/<namespace>.json (the namespace is the key's first segment), creating the
        // file, and reloads it; the file is restored when the result does not load. No text removes the message (a
        // key the table file lacks, or a table that does not exist, is refused: nothing is written). Returns the
        // error, empty on success.
        [[nodiscard]] std::string setMessage(std::string_view locale, std::string_view key,
                                             std::optional<std::string_view> text);

        // files that failed their last load and have not changed since
        [[nodiscard]] std::vector<LocFileError> errors() const;
        [[nodiscard]] const std::filesystem::path& root() const;

        // Called from poll() once per frame in which the revision moved (any number of changes coalesce)
        using ChangeFn = std::function<void(u64 revision, std::string_view locale)>;
        [[nodiscard]] u64 subscribe(ChangeFn listener);
        void unsubscribe(u64 id);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace spite
