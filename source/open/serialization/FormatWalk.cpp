#include "serialization/FormatWalk.hpp"

#include <vector>

namespace spite
{
    namespace
    {
        bool isSelected(const FieldDesc& field, const FieldSet set)
        {
            return set == FieldSet::All || (field.flags & fieldFlags::PERSISTED) != 0;
        }

        u32 floatCount(const FieldKind kind)
        {
            switch (kind)
            {
            case FieldKind::Vec2:
                return 2;
            case FieldKind::Vec3:
                return 3;
            case FieldKind::Vec4:
            case FieldKind::Quat:
                return 4;
            case FieldKind::Mat4:
                return 16;
            default:
                return 0;
            }
        }

        void writeStruct(const StructSchema& schema, const void* instance, IFormatWriter& out, FieldSet set);

        void writeElement(const FieldDesc& field, const void* ptr, const std::string_view name, IFormatWriter& out,
                          const FieldSet set)
        {
            switch (field.kind)
            {
            case FieldKind::Bool:
                out.writeBool(name, *static_cast<const bool*>(ptr));
                break;
            case FieldKind::U8:
                out.writeU8(name, *static_cast<const u8*>(ptr));
                break;
            case FieldKind::U16:
                out.writeU16(name, *static_cast<const u16*>(ptr));
                break;
            case FieldKind::U32:
                out.writeU32(name, *static_cast<const u32*>(ptr));
                break;
            case FieldKind::U64:
                out.writeU64(name, *static_cast<const u64*>(ptr));
                break;
            case FieldKind::I32:
                out.writeI32(name, *static_cast<const i32*>(ptr));
                break;
            case FieldKind::F32:
                out.writeF32(name, *static_cast<const f32*>(ptr));
                break;
            case FieldKind::F64:
                out.writeF64(name, *static_cast<const double*>(ptr));
                break;
            case FieldKind::Vec2:
            case FieldKind::Vec3:
            case FieldKind::Vec4:
            case FieldKind::Quat:
            case FieldKind::Mat4:
                out.writeFloats(name, {static_cast<const f32*>(ptr), floatCount(field.kind)});
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
            case FieldKind::Entity:
                out.writeEntity(name, *static_cast<const Entity*>(ptr));
                break;
            case FieldKind::EntityList:
                {
                    const auto& list = *static_cast<const std::vector<Entity>*>(ptr);
                    out.beginArray(name, static_cast<u32>(list.size()));
                    for (const Entity e : list)
                    {
                        out.writeEntity({}, e);
                    }
                    out.endArray();
                    break;
                }
            case FieldKind::Struct:
                out.beginObject(name);
                writeStruct(*field.structSchema, ptr, out, set);
                out.endObject();
                break;
            case FieldKind::StructList:
                {
                    const u32 count = field.listOps->count(ptr);
                    out.beginArray(name, count);
                    for (u32 i = 0; i < count; ++i)
                    {
                        out.beginObject({});
                        writeStruct(*field.structSchema, field.listOps->element(const_cast<void*>(ptr), i), out, set);
                        out.endObject();
                    }
                    out.endArray();
                    break;
                }
            }
        }

        void writeStruct(const StructSchema& schema, const void* instance, IFormatWriter& out, const FieldSet set)
        {
            for (const FieldDesc& field : schema.fields)
            {
                if (!isSelected(field, set))
                {
                    continue;
                }
                const u8* ptr = static_cast<const u8*>(instance) + field.offset;
                if (field.arrayCount == 0)
                {
                    writeElement(field, ptr, field.name, out, set);
                    continue;
                }
                out.beginArray(field.name, field.arrayCount);
                for (u32 i = 0; i < field.arrayCount; ++i)
                {
                    writeElement(field, ptr + i * field.elementSize, {}, out, set);
                }
                out.endArray();
            }
        }

        struct ReadContext
        {
            IFormatReader& in;
            std::string& outError;
            std::vector<FieldIssue>* issues; // tolerant when non-null
        };

        // Strict: fills outError and returns false. Tolerant: records the
        // issue under the field's dotted path and returns true (keep going).
        bool fail(ReadContext& ctx, const StructSchema& schema, const std::string_view prefix, const FieldDesc& field,
                  const std::string_view message)
        {
            if (ctx.issues != nullptr)
            {
                std::string path =
                    prefix.empty() ? std::string(field.name) : std::string(prefix) + "." + std::string(field.name);
                ctx.issues->push_back({std::move(path), std::string(message)});
                return true;
            }
            ctx.outError = std::string(schema.typeName) + "." + std::string(field.name) + ": " + std::string(message);
            return false;
        }

        bool readStruct(const StructSchema& schema, void* instance, ReadContext& ctx, std::string_view prefix);

        // False only on malformed input in strict mode (outError filled); an
        // Absent value keeps the current one.
        bool readElement(const StructSchema& schema, const FieldDesc& field, void* ptr, const std::string_view name,
                         ReadContext& ctx, const std::string_view prefix)
        {
            IFormatReader& in = ctx.in;
            FieldRead r = FieldRead::Ok;
            switch (field.kind)
            {
            case FieldKind::Bool:
                r = in.readBool(name, *static_cast<bool*>(ptr));
                break;
            case FieldKind::U8:
                r = in.readU8(name, *static_cast<u8*>(ptr));
                break;
            case FieldKind::U16:
                r = in.readU16(name, *static_cast<u16*>(ptr));
                break;
            case FieldKind::U32:
                r = in.readU32(name, *static_cast<u32*>(ptr));
                break;
            case FieldKind::U64:
                r = in.readU64(name, *static_cast<u64*>(ptr));
                break;
            case FieldKind::I32:
                r = in.readI32(name, *static_cast<i32*>(ptr));
                break;
            case FieldKind::F32:
                r = in.readF32(name, *static_cast<f32*>(ptr));
                break;
            case FieldKind::F64:
                r = in.readF64(name, *static_cast<double*>(ptr));
                break;
            case FieldKind::Vec2:
            case FieldKind::Vec3:
            case FieldKind::Vec4:
            case FieldKind::Quat:
            case FieldKind::Mat4:
                r = in.readFloats(name, {static_cast<f32*>(ptr), floatCount(field.kind)});
                break;
            case FieldKind::String:
                r = in.readString(name, *static_cast<std::string*>(ptr));
                break;
            case FieldKind::InternedString:
                {
                    std::string text;
                    r = in.readString(name, text);
                    if (r == FieldRead::Ok)
                    {
                        *static_cast<InternedString*>(ptr) =
                            text.empty() ? InternedString{} : InternedString(std::string_view(text));
                    }
                    break;
                }
            case FieldKind::Entity:
                r = in.readEntity(name, *static_cast<Entity*>(ptr));
                break;
            case FieldKind::EntityList:
                {
                    u32 count = 0;
                    r = in.beginArray(name, count);
                    if (r != FieldRead::Ok)
                    {
                        break;
                    }
                    // Grows per element read, so a corrupt count cannot force a huge allocation.
                    std::vector<Entity> list;
                    for (u32 i = 0; i < count; ++i)
                    {
                        Entity e;
                        if (in.readEntity({}, e) != FieldRead::Ok)
                        {
                            const std::string error(in.lastError());
                            in.endArray();
                            return fail(ctx, schema, prefix, field, error);
                        }
                        list.push_back(e);
                    }
                    in.endArray();
                    *static_cast<std::vector<Entity>*>(ptr) = std::move(list);
                    break;
                }
            case FieldKind::Struct:
                r = in.beginObject(name);
                if (r == FieldRead::Ok)
                {
                    // the nested path is only needed to name issues
                    const std::string nested = ctx.issues == nullptr ? std::string()
                        : prefix.empty()                             ? std::string(field.name)
                                         : std::string(prefix) + "." + std::string(field.name);
                    if (!readStruct(*field.structSchema, ptr, ctx, nested))
                    {
                        return false;
                    }
                    in.endObject();
                }
                break;
            case FieldKind::StructList:
                {
                    u32 count = 0;
                    r = in.beginArray(name, count);
                    if (r != FieldRead::Ok)
                    {
                        break;
                    }
                    const std::string nested = ctx.issues == nullptr ? std::string()
                        : prefix.empty()                             ? std::string(field.name)
                                         : std::string(prefix) + "." + std::string(field.name);
                    // Grows per element read, so a corrupt count cannot force a huge allocation.
                    field.listOps->resize(ptr, 0);
                    for (u32 i = 0; i < count; ++i)
                    {
                        field.listOps->resize(ptr, i + 1);
                        if (in.beginObject({}) != FieldRead::Ok)
                        {
                            const std::string error(in.lastError());
                            in.endArray();
                            return fail(ctx, schema, prefix, field, error);
                        }
                        if (!readStruct(*field.structSchema, field.listOps->element(ptr, i), ctx, nested))
                        {
                            return false;
                        }
                        in.endObject();
                    }
                    in.endArray();
                    break;
                }
            }
            if (r != FieldRead::Error)
            {
                return true;
            }
            const std::string_view error = in.lastError();
            if (!field.enumNames.empty() && error == "expected an integer")
            {
                std::string detail(error);
                detail += " (named values: ";
                for (size_t i = 0; i < field.enumNames.size(); ++i)
                {
                    if (i != 0)
                        detail += ", ";
                    detail += field.enumNames[i].name;
                    detail += "=" + std::to_string(field.enumNames[i].value);
                }
                detail += ")";
                return fail(ctx, schema, prefix, field, detail);
            }
            return fail(ctx, schema, prefix, field, error);
        }

        bool readStruct(const StructSchema& schema, void* instance, ReadContext& ctx, const std::string_view prefix)
        {
            IFormatReader& in = ctx.in;
            for (const FieldDesc& field : schema.fields)
            {
                if (!isSelected(field, FieldSet::Persisted))
                {
                    continue;
                }
                u8* ptr = static_cast<u8*>(instance) + field.offset;
                if (field.arrayCount == 0)
                {
                    if (!readElement(schema, field, ptr, field.name, ctx, prefix))
                    {
                        return false;
                    }
                    continue;
                }

                u32 count = 0;
                const FieldRead r = in.beginArray(field.name, count);
                if (r == FieldRead::Absent)
                {
                    continue;
                }
                if (r == FieldRead::Error)
                {
                    if (!fail(ctx, schema, prefix, field, in.lastError()))
                    {
                        return false;
                    }
                    continue;
                }
                if (count != field.arrayCount)
                {
                    in.endArray();
                    if (!fail(ctx, schema, prefix, field, "expected " + std::to_string(field.arrayCount) + " elements"))
                    {
                        return false;
                    }
                    continue;
                }
                for (u32 i = 0; i < count; ++i)
                {
                    if (!readElement(schema, field, ptr + i * field.elementSize, {}, ctx, prefix))
                    {
                        return false;
                    }
                }
                in.endArray();
            }
            return true;
        }
    } // namespace

    void writeValue(const StructSchema& schema, const void* instance, IFormatWriter& out, const FieldSet set)
    {
        writeStruct(schema, instance, out, set);
    }

    bool readValue(const StructSchema& schema, void* instance, IFormatReader& in, std::string& outError,
                   std::vector<FieldIssue>* outIssues)
    {
        outError.clear();
        ReadContext ctx{in, outError, outIssues};
        return readStruct(schema, instance, ctx, {});
    }

    u64 persistedLayoutHash(const StructSchema& schema)
    {
        constexpr u64 FNV_PRIME = 0x100000001b3ull;
        u64 hash = 0xcbf29ce484222325ull;
        const auto mix = [&hash](const void* data, const size_t size)
        {
            for (size_t i = 0; i < size; ++i)
            {
                hash = (hash ^ static_cast<const u8*>(data)[i]) * FNV_PRIME;
            }
        };
        const auto walk = [&mix](const auto& self, const StructSchema& current) -> void
        {
            for (const FieldDesc& field : current.fields)
            {
                if (!isSelected(field, FieldSet::Persisted))
                {
                    continue;
                }
                mix(field.name.data(), field.name.size());
                const u8 separator = 0;
                mix(&separator, 1);
                mix(&field.kind, sizeof(field.kind));
                mix(&field.arrayCount, sizeof(field.arrayCount));
                if (field.kind == FieldKind::Struct || field.kind == FieldKind::StructList)
                {
                    self(self, *field.structSchema);
                }
                mix(&separator, 1);
            }
        };
        walk(walk, schema);
        return hash;
    }
} // namespace spite
