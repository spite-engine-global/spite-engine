#include "scene/SceneStream.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <nlohmann/json.hpp>

#include "base/Assert.hpp"
#include "base/Logging.hpp"
#include "base/StringInterner.hpp"
#include "SceneInternal.hpp"
#include "serialization/FormatWalk.hpp"
#include "serialization/JsonFormat.hpp"
#include "serialization/Serializer.hpp"

namespace spite
{
    namespace
    {
        // Layout guard for parse-time allocation sizing: an entity record is
        // at least 16 bytes (sourceId + parentIndex + componentCount), a
        // component record at least 16 (name length + layout hash + payload
        // length).
        constexpr size_t MIN_ENTITY_BYTES = 16;
        constexpr size_t MIN_COMPONENT_BYTES = 16;

        const char* stableName(const std::string& name)
        {
            // Unknown component names outlive the parse call: interner-owned
            // strings are stable for the process lifetime.
            return toHashedString(name.c_str()).toView().data();
        }

        void appendU32(std::vector<u8>& out, const u32 v)
        {
            const auto* bytes = reinterpret_cast<const u8*>(&v);
            out.insert(out.end(), bytes, bytes + sizeof(v));
        }

        void appendU64(std::vector<u8>& out, const u64 v)
        {
            const auto* bytes = reinterpret_cast<const u8*>(&v);
            out.insert(out.end(), bytes, bytes + sizeof(v));
        }

        void appendString(std::vector<u8>& out, const std::string_view v)
        {
            appendU32(out, static_cast<u32>(v.size()));
            const auto* bytes = reinterpret_cast<const u8*>(v.data());
            out.insert(out.end(), bytes, bytes + v.size());
        }

        // -- JSON field accessors that never throw -----------------------------

        u64 readU64Field(const nlohmann::json& object, const char* key, const u64 fallback)
        {
            const auto it = object.find(key);
            if (it == object.end() || !it->is_number_unsigned())
            {
                return fallback;
            }
            return it->get<u64>();
        }

        // Records the persisted fields a saved object lacks (defaulted) and the
        // saved keys the schema does not persist (dropped), into nested structs.
        void diffFields(const StructSchema& schema, const nlohmann::json& object, const std::string& prefix,
                        const std::string_view component, std::vector<SceneChange>& changes)
        {
            const auto pathOf = [&prefix](const std::string_view name)
            { return prefix.empty() ? std::string(name) : prefix + "." + std::string(name); };
            const auto persisted = [](const FieldDesc& field) { return (field.flags & fieldFlags::PERSISTED) != 0; };

            for (const FieldDesc& field : schema.fields)
            {
                if (!persisted(field))
                {
                    continue;
                }
                const auto it = object.find(std::string(field.name));
                if (it == object.end())
                {
                    recordChange(changes, component, pathOf(field.name), SceneChangeKind::Defaulted);
                }
                else if (field.kind == FieldKind::Struct && field.arrayCount == 0 && it->is_object())
                {
                    diffFields(*field.structSchema, *it, pathOf(field.name), component, changes);
                }
                else if (field.kind == FieldKind::Struct && it->is_array() && !it->empty() && it->front().is_object())
                {
                    // elements share one schema: the first one reports for all
                    diffFields(*field.structSchema, it->front(), pathOf(field.name), component, changes);
                }
            }
            for (const auto& [key, value] : object.items())
            {
                if (std::ranges::none_of(schema.fields,
                                         [&](const FieldDesc& field) { return persisted(field) && field.name == key; }))
                {
                    recordChange(changes, component, pathOf(key), SceneChangeKind::Dropped);
                }
            }
        }

        // Renders one document component into a readable JSON object; false
        // when the component is not scene data here or the payload is stale.
        bool renderComponentJson(const DocumentComponent& component, LayoutHashes& hashes, nlohmann::json& out)
        {
            const ComponentMetadata* metadata = sceneComponentMetadata(component.typeName);
            if (metadata == nullptr)
            {
                SWARNING_LOG("emitSceneJson: unknown component {} - skipped", component.typeName);
                return false;
            }
            if (hashes.of(*metadata) != component.layoutHash)
            {
                SWARNING_LOG("emitSceneJson: component {} changed layout since the payload was written - skipped",
                             component.typeName);
                return false;
            }

            ScopedInstance instance(*metadata);
            std::string error;
            if (!deserializeFields(*metadata->schema, instance.get(), component.payload, error))
            {
                SWARNING_LOG("emitSceneJson: stale payload for component {} - skipped ({})", component.typeName, error);
                return false;
            }

            JsonFragmentWriter writer;
            writeValue(*metadata->schema, instance.get(), writer);
            std::string fragment;
            writer.dump(fragment);
            out = nlohmann::json::parse(fragment);
            return true;
        }
    } // namespace

    void emitSceneBinary(const SceneDocument& document, std::vector<u8>& outBytes)
    {
        outBytes.clear();
        outBytes.insert(outBytes.end(), SCENE_BINARY_MAGIC, SCENE_BINARY_MAGIC + sizeof(SCENE_BINARY_MAGIC));
        appendU32(outBytes, SCENE_BINARY_VERSION);
        appendU32(outBytes, static_cast<u32>(document.entities.size()));

        for (const DocumentEntity& entity : document.entities)
        {
            appendU64(outBytes, entity.sourceId);
            appendU32(outBytes, entity.parentIndex);
            appendU32(outBytes, static_cast<u32>(entity.components.size()));
            for (const DocumentComponent& component : entity.components)
            {
                appendString(outBytes, component.typeName);
                appendU64(outBytes, component.layoutHash);
                appendU32(outBytes, static_cast<u32>(component.payload.size()));
                outBytes.insert(outBytes.end(), component.payload.begin(), component.payload.end());
            }
        }
    }

    Expected<SceneDocument> parseSceneBinary(const std::span<const u8> bytes)
    {
        size_t pos = 0;
        const auto take = [bytes, &pos](void* dst, const size_t size)
        {
            if (size > bytes.size() - pos)
            {
                return false;
            }
            std::memcpy(dst, bytes.data() + pos, size);
            pos += size;
            return true;
        };
        const auto remaining = [bytes, &pos]() { return bytes.size() - pos; };

        u8 magic[4] = {};
        u32 version = 0;
        u32 entityCount = 0;
        if (!take(magic, sizeof(magic)) || !take(&version, sizeof(version)) || !take(&entityCount, sizeof(entityCount)))
        {
            return std::unexpected(
                makeError(ErrorCode::CorruptedData, "parseSceneBinary: data shorter than the header"));
        }
        if (std::memcmp(magic, SCENE_BINARY_MAGIC, sizeof(magic)) != 0)
        {
            return std::unexpected(makeError(ErrorCode::InvalidMagicNumber, "parseSceneBinary: bad magic"));
        }
        if (version != SCENE_BINARY_VERSION)
        {
            return std::unexpected(
                makeError(ErrorCode::UnsupportedVersion, "parseSceneBinary: unsupported format version"));
        }
        if (static_cast<size_t>(entityCount) * MIN_ENTITY_BYTES > remaining())
        {
            return std::unexpected(
                makeError(ErrorCode::CorruptedData, "parseSceneBinary: entity count exceeds remaining data"));
        }

        SceneDocument document;
        document.entities.resize(entityCount);
        for (DocumentEntity& entity : document.entities)
        {
            u32 componentCount = 0;
            if (!take(&entity.sourceId, sizeof(entity.sourceId)) ||
                !take(&entity.parentIndex, sizeof(entity.parentIndex)) ||
                !take(&componentCount, sizeof(componentCount)))
            {
                return std::unexpected(makeError(ErrorCode::UnexpectedEndOfFile, "parseSceneBinary: truncated entity"));
            }
            if (static_cast<size_t>(componentCount) * MIN_COMPONENT_BYTES > remaining())
            {
                return std::unexpected(
                    makeError(ErrorCode::CorruptedData, "parseSceneBinary: component count exceeds remaining data"));
            }

            entity.components.resize(componentCount);
            for (DocumentComponent& component : entity.components)
            {
                u32 nameLength = 0;
                u32 payloadSize = 0;
                if (!take(&nameLength, sizeof(nameLength)))
                {
                    return std::unexpected(
                        makeError(ErrorCode::UnexpectedEndOfFile, "parseSceneBinary: truncated component name"));
                }
                std::string name(nameLength, '\0');
                if (!take(name.data(), nameLength))
                {
                    return std::unexpected(
                        makeError(ErrorCode::UnexpectedEndOfFile, "parseSceneBinary: truncated component name"));
                }
                if (!take(&component.layoutHash, sizeof(component.layoutHash)) ||
                    !take(&payloadSize, sizeof(payloadSize)) || static_cast<size_t>(payloadSize) > remaining())
                {
                    return std::unexpected(
                        makeError(ErrorCode::UnexpectedEndOfFile, "parseSceneBinary: truncated component payload"));
                }
                component.typeName = stableName(name);
                component.payload.resize(payloadSize);
                if (!take(component.payload.data(), payloadSize))
                {
                    return std::unexpected(
                        makeError(ErrorCode::UnexpectedEndOfFile, "parseSceneBinary: truncated component payload"));
                }
            }
        }

        // binary is a same-build cache: positional payloads cannot follow a
        // field change, so a changed layout refuses the file instead of
        // loading part of it (the JSON save carries the data across)
        if (const std::string stale = staleLayouts(document); !stale.empty())
        {
            return std::unexpected(makeError(ErrorCode::UnsupportedVersion,
                                             "parseSceneBinary: component layouts changed since the save: " + stale +
                                                 " (binary scenes are same-build caches; load the JSON save)"));
        }
        return document;
    }

    void emitSceneJson(const SceneDocument& document, std::string& outText)
    {
        nlohmann::json root = nlohmann::json::object();
        root["version"] = document.version;
        nlohmann::json entities = nlohmann::json::array();

        LayoutHashes hashes;
        for (const DocumentEntity& entity : document.entities)
        {
            nlohmann::json entityJson = nlohmann::json::object();
            entityJson["id"] = entity.sourceId;
            entityJson["parent"] = entity.parentIndex;
            nlohmann::json components = nlohmann::json::object();
            for (const DocumentComponent& component : entity.components)
            {
                nlohmann::json fields;
                if (renderComponentJson(component, hashes, fields))
                {
                    components[component.typeName] = std::move(fields);
                }
            }
            entityJson["components"] = std::move(components);
            entities.push_back(std::move(entityJson));
        }

        root["entities"] = std::move(entities);
        outText = root.dump(2);
    }

    Expected<SceneDocument> parseSceneJson(const std::string_view text)
    {
        const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded())
        {
            return std::unexpected(makeError(ErrorCode::SceneParsingFailed, "parseSceneJson: invalid JSON"));
        }
        if (!parsed.is_object())
        {
            return std::unexpected(
                makeError(ErrorCode::SceneParsingFailed, "parseSceneJson: expected a JSON object at the root"));
        }
        const u64 version = readU64Field(parsed, "version", SCENE_DOCUMENT_VERSION);
        if (version != SCENE_DOCUMENT_VERSION)
        {
            return std::unexpected(
                makeError(ErrorCode::UnsupportedVersion, "parseSceneJson: unsupported scene version"));
        }
        const auto entitiesIt = parsed.find("entities");
        if (entitiesIt == parsed.end() || !entitiesIt->is_array())
        {
            return std::unexpected(makeError(ErrorCode::SceneParsingFailed, "parseSceneJson: missing entities array"));
        }

        SceneDocument document;
        document.entities.resize(entitiesIt->size());
        LayoutHashes hashes;
        size_t entityIndex = 0;
        for (const nlohmann::json& entityJson : *entitiesIt)
        {
            if (!entityJson.is_object())
            {
                return std::unexpected(
                    makeError(ErrorCode::CorruptedData, "parseSceneJson: entity entry is not an object"));
            }
            DocumentEntity& entity = document.entities[entityIndex++];
            entity.sourceId = readU64Field(entityJson, "id", 0);
            entity.parentIndex = static_cast<u32>(readU64Field(entityJson, "parent", NO_PARENT_INDEX));

            const auto componentsIt = entityJson.find("components");
            if (componentsIt == entityJson.end() || !componentsIt->is_object())
            {
                continue;
            }
            for (const auto& [name, fields] : componentsIt->items())
            {
                const ComponentMetadata* metadata = sceneComponentMetadata(name.c_str());
                if (metadata == nullptr || !fields.is_object())
                {
                    const char* reason = metadata == nullptr ? "not scene data in this process" : "expected an object";
                    SWARNING_LOG("parseSceneJson: component {} skipped: {}", name, reason);
                    ++document.skippedComponents;
                    recordChange(document.changes, name, {}, SceneChangeKind::Skipped, reason);
                    continue;
                }

                // by field name: added fields default, removed ones drop,
                // unreadable values default - each reported, the rest kept
                diffFields(*metadata->schema, fields, {}, name, document.changes);
                ScopedInstance instance(*metadata);
                JsonFragmentReader reader(fields.dump());
                std::string error;
                std::vector<FieldIssue> issues;
                readValue(*metadata->schema, instance.get(), reader, error, &issues);
                for (size_t i = 0; i < issues.size(); ++i)
                {
                    const FieldIssue& issue = issues[i];
                    // array elements share a path: count this instance once
                    if (std::ranges::any_of(issues.begin(), issues.begin() + static_cast<std::ptrdiff_t>(i),
                                            [&](const FieldIssue& earlier) { return earlier.path == issue.path; }))
                    {
                        continue;
                    }
                    SWARNING_LOG("parseSceneJson: {}.{} unreadable - default kept ({})", name, issue.path, issue.error);
                    recordChange(document.changes, name, issue.path, SceneChangeKind::Unreadable, issue.error);
                }

                DocumentComponent component;
                component.typeName = metadata->name.toView().data();
                component.layoutHash = hashes.of(*metadata);
                serializeFields(*metadata->schema, instance.get(), component.payload);
                entity.components.push_back(std::move(component));
            }
        }

        return document;
    }
} // namespace spite
