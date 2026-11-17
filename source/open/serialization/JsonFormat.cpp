#include "serialization/JsonFormat.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <nlohmann/json.hpp>

#include "base/Assert.hpp"
#include "serialization/Serializer.hpp"

namespace spite
{
    // Self-describing projection: objects keyed by field name, arrays sized by
    // their payload. Stacked node pointers stay valid: an array only grows
    // after its previous element was closed and popped.
    struct JsonFragmentWriter::Impl
    {
        nlohmann::json root = nlohmann::json::object();
        std::vector<nlohmann::json*> stack{&root};

        void beginObject(const std::string_view name)
        {
            nlohmann::json& current = *stack.back();
            if (current.is_array())
            {
                current.push_back(nlohmann::json::object());
                stack.push_back(&current.back());
            }
            else
            {
                stack.push_back(&current[std::string(name)]);
                *stack.back() = nlohmann::json::object();
            }
        }

        void endObject() { stack.pop_back(); }

        void beginArray(const std::string_view name, u32)
        {
            nlohmann::json& current = *stack.back();
            if (current.is_array())
            {
                current.push_back(nlohmann::json::array());
                stack.push_back(&current.back());
            }
            else
            {
                stack.push_back(&current[std::string(name)]);
                *stack.back() = nlohmann::json::array();
            }
        }

        void endArray() { stack.pop_back(); }

        void writeBool(const std::string_view name, const bool v) { add(name, nlohmann::json(v)); }
        void writeU8(const std::string_view name, const u8 v) { add(name, nlohmann::json(static_cast<u32>(v))); }
        void writeU16(const std::string_view name, const u16 v) { add(name, nlohmann::json(static_cast<u32>(v))); }
        void writeU32(const std::string_view name, const u32 v) { add(name, nlohmann::json(v)); }
        void writeU64(const std::string_view name, const u64 v) { add(name, nlohmann::json(v)); }
        void writeI32(const std::string_view name, const i32 v) { add(name, nlohmann::json(v)); }
        void writeF32(const std::string_view name, const f32 v) { add(name, nlohmann::json(static_cast<double>(v))); }
        void writeF64(const std::string_view name, const double v) { add(name, nlohmann::json(v)); }

        void writeFloats(const std::string_view name, const std::span<const f32> v)
        {
            nlohmann::json array = nlohmann::json::array();
            for (const f32 component : v)
            {
                array.push_back(static_cast<double>(component));
            }
            add(name, std::move(array));
        }

        void writeString(const std::string_view name, const std::string_view v) { add(name, nlohmann::json(v)); }

        void writeEntity(const std::string_view name, const Entity v) { add(name, nlohmann::json(v.id())); }

        void add(const std::string_view name, nlohmann::json&& value)
        {
            nlohmann::json& current = *stack.back();
            if (current.is_array())
            {
                current.push_back(std::move(value));
            }
            else
            {
                current[std::string(name)] = std::move(value);
            }
        }
    };

    JsonFragmentWriter::JsonFragmentWriter() : m_impl(std::make_unique<Impl>()) {}
    JsonFragmentWriter::~JsonFragmentWriter() = default;

    void JsonFragmentWriter::beginObject(const std::string_view name) { m_impl->beginObject(name); }
    void JsonFragmentWriter::endObject() { m_impl->endObject(); }
    void JsonFragmentWriter::beginArray(const std::string_view name, const u32 count)
    {
        m_impl->beginArray(name, count);
    }
    void JsonFragmentWriter::endArray() { m_impl->endArray(); }
    void JsonFragmentWriter::writeBool(const std::string_view name, const bool v) { m_impl->writeBool(name, v); }
    void JsonFragmentWriter::writeU8(const std::string_view name, const u8 v) { m_impl->writeU8(name, v); }
    void JsonFragmentWriter::writeU16(const std::string_view name, const u16 v) { m_impl->writeU16(name, v); }
    void JsonFragmentWriter::writeU32(const std::string_view name, const u32 v) { m_impl->writeU32(name, v); }
    void JsonFragmentWriter::writeU64(const std::string_view name, const u64 v) { m_impl->writeU64(name, v); }
    void JsonFragmentWriter::writeI32(const std::string_view name, const i32 v) { m_impl->writeI32(name, v); }
    void JsonFragmentWriter::writeF32(const std::string_view name, const f32 v) { m_impl->writeF32(name, v); }
    void JsonFragmentWriter::writeF64(const std::string_view name, const double v) { m_impl->writeF64(name, v); }
    void JsonFragmentWriter::writeFloats(const std::string_view name, const std::span<const f32> v)
    {
        m_impl->writeFloats(name, v);
    }
    void JsonFragmentWriter::writeString(const std::string_view name, const std::string_view v)
    {
        m_impl->writeString(name, v);
    }
    void JsonFragmentWriter::writeEntity(const std::string_view name, const Entity v) { m_impl->writeEntity(name, v); }
    void JsonFragmentWriter::writeNull(const std::string_view name) { m_impl->add(name, nlohmann::json()); }

    void JsonFragmentWriter::dump(std::string& outText, const int indent) const { outText = m_impl->root.dump(indent); }

    void JsonFragmentWriter::dumpReply(std::string& outText) const
    {
        outText = m_impl->root.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    struct JsonFragmentReader::Impl
    {
        explicit Impl(const std::string_view text)
        {
            const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
            if (parsed.is_discarded() || !parsed.is_object())
            {
                m_parseFailed = true;
                m_error = "invalid JSON fragment";
                return;
            }
            m_root = std::make_unique<nlohmann::json>(std::move(parsed));
            m_stack.push_back(m_root.get());
        }

        void endObject() { pop(); }
        void endArray()
        {
            m_arrayPositions.pop_back();
            pop();
        }

        FieldRead beginObject(const std::string_view name)
        {
            const nlohmann::json* found = locate(name);
            if (!found)
            {
                return FieldRead::Absent;
            }
            if (!found->is_object())
            {
                return fail("expected an object");
            }
            m_stack.push_back(found);
            return FieldRead::Ok;
        }

        FieldRead beginArray(const std::string_view name, u32& outCount)
        {
            const nlohmann::json* found = locate(name);
            if (!found)
            {
                return FieldRead::Absent;
            }
            if (!found->is_array())
            {
                return fail("expected an array");
            }
            m_stack.push_back(found);
            m_arrayPositions.push_back(0);
            outCount = static_cast<u32>(found->size());
            return FieldRead::Ok;
        }

        FieldRead readBool(const std::string_view name, bool& out)
        {
            const nlohmann::json* v = locate(name);
            if (!v)
            {
                return FieldRead::Absent;
            }
            if (!v->is_boolean())
            {
                return fail("expected a bool");
            }
            out = v->get<bool>();
            return FieldRead::Ok;
        }

        FieldRead readU8(const std::string_view name, u8& out)
        {
            u32 t = 0;
            const FieldRead r = readUnsigned(name, 0xFFu, t);
            if (r == FieldRead::Ok)
                out = static_cast<u8>(t);
            return r;
        }

        FieldRead readU16(const std::string_view name, u16& out)
        {
            u32 t = 0;
            const FieldRead r = readUnsigned(name, 0xFFFFu, t);
            if (r == FieldRead::Ok)
                out = static_cast<u16>(t);
            return r;
        }

        FieldRead readU32(const std::string_view name, u32& out)
        {
            u32 t = 0;
            const FieldRead r = readUnsigned(name, 0xFFFFFFFFu, t);
            if (r == FieldRead::Ok)
                out = t;
            return r;
        }

        FieldRead readU64(const std::string_view name, u64& out) { return readUnsigned(name, ~0ull, out); }

        FieldRead readI32(const std::string_view name, i32& out)
        {
            const nlohmann::json* v = locate(name);
            if (!v)
            {
                return FieldRead::Absent;
            }
            if (!v->is_number_integer())
            {
                return fail("expected an integer");
            }
            if (v->is_number_unsigned() ? v->get<u64>() > static_cast<u64>(INT32_MAX)
                                        : (v->get<std::int64_t>() < INT32_MIN || v->get<std::int64_t>() > INT32_MAX))
            {
                return fail("integer out of range");
            }
            out = static_cast<i32>(v->get<std::int64_t>());
            return FieldRead::Ok;
        }

        FieldRead readF32(const std::string_view name, f32& out)
        {
            double t = 0.0;
            const FieldRead r = readF64(name, t);
            if (r != FieldRead::Ok)
                return r;
            if (!std::isfinite(static_cast<f32>(t)))
                return fail("number does not fit a 32-bit float");
            out = static_cast<f32>(t);
            return r;
        }

        FieldRead readF64(const std::string_view name, double& out)
        {
            const nlohmann::json* v = locate(name);
            if (!v)
            {
                return FieldRead::Absent;
            }
            if (!v->is_number())
            {
                return fail("expected a number");
            }
            out = v->get<double>();
            if (!std::isfinite(out))
                return fail("number is not finite");
            return FieldRead::Ok;
        }

        FieldRead readFloats(const std::string_view name, const std::span<f32> out)
        {
            const nlohmann::json* v = locate(name);
            if (!v)
            {
                return FieldRead::Absent;
            }
            if (!v->is_array() || v->size() != out.size() ||
                !std::ranges::all_of(*v, [](const nlohmann::json& e) { return e.is_number(); }))
            {
                m_error = "expected a " + std::to_string(out.size()) + "-element number array";
                return FieldRead::Error;
            }
            for (size_t i = 0; i < out.size(); ++i)
            {
                out[i] = static_cast<f32>((*v)[i].get<double>());
                if (!std::isfinite(out[i]))
                {
                    m_error = "number does not fit a 32-bit float";
                    return FieldRead::Error;
                }
            }
            return FieldRead::Ok;
        }

        FieldRead readString(const std::string_view name, std::string& out)
        {
            const nlohmann::json* v = locate(name);
            if (!v)
            {
                return FieldRead::Absent;
            }
            if (!v->is_string())
            {
                return fail("expected a string");
            }
            out = v->get<std::string>();
            return FieldRead::Ok;
        }

        FieldRead readEntity(const std::string_view name, Entity& out)
        {
            u64 t = 0;
            const FieldRead r = readUnsigned(name, ~0ull, t);
            if (r == FieldRead::Ok)
                out = Entity(t);
            return r;
        }

        [[nodiscard]] std::string_view lastError() const { return m_error; }
        [[nodiscard]] bool valid() const { return !m_parseFailed; }

    private:
        // Resolves a field by name, or positionally when the current node
        // is an array (array elements carry an empty name).
        const nlohmann::json* locate(const std::string_view name)
        {
            if (m_parseFailed)
            {
                return nullptr;
            }
            const nlohmann::json& current = *m_stack.back();
            if (current.is_array())
            {
                size_t& pos = m_arrayPositions.back();
                if (pos >= current.size())
                {
                    return nullptr;
                }
                return &current[pos++];
            }
            if (name.empty())
            {
                return nullptr;
            }
            const auto it = current.find(std::string(name));
            return it == current.end() ? nullptr : &it.value();
        }

        template <typename T>
        FieldRead readUnsigned(const std::string_view name, const u64 maxValue, T& out)
        {
            const nlohmann::json* v = locate(name);
            if (!v)
            {
                return FieldRead::Absent;
            }
            u64 t = 0;
            if (v->is_number_unsigned())
            {
                t = v->get<u64>();
            }
            else if (v->is_number_integer())
            {
                const std::int64_t signedValue = v->get<std::int64_t>();
                if (signedValue < 0)
                {
                    return fail("negative value for an unsigned field");
                }
                t = static_cast<u64>(signedValue);
            }
            else
            {
                return fail("expected an integer");
            }
            if (t > maxValue)
            {
                return fail("integer out of range");
            }
            out = static_cast<T>(t);
            return FieldRead::Ok;
        }

        void pop()
        {
            SASSERT(m_stack.size() > 1);
            m_stack.pop_back();
        }

        FieldRead fail(const char* message)
        {
            m_error = message;
            return FieldRead::Error;
        }

        std::unique_ptr<nlohmann::json> m_root;
        std::vector<const nlohmann::json*> m_stack;
        std::vector<size_t> m_arrayPositions;
        std::string m_error;
        bool m_parseFailed = false;
    };

    JsonFragmentReader::JsonFragmentReader(const std::string_view text) : m_impl(std::make_unique<Impl>(text)) {}
    JsonFragmentReader::~JsonFragmentReader() = default;

    FieldRead JsonFragmentReader::beginObject(const std::string_view name) { return m_impl->beginObject(name); }
    void JsonFragmentReader::endObject() { m_impl->endObject(); }
    FieldRead JsonFragmentReader::beginArray(const std::string_view name, u32& outCount)
    {
        return m_impl->beginArray(name, outCount);
    }
    void JsonFragmentReader::endArray() { m_impl->endArray(); }
    FieldRead JsonFragmentReader::readBool(const std::string_view name, bool& out)
    {
        return m_impl->readBool(name, out);
    }
    FieldRead JsonFragmentReader::readU8(const std::string_view name, u8& out) { return m_impl->readU8(name, out); }
    FieldRead JsonFragmentReader::readU16(const std::string_view name, u16& out) { return m_impl->readU16(name, out); }
    FieldRead JsonFragmentReader::readU32(const std::string_view name, u32& out) { return m_impl->readU32(name, out); }
    FieldRead JsonFragmentReader::readU64(const std::string_view name, u64& out) { return m_impl->readU64(name, out); }
    FieldRead JsonFragmentReader::readI32(const std::string_view name, i32& out) { return m_impl->readI32(name, out); }
    FieldRead JsonFragmentReader::readF32(const std::string_view name, f32& out) { return m_impl->readF32(name, out); }
    FieldRead JsonFragmentReader::readF64(const std::string_view name, double& out)
    {
        return m_impl->readF64(name, out);
    }
    FieldRead JsonFragmentReader::readFloats(const std::string_view name, const std::span<f32> out)
    {
        return m_impl->readFloats(name, out);
    }
    FieldRead JsonFragmentReader::readString(const std::string_view name, std::string& out)
    {
        return m_impl->readString(name, out);
    }
    FieldRead JsonFragmentReader::readEntity(const std::string_view name, Entity& out)
    {
        return m_impl->readEntity(name, out);
    }
    std::string_view JsonFragmentReader::lastError() const { return m_impl->lastError(); }
    bool JsonFragmentReader::valid() const { return m_impl->valid(); }

    void serializeJson(const StructSchema& schema, const void* instance, std::string& outText, const int indent)
    {
        JsonFragmentWriter writer;
        writeValue(schema, instance, writer);
        writer.dump(outText, indent);
    }

    bool deserializeJson(const StructSchema& schema, void* instance, const std::string_view text, std::string& outError,
                         std::vector<FieldIssue>* outIssues)
    {
        outError.clear();
        JsonFragmentReader reader(text);
        if (!reader.valid())
        {
            // the cold path: parse again to say which of the two it was
            const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
            outError = parsed.is_discarded() ? "invalid JSON" : "expected a JSON object at the root";
            return false;
        }
        return readValue(schema, instance, reader, outError, outIssues);
    }
} // namespace spite
