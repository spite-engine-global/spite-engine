#pragma once

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "SerializationExport.hpp"
#include "base/Schema.hpp"

namespace spite
{
    struct FieldIssue; // FormatWalk.hpp

    inline constexpr u32 SERIALIZED_BINARY_VERSION = 1;

    // Every function here persists: it walks the PERSISTED fields only.
    //
    // Binary: schema-ordered values behind an 8-byte header (magic + version);
    // no field names in the payload. Every read is bounds-checked.
    SPITE_SERIALIZATION_API void serializeBinary(const StructSchema& schema, const void* instance,
                                                 std::vector<u8>& outBytes);
    // On false, fields read before the error may already be modified.
    SPITE_SERIALIZATION_API bool deserializeBinary(const StructSchema& schema, void* instance,
                                                   std::span<const u8> bytes, std::string& outError);

    // Headerless field walk: the per-component payload form used by the scene
    // document. Same body as serializeBinary/deserializeBinary without the
    // file header; every read is still bounds-checked. When entityIdMap is
    // given, Entity values are remapped through it (missing key ->
    // Entity::undefined, counted in outDanglingCount when non-null).
    SPITE_SERIALIZATION_API void serializeFields(const StructSchema& schema, const void* instance,
                                                 std::vector<u8>& outBytes);
    SPITE_SERIALIZATION_API bool deserializeFields(const StructSchema& schema, void* instance,
                                                   std::span<const u8> bytes, std::string& outError,
                                                   const std::unordered_map<u64, u64>* entityIdMap = nullptr,
                                                   u32* outDanglingCount = nullptr);

    // JSON: objects keyed by field name; unknown keys are ignored, missing
    // keys keep the current value. Vectors, quaternions and matrices
    // (column-major) are number arrays, enums their integer, Entity a u64,
    // nested structs objects, fixed arrays arrays. With outIssues, a
    // malformed field keeps its value and is reported there instead of
    // failing the read (see readValue). serializeJson's indent >= 0 writes that many spaces per level, one member
    // per line (a file a person reads and reviews); the default is one line.
    SPITE_SERIALIZATION_API void serializeJson(const StructSchema& schema, const void* instance, std::string& outText,
                                               int indent = -1);
    SPITE_SERIALIZATION_API bool deserializeJson(const StructSchema& schema, void* instance, std::string_view text,
                                                 std::string& outError, std::vector<FieldIssue>* outIssues = nullptr);
} // namespace spite
