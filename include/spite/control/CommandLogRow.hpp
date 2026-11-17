#pragma once

#include <string>

#include "base/Platform.hpp"
#include "base/Schema.hpp"

namespace spite
{
    // One row of the CommandHistory service's `log` table (Command log): a logged request, applied
    // (result "ok", replayed) or an edit command that failed (result is its error, replay skips it). source and
    // client are the request's Request source; time is UTC, "YYYY-MM-DDTHH:MM:SS.mmmZ". A reader of the table
    // (the editor's Activity tab) casts the row pointer queryTable gives to this type.
    struct CommandLogRow
    {
        u64 sequence = 0;
        std::string command;
        std::string args; // JSON object
        std::string source;
        std::string client;
        std::string time;
        std::string result;
        u64 durationUs = 0;
        [[nodiscard]] bool applied() const { return result == "ok"; }
        SPITE_OBSERVED_SCHEMA(CommandLogRow, sequence, command, args, source, client, time, result, durationUs)
    };
} // namespace spite
