#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "FormatWalk.hpp"
#include "SerializationExport.hpp"

namespace spite
{
    // JSON projection of the field walk rooted at one object - the component
    // payload form of scene JSON files. The writer emits the fields of one
    // object (the caller brackets nothing; the walk opens nested objects
    // itself). nlohmann stays confined to the implementation.
    class SPITE_SERIALIZATION_API JsonFragmentWriter final : public IFormatWriter
    {
    public:
        JsonFragmentWriter();
        ~JsonFragmentWriter() override;

        void beginObject(std::string_view name) override;
        void endObject() override;
        void beginArray(std::string_view name, u32 count) override;
        void endArray() override;
        void writeBool(std::string_view name, bool v) override;
        void writeU8(std::string_view name, u8 v) override;
        void writeU16(std::string_view name, u16 v) override;
        void writeU32(std::string_view name, u32 v) override;
        void writeU64(std::string_view name, u64 v) override;
        void writeI32(std::string_view name, i32 v) override;
        void writeF32(std::string_view name, f32 v) override;
        void writeF64(std::string_view name, double v) override;
        void writeFloats(std::string_view name, std::span<const f32> v) override;
        void writeString(std::string_view name, std::string_view v) override;
        void writeEntity(std::string_view name, Entity v) override;
        void writeNull(std::string_view name) override;

        // indent < 0: one line; otherwise that many spaces per level, one member per line (keys sorted)
        void dump(std::string& outText, int indent = -1) const;
        // One line for a control reply: an invalid UTF-8 byte becomes U+FFFD where dump throws. Never for a file
        void dumpReply(std::string& outText) const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    class SPITE_SERIALIZATION_API JsonFragmentReader final : public IFormatReader
    {
    public:
        // Parses text as one JSON object; a failed parse surfaces as
        // FieldRead::Error on every read.
        explicit JsonFragmentReader(std::string_view text);
        ~JsonFragmentReader() override;

        FieldRead beginObject(std::string_view name) override;
        void endObject() override;
        FieldRead beginArray(std::string_view name, u32& outCount) override;
        void endArray() override;
        FieldRead readBool(std::string_view name, bool& out) override;
        FieldRead readU8(std::string_view name, u8& out) override;
        FieldRead readU16(std::string_view name, u16& out) override;
        FieldRead readU32(std::string_view name, u32& out) override;
        FieldRead readU64(std::string_view name, u64& out) override;
        FieldRead readI32(std::string_view name, i32& out) override;
        FieldRead readF32(std::string_view name, f32& out) override;
        FieldRead readF64(std::string_view name, double& out) override;
        FieldRead readFloats(std::string_view name, std::span<f32> out) override;
        FieldRead readString(std::string_view name, std::string& out) override;
        FieldRead readEntity(std::string_view name, Entity& out) override;
        [[nodiscard]] std::string_view lastError() const override;
        // false when the text was not one JSON object
        [[nodiscard]] bool valid() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };
} // namespace spite
