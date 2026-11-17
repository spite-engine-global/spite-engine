#pragma once

#include <string>
#include <vector>

#include "base/Platform.hpp"

namespace spite
{
    inline constexpr u32 SCENE_DOCUMENT_VERSION = 1;
    inline constexpr u32 NO_PARENT_INDEX = static_cast<u32>(-1);

    // The plain-data pivot of scene streaming: collect (world -> document),
    // emit (document -> bytes), parse (bytes -> document), apply (document ->
    // world). The document knows nothing about fields - payloads are opaque
    // field-walk bytes keyed by component name; schema knowledge stays with the
    // registered components.
    struct DocumentComponent
    {
        // ecs component-name identity, interned: stable for the process lifetime.
        const char* typeName = nullptr;
        // persistedLayoutHash of the schema the payload was written under;
        // apply refuses a payload whose component's layout changed since
        u64 layoutHash = 0;
        std::vector<u8> payload;
    };

    struct DocumentEntity
    {
        u64 sourceId = 0; // live entity id at collect time; the cross-reference key inside payloads
        u32 parentIndex = NO_PARENT_INDEX; // index into SceneDocument::entities
        std::vector<DocumentComponent> components;
    };

    // How loading met the current schemas, aggregated per component and field.
    // Each category records whether saved data was defaulted, converted or omitted.
    enum class SceneChangeKind : u8
    {
        Defaulted, // a persisted field the saved component lacks: it took its default
        Dropped, // a saved field the schema no longer persists: ignored
        Unreadable, // a saved value of the wrong shape: the field took its default
        Skipped, // the whole component: not scene data in this process
    };

    struct SceneChange
    {
        std::string component;
        std::string field; // dotted path; empty for Skipped
        SceneChangeKind kind = SceneChangeKind::Defaulted;
        u32 count = 0; // component instances affected
        std::string detail; // first reader error (Unreadable, Skipped)
    };

    struct SceneDocument
    {
        u32 version = SCENE_DOCUMENT_VERSION;
        std::vector<DocumentEntity> entities; // parents before children
        u32 skippedComponents = 0; // unknown or malformed components a parse dropped, each logged
        std::vector<SceneChange> changes; // what a JSON parse met against the current schemas
    };

    [[nodiscard]] constexpr const char* sceneChangeName(const SceneChangeKind kind)
    {
        switch (kind)
        {
        case SceneChangeKind::Defaulted:
            return "defaulted";
        case SceneChangeKind::Dropped:
            return "dropped";
        case SceneChangeKind::Unreadable:
            return "unreadable";
        case SceneChangeKind::Skipped:
            return "skipped";
        }
        return "unknown";
    }
} // namespace spite
