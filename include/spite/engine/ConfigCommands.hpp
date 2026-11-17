#pragma once

#include <functional>
#include <string_view>

#include "engine/EngineToolsExport.hpp"

namespace spite
{
    class ConfigStore;
    class ControlServer;

    // Takes a field of a startup-only section whose row is apply=live into the running state now, called by setConfig
    // and resetConfig on the frame thread once the value is staged: the section id, the field's path in it and its
    // new value as JSON text. Only the host knows the running state (the engine's config table).
    using ConfigLiveApplyFn =
        std::function<void(std::string_view section, std::string_view path, std::string_view valueJson)>;

    // Registers on control (owner "engine"), over the application's config store (the query and the
    // write side):
    //   config [section=<id>] -> sections: one entry per config section and per file a module declared, by id:
    //     id         the section id ("application.graphics", "physics.world", "ui.navigation")
    //     kind       "section" (a value the store loads, edits and saves) or "file" (listed only: readOnly, reason)
    //     doc        what it configures
    //     apply      "startup" (a change takes a restart) or "live" (at the frame boundary); sections only
    //     layer      where a save writes: "project", "user" or "memory" (never saved)
    //     file       the file a save writes, "<layer>/<path>" under the config directory ('/' separated); absent
    //                for layer "memory"; a file a module declared by its absolute path shows that path unless it
    //                lies under its layer's root
    //     files      every file the section reads in order, a later one overriding: layer, file, exists, target
    //     dirty      the staged value differs from what was last saved or loaded
    //     pending    a committed change not yet in force (startup sections: until a restart)
    //     values     the staged value as nested JSON, in the same names the file uses; enum fields show their name
    //   With section=, that one entry also carries:
    //     fields     one row per leaf of the schema, in schema order: path (dotted, relative to the section),
    //                group (the nested struct's path), type (u32, f32, bool, vec3, string, ...), kind (the widget a
    //                form shows: number, bool, enum, string, path, vec2, vec3, vec4), doc, unit, default, value,
    //                running (the value in force: differs from value while a change waits for a boundary or a
    //                restart), saved (what the file holds: the last loaded or saved value) and source ("default" while
    //                saved is the default, else the layer of the last existing file: "project", "user" or "memory"),
    //                min and max where declared, choices (enum: value and name), root and directory (path
    //                fields), apply ("live" or "startup"), readOnly
    //     groups     the nested structs: path, doc
    //   The staged value is what a save writes; the running value is the ApplicationConfig_config table.
    //
    //   setConfig [section=<id>] path=<dotted field> value=<json> (undoable): checks the value against the row's
    //     type, range and choices (a refusal states the range or the choices), stages it and answers path,
    //     previous, value, apply (the row's) and status: "applied" (in force now), "deferred" (a live section: at
    //     the next frame boundary) or "restartRequired" (read at startup; a save keeps it for the next run). A
    //     row with apply=live in a startup-only section (graphics.shadows.*, objectCapacityCeiling) is applied
    //     now through `applyLive` (also when the staged value is unchanged: the running one may differ); the
    //     section's other fields stay staged. Without section, path "graphics.<f>" names application.graphics (the
    //     spelling from before sections). A path field takes a plain relative path (no absolute, "..", ':' or
    //     device name; a file field names a file that loads, a directory field may be "."). The status
    //     is the field's state after the commit: a value back at the one in force is "applied".
    //   resetConfig section=<id> [path=<field or group>] (undoable): stages the defaults (the rows' default) of the
    //     section, a group of its fields or one field; answers each changed field as setConfig does, in `changed`.
    //   restoreConfig section=<id> values=<{path: value}> (undoable): the inverse setConfig and resetConfig record,
    //     all changed fields in one request; takes the values the store held (no row limit but the type's own), the
    //     section's validator gates the whole value. Runs only as a replay of the history (ControlServer::
    //     replayingHistory): a request is refused, so no client value reaches it without setConfig's checks.
    //   saveConfig section=<id>: writes the staged value to the section's file in its layer (project or user), as
    //     the checked-in files are laid out (4-space indent, keys sorted, the replaced file's line endings);
    //     answers the file and restartRequired (a field whose staged value differs from the one in force and that
    //     only a restart applies). Nothing else writes a project file: the run's end saves only the user layer. The
    //     first save pins every default value of the section in the file (a later default change in code does not
    //     reach a project that saved).
    SPITE_ENGINE_TOOLS_API void registerConfigCommands(ControlServer& control, ConfigStore& store,
                                                       ConfigLiveApplyFn applyLive = {});
} // namespace spite
