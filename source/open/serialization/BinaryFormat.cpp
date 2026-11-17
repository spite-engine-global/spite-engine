#include "serialization/Serializer.hpp"

#include <cstring>

#include "base/Assert.hpp"
#include "serialization/FormatWalk.hpp"

namespace spite
{
    namespace
    {
        constexpr u8 BINARY_MAGIC[4] = {'S', 'P', 'S', 'B'};
        constexpr u32 HEADER_SIZE = 8; // magic + version

        // Positional backend: no names, no structure markers - the schema walk
        // is the contract. Strings are length-prefixed, no terminator. Entity
        // values pass through an optional remap map (missing -> undefined).
        class BinaryWriter final : public IFormatWriter
        {
        public:
            explicit BinaryWriter(std::vector<u8>& out) : m_out(out) {}

            void beginObject(std::string_view) override {}
            void endObject() override {}
            void endArray() override {}
            void beginArray(std::string_view, const u32 count) override { raw(&count, sizeof(count)); }
            void writeBool(std::string_view, const bool v) override { raw(&v, sizeof(v)); }
            void writeU8(std::string_view, const u8 v) override { raw(&v, sizeof(v)); }
            void writeU16(std::string_view, const u16 v) override { raw(&v, sizeof(v)); }
            void writeU32(std::string_view, const u32 v) override { raw(&v, sizeof(v)); }
            void writeU64(std::string_view, const u64 v) override { raw(&v, sizeof(v)); }
            void writeI32(std::string_view, const i32 v) override { raw(&v, sizeof(v)); }
            void writeF32(std::string_view, const f32 v) override { raw(&v, sizeof(v)); }
            void writeF64(std::string_view, const double v) override { raw(&v, sizeof(v)); }
            void writeFloats(std::string_view, const std::span<const f32> v) override { raw(v.data(), v.size_bytes()); }

            void writeString(std::string_view, const std::string_view v) override
            {
                const u32 len = static_cast<u32>(v.size());
                raw(&len, sizeof(len));
                raw(v.data(), v.size());
            }

            void writeEntity(std::string_view, const Entity v) override
            {
                const u64 id = v.id();
                raw(&id, sizeof(id));
            }

            void writeNull(std::string_view) override { SASSERTM(false, "The binary format has no null."); }

        private:
            void raw(const void* data, const size_t size)
            {
                const auto* bytes = static_cast<const u8*>(data);
                m_out.insert(m_out.end(), bytes, bytes + size);
            }

            std::vector<u8>& m_out;
        };

        class BinaryReader final : public IFormatReader
        {
        public:
            BinaryReader(const std::span<const u8> bytes, const std::unordered_map<u64, u64>* entityIdMap = nullptr,
                         u32* outDanglingCount = nullptr) :
                m_bytes(bytes), m_entityIdMap(entityIdMap), m_danglingCount(outDanglingCount)
            {
            }

            void endObject() override {}
            void endArray() override {}
            FieldRead beginObject(std::string_view) override { return FieldRead::Ok; }

            FieldRead beginArray(std::string_view, u32& outCount) override
            {
                if (!take(&outCount, sizeof(outCount)))
                {
                    return fail("truncated array count");
                }
                return FieldRead::Ok;
            }

            FieldRead readBool(std::string_view, bool& out) override
            {
                u8 raw = 0;
                if (!take(&raw, sizeof(raw)))
                {
                    return fail("truncated bool");
                }
                out = raw != 0;
                return FieldRead::Ok;
            }

            FieldRead readU8(std::string_view, u8& out) override
            {
                if (!take(&out, sizeof(out)))
                {
                    return fail("truncated u8");
                }
                return FieldRead::Ok;
            }

            FieldRead readU16(std::string_view, u16& out) override
            {
                if (!take(&out, sizeof(out)))
                {
                    return fail("truncated u16");
                }
                return FieldRead::Ok;
            }

            FieldRead readU32(std::string_view, u32& out) override
            {
                if (!take(&out, sizeof(out)))
                {
                    return fail("truncated u32");
                }
                return FieldRead::Ok;
            }

            FieldRead readU64(std::string_view, u64& out) override
            {
                if (!take(&out, sizeof(out)))
                {
                    return fail("truncated u64");
                }
                return FieldRead::Ok;
            }

            FieldRead readI32(std::string_view, i32& out) override
            {
                if (!take(&out, sizeof(out)))
                {
                    return fail("truncated i32");
                }
                return FieldRead::Ok;
            }

            FieldRead readF32(std::string_view, f32& out) override
            {
                if (!take(&out, sizeof(out)))
                {
                    return fail("truncated f32");
                }
                return FieldRead::Ok;
            }

            FieldRead readF64(std::string_view, double& out) override
            {
                if (!take(&out, sizeof(out)))
                {
                    return fail("truncated f64");
                }
                return FieldRead::Ok;
            }

            FieldRead readFloats(std::string_view, const std::span<f32> out) override
            {
                if (!take(out.data(), out.size_bytes()))
                {
                    return fail("truncated float vector");
                }
                return FieldRead::Ok;
            }

            FieldRead readString(std::string_view, std::string& out) override
            {
                u32 len = 0;
                if (!take(&len, sizeof(len)))
                {
                    return fail("truncated string length");
                }
                const u8* data = takeRef(len);
                if (!data)
                {
                    return fail("truncated string data");
                }
                out.assign(reinterpret_cast<const char*>(data), len);
                return FieldRead::Ok;
            }

            FieldRead readEntity(std::string_view, Entity& out) override
            {
                u64 id = 0;
                if (!take(&id, sizeof(id)))
                {
                    return fail("truncated entity");
                }
                if (m_entityIdMap != nullptr)
                {
                    const auto it = m_entityIdMap->find(id);
                    if (it != m_entityIdMap->end())
                    {
                        id = it->second;
                    }
                    else
                    {
                        id = 0; // dangling reference; apply reports it
                        if (m_danglingCount != nullptr)
                        {
                            ++*m_danglingCount;
                        }
                    }
                }
                out = Entity(id);
                return FieldRead::Ok;
            }

            [[nodiscard]] std::string_view lastError() const override { return m_error; }

        private:
            [[nodiscard]] size_t remaining() const { return m_bytes.size() - m_pos; }

            bool take(void* dst, const size_t size)
            {
                if (size > remaining())
                {
                    return false;
                }
                std::memcpy(dst, m_bytes.data() + m_pos, size);
                m_pos += size;
                return true;
            }

            const u8* takeRef(const size_t size)
            {
                if (size > remaining())
                {
                    return nullptr;
                }
                const u8* data = m_bytes.data() + m_pos;
                m_pos += size;
                return data;
            }

            FieldRead fail(const char* message)
            {
                m_error = message;
                return FieldRead::Error;
            }

            std::span<const u8> m_bytes;
            size_t m_pos = 0;
            std::string m_error;
            const std::unordered_map<u64, u64>* m_entityIdMap = nullptr;
            u32* m_danglingCount = nullptr;
        };
    } // namespace

    void serializeBinary(const StructSchema& schema, const void* instance, std::vector<u8>& outBytes)
    {
        outBytes.clear();
        outBytes.insert(outBytes.end(), BINARY_MAGIC, BINARY_MAGIC + sizeof(BINARY_MAGIC));
        const u32 version = SERIALIZED_BINARY_VERSION;
        const auto* versionBytes = reinterpret_cast<const u8*>(&version);
        outBytes.insert(outBytes.end(), versionBytes, versionBytes + sizeof(version));

        BinaryWriter writer(outBytes);
        writeValue(schema, instance, writer);
    }

    bool deserializeBinary(const StructSchema& schema, void* instance, const std::span<const u8> bytes,
                           std::string& outError)
    {
        outError.clear();
        if (bytes.size() < HEADER_SIZE)
        {
            outError = "binary data shorter than the 8-byte header";
            return false;
        }
        if (std::memcmp(bytes.data(), BINARY_MAGIC, sizeof(BINARY_MAGIC)) != 0)
        {
            outError = "bad binary magic";
            return false;
        }
        u32 version = 0;
        std::memcpy(&version, bytes.data() + sizeof(BINARY_MAGIC), sizeof(version));
        if (version != SERIALIZED_BINARY_VERSION)
        {
            outError = "unsupported binary format version";
            return false;
        }

        BinaryReader reader(bytes.subspan(HEADER_SIZE));
        return readValue(schema, instance, reader, outError);
    }

    void serializeFields(const StructSchema& schema, const void* instance, std::vector<u8>& outBytes)
    {
        outBytes.clear();
        BinaryWriter writer(outBytes);
        writeValue(schema, instance, writer);
    }

    bool deserializeFields(const StructSchema& schema, void* instance, const std::span<const u8> bytes,
                           std::string& outError, const std::unordered_map<u64, u64>* entityIdMap,
                           u32* outDanglingCount)
    {
        outError.clear();
        BinaryReader reader(bytes, entityIdMap, outDanglingCount);
        return readValue(schema, instance, reader, outError);
    }
} // namespace spite
