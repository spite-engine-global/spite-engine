#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/Platform.hpp"

namespace spite
{
    class Engine;
    class World;

    // `text` cut to maxBytes at a UTF-8 boundary, ending in an ellipsis (the editor's command lists show one line)
    std::string cutText(std::string text, sizet maxBytes);
    // "HH:MM:SS" on the clock of the person's zone for a UTC stamp "YYYY-MM-DDTHH:MM:SS.mmmZ"; the stamp itself when
    // it does not parse
    std::string localClock(std::string_view utc);
    // "YYYY-MM-DD HH:MM:SS" on the same local clock, for a stamp with a date
    std::string localStamp(std::string_view utc);
    // A command as a person reads it in a list: its name as words, then the args that say what it touched
    // ("addComponent {component: PointLightComponent, id: 7}" -> "Add component · Point Light · Player"); an entity
    // id reads as the entity's label when `world` holds it. Args it does not know are left out, args that are not a
    // JSON object read as they are.
    std::string commandSentence(std::string_view command, std::string_view args, const World* world);

    // One command of a world's command log as an editor list shows it.
    struct CommandLogEntry
    {
        u64 sequence = 0; // the log's own order; a folded drag step keeps its row's sequence
        std::string command; // "set"
        std::string text; // the command and its args as one line, cut to MAX_TEXT_BYTES
        std::string source; // editor, host, stdio or pipe: what the client REPORTED
        std::string client; // the name its connection announced; empty when none
        std::string time; // UTC, "YYYY-MM-DDTHH:MM:SS.mmmZ"
        std::string result; // "ok", or why a failed edit command was refused
        u64 durationUs = 0;
        bool failed = false; // result is an error
        bool operator==(const CommandLogEntry&) const = default;
    };

    // The data of the editor's command list: the Activity tab reads it, and the History panel
    // lists the same world's undo steps (the CommandHistory `steps` table), so both show one history. It reads the
    // displayed world's CommandHistory `log` table (the play world while playing, else the scene) in process, never
    // through a request, and keeps the newest MAX_ENTRIES rows (oldest first) with their text built once.
    //
    // refresh() is the whole cost: it re-reads only when a command ran (the control server's command generation)
    // or the displayed world changed, and says whether the entries are not the ones it held; a panel calls it every
    // frame while shown and costs two integer compares when nothing happened.
    class EditorCommandLog
    {
    public:
        static constexpr sizet MAX_ENTRIES = 500;
        static constexpr sizet MAX_TEXT_BYTES = 160;

        explicit EditorCommandLog(Engine& engine);
        ~EditorCommandLog();
        EditorCommandLog(const EditorCommandLog&) = delete;
        EditorCommandLog& operator=(const EditorCommandLog&) = delete;

        bool refresh();
        // "scene" or "play": the world the entries are of
        [[nodiscard]] std::string_view world() const;
        [[nodiscard]] const std::vector<CommandLogEntry>& entries() const;
        // the rows the log holds; entries() are the newest of them
        [[nodiscard]] u64 total() const;
        // table reads so far (a test, and the Activity query, count them)
        [[nodiscard]] u64 reads() const;

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace spite
