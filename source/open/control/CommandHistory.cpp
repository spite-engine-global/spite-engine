#include "CommandHistory.hpp"
#include "profiling/Zone.hpp"

#include <algorithm>
#include <memory>
#include <unordered_set>

#include "base/ServiceLocator.hpp"
#include "control/UndoStepRow.hpp"

namespace spite
{
    namespace
    {
        sizet rowBytes(const CommandLogRow& row)
        {
            return row.command.size() + row.args.size() + row.source.size() + row.client.size() + row.time.size() +
                row.result.size();
        }

        void addField(std::vector<std::string>& fields, std::string field)
        {
            if (std::ranges::find(fields, field) == fields.end())
            {
                fields.push_back(std::move(field));
            }
        }

        sizet measureRequest(const CommandRequest& request)
        {
            return request.command.size() + request.args.dump().size() + request.created.size() * sizeof(u64);
        }

        template <typename Fn>
        void forEachRequest(const UndoStep& step, Fn&& fn)
        {
            for (const CommandRecord& record : step.records)
            {
                fn(record.forward);
                for (const CommandRequest& inverse : record.inverse)
                {
                    fn(inverse);
                }
            }
        }

        // fn(id, request, created) for each entity a scene-edit step's requests name (the id arg, read as the handler
        // read it: an Entity arg may be a digit string) or create
        template <typename Fn>
        void forEachTouched(const UndoStep& step, Fn&& fn)
        {
            for (const CommandRecord& record : step.records)
            {
                const CommandRequest& request = record.forward;
                if (const std::optional<u64> id = ControlArgs(&request.args).integer("id"))
                {
                    fn(*id, request, false);
                }
                for (const u64 created : request.created)
                {
                    if (created != 0)
                    {
                        fn(created, request, true);
                    }
                }
            }
        }

        // What a request touched of an entity: a set's path, an add/removeComponent's component, else the command
        // (also for an entity the request created)
        std::string fieldOf(const CommandRequest& request, const bool created)
        {
            if (!created)
            {
                for (const char* key : {"path", "component"})
                {
                    if (const auto it = request.args.find(key); it != request.args.end() && it->is_string())
                    {
                        return it->get<std::string>();
                    }
                }
            }
            return request.command;
        }

        // Equal fields, or one names a part of the other ("A.b" and "A.b.c", "A" and "A.b", "A.l" and "A.l[2]")
        bool fieldsOverlap(const std::string_view a, const std::string_view b)
        {
            const auto [shorter, longer] = a.size() <= b.size() ? std::pair(a, b) : std::pair(b, a);
            return longer.starts_with(shorter) &&
                (longer.size() == shorter.size() || longer[shorter.size()] == '.' || longer[shorter.size()] == '[');
        }

        // The members an updateMaterial request names
        std::vector<std::string> materialMembers(const nlohmann::json& args)
        {
            std::vector<std::string> members;
            if (const auto fields = args.find("fields"); fields != args.end() && fields->is_object())
            {
                for (const auto& [key, value] : fields->items())
                {
                    members.push_back(key);
                }
            }
            return members;
        }

        // The inverse requests of two records revert the same fields: the same commands on the same paths. An
        // attachToBone that changed only the position folds with one that changed only the position, not with one that
        // changed the rotation: the first call's inverse stays, so it must cover what the later call changed.
        bool sameInverseFields(const std::vector<CommandRequest>& a, const std::vector<CommandRequest>& b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (sizet i = 0; i < a.size(); ++i)
            {
                if (a[i].command != b[i].command ||
                    a[i].args.value("path", nlohmann::json()) != b[i].args.value("path", nlohmann::json()) ||
                    a[i].args.value("component", nlohmann::json()) != b[i].args.value("component", nlohmann::json()))
                {
                    return false;
                }
            }
            return true;
        }

        // Every record of both steps sets the same field of the same entity, updates the same members of the same
        // material, seeks (or sets the speed of) the same animator, attaches the same entity to a bone, or sets the
        // same parameter of its controller.
        bool setsSameFields(const UndoStep& last, const UndoStep& step)
        {
            if (last.merge != step.merge || last.label != step.label || last.records.size() != step.records.size() ||
                last.by.source != step.by.source || last.by.client != step.by.client)
            {
                return false;
            }
            for (sizet i = 0; i < step.records.size(); ++i)
            {
                const CommandRequest& before = last.records[i].forward;
                const CommandRequest& after = step.records[i].forward;
                if (before.command == "updateMaterial" && after.command == "updateMaterial")
                {
                    if (before.args.value("name", nlohmann::json()) != after.args.value("name", nlohmann::json()) ||
                        before.args.value("world", nlohmann::json()) != after.args.value("world", nlohmann::json()) ||
                        materialMembers(before.args) != materialMembers(after.args))
                    {
                        return false;
                    }
                    continue;
                }
                if ((before.command == "seekAnimation" || before.command == "setAnimationSpeed" ||
                     before.command == "attachToBone") &&
                    before.command == after.command)
                {
                    // a scrub, a speed drag or an offset drag of one animator or attachment: the first call's inverse
                    // stays, so one undo returns to where it began
                    if (before.args.value("world", nlohmann::json()) != after.args.value("world", nlohmann::json()) ||
                        before.args.value("id", nlohmann::json()) != after.args.value("id", nlohmann::json()) ||
                        (before.command == "attachToBone" &&
                         !sameInverseFields(last.records[i].inverse, step.records[i].inverse)))
                    {
                        return false;
                    }
                    continue;
                }
                if (before.command == "setAnimationParameter" && after.command == "setAnimationParameter")
                {
                    // a drag of one parameter of one animator's controller
                    if (before.args.value("world", nlohmann::json()) != after.args.value("world", nlohmann::json()) ||
                        before.args.value("id", nlohmann::json()) != after.args.value("id", nlohmann::json()) ||
                        before.args.value("name", nlohmann::json()) != after.args.value("name", nlohmann::json()))
                    {
                        return false;
                    }
                    continue;
                }
                if (before.command != "set" || after.command != "set")
                {
                    return false;
                }
                for (const char* key : {"world", "id", "path"})
                {
                    if (before.args.value(key, nlohmann::json()) != after.args.value(key, nlohmann::json()))
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    } // namespace

    CommandHistory& CommandHistory::attach(ServiceLocator& services)
    {
        if (!services.contains<CommandHistory>())
        {
            services.registerService<CommandHistory>(std::make_shared<CommandHistory>());
            services.registerTable<CommandHistory>("log", CommandLogRow::spiteSchema(),
                                                   [](const CommandHistory& history, const TableRowSink& sink)
                                                   {
                                                       for (const CommandLogRow& row : history.logRows())
                                                       {
                                                           sink(&row);
                                                       }
                                                   });
            services.registerTable<CommandHistory>(
                "steps", UndoStepRow::spiteSchema(),
                [](const CommandHistory& history, const TableRowSink& sink)
                {
                    for (sizet i = 0; i < history.steps().size(); ++i)
                    {
                        const UndoStep& step = history.steps()[i];
                        UndoStepRow row{.step = step.index,
                                        .done = i < history.done(),
                                        .label = step.label,
                                        .commands = static_cast<u32>(step.records.size()),
                                        .source = step.by.source,
                                        .client = step.by.client,
                                        .time = step.by.time};
                        if (!step.records.empty())
                        {
                            const CommandRequest& first = step.records.front().forward;
                            nlohmann::json args = summarizeArgs(first.args, false);
                            args.erase("world");
                            row.command = first.command;
                            row.args = args.dump();
                        }
                        for (const CommandRecord& record : step.records)
                        {
                            const auto& request = record.forward;
                            if (request.command == "createEntity" || request.command == "instantiateModel" ||
                                request.command == "createPrimitive")
                                ++row.added;
                            else if (request.command == "deleteEntity")
                                ++row.deleted;
                            else if (request.command == "set")
                            {
                                const auto path = request.args.find("path");
                                if (path != request.args.end() && path->is_string() &&
                                    path->get_ref<const std::string&>().starts_with("TransformComponent."))
                                    ++row.moved;
                            }
                        }
                        sink(&row);
                    }
                });
        }
        return services.get<CommandHistory>();
    }

    nlohmann::json CommandHistory::summarizeArgs(const nlohmann::json& requestArgs, const bool full)
    {
        if (full)
        {
            return requestArgs;
        }
        nlohmann::json summary = nlohmann::json::object();
        for (const auto& [key, value] : requestArgs.items())
        {
            const sizet size = value.is_string() ? value.get_ref<const std::string&>().size()
                : value.is_primitive()           ? 0
                                                 : value.dump().size();
            summary[key] = size > SUMMARY_BYTES
                ? nlohmann::json("<" + std::to_string(size) + " bytes; history full=true>")
                : value;
        }
        return summary;
    }

    void CommandHistory::detach(ServiceLocator& services) { services.unregisterService<CommandHistory>(); }

    sizet CommandHistory::measure(const UndoStep& step)
    {
        sizet bytes = step.label.size();
        forEachRequest(step, [&bytes](const CommandRequest& request) { bytes += measureRequest(request); });
        return bytes;
    }

    void CommandHistory::record(UndoStep step, const std::string& command, const nlohmann::json& args,
                                const u64 durationUs)
    {
        SPITE_ZONE("CommandHistory.record");
        const bool merging = !step.merge.empty();
        const Attribution by = step.by;
        const bool folded = push(std::move(step));
        if (!m_steps.back().view)
        {
            noteProvenance(m_steps.back());
        }
        if (!folded)
        {
            for (const u32 layer : m_steps.back().savedLayers)
            {
                m_layerSaved[layer] = m_steps.back().state;
            }
        }
        // a set and a batch of that set fold into one step, but a row replays only its own command
        if (folded && m_logTail == m_steps.back().index && !m_log.empty() && m_log.back().command == command)
        {
            CommandLogRow& row = m_log.back();
            m_logBytes -= rowBytes(row);
            row.args = args.dump();
            row.time = by.time;
            row.durationUs = durationUs;
            m_logBytes += rowBytes(row);
            trimLog();
            return;
        }
        log(command, args, by, durationUs);
        m_logTail = merging ? m_steps.back().index : 0;
    }

    std::unordered_map<u64, std::vector<std::string>> CommandHistory::touchedFields(const UndoStep& step) const
    {
        SPITE_ZONE("CommandHistory.touchedFields");
        std::unordered_map<u64, std::vector<std::string>> touched;
        forEachTouched(step, [this, &touched](const u64 id, const CommandRequest& request, const bool created)
                       { addField(touched[resolve(id)], fieldOf(request, created)); });
        return touched;
    }

    std::optional<sizet> CommandHistory::find(const u64 index) const
    {
        const auto it = std::ranges::lower_bound(m_steps, index, {}, &UndoStep::index);
        return it != m_steps.end() && it->index == index ? std::optional<sizet>(it - m_steps.begin()) : std::nullopt;
    }

    std::optional<RevertConflict> CommandHistory::revertConflict(const sizet position,
                                                                 const std::function<u64(u64)>& parentOf) const
    {
        struct Touch
        {
            std::vector<std::string> fields;
            bool created = false;
        };
        std::unordered_map<u64, Touch> mine;
        forEachTouched(m_steps[position],
                       [this, &mine](const u64 id, const CommandRequest& request, const bool created)
                       {
                           Touch& touch = mine[resolve(id)];
                           touch.created = touch.created || created;
                           addField(touch.fields, fieldOf(request, created));
                       });
        // the revert runs the inverse requests, which may write entities the forward requests never name (the outside
        // references a delete restores)
        for (const CommandRecord& record : m_steps[position].records)
        {
            for (const CommandRequest& inverse : record.inverse)
            {
                if (const std::optional<u64> id = ControlArgs(&inverse.args).integer("id"))
                {
                    addField(mine[resolve(*id)].fields, fieldOf(inverse, false));
                }
            }
        }
        const bool anyCreated = std::ranges::any_of(mine, [](const auto& entry) { return entry.second.created; });
        for (sizet i = position + 1; i < m_done; ++i)
        {
            const UndoStep& later = m_steps[i];
            // an undoable view command is no conflict even when it writes components (uiImport): a view step is not a
            // scene edit, the same line that keeps it out of the provenance, and a revert that overwrites one is
            // undone like any step
            if (later.view)
            {
                continue;
            }
            std::optional<RevertConflict> found;
            const auto note = [&](const u64 id, std::string field, const bool anyField)
            {
                const u64 live = resolve(id);
                const auto it = mine.find(live);
                if (found)
                {
                    return;
                }
                bool hit = false;
                if (it != mine.end())
                {
                    hit = anyField || it->second.created ||
                        std::ranges::any_of(it->second.fields, [&](const auto& f) { return fieldsOverlap(f, field); });
                }
                else if (anyCreated && parentOf)
                {
                    // a descendant of an entity the step created dies with it (a model's meshes)
                    u64 up = parentOf(live);
                    for (u32 hops = 0; up != 0 && hops < 256 && !hit; up = parentOf(up), ++hops)
                    {
                        const auto ancestor = mine.find(up);
                        hit = ancestor != mine.end() && ancestor->second.created;
                    }
                }
                if (hit)
                {
                    found = RevertConflict{later.index, live, std::move(field)};
                }
            };
            forEachTouched(later, [&](const u64 id, const CommandRequest& request, const bool created)
                           { note(id, fieldOf(request, created), false); });
            for (const CommandRecord& record : later.records)
            {
                // a child made under an entity the reverted step created would die with it
                if (const std::optional<u64> parent = ControlArgs(&record.forward.args).integer("parent"))
                {
                    note(*parent, record.forward.command, false);
                }
                // an entity the later step destroyed
                for (const CommandRequest& inverse : record.inverse)
                {
                    for (const u64 revived : inverse.created)
                    {
                        if (revived != 0)
                        {
                            note(revived, record.forward.command, true);
                        }
                    }
                }
            }
            if (found)
            {
                return found;
            }
        }
        return std::nullopt;
    }

    void CommandHistory::markReviewed(const UndoStep& step)
    {
        bool changed = false;
        const auto reviewed = touchedFields(step);
        std::unordered_map<u64, std::unordered_map<std::string, u64>> laterTouches;
        for (sizet index = m_done; index > 0; --index)
        {
            const auto& later = m_steps[index - 1];
            if (later.index <= step.index)
                break;
            if (later.view || later.by.source == "editor")
                continue;
            forEachTouched(later,
                           [&](u64 id, const CommandRequest& request, bool created)
                           {
                               id = resolve(id);
                               if (reviewed.contains(id))
                                   laterTouches[id][fieldOf(request, created)] = later.index;
                           });
        }
        for (const auto& [id, fields] : reviewed)
        {
            if (const auto it = m_provenance.find(id); it != m_provenance.end())
            {
                auto& unreviewed = it->second.unreviewed;
                if (const auto later = laterTouches.find(id); later != laterTouches.end())
                {
                    for (auto& field : unreviewed)
                    {
                        if (field.firstStep > step.index || !std::ranges::contains(fields, field.name))
                            continue;
                        if (const auto next = later->second.find(field.name);
                            next != later->second.end() && field.firstStep != next->second)
                        {
                            field.firstStep = next->second;
                            changed = true;
                        }
                    }
                }
                changed |=
                    std::erase_if(
                        unreviewed, [&](const auto& field)
                        { return field.firstStep <= step.index && std::ranges::contains(fields, field.name); }) != 0;
            }
        }
        if (changed)
            touch();
    }

    void CommandHistory::noteProvenance(const UndoStep& step)
    {
        SPITE_ZONE("CommandHistory.noteProvenance");
        const bool editor = step.by.source == "editor";
        for (auto& [id, fields] : touchedFields(step))
        {
            EntityProvenance& entry = m_provenance[id];
            entry.step = step.index;
            entry.by = step.by;
            if (!editor)
            {
                for (const std::string& name : fields)
                {
                    if (std::ranges::find(entry.unreviewed, name, &EntityProvenance::UnreviewedField::name) ==
                        entry.unreviewed.end())
                    {
                        entry.unreviewed.push_back({name, step.index});
                    }
                }
            }
            entry.fields = std::move(fields);
        }
    }

    void CommandHistory::setDone(const sizet done)
    {
        if (done < m_done)
        {
            const UndoStep& step = m_steps[done];
            for (const auto& [id, fields] : touchedFields(step))
            {
                if (const auto it = m_provenance.find(id); it != m_provenance.end())
                {
                    std::erase_if(it->second.unreviewed, [&step](const EntityProvenance::UnreviewedField& field)
                                  { return field.firstStep == step.index; });
                }
            }
            forgetProvenance(step);
        }
        else if (done > m_done && !m_steps[m_done].view)
        {
            noteProvenance(m_steps[m_done]);
        }
        m_done = done;
        touch();
    }

    void CommandHistory::forgetProvenance(const UndoStep& step)
    {
        if (step.view || m_provenance.empty())
        {
            return;
        }
        std::vector<u64> pending; // the entities whose entry names this step
        for (const auto& [id, fields] : touchedFields(step))
        {
            if (const auto it = m_provenance.find(id); it != m_provenance.end() && it->second.step == step.index)
            {
                pending.push_back(id);
            }
        }
        // step indices ascend: the steps before this one, newest first
        auto earlier = std::ranges::lower_bound(m_steps, step.index, {}, &UndoStep::index);
        while (!pending.empty() && earlier != m_steps.begin())
        {
            --earlier;
            if (earlier->view)
            {
                continue;
            }
            auto touched = touchedFields(*earlier);
            std::erase_if(pending,
                          [this, &earlier, &touched](const u64 id)
                          {
                              const auto fields = touched.find(id);
                              if (fields == touched.end())
                              {
                                  return false;
                              }
                              EntityProvenance& entry = m_provenance.find(id)->second;
                              entry.step = earlier->index;
                              entry.by = earlier->by;
                              entry.fields = std::move(fields->second);
                              return true;
                          });
        }
        for (const u64 id : pending)
        {
            const auto it = m_provenance.find(id);
            if (it->second.unreviewed.empty())
            {
                m_provenance.erase(it);
            }
            else
            {
                it->second.step = 0;
            }
        }
    }

    void CommandHistory::sweepProvenance(const std::function<bool(u64)>& alive)
    {
        std::optional<std::unordered_set<u64>> revivable;
        std::erase_if(m_provenance,
                      [&](const auto& entry)
                      {
                          if (alive(entry.first))
                          {
                              return false;
                          }
                          if (!revivable)
                          {
                              revivable.emplace();
                              for (const UndoStep& step : m_steps)
                              {
                                  forEachRequest(step,
                                                 [&](const CommandRequest& request)
                                                 {
                                                     for (const u64 id : request.created)
                                                     {
                                                         revivable->insert(resolve(id));
                                                     }
                                                 });
                              }
                          }
                          return !revivable->contains(entry.first);
                      });
        m_provenanceSweepAt = std::max(MIN_PRUNE_AT, m_provenance.size() * 2);
    }

    u32 CommandHistory::review(const std::optional<u64> id, const std::function<bool(u64)>& alive)
    {
        const auto mark = [&alive](const u64 entity, EntityProvenance& entry)
        {
            if (entry.unreviewed.empty() || !alive(entity))
            {
                return 0u;
            }
            entry.unreviewed.clear();
            return 1u;
        };
        if (id)
        {
            const auto it = m_provenance.find(*id);
            const u32 reviewed = it == m_provenance.end() ? 0u : mark(*id, it->second);
            if (reviewed)
            {
                touch();
            }
            return reviewed;
        }
        u32 reviewed = 0;
        for (auto& [entity, entry] : m_provenance)
        {
            reviewed += mark(entity, entry);
        }
        if (reviewed)
        {
            touch();
        }
        return reviewed;
    }

    void CommandHistory::trimLog()
    {
        // failed rows are bounded apart, so a client failing in a loop never pushes the applied rows (the replay) out;
        // rows are in sequence order, so the oldest failed one is found by its sequence
        while (m_failedSequences.size() > 1 &&
               (m_failedSequences.size() > m_limits.failedLogRows || m_failedBytes > m_limits.failedLogBytes))
        {
            const auto oldest =
                std::ranges::lower_bound(m_log, m_failedSequences.front(), {}, &CommandLogRow::sequence);
            m_failedBytes -= rowBytes(*oldest);
            m_log.erase(oldest);
            m_failedSequences.pop_front();
        }
        // the newest applied row stays even when it alone exceeds the byte limit
        const auto applied = [this] { return m_log.size() - m_failedSequences.size(); };
        while (applied() > 1 && (applied() > m_limits.logRows || m_logBytes > m_limits.logBytes))
        {
            const CommandLogRow& front = m_log.front();
            if (front.applied())
            {
                m_logBytes -= rowBytes(front);
                ++m_droppedLogRows;
            }
            else
            {
                m_failedBytes -= rowBytes(front);
                m_failedSequences.pop_front();
            }
            m_log.pop_front();
        }
    }

    bool CommandHistory::push(UndoStep step)
    {
        SPITE_ZONE("CommandHistory.push");
        if (!step.merge.empty() && m_done == m_steps.size() && !m_steps.empty() && setsSameFields(m_steps.back(), step))
        {
            UndoStep& last = m_steps.back();
            if (!last.view)
            {
                last.state = ++m_nextState; // a view step leaves the world's position alone, folded or not
            }
            last.by = std::move(step.by);
            for (const u32 layer : step.layers)
            {
                if (std::ranges::find(last.layers, layer) == last.layers.end())
                {
                    last.layers.push_back(layer);
                }
            }
            for (sizet i = 0; i < step.records.size(); ++i)
            {
                last.records[i].forward = std::move(step.records[i].forward);
            }
            remeasure(m_steps.size() - 1);
            return true;
        }
        bool dropped = m_steps.size() > m_done;
        while (m_steps.size() > m_done)
        {
            m_bytes -= m_steps.back().bytes;
            forgetProvenance(m_steps.back());
            m_steps.pop_back();
        }
        step.index = m_nextStep++;
        step.state = step.view ? state() : ++m_nextState;
        step.bytes = measure(step);
        m_bytes += step.bytes;
        m_steps.push_back(std::move(step));
        // the newest step stays even when it alone exceeds the byte limit
        while (m_steps.size() > 1 && (m_steps.size() > m_limits.steps || m_bytes > m_limits.stepBytes))
        {
            m_bytes -= m_steps.front().bytes;
            m_baseState = m_steps.front().state;
            retireLayers(m_steps.front());
            forgetProvenance(m_steps.front());
            m_steps.pop_front();
            dropped = true;
        }
        m_done = m_steps.size();
        touch();
        if (dropped)
        {
            pruneAliases();
        }
        return false;
    }

    void CommandHistory::remeasure(const sizet position)
    {
        UndoStep& step = m_steps[position];
        m_bytes -= step.bytes;
        step.bytes = measure(step);
        m_bytes += step.bytes;
        touch();
    }

    void CommandHistory::log(const std::string& command, const nlohmann::json& args, const Attribution& by,
                             const u64 durationUs, const std::string_view error)
    {
        m_logTail = 0;
        CommandLogRow row{.sequence = m_nextSequence++,
                          .command = command,
                          .args = args.dump(),
                          .source = by.source,
                          .client = by.client,
                          .time = by.time,
                          .result = error.empty() ? std::string("ok") : std::string(error),
                          .durationUs = durationUs};
        if (row.applied())
        {
            m_logBytes += rowBytes(row);
        }
        else
        {
            m_failedBytes += rowBytes(row);
            m_failedSequences.push_back(row.sequence);
        }
        m_log.push_back(std::move(row));
        trimLog();
    }

    void CommandHistory::logStep(const nlohmann::json& args, const u64 maxCount, const Attribution& by,
                                 const u64 durationUs)
    {
        const auto kindOf = [](const nlohmann::json& json)
        { return std::string(json.contains("fixed") ? "fixed" : "frames"); };
        const auto countOf = [](const nlohmann::json& json, const std::string& key)
        {
            // as the step handler read it: a count may be a digit string
            return json.contains(key) ? ControlArgs(&json).integer(key).value_or(0) : u64{1};
        };
        if (!m_log.empty() && m_log.back().command == "step" && m_log.back().applied() &&
            m_log.back().source == by.source && m_log.back().client == by.client)
        {
            CommandLogRow& row = m_log.back();
            nlohmann::json last = nlohmann::json::parse(row.args, nullptr, false);
            const std::string kind = kindOf(args);
            if (last.is_object() && kindOf(last) == kind)
            {
                const u64 sum = countOf(last, kind) + countOf(args, kind);
                nlohmann::json rest = args;
                rest.erase(kind);
                nlohmann::json lastRest = last;
                lastRest.erase(kind);
                if (sum <= maxCount && rest == lastRest)
                {
                    last[kind] = sum;
                    m_logBytes -= rowBytes(row);
                    row.args = last.dump();
                    row.time = by.time;
                    row.durationUs += durationUs;
                    m_logBytes += rowBytes(row);
                    trimLog();
                    return;
                }
            }
        }
        log("step", args, by, durationUs);
    }

    u64 CommandHistory::layerState(const u32 layer) const
    {
        for (sizet i = m_done; i-- > 0;)
        {
            const std::vector<u32>& touched = m_steps[i].layers;
            if (std::ranges::find(touched, layer) != touched.end() ||
                std::ranges::find(touched, ControlServer::ALL_LAYERS) != touched.end())
            {
                return m_steps[i].state;
            }
        }
        const auto base = m_layerBases.find(layer);
        return base != m_layerBases.end() ? base->second : m_layerEpoch;
    }

    void CommandHistory::retireLayers(const UndoStep& step)
    {
        if (std::ranges::find(step.layers, ControlServer::ALL_LAYERS) != step.layers.end())
        {
            m_layerEpoch = step.state;
            m_layerBases.clear();
            return;
        }
        for (const u32 layer : step.layers)
        {
            m_layerBases[layer] = step.state;
        }
    }

    void CommandHistory::renewStates()
    {
        m_layerEpoch = ++m_nextState;
        m_layerBases.clear();
        m_layerSaved.clear();
        u64 token = m_baseState = ++m_nextState;
        for (UndoStep& step : m_steps)
        {
            token = step.view ? token : ++m_nextState;
            step.state = token;
        }
    }

    void CommandHistory::abandonFrom(const sizet position)
    {
        while (m_steps.size() > position)
        {
            m_bytes -= m_steps.back().bytes;
            forgetProvenance(m_steps.back());
            m_steps.pop_back();
        }
        m_done = std::min(m_done, position);
        touch();
        renewStates();
        pruneAliases();
    }

    void CommandHistory::clear()
    {
        m_steps.clear();
        m_done = 0;
        touch();
        m_bytes = 0;
        m_baseState = ++m_nextState;
        m_layerEpoch = ++m_nextState;
        m_layerBases.clear();
        m_layerSaved.clear();
        m_provenance.clear();
        m_aliases.clear();
        m_aliasedOnto.clear();
    }

    u64 CommandHistory::resolve(u64 id) const
    {
        for (auto it = m_aliases.find(id); it != m_aliases.end(); it = m_aliases.find(id))
        {
            id = it->second;
        }
        return id;
    }

    sizet CommandHistory::longestAliasChain() const
    {
        sizet longest = 0;
        for (const auto& entry : m_aliases)
        {
            sizet hops = 1;
            for (auto next = m_aliases.find(entry.second); next != m_aliases.end(); next = m_aliases.find(next->second))
            {
                ++hops;
            }
            longest = std::max(longest, hops);
        }
        return longest;
    }

    void CommandHistory::alias(const u64 dead, const u64 live)
    {
        if (dead == live)
        {
            return;
        }
        // keeps every chain one hop: what pointed at dead now points at live
        std::vector<u64> moved;
        if (const auto behind = m_aliasedOnto.find(dead); behind != m_aliasedOnto.end())
        {
            moved = std::move(behind->second);
            m_aliasedOnto.erase(behind);
        }
        moved.push_back(dead);
        if (const auto entry = m_provenance.find(dead); entry != m_provenance.end())
        {
            EntityProvenance carried = std::move(entry->second);
            m_provenance.erase(entry);
            const auto [at, inserted] = m_provenance.try_emplace(live, std::move(carried));
            if (!inserted)
            {
                for (auto& field : carried.unreviewed)
                {
                    const auto previous =
                        std::ranges::find(at->second.unreviewed, field.name, &EntityProvenance::UnreviewedField::name);
                    if (previous == at->second.unreviewed.end())
                    {
                        at->second.unreviewed.push_back(std::move(field));
                    }
                    else
                    {
                        previous->firstStep = std::min(previous->firstStep, field.firstStep);
                    }
                }
            }
        }
        std::vector<u64>& onto = m_aliasedOnto[live];
        for (const u64 id : moved)
        {
            m_aliases[id] = live;
            onto.push_back(id);
        }
        // toggling undo and redo recreates entities without evicting a step: drop the ids no step names any more
        // once the map doubles past its last pruned size, so the cost stays amortized
        if (m_aliases.size() > m_pruneAt)
        {
            pruneAliases();
        }
    }

    void CommandHistory::pruneAliases()
    {
        if (m_aliases.empty())
        {
            return;
        }
        std::unordered_map<u64, u64> kept;
        const auto keep = [this, &kept](const u64 id)
        {
            if (const u64 live = resolve(id); live != id)
            {
                kept.emplace(id, live);
            }
        };
        for (const UndoStep& step : m_steps)
        {
            forEachRequest(step,
                           [&keep](const CommandRequest& request)
                           {
                               for (const char* key : {"id", "parent"})
                               {
                                   if (const std::optional<u64> id = ControlArgs(&request.args).integer(key))
                                   {
                                       keep(*id);
                                   }
                               }
                               for (const u64 id : request.created)
                               {
                                   keep(id);
                               }
                           });
        }
        m_aliases = std::move(kept);
        m_aliasedOnto.clear();
        for (const auto& [id, live] : m_aliases)
        {
            m_aliasedOnto[live].push_back(id);
        }
        m_pruneAt = std::max(MIN_PRUNE_AT, m_aliases.size() * 2);
    }
} // namespace spite
