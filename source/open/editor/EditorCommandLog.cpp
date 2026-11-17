#include "EditorCommandLog.hpp"
#include <spite/platform/LocalTime.hpp>
#include "EditorEntityLabel.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <format>

#include "base/ServiceLocator.hpp"
#include "control/CommandLogRow.hpp"
#include "control/ControlServer.hpp"
#include "ecs/World.hpp"
#include "engine/Engine.hpp"
#include "engine/PlayWorld.hpp"
#include <spite/profiling/Zone.hpp>
#include <nlohmann/json.hpp>

namespace spite
{
    namespace
    {
        constexpr std::string_view SERVICE = "CommandHistory";
        constexpr std::string_view TABLE = "log";
        constexpr std::string_view ELLIPSIS = "\xE2\x80\xA6"; // U+2026

        // the command as a sentence on one line; the play world's or the scene's entities name the ids
        std::string textOf(const CommandLogRow& row, const World* world)
        {
            return cutText(commandSentence(row.command, row.args, world), EditorCommandLog::MAX_TEXT_BYTES);
        }
    } // namespace

    std::string commandSentence(const std::string_view command, const std::string_view args, const World* world)
    {
        using Json = nlohmann::json;
        std::string text = identifierWords(command);
        for (sizet i = 1; i < text.size(); ++i)
        {
            if (text[i - 1] == ' ' && text[i] >= 'A' && text[i] <= 'Z' &&
                (i + 1 >= text.size() || text[i + 1] < 'A' || text[i + 1] > 'Z'))
            {
                text[i] = static_cast<char>(text[i] - 'A' + 'a'); // "Add component": a sentence, not a title
            }
        }
        if (args.empty() || args == "{}")
        {
            return text;
        }
        const Json parsed = Json::parse(args, nullptr, false);
        if (!parsed.is_object())
        {
            return text + ' ' + std::string(args);
        }
        const auto add = [&text](const std::string_view part)
        {
            if (!part.empty())
            {
                text += " \xC2\xB7 "; // a middle dot
                text += part;
            }
        };
        const auto shortValue = [](const Json& value)
        {
            std::string shown = value.is_string() ? value.get<std::string>() : value.dump();
            return shown.size() > 32 ? shown.substr(0, 31) + "\xE2\x80\xA6" : shown;
        };
        const auto entityText = [world](const Json& value)
        {
            if (!value.is_number_unsigned())
            {
                return value.dump();
            }
            const Entity entity(value.get<u64>());
            std::string buffer;
            return world && world->isEntityAlive(entity) ? std::string(entityLabel(buffer, *world, entity))
                                                         : std::format("#{}", entity.index());
        };
        if (const auto it = parsed.find("component"); it != parsed.end() && it->is_string())
        {
            std::string name = it->get<std::string>();
            if (name.size() > 9 && name.ends_with("Component"))
            {
                name.resize(name.size() - 9);
            }
            add(identifierWords(name));
        }
        if (const auto it = parsed.find("field"); it != parsed.end() && it->is_string())
        {
            add(identifierWords(it->get<std::string>()));
        }
        if (const auto it = parsed.find("path"); it != parsed.end() && it->is_string())
        {
            add(pathWords(it->get_ref<const std::string&>())); // a set's field
        }
        for (const char* key : {"model", "name", "panel", "text"})
        {
            if (const auto it = parsed.find(key); it != parsed.end() && !it->is_null())
            {
                add(shortValue(*it));
            }
        }
        if (const auto it = parsed.find("value"); it != parsed.end())
        {
            add("= " + shortValue(*it));
        }
        if (const auto it = parsed.find("id"); it != parsed.end())
        {
            add(entityText(*it));
        }
        else if (const auto ids = parsed.find("ids"); ids != parsed.end() && ids->is_array())
        {
            add(ids->size() == 1 ? entityText(ids->front()) : std::format("{} entities", ids->size()));
        }
        return text;
    }

    std::string cutText(std::string text, const sizet maxBytes)
    {
        if (text.size() > maxBytes)
        {
            sizet cut = maxBytes - ELLIPSIS.size();
            while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u)
            {
                --cut;
            }
            text.resize(cut);
            text += ELLIPSIS;
        }
        return text;
    }

    namespace
    {
        bool toLocal(const std::string_view utc, std::tm& local)
        {
            if (utc.size() < 19)
            {
                return false;
            }
            const auto field = [&utc](const sizet at, const sizet size)
            { return std::atoi(std::string(utc.substr(at, size)).c_str()); };
            std::tm stamp{};
            stamp.tm_year = field(0, 4) - 1900;
            stamp.tm_mon = field(5, 2) - 1;
            stamp.tm_mday = field(8, 2);
            stamp.tm_hour = field(11, 2);
            stamp.tm_min = field(14, 2);
            stamp.tm_sec = field(17, 2);
            const std::time_t seconds = spite::platform::fromUtc(stamp);
            return seconds != static_cast<std::time_t>(-1) && spite::platform::toLocalTime(seconds, local);
        }
    } // namespace

    std::string localClock(const std::string_view utc)
    {
        std::tm local{};
        return toLocal(utc, local) ? std::format("{:02}:{:02}:{:02}", local.tm_hour, local.tm_min, local.tm_sec)
                                   : std::string(utc);
    }

    std::string localStamp(const std::string_view utc)
    {
        std::tm local{};
        return toLocal(utc, local)
            ? std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                          local.tm_hour, local.tm_min, local.tm_sec)
            : std::string(utc);
    }

    struct EditorCommandLog::State
    {
        explicit State(Engine& engine) : engine(&engine), control(&engine.control()) {}

        Engine* engine;
        ControlServer* control;
        std::string world = "scene";
        u64 seenGeneration = ~0ull;
        std::string seenWorld;
        std::vector<CommandLogEntry> entries;
        u64 total = 0;
        u64 reads = 0;
    };

    EditorCommandLog::EditorCommandLog(Engine& engine) : m_state(std::make_unique<State>(engine)) {}

    EditorCommandLog::~EditorCommandLog() = default;

    std::string_view EditorCommandLog::world() const { return m_state->world; }

    const std::vector<CommandLogEntry>& EditorCommandLog::entries() const { return m_state->entries; }

    u64 EditorCommandLog::total() const { return m_state->total; }

    u64 EditorCommandLog::reads() const { return m_state->reads; }

    bool EditorCommandLog::refresh()
    {
        State& state = *m_state;
        PlayWorld* play = state.engine->playWorld();
        const std::string_view world = play ? "play" : "scene";
        const u64 generation = state.control->commandGeneration();
        if (generation == state.seenGeneration && world == state.seenWorld)
        {
            return false;
        }
        state.seenGeneration = generation;
        state.seenWorld = std::string(world);
        state.world = state.seenWorld;
        SPITE_ZONE("EditorCommandLog.read");
        ++state.reads;
        World& source = play ? play->world() : state.engine->world();
        u64 total = 0;
        source.services().queryTable(SERVICE, TABLE, [&total](const void*) { ++total; });
        const u64 skip = total > MAX_ENTRIES ? total - MAX_ENTRIES : 0;
        std::vector<CommandLogEntry> next;
        next.reserve(static_cast<sizet>(total - skip));
        u64 at = 0;
        source.services().queryTable(SERVICE, TABLE,
                                     [&](const void* row)
                                     {
                                         if (at++ < skip)
                                         {
                                             return;
                                         }
                                         const auto& logged = *static_cast<const CommandLogRow*>(row);
                                         next.push_back({.sequence = logged.sequence,
                                                         .command = logged.command,
                                                         .text = textOf(logged, &source),
                                                         .source = logged.source,
                                                         .client = logged.client,
                                                         .time = logged.time,
                                                         .result = logged.result,
                                                         .durationUs = logged.durationUs,
                                                         .failed = !logged.applied()});
                                     });
        const bool changed = next != state.entries || total != state.total;
        state.entries = std::move(next);
        state.total = total;
        return changed;
    }
} // namespace spite
