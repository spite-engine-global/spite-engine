#include "engine/ConfigCommands.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "application/ConfigStore.hpp"
#include "base/Schema.hpp"
#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "serialization/FormatWalk.hpp"
#include "serialization/JsonFormat.hpp"

namespace spite
{
    namespace
    {
        const ConfigFieldMeta* findMeta(const std::span<const ConfigFieldMeta> fields, const std::string_view path)
        {
            for (const ConfigFieldMeta& meta : fields)
            {
                if (meta.path == path)
                {
                    return &meta;
                }
            }
            return nullptr;
        }

        std::string joinPath(const std::string_view prefix, const std::string_view name)
        {
            return prefix.empty() ? std::string(name) : std::format("{}.{}", prefix, name);
        }

        // Fixed arrays, entities, lists and the like have no row: a config section is flat values and nested structs.
        bool isLeaf(const FieldDesc& field)
        {
            if (field.arrayCount != 0)
            {
                return false;
            }
            switch (field.kind)
            {
            case FieldKind::Bool:
            case FieldKind::U8:
            case FieldKind::U16:
            case FieldKind::U32:
            case FieldKind::U64:
            case FieldKind::I32:
            case FieldKind::F32:
            case FieldKind::F64:
            case FieldKind::Vec2:
            case FieldKind::Vec3:
            case FieldKind::Vec4:
            case FieldKind::String:
            case FieldKind::InternedString:
                return true;
            default:
                return false;
            }
        }

        bool isGroup(const FieldDesc& field) { return field.kind == FieldKind::Struct && field.arrayCount == 0; }

        std::string_view typeName(const FieldKind kind)
        {
            switch (kind)
            {
            case FieldKind::Bool:
                return "bool";
            case FieldKind::U8:
                return "u8";
            case FieldKind::U16:
                return "u16";
            case FieldKind::U32:
                return "u32";
            case FieldKind::U64:
                return "u64";
            case FieldKind::I32:
                return "i32";
            case FieldKind::F32:
                return "f32";
            case FieldKind::F64:
                return "f64";
            case FieldKind::Vec2:
                return "vec2";
            case FieldKind::Vec3:
                return "vec3";
            case FieldKind::Vec4:
                return "vec4";
            case FieldKind::String:
            case FieldKind::InternedString:
                return "string";
            default:
                return "unsupported";
            }
        }

        bool isInteger(const FieldKind kind)
        {
            return kind == FieldKind::U8 || kind == FieldKind::U16 || kind == FieldKind::U32 ||
                kind == FieldKind::U64 || kind == FieldKind::I32;
        }

        // The widget a form shows for the field (UiFormKind): an enum choice list and a path are strings or integers
        // the field's meta says more about.
        std::string_view formKind(const FieldDesc& field, const ConfigFieldMeta* meta)
        {
            if (meta && !meta->choices.empty() && isInteger(field.kind))
            {
                return "enum";
            }
            switch (field.kind)
            {
            case FieldKind::Bool:
                return "bool";
            case FieldKind::Vec2:
                return "vec2";
            case FieldKind::Vec3:
                return "vec3";
            case FieldKind::Vec4:
                return "vec4";
            case FieldKind::String:
            case FieldKind::InternedString:
                return meta && !meta->root.empty() ? "path" : "string";
            default:
                return "number";
            }
        }

        std::string_view applyName(const ConfigApplyMode section, const ConfigFieldMeta* meta)
        {
            const ConfigFieldApply apply = meta ? meta->apply : ConfigFieldApply::Section;
            if (apply == ConfigFieldApply::Section)
            {
                return section == ConfigApplyMode::LiveAtFrameBoundary ? "live" : "startup";
            }
            return apply == ConfigFieldApply::Live ? "live" : "startup";
        }

        std::string_view layerName(const ConfigPersistenceLayer layer)
        {
            switch (layer)
            {
            case ConfigPersistenceLayer::Project:
                return "project";
            case ConfigPersistenceLayer::User:
                return "user";
            case ConfigPersistenceLayer::Memory:
                break;
            }
            return "memory";
        }

        // One leaf of a struct laid out at `base`, an enum field as its choice name.
        void writeLeaf(IFormatWriter& out, const std::string_view name, const FieldDesc& field, const void* base,
                       const ConfigFieldMeta* meta)
        {
            const void* ptr = static_cast<const std::byte*>(base) + field.offset;
            const auto named = [&](const std::int64_t value)
            {
                if (meta)
                {
                    for (const ConfigChoice& choice : meta->choices)
                    {
                        if (choice.value == value)
                        {
                            out.writeString(name, choice.name);
                            return true;
                        }
                    }
                }
                return false;
            };
            switch (field.kind)
            {
            case FieldKind::Bool:
                out.writeBool(name, *static_cast<const bool*>(ptr));
                break;
            case FieldKind::U8:
                if (const u8 value = *static_cast<const u8*>(ptr); !named(value))
                {
                    out.writeU8(name, value);
                }
                break;
            case FieldKind::U16:
                if (const u16 value = *static_cast<const u16*>(ptr); !named(value))
                {
                    out.writeU16(name, value);
                }
                break;
            case FieldKind::U32:
                if (const u32 value = *static_cast<const u32*>(ptr); !named(value))
                {
                    out.writeU32(name, value);
                }
                break;
            case FieldKind::U64:
                if (const u64 value = *static_cast<const u64*>(ptr);
                    value > static_cast<u64>(INT64_MAX) || !named(static_cast<std::int64_t>(value)))
                {
                    out.writeU64(name, value);
                }
                break;
            case FieldKind::I32:
                if (const i32 value = *static_cast<const i32*>(ptr); !named(value))
                {
                    out.writeI32(name, value);
                }
                break;
            case FieldKind::F32:
                out.writeF32(name, *static_cast<const f32*>(ptr));
                break;
            case FieldKind::F64:
                out.writeF64(name, *static_cast<const double*>(ptr));
                break;
            case FieldKind::Vec2:
                out.writeFloats(name, {static_cast<const f32*>(ptr), 2});
                break;
            case FieldKind::Vec3:
                out.writeFloats(name, {static_cast<const f32*>(ptr), 3});
                break;
            case FieldKind::Vec4:
                out.writeFloats(name, {static_cast<const f32*>(ptr), 4});
                break;
            case FieldKind::String:
                out.writeString(name, *static_cast<const std::string*>(ptr));
                break;
            case FieldKind::InternedString:
                {
                    const InternedString& text = *static_cast<const InternedString*>(ptr);
                    out.writeString(name, text.isValid() ? text.toView() : std::string_view{});
                    break;
                }
            default:
                break;
            }
        }

        // The nested value object: the section's staged value under the names its file uses.
        void writeValueObject(IFormatWriter& out, const StructSchema& schema, const void* value,
                              const std::span<const ConfigFieldMeta> metas, const std::string_view prefix)
        {
            for (const FieldDesc& field : schema.fields)
            {
                if ((field.flags & fieldFlags::PERSISTED) == 0)
                {
                    continue;
                }
                const std::string path = joinPath(prefix, field.name);
                if (isGroup(field))
                {
                    out.beginObject(field.name);
                    writeValueObject(out, *field.structSchema, static_cast<const std::byte*>(value) + field.offset,
                                     metas, path);
                    out.endObject();
                }
                else if (isLeaf(field))
                {
                    writeLeaf(out, field.name, field, value, findMeta(metas, path));
                }
            }
        }

        // A leaf of a section's value: its dotted path and where the struct that holds it starts.
        struct Leaf
        {
            std::string path;
            const FieldDesc* field = nullptr;
            sizet offset = 0;
        };

        // the leaf's value in a section's value, as the bare JSON text the field's kind writes; an enum is its
        // choice name when named, its integer otherwise
        std::string leafJson(const Leaf& leaf, const void* value, const ConfigFieldMeta* named)
        {
            JsonFragmentWriter writer;
            writeLeaf(writer, "v", *leaf.field, static_cast<const std::byte*>(value) + leaf.offset, named);
            std::string text;
            writer.dump(text);
            return nlohmann::json::parse(text)["v"].dump();
        }

        struct RowCounts
        {
            u32 leaves = 0;
            u32 groups = 0;
        };

        RowCounts countRows(const StructSchema& schema)
        {
            RowCounts counts;
            for (const FieldDesc& field : schema.fields)
            {
                if ((field.flags & fieldFlags::PERSISTED) == 0)
                {
                    continue;
                }
                if (isGroup(field))
                {
                    const RowCounts inner = countRows(*field.structSchema);
                    counts.leaves += inner.leaves;
                    counts.groups += inner.groups + 1;
                }
                else
                {
                    ++counts.leaves;
                }
            }
            return counts;
        }

        // `saved` (null: unknown) is the value the file holds; a row's source is "default" while that equals the
        // default, else `fileLayer`, the layer of the last file that exists (a later one overrides). `running` is the
        // value in force.
        void writeRows(IFormatWriter& out, const StructSchema& schema, const void* value, const void* defaults,
                       const void* saved, const void* running, const std::string_view fileLayer,
                       const ConfigApplyMode applyMode, const std::span<const ConfigFieldMeta> metas,
                       const std::string_view prefix)
        {
            for (const FieldDesc& field : schema.fields)
            {
                if ((field.flags & fieldFlags::PERSISTED) == 0)
                {
                    continue;
                }
                const std::string path = joinPath(prefix, field.name);
                if (isGroup(field))
                {
                    const auto inner = [&](const void* base)
                    { return base ? static_cast<const std::byte*>(base) + field.offset : nullptr; };
                    writeRows(out, *field.structSchema, static_cast<const std::byte*>(value) + field.offset,
                              static_cast<const std::byte*>(defaults) + field.offset, inner(saved), inner(running),
                              fileLayer, applyMode, metas, path);
                    continue;
                }
                const ConfigFieldMeta* meta = findMeta(metas, path);
                const bool leaf = isLeaf(field);
                out.beginObject({});
                out.writeString("path", path);
                if (!prefix.empty())
                {
                    out.writeString("group", prefix);
                }
                out.writeString("type", typeName(field.kind));
                out.writeString("kind", leaf ? formKind(field, meta) : std::string_view("unsupported"));
                if (meta && !meta->doc.empty())
                {
                    out.writeString("doc", meta->doc);
                }
                if (meta && !meta->unit.empty())
                {
                    out.writeString("unit", meta->unit);
                }
                if (leaf)
                {
                    writeLeaf(out, "default", field, defaults, meta);
                    writeLeaf(out, "value", field, value, meta);
                    if (running)
                    {
                        writeLeaf(out, "running", field, running, meta);
                    }
                    if (saved)
                    {
                        writeLeaf(out, "saved", field, saved, meta);
                        const Leaf leaf{path, &field, 0};
                        out.writeString("source",
                                        leafJson(leaf, saved, nullptr) == leafJson(leaf, defaults, nullptr)
                                            ? std::string_view("default")
                                            : fileLayer);
                    }
                }
                if (meta && std::isfinite(meta->min))
                {
                    out.writeF64("min", meta->min);
                }
                if (meta && std::isfinite(meta->max))
                {
                    out.writeF64("max", meta->max);
                }
                if (meta && !meta->choices.empty())
                {
                    out.beginArray("choices", static_cast<u32>(meta->choices.size()));
                    for (const ConfigChoice& choice : meta->choices)
                    {
                        out.beginObject({});
                        out.writeI32("value", choice.value);
                        out.writeString("name", choice.name);
                        out.endObject();
                    }
                    out.endArray();
                }
                if (meta && !meta->root.empty())
                {
                    out.writeString("root", meta->root);
                    out.writeBool("directory", meta->directory);
                }
                out.writeString("apply", applyName(applyMode, meta));
                out.writeBool("readOnly", (field.flags & fieldFlags::READONLY) != 0);
                out.endObject();
            }
        }

        void writeGroups(IFormatWriter& out, const StructSchema& schema, const std::span<const ConfigFieldMeta> metas,
                         const std::string_view prefix)
        {
            for (const FieldDesc& field : schema.fields)
            {
                if ((field.flags & fieldFlags::PERSISTED) == 0 || !isGroup(field))
                {
                    continue;
                }
                const std::string path = joinPath(prefix, field.name);
                out.beginObject({});
                out.writeString("path", path);
                if (const ConfigFieldMeta* meta = findMeta(metas, path); meta && !meta->doc.empty())
                {
                    out.writeString("doc", meta->doc);
                }
                out.endObject();
                writeGroups(out, *field.structSchema, metas, path);
            }
        }

        void writeSection(IFormatWriter& out, const ConfigSectionInfo& info, const bool withFields)
        {
            const bool declared = !info.readOnlyReason.empty();
            out.writeString("id", info.id.toView());
            out.writeString("kind", declared ? "file" : "section");
            if (!info.doc.empty())
            {
                out.writeString("doc", info.doc);
            }
            if (declared)
            {
                out.writeBool("readOnly", true);
                out.writeString("reason", info.readOnlyReason);
            }
            else
            {
                out.writeString("apply", info.applyMode == ConfigApplyMode::LiveAtFrameBoundary ? "live" : "startup");
            }
            out.writeString("layer", layerName(info.layer));
            for (const ConfigFileState& file : info.files)
            {
                if (file.target)
                {
                    out.writeString("file", file.file);
                }
            }
            if (declared && !info.files.empty())
            {
                out.writeString("file", info.files.front().file);
            }
            out.beginArray("files", static_cast<u32>(info.files.size()));
            for (const ConfigFileState& file : info.files)
            {
                out.beginObject({});
                out.writeString("layer", file.layer);
                out.writeString("file", file.file);
                out.writeBool("exists", file.exists);
                out.writeBool("target", file.target);
                out.endObject();
            }
            out.endArray();
            if (declared)
            {
                return;
            }
            out.writeBool("dirty", info.dirty);
            out.writeBool("pending", info.pending);
            out.beginObject("values");
            if (info.schema)
            {
                writeValueObject(out, *info.schema, info.value.get(), info.fields, {});
            }
            else if (info.writeValues)
            {
                info.writeValues(info.value.get(), out);
            }
            out.endObject();
            if (withFields && info.schema)
            {
                out.beginArray("fields", countRows(*info.schema).leaves);
                std::string_view fileLayer = layerName(info.layer);
                for (const ConfigFileState& file : info.files)
                {
                    if (file.exists)
                    {
                        fileLayer = file.layer;
                    }
                }
                writeRows(out, *info.schema, info.value.get(), info.defaults.get(), info.saved.get(),
                          info.published.get(), fileLayer, info.applyMode, info.fields, {});
                out.endArray();
                out.beginArray("groups", countRows(*info.schema).groups);
                writeGroups(out, *info.schema, info.fields, {});
                out.endArray();
            }
        }

        // -- the write commands --

        const ConfigSectionInfo* findSection(const std::span<const ConfigSectionInfo> infos, const std::string_view id)
        {
            const auto it =
                std::ranges::find(infos, id, [](const ConfigSectionInfo& info) { return info.id.toView(); });
            return it == infos.end() ? nullptr : &*it;
        }

        std::string unknownSection(const std::span<const ConfigSectionInfo> infos, const std::string_view id)
        {
            std::string known;
            for (const ConfigSectionInfo& info : infos)
            {
                known += (known.empty() ? "" : ", ") + std::string(info.id.toView());
            }
            return std::format("no config section '{}'; sections: {}", id, known.empty() ? "none" : known);
        }

        // Why a section takes no field edit, empty when it does.
        std::string notEditable(const ConfigSectionInfo& info)
        {
            if (!info.readOnlyReason.empty())
            {
                return std::format("'{}' is a file the engine only lists, never edits: {}", info.id.toView(),
                                   info.readOnlyReason);
            }
            if (!info.schema)
            {
                return std::format("'{}' has no fields a command sets: {}", info.id.toView(), info.doc);
            }
            return {};
        }

        void collectLeaves(const StructSchema& schema, const std::string_view prefix, const sizet offset,
                           std::vector<Leaf>& out)
        {
            for (const FieldDesc& field : schema.fields)
            {
                if ((field.flags & fieldFlags::PERSISTED) == 0)
                {
                    continue;
                }
                std::string path = joinPath(prefix, field.name);
                if (isGroup(field))
                {
                    collectLeaves(*field.structSchema, path, offset + field.offset, out);
                }
                else if (isLeaf(field))
                {
                    out.push_back({std::move(path), &field, offset});
                }
            }
        }

        std::string rangeText(const double min, const double max)
        {
            if (std::isfinite(min) && std::isfinite(max))
            {
                return std::format("{} to {}", min, max);
            }
            return std::isfinite(min) ? std::format(">= {}", min) : std::format("<= {}", max);
        }

        // The value as JSON the field's kind takes, in `out`; otherwise what the field accepts. `restore` is for a
        // value the store itself held (the history's inverse, a file's value included): the type's own bounds only,
        // none of the row's command-only limits (the range, the path rules, the choices of an integer); the
        // section's validator is the gate. Never for a value a client sent.
        std::string normalizeValue(const FieldDesc& field, const ConfigFieldMeta* meta, const nlohmann::json& given,
                                   nlohmann::json& out, const bool restore)
        {
            const std::string_view form = formKind(field, meta);
            if (form == "bool")
            {
                out = given;
                return given.is_boolean() ? std::string{} : std::string("expects true or false");
            }
            if (form == "enum")
            {
                if (restore && given.is_number_integer())
                {
                    out = given;
                    return {};
                }
                std::string names;
                for (const ConfigChoice& choice : meta->choices)
                {
                    if ((given.is_string() && given.get_ref<const std::string&>() == choice.name) ||
                        (given.is_number_integer() && given.get<std::int64_t>() == choice.value))
                    {
                        out = choice.value;
                        return {};
                    }
                    names += std::format("{}{} ({})", names.empty() ? "" : ", ", choice.name, choice.value);
                }
                return "expects one of the choices: " + names;
            }
            if (form == "string" || form == "path")
            {
                out = given;
                if (!given.is_string())
                {
                    return "expects a string";
                }
                if (form == "path" && !restore)
                {
                    const std::string& text = given.get_ref<const std::string&>();
                    if (const std::string problem = plainRelativePathProblem(text, meta->directory); !problem.empty())
                    {
                        return std::format("expects a plain relative path under the {} root, but it {}", meta->root,
                                           problem);
                    }
                    // the shader and mapping loaders build their path from this text through the ANSI code page
                    if (std::ranges::any_of(text, [](const char c) { return static_cast<unsigned char>(c) >= 0x80; }))
                    {
                        return "expects an ASCII path: the loaders read it through the ANSI code page";
                    }
                    if (meta->fileProblem)
                    {
                        if (const std::string problem = meta->fileProblem(text); !problem.empty())
                        {
                            return std::format("names a file that cannot be used: {}", problem);
                        }
                    }
                }
                return {};
            }
            if (form == "vec2" || form == "vec3" || form == "vec4")
            {
                const sizet count = form == "vec2" ? 2 : form == "vec3" ? 3 : 4;
                out = given;
                bool numbers = given.is_array() && given.size() == count;
                for (sizet i = 0; numbers && i < count; ++i)
                {
                    numbers = given[i].is_number() && std::isfinite(given[i].get<double>());
                }
                if (!numbers)
                {
                    return std::format("expects an array of {} numbers", count);
                }
                // the row's range holds for each component
                for (const nlohmann::json& component : given)
                {
                    const double value = component.get<double>();
                    if (!std::isfinite(static_cast<float>(value)))
                    {
                        return std::format("{} does not fit a 32-bit float", component.dump());
                    }
                    if (meta && !restore && !(value >= meta->min && value <= meta->max))
                    {
                        return std::format("{} is outside the range {} for each component", component.dump(),
                                           rangeText(meta->min, meta->max));
                    }
                }
                return {};
            }

            const bool integer = isInteger(field.kind);
            if (!given.is_number() || (integer && !given.is_number_integer()))
            {
                return integer ? "expects an integer" : "expects a number";
            }
            double min = meta && !restore ? meta->min : -CONFIG_UNBOUNDED;
            double max = meta && !restore ? meta->max : CONFIG_UNBOUNDED;
            switch (field.kind)
            {
            case FieldKind::U8:
                min = std::max(min, 0.0);
                max = std::min(max, 255.0);
                break;
            case FieldKind::U16:
                min = std::max(min, 0.0);
                max = std::min(max, 65535.0);
                break;
            case FieldKind::U32:
                min = std::max(min, 0.0);
                max = std::min(max, 4294967295.0);
                break;
            case FieldKind::U64:
                min = std::max(min, 0.0);
                break;
            case FieldKind::I32:
                min = std::max(min, -2147483648.0);
                max = std::min(max, 2147483647.0);
                break;
            default:
                break;
            }
            const double value = given.get<double>();
            if (!(value >= min && value <= max))
            {
                return std::format("{} is outside the range {}", given.dump(), rangeText(min, max));
            }
            if (field.kind == FieldKind::F32 && !std::isfinite(static_cast<float>(value)))
            {
                return std::format("{} does not fit a 32-bit float", given.dump());
            }
            out = given;
            return {};
        }

        // Whether the leaf's value reaches the running state only at a restart: a startup row, or any row of a
        // startup-only section that the host does not apply now (`hosted`: it takes a live row of one).
        bool needsRestart(const ConfigSectionInfo& info, const ConfigFieldMeta* meta, const bool hosted)
        {
            const ConfigFieldApply row = meta ? meta->apply : ConfigFieldApply::Section;
            if (row == ConfigFieldApply::Startup)
            {
                return true;
            }
            return info.applyMode == ConfigApplyMode::StartupOnly && !(row == ConfigFieldApply::Live && hosted);
        }

        // How a leaf takes effect, from the store's state after the commit: `waiting` is the leaf's staged value
        // differing from the one in force (a reply derived from a before/after diff would call an edit back to the
        // value in force pending).
        std::string_view changeStatus(const ConfigSectionInfo& info, const ConfigFieldMeta* meta, const bool waiting,
                                      const bool hosted)
        {
            const bool appliedNow = meta && meta->apply == ConfigFieldApply::Live &&
                info.applyMode == ConfigApplyMode::StartupOnly && hosted;
            if (!waiting || appliedNow)
            {
                return "applied";
            }
            return needsRestart(info, meta, hosted) ? "restartRequired" : "deferred";
        }

        struct Change
        {
            const Leaf* leaf = nullptr;
            const ConfigFieldMeta* meta = nullptr;
            std::string_view status;
        };

        std::string_view worseStatus(const std::string_view a, const std::string_view b)
        {
            const auto rank = [](const std::string_view status)
            {
                return status == "restartRequired" ? 2 : status == "deferred" ? 1 : 0;
            };
            return rank(b) > rank(a) ? b : a;
        }

        void writeChange(IFormatWriter& out, const Leaf& leaf, const ConfigFieldMeta* meta, const void* before,
                         const void* after, const ConfigSectionInfo& info, const std::string_view status)
        {
            out.writeString("path", leaf.path);
            writeLeaf(out, "previous", *leaf.field, static_cast<const std::byte*>(before) + leaf.offset, meta);
            writeLeaf(out, "value", *leaf.field, static_cast<const std::byte*>(after) + leaf.offset, meta);
            out.writeString("apply", applyName(info.applyMode, meta));
            out.writeString("status", status);
        }

        // The edit setConfig and resetConfig share: `change` sets `targets` in a copy of the section's staged value.
        // Commits, applies the live rows of a startup section through the host, records the inverse (one
        // restoreConfig of every changed leaf's previous value) and writes the reply. `only` is the one field a
        // single-field command names: its change is also written at the top level.
        std::string editFields(ControlServer& control, ConfigStore& store, const ConfigLiveApplyFn& applyLive,
                               const ConfigSectionInfo& info, const std::span<const Leaf> targets,
                               const std::function<std::string(void*)>& change, const Leaf* only,
                               const std::string_view subject, IFormatWriter& out)
        {
            std::string error;
            std::shared_ptr<const void> after;
            const ConfigCommitResult result = store.commitEdit(info.id, change, error, after);
            if (!error.empty())
            {
                return error;
            }
            if (result == ConfigCommitResult::Rejected)
            {
                return std::format("'{}' refuses {}: its validator rejects the combination of values; {}",
                                   info.id.toView(), subject, info.doc);
            }
            if (result == ConfigCommitResult::NotFound || result == ConfigCommitResult::TypeMismatch)
            {
                return std::format("'{}' cannot be edited", info.id.toView());
            }

            std::vector<Change> changes;
            std::string_view overall = "applied";
            for (const Leaf& leaf : targets)
            {
                const ConfigFieldMeta* meta = findMeta(info.fields, leaf.path);
                const std::string staged = leafJson(leaf, after.get(), nullptr);
                // a live row of a startup-only section goes to the host whether or not the staged value changed: the
                // running value can differ from the staged one (a host's override of the file)
                if (info.applyMode == ConfigApplyMode::StartupOnly && meta && meta->apply == ConfigFieldApply::Live &&
                    applyLive)
                {
                    applyLive(info.id.toView(), leaf.path, staged);
                }
                const bool waiting = staged != leafJson(leaf, info.published.get(), nullptr);
                const std::string_view status = changeStatus(info, meta, waiting, applyLive != nullptr);
                overall = worseStatus(overall, status);
                if (leafJson(leaf, info.value.get(), nullptr) != staged)
                {
                    changes.push_back({&leaf, meta, status});
                }
                if (&leaf == only)
                {
                    writeChange(out, leaf, meta, info.value.get(), after.get(), info, status);
                }
            }
            if (!changes.empty())
            {
                // one request restores every changed field together: the section's validator then sees the whole
                // previous value, never a half-restored one
                JsonFragmentWriter inverse;
                inverse.writeString("section", info.id.toView());
                inverse.beginObject("values");
                for (const Change& entry : changes)
                {
                    writeLeaf(inverse, entry.leaf->path, *entry.leaf->field,
                              static_cast<const std::byte*>(info.value.get()) + entry.leaf->offset, entry.meta);
                }
                inverse.endObject();
                std::string inverseArgs;
                inverse.dump(inverseArgs);
                control.recordInverse("restoreConfig", inverseArgs);
            }
            out.writeString("section", info.id.toView());
            out.writeString("status", overall);
            if (!only)
            {
                out.beginArray("changed", static_cast<u32>(changes.size()));
                for (const Change& entry : changes)
                {
                    out.beginObject({});
                    writeChange(out, *entry.leaf, entry.meta, info.value.get(), after.get(), info, entry.status);
                    out.endObject();
                }
                out.endArray();
            }
            return {};
        }

        std::string setConfig(ControlServer& control, ConfigStore& store, const ConfigLiveApplyFn& applyLive,
                              const ControlArgs& args, IFormatWriter& out)
        {
            std::string id = args.string("section").value_or(std::string{});
            std::string path = args.string("path").value_or(std::string{});
            if (id.empty())
            {
                // the spelling from before sections: "graphics.<field>" is a field of application.graphics
                constexpr std::string_view LEGACY = "graphics.";
                if (!path.starts_with(LEGACY))
                {
                    return "setConfig needs section (a config section id, as the config query lists them)";
                }
                id = "application.graphics";
                path.erase(0, LEGACY.size());
            }
            const nlohmann::json given = nlohmann::json::parse(args.json("value").value_or("null"), nullptr, false);
            if (given.is_discarded() || given.is_null())
            {
                return "value must be a JSON value (a number, true or false, a string or an array)";
            }

            const std::vector<ConfigSectionInfo> infos = store.describe();
            const ConfigSectionInfo* info = findSection(infos, id);
            if (!info)
            {
                return unknownSection(infos, id);
            }
            if (std::string refusal = notEditable(*info); !refusal.empty())
            {
                return refusal;
            }
            std::vector<Leaf> leaves;
            collectLeaves(*info->schema, {}, 0, leaves);
            const auto leaf = std::ranges::find(leaves, path, &Leaf::path);
            if (leaf == leaves.end())
            {
                return std::format("section '{}' has no field '{}'; the config query with section= lists its fields",
                                   id, path);
            }
            const ConfigFieldMeta* meta = findMeta(info->fields, path);
            const std::string label = std::format("{} {}", id, path);
            if (leaf->field->flags & fieldFlags::READONLY)
            {
                return std::format("{}: the field is read-only", label);
            }
            nlohmann::json value;
            if (std::string problem = normalizeValue(*leaf->field, meta, given, value, false); !problem.empty())
            {
                return std::format("{}: {}; {}", label, problem, meta ? meta->doc : "the config query lists the field");
            }

            const Leaf& target = *leaf;
            const std::string text = value.dump();
            const auto change = [&](void* staged)
            {
                JsonFragmentWriter ignored;
                return setSchemaField(*info->schema, staged, path, text, ignored);
            };
            return editFields(control, store, applyLive, *info, std::span<const Leaf>(&target, 1), change, &target,
                              std::format("{} = {}", path, given.dump()), out);
        }

        std::string resetConfig(ControlServer& control, ConfigStore& store, const ConfigLiveApplyFn& applyLive,
                                const ControlArgs& args, IFormatWriter& out)
        {
            const std::string id = args.string("section").value_or(std::string{});
            const std::string path = args.string("path").value_or(std::string{});
            const std::vector<ConfigSectionInfo> infos = store.describe();
            const ConfigSectionInfo* info = findSection(infos, id);
            if (!info)
            {
                return unknownSection(infos, id);
            }
            if (std::string refusal = notEditable(*info); !refusal.empty())
            {
                return refusal;
            }
            std::vector<Leaf> leaves;
            collectLeaves(*info->schema, {}, 0, leaves);
            const Leaf* only = nullptr;
            std::vector<Leaf> group;
            if (!path.empty())
            {
                const auto leaf = std::ranges::find(leaves, path, &Leaf::path);
                if (leaf != leaves.end())
                {
                    only = &*leaf;
                }
                else
                {
                    // a group (shadows) resets its leaves, listed as a section's are
                    const std::string prefix = path + ".";
                    std::ranges::copy_if(leaves, std::back_inserter(group),
                                         [&](const Leaf& candidate) { return candidate.path.starts_with(prefix); });
                    if (group.empty())
                    {
                        return std::format(
                            "section '{}' has no field or group '{}'; the config query with section= lists them", id,
                            path);
                    }
                }
            }
            // each target to its default as the schema writes it (an enum its integer)
            const std::span<const Leaf> targets = only ? std::span<const Leaf>(only, 1)
                : !group.empty()                       ? std::span<const Leaf>(group)
                                                       : std::span<const Leaf>(leaves);
            const auto change = [&](void* staged)
            {
                JsonFragmentWriter ignored;
                for (const Leaf& leaf : targets)
                {
                    if (leaf.field->flags & fieldFlags::READONLY)
                    {
                        continue;
                    }
                    if (std::string error = setSchemaField(*info->schema, staged, leaf.path,
                                                           leafJson(leaf, info->defaults.get(), nullptr), ignored);
                        !error.empty())
                    {
                        return error;
                    }
                }
                return std::string{};
            };
            return editFields(control, store, applyLive, *info, targets, change, only, "the defaults", out);
        }

        // The undo of setConfig and resetConfig: sets several fields at once to the values the store held before the
        // edit (a file's value past a row's command limit among them), so it takes no row limit and no path rule.
        // Only the history's own replay of an inverse it recorded may run it: a request's values are a client's, and
        // would skip the checks setConfig gives them. The section's validator still gates the whole value.
        std::string restoreConfig(ControlServer& control, ConfigStore& store, const ConfigLiveApplyFn& applyLive,
                                  const ControlArgs& args, IFormatWriter& out)
        {
            if (!control.replayingHistory())
            {
                return "restoreConfig is the undo step of setConfig and resetConfig and takes no request: undo "
                       "reverts an edit, setConfig names a value";
            }
            const std::string id = args.string("section").value_or(std::string{});
            const nlohmann::json values = nlohmann::json::parse(args.json("values").value_or("null"), nullptr, false);
            if (!values.is_object() || values.empty())
            {
                return "values must be a JSON object of field path to value";
            }
            const std::vector<ConfigSectionInfo> infos = store.describe();
            const ConfigSectionInfo* info = findSection(infos, id);
            if (!info)
            {
                return unknownSection(infos, id);
            }
            if (std::string refusal = notEditable(*info); !refusal.empty())
            {
                return refusal;
            }
            std::vector<Leaf> leaves;
            collectLeaves(*info->schema, {}, 0, leaves);
            std::vector<Leaf> targets;
            std::vector<std::string> texts;
            for (const auto& [path, given] : values.items())
            {
                const auto leaf = std::ranges::find(leaves, path, &Leaf::path);
                if (leaf == leaves.end())
                {
                    return std::format(
                        "section '{}' has no field '{}'; the config query with section= lists its fields", id, path);
                }
                if (leaf->field->flags & fieldFlags::READONLY)
                {
                    return std::format("{} {}: the field is read-only", id, path);
                }
                nlohmann::json value;
                if (std::string problem =
                        normalizeValue(*leaf->field, findMeta(info->fields, path), given, value, true);
                    !problem.empty())
                {
                    return std::format("{} {}: {}", id, path, problem);
                }
                targets.push_back(*leaf);
                texts.push_back(value.dump());
            }
            const auto change = [&](void* staged)
            {
                JsonFragmentWriter ignored;
                for (sizet i = 0; i < targets.size(); ++i)
                {
                    if (std::string error = setSchemaField(*info->schema, staged, targets[i].path, texts[i], ignored);
                        !error.empty())
                    {
                        return error;
                    }
                }
                return std::string{};
            };
            return editFields(control, store, applyLive, *info, targets, change, nullptr, "the restored values", out);
        }

        std::string saveConfig(ConfigStore& store, const bool hosted, const ControlArgs& args, IFormatWriter& out)
        {
            const std::string id = args.string("section").value_or(std::string{});
            const std::vector<ConfigSectionInfo> infos = store.describe();
            const ConfigSectionInfo* info = findSection(infos, id);
            if (!info)
            {
                return unknownSection(infos, id);
            }
            if (!info->readOnlyReason.empty())
            {
                return std::format("'{}' is a file the engine only lists, never writes: {}", id, info->readOnlyReason);
            }
            if (info->layer == ConfigPersistenceLayer::Memory)
            {
                return std::format("'{}' is kept in memory and has no file: this run neither reads nor writes it", id);
            }
            const auto target = std::ranges::find(info->files, true, &ConfigFileState::target);
            if (!store.saveSection(info->id))
            {
                return std::format("could not write '{}' (the log has the reason)",
                                   target != info->files.end() ? target->file : id);
            }
            out.writeString("section", id);
            out.writeString("layer", layerName(info->layer));
            if (target != info->files.end())
            {
                out.writeString("file", target->file);
            }
            // the file holds the staged value: a restart is due for the fields whose staged value differs from the one
            // in force and that no frame boundary or live row applies (a live row of a startup section that the
            // host took now is in force, though its staged value is never published)
            bool restart = !info->schema && info->pending && info->applyMode == ConfigApplyMode::StartupOnly;
            if (info->pending && info->schema)
            {
                std::vector<Leaf> leaves;
                collectLeaves(*info->schema, {}, 0, leaves);
                for (const Leaf& leaf : leaves)
                {
                    restart = restart ||
                        (needsRestart(*info, findMeta(info->fields, leaf.path), hosted) &&
                         leafJson(leaf, info->value.get(), nullptr) != leafJson(leaf, info->published.get(), nullptr));
                }
            }
            out.writeBool("restartRequired", restart);
            return {};
        }
    } // namespace

    void registerConfigCommands(ControlServer& control, ConfigStore& store, ConfigLiveApplyFn applyLive)
    {
        control.registerUndoableViewCommand(
            "setConfig", "engine",
            "sets one field of a config section (the config query lists sections and, with section=, each field's "
            "type, range and choices): the value is checked against the field's type, range and choices, staged in "
            "the config store and applied by the field's apply mode, which the result's status says: applied (now), "
            "deferred (to the next frame boundary) or restartRequired (read at startup; a save keeps it for the "
            "next run). Nothing reaches a file until saveConfig. The result carries the previous value; undo "
            "restores it",
            {optionalArg("section", ArgKind::String, "a config section id, as the config query lists them",
                         "none: path is then graphics.<field>, a field of application.graphics"),
             requiredArg("path", ArgKind::String,
                         "the field, dotted from the section's value as the config rows name it (shadows.maxDistance)"),
             requiredArg("value", ArgKind::Json,
                         "the new value: a number, true or false, a string, or an array; an enum field takes a choice "
                         "name or number; a path field a plain relative path")},
            [&control, &store, applyLive](const ControlArgs& args, IFormatWriter& out)
            { return setConfig(control, store, applyLive, args, out); });
        control.registerUndoableViewCommand(
            "resetConfig", "engine",
            "restores a config section, a group of its fields or one field to its default (the config rows' "
            "default): staged like setConfig, so the result lists each changed field with its previous value and "
            "status; undo restores them",
            {requiredArg("section", ArgKind::String, "a config section id, as the config query lists them"),
             optionalArg("path", ArgKind::String,
                         "one field or a group of fields (shadows), dotted as setConfig "
                         "names them",
                         "every field")},
            [&control, &store, applyLive](const ControlArgs& args, IFormatWriter& out)
            { return resetConfig(control, store, applyLive, args, out); });
        control.registerUndoableViewCommand(
            "restoreConfig", "engine",
            "the undo step of setConfig and resetConfig: sets several fields of a config section at once back to the "
            "values the store held (a file's value past a row's command limit among them), which those commands "
            "recorded. Only undo, redo and a batch's rollback run it: a request is refused, because a value a client "
            "sends gets the checks setConfig applies. The section's own validator still refuses the whole value",
            {requiredArg("section", ArgKind::String, "a config section id, as the config query lists them"),
             requiredArg("values", ArgKind::Json,
                         "an object of field path to value, as setConfig takes them: {\"timeScale\": 2000}")},
            [&control, &store, applyLive](const ControlArgs& args, IFormatWriter& out)
            { return restoreConfig(control, store, applyLive, args, out); });
        control.registerViewCommand(
            "saveConfig", "engine",
            "writes a config section's staged value to its file, in the layer the config query names (project or "
            "user), replacing the file; a startup-only section runs its old value until a restart, which the result's "
            "restartRequired says. The first save pins every default value of the section in the file: a later "
            "change of a default in code does not reach a project that saved",
            {requiredArg("section", ArgKind::String, "a config section id, as the config query lists them")},
            [&store, hosted = applyLive != nullptr](const ControlArgs& args, IFormatWriter& out)
            { return saveConfig(store, hosted, args, out); });

        control.registerQuery(
            "config", "engine",
            "the config sections and the files modules declared, by id: kind (section or file), doc, apply (startup "
            "or live), layer (where a save writes), file, files read (with exists), dirty, pending and values (the "
            "staged value, in the names its file uses); with section= that entry adds fields (per leaf: path, group, "
            "type, kind, doc, unit, default, value, running, saved, source, min, max, choices, root, directory, apply, "
            "readOnly) and groups. "
            "A declared file is read-only with a reason. The running config is the ApplicationConfig_config table.",
            {optionalArg("section", ArgKind::String, "a section id as the list shows it: adds its fields and groups",
                         "every section, without fields")},
            [&store](const ControlArgs& args, IFormatWriter& out)
            {
                const std::optional<std::string> section = args.string("section");
                const std::vector<ConfigSectionInfo> infos = store.describe(section && !section->empty());
                if (section && !section->empty())
                {
                    const ConfigSectionInfo* found = findSection(infos, *section);
                    if (!found)
                    {
                        return unknownSection(infos, *section);
                    }
                    out.beginArray("sections", 1);
                    out.beginObject({});
                    writeSection(out, *found, true);
                    out.endObject();
                    out.endArray();
                    return std::string{};
                }
                out.beginArray("sections", static_cast<u32>(infos.size()));
                for (const ConfigSectionInfo& info : infos)
                {
                    out.beginObject({});
                    writeSection(out, info, false);
                    out.endObject();
                }
                out.endArray();
                return std::string{};
            });
    }
} // namespace spite
