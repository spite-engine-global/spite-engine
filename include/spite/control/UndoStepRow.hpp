#pragma once

#include <string>

#include "base/Platform.hpp"
#include "base/Schema.hpp"

namespace spite
{
    // One row of the CommandHistory service's `steps` table (Undo step), oldest first: done is whether
    // the step is applied (undo reverts the newest done step, redo reapplies the oldest undone one). command and args
    // are the step's first command and its args as the history query shows them (a value over 256 bytes as its size,
    // without the world), commands how many the step holds (a batch holds several); source, client and time (UTC,
    // "YYYY-MM-DDTHH:MM:SS.mmmZ") are the attribution of the request that made it. A reader of the table (the
    // editor's History panel) casts the row pointer queryTable gives to this type.
    struct UndoStepRow
    {
        u64 step = 0; // the step's index, as history and historyJump name it
        bool done = false;
        std::string command;
        std::string args; // JSON object
        std::string label; // optional name of a batch
        u32 commands = 0;
        u32 added = 0;
        u32 moved = 0;
        u32 deleted = 0;
        std::string source;
        std::string client;
        std::string time;
        SPITE_OBSERVED_SCHEMA(UndoStepRow, step, done, command, args, label, commands, added, moved, deleted, source,
                              client, time)
    };
} // namespace spite
