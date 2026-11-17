#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "SerializationExport.hpp"
#include "base/Schema.hpp"
#include "ecs/EcsTypes.hpp"

namespace spite
{
    // Backend visitor for the schema walk: one method per primitive of the
    // lattice. Names are for self-describing formats (JSON); positional
    // formats (binary) ignore them. Array elements carry an empty name.
    // The walk emits begin/endObject pairs only for nested Struct fields; the
    // root destination is positioned by the caller.
    class IFormatWriter
    {
    public:
        virtual ~IFormatWriter() = default;
        virtual void beginObject(std::string_view name) = 0;
        virtual void endObject() = 0;
        virtual void beginArray(std::string_view name, u32 count) = 0;
        virtual void endArray() = 0;
        virtual void writeBool(std::string_view name, bool v) = 0;
        virtual void writeU8(std::string_view name, u8 v) = 0;
        virtual void writeU16(std::string_view name, u16 v) = 0;
        virtual void writeU32(std::string_view name, u32 v) = 0;
        virtual void writeU64(std::string_view name, u64 v) = 0;
        virtual void writeI32(std::string_view name, i32 v) = 0;
        virtual void writeF32(std::string_view name, f32 v) = 0;
        virtual void writeF64(std::string_view name, double v) = 0;
        // Vec2/Vec3/Vec4/Quat components, or a Mat4's 16 elements column-major.
        virtual void writeFloats(std::string_view name, std::span<const f32> v) = 0;
        virtual void writeString(std::string_view name, std::string_view v) = 0;
        virtual void writeEntity(std::string_view name, Entity v) = 0;
        // JSON null; the schema walk never emits it (control replies carrying
        // request JSON do).
        virtual void writeNull(std::string_view name) = 0;
    };

    enum class FieldRead : u8
    {
        Ok, // value read into out
        Absent, // field not present in the input; the walk keeps the current value
        Error, // malformed input; description in lastError()
    };

    class IFormatReader
    {
    public:
        virtual ~IFormatReader() = default;
        virtual FieldRead beginObject(std::string_view name) = 0;
        virtual void endObject() = 0;
        virtual FieldRead beginArray(std::string_view name, u32& outCount) = 0;
        virtual void endArray() = 0;
        virtual FieldRead readBool(std::string_view name, bool& out) = 0;
        virtual FieldRead readU8(std::string_view name, u8& out) = 0;
        virtual FieldRead readU16(std::string_view name, u16& out) = 0;
        virtual FieldRead readU32(std::string_view name, u32& out) = 0;
        virtual FieldRead readU64(std::string_view name, u64& out) = 0;
        virtual FieldRead readI32(std::string_view name, i32& out) = 0;
        virtual FieldRead readF32(std::string_view name, f32& out) = 0;
        virtual FieldRead readF64(std::string_view name, double& out) = 0;
        virtual FieldRead readFloats(std::string_view name, std::span<f32> out) = 0;
        virtual FieldRead readString(std::string_view name, std::string& out) = 0;
        virtual FieldRead readEntity(std::string_view name, Entity& out) = 0;
        [[nodiscard]] virtual std::string_view lastError() const = 0;
    };

    enum class FieldSet : u8
    {
        Persisted, // what scenes save and load
        All, // what tools observe: persisted and observed fields
    };

    // Emits the fields of schema over instance as primitive events. Does not
    // open or close a root object.
    SPITE_SERIALIZATION_API void writeValue(const StructSchema& schema, const void* instance, IFormatWriter& out,
                                            FieldSet set = FieldSet::Persisted);

    // A field a tolerant read could not take: dotted path from the root
    // ("light.color") and the reader's error.
    struct FieldIssue
    {
        std::string path;
        std::string error;
    };

    // Applies primitive events for the persisted fields into instance. Fields
    // reported Absent keep their current value. On malformed input returns
    // false and fills outError - unless outIssues is given: then a malformed
    // field keeps its current value, is appended there and the walk goes on
    // (for self-describing formats; a positional reader cannot resync).
    SPITE_SERIALIZATION_API bool readValue(const StructSchema& schema, void* instance, IFormatReader& in,
                                           std::string& outError, std::vector<FieldIssue>* outIssues = nullptr);

    // FNV-1a over the persisted walk shape (field names, kinds, array counts,
    // nested structs): payloads written under one hash decode positionally
    // under another schema only when the hashes match.
    SPITE_SERIALIZATION_API u64 persistedLayoutHash(const StructSchema& schema);
} // namespace spite
