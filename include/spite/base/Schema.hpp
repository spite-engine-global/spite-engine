#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "base/InternedString.hpp"
#include "base/MathTypes.hpp"
#include "base/Platform.hpp"

namespace spite
{
    // Closed lattice of value kinds every schema backend implements. Types that
    // are not values (pointers, handles into runtime state) stay out of a schema;
    // the systems owning them rebuild that state.
    enum class FieldKind : u8
    {
        Bool,
        U8,
        U16,
        U32,
        U64,
        I32,
        F32,
        F64,
        Vec2,
        Vec3,
        Vec4,
        Quat,
        Mat4,
        String, // std::string
        InternedString, // carried as its text
        Entity, // ecs Entity; its trait lives in ecs/EcsTypes.hpp
        EntityList, // std::vector<Entity>
        Struct, // nested aggregate described by its own schema
        StructList, // std::vector<T> of a type with its own schema; serializers only, never a component field
    };

    namespace fieldFlags
    {
        constexpr u32 PERSISTED = 1u << 0; // saved in scenes, inspectable
        constexpr u32 OBSERVED = 1u << 1; // inspectable, never saved
        constexpr u32 READONLY = 1u << 2; // tools read it but never write it
        constexpr u32 COLOR =
            1u << 3; // a Vec3 (rgb) or Vec4 (rgba) value edited as a colour; the numbers stay as stored
    } // namespace fieldFlags

    struct StructSchema;

    // One named value of an enum field: what a tool shows in place of the underlying integer. Enums of up to 32 bits;
    // a value is negative only for an i32 enum.
    struct EnumName
    {
        std::int64_t value = 0;
        std::string_view name;
    };

    // Type-erased std::vector<T> access for StructList.
    struct StructListOps
    {
        u32 (*count)(const void* list);
        void (*resize)(void* list, u32 count);
        void* (*element)(void* list, u32 index);
    };

    struct FieldDesc
    {
        std::string_view name;
        u32 offset = 0;
        FieldKind kind = FieldKind::Bool;
        u32 flags = fieldFlags::PERSISTED;
        u32 arrayCount = 0; // 0: scalar; N: fixed array of N elements elementSize bytes apart
        u32 elementSize = 0;
        const StructSchema* structSchema = nullptr; // Struct; StructList: the element's
        const StructListOps* listOps = nullptr; // StructList
        // An enum field whose type has SPITE_ENUM_NAMES: its named values; empty: the integer only. A stored value
        // outside the list is legal (shown and kept as it is).
        std::span<const EnumName> enumNames;
        // Tools' metadata of a number field (fieldUnit, fieldRange, fieldStep): the unit shown after the value ("m/s"),
        // the bounds a `set` refuses a number outside of (each lane of a vector, each element of an array) and the
        // step a slider moves by (0: none). A stored value outside the range (a scene, code) is kept as it is.
        std::string_view unit;
        double min = -std::numeric_limits<double>::infinity();
        double max = std::numeric_limits<double>::infinity();
        double step = 0.0;
        std::string_view assetType; // optional asset kind token for tools
    };

    // Both bounds finite and apart: a tool may draw the field as a slider.
    [[nodiscard]] constexpr bool hasFiniteRange(const FieldDesc& field)
    {
        return field.min < field.max && field.min > -std::numeric_limits<double>::infinity() &&
            field.max < std::numeric_limits<double>::infinity();
    }

    // What SPITE_FIELD declares: flags and the field's metadata, combined with |
    // (SPITE_FIELD(speed, fieldRange(0.0, 10.0) | fieldStep(0.05) | fieldUnit("m/s") | READONLY)).
    struct FieldOptions
    {
        u32 flags = 0;
        std::string_view unit{};
        double min = -std::numeric_limits<double>::infinity();
        double max = std::numeric_limits<double>::infinity();
        double step = 0.0;
        std::string_view assetType;

        constexpr FieldOptions() = default;
        constexpr FieldOptions(const u32 bits) : flags(bits) {} // NOLINT(google-explicit-constructor): a flag mask
    };

    // The right side's declared parts win.
    [[nodiscard]] constexpr FieldOptions operator|(FieldOptions a, const FieldOptions& b)
    {
        a.flags |= b.flags;
        a.unit = b.unit.empty() ? a.unit : b.unit;
        if (b.min > -std::numeric_limits<double>::infinity() || b.max < std::numeric_limits<double>::infinity())
        {
            a.min = b.min;
            a.max = b.max;
        }
        a.step = b.step > 0.0 ? b.step : a.step;
        a.assetType = b.assetType.empty() ? a.assetType : b.assetType;
        return a;
    }

    // In fieldFlags, which SPITE_SCHEMA's body brings into scope beside the flags.
    namespace fieldFlags
    {
        constexpr FieldOptions fieldAssetType(const std::string_view type)
        {
            FieldOptions options;
            options.assetType = type;
            return options;
        }
        // A static string: the schema keeps the view.
        constexpr FieldOptions fieldUnit(const std::string_view unit)
        {
            FieldOptions options;
            options.unit = unit;
            return options;
        }

        // Inclusive; an infinite bound leaves that side open. A slider needs both finite and min < max.
        constexpr FieldOptions fieldRange(const double min, const double max)
        {
            FieldOptions options;
            options.min = min;
            options.max = max;
            return options;
        }

        constexpr FieldOptions fieldStep(const double step)
        {
            FieldOptions options;
            options.step = step;
            return options;
        }
    } // namespace fieldFlags

    // The runtime description of a type: what makes it observable.
    // Declared once in the type's header with SPITE_SCHEMA.
    struct StructSchema
    {
        std::string_view typeName; // diagnostics only; components are named by the ecs registry
        u32 sizeOfType = 0;
        std::span<const FieldDesc> fields;
        // A component with no fields whose presence is itself scene data (SPITE_PERSISTED_TAG_SCHEMA).
        bool persistedTag = false;
    };

    // The scene-data test: true for a persisted field and also for a persisted tag (no fields).
    [[nodiscard]] constexpr bool hasPersistedFields(const StructSchema& schema)
    {
        if (schema.persistedTag)
        {
            return true;
        }
        for (const FieldDesc& field : schema.fields)
        {
            if (field.flags & fieldFlags::PERSISTED)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] inline bool hasStructList(const StructSchema& schema)
    {
        for (const FieldDesc& field : schema.fields)
        {
            if (field.kind == FieldKind::StructList ||
                (field.kind == FieldKind::Struct && hasStructList(*field.structSchema)))
            {
                return true;
            }
        }
        return false;
    }

    // Maps a C++ field type to its kind. Specialize it to admit another value
    // type; a type with its own SPITE_SCHEMA is a nested Struct automatically.
    template <class T>
    struct SchemaFieldTraits
    {
        static constexpr bool supported = false;
    };

    namespace detail
    {
        template <FieldKind K>
        struct SchemaKind
        {
            static constexpr bool supported = true;
            static constexpr FieldKind kind = K;
        };
    } // namespace detail

    // clang-format off
    template <> struct SchemaFieldTraits<bool> : detail::SchemaKind<FieldKind::Bool> {};
    template <> struct SchemaFieldTraits<u8> : detail::SchemaKind<FieldKind::U8> {};
    template <> struct SchemaFieldTraits<u16> : detail::SchemaKind<FieldKind::U16> {};
    template <> struct SchemaFieldTraits<u32> : detail::SchemaKind<FieldKind::U32> {};
    template <> struct SchemaFieldTraits<u64> : detail::SchemaKind<FieldKind::U64> {};
    template <> struct SchemaFieldTraits<i32> : detail::SchemaKind<FieldKind::I32> {};
    template <> struct SchemaFieldTraits<f32> : detail::SchemaKind<FieldKind::F32> {};
    template <> struct SchemaFieldTraits<double> : detail::SchemaKind<FieldKind::F64> {};
    template <> struct SchemaFieldTraits<Vec2> : detail::SchemaKind<FieldKind::Vec2> {};
    template <> struct SchemaFieldTraits<Vec3> : detail::SchemaKind<FieldKind::Vec3> {};
    template <> struct SchemaFieldTraits<Vec4> : detail::SchemaKind<FieldKind::Vec4> {};
    template <> struct SchemaFieldTraits<Quat> : detail::SchemaKind<FieldKind::Quat> {};
    template <> struct SchemaFieldTraits<Mat4> : detail::SchemaKind<FieldKind::Mat4> {};
    template <> struct SchemaFieldTraits<std::string> : detail::SchemaKind<FieldKind::String> {};
    template <> struct SchemaFieldTraits<InternedString> : detail::SchemaKind<FieldKind::InternedString> {};
    // clang-format on

    // Enums carry their underlying integer.
    template <class T>
        requires std::is_enum_v<T>
    struct SchemaFieldTraits<T> : SchemaFieldTraits<std::underlying_type_t<T>>
    {
    };

    template <class T>
        requires requires {
            { T::spiteSchema() } -> std::same_as<const StructSchema&>;
        }
    struct SchemaFieldTraits<T> : detail::SchemaKind<FieldKind::Struct>
    {
    };

    template <class T>
        requires requires {
            { T::spiteSchema() } -> std::same_as<const StructSchema&>;
        }
    struct SchemaFieldTraits<std::vector<T>> : detail::SchemaKind<FieldKind::StructList>
    {
    };

    namespace detail
    {
        template <class V>
        constexpr StructListOps STRUCT_LIST_OPS{
            [](const void* list) { return static_cast<u32>(static_cast<const V*>(list)->size()); },
            [](void* list, const u32 count) { static_cast<V*>(list)->resize(count); },
            [](void* list, const u32 index) -> void* { return &(*static_cast<V*>(list))[index]; }};

        template <class T>
        struct SchemaArray
        {
            using Element = T;
            static constexpr u32 COUNT = 0;
        };

        template <class T, sizet N>
        struct SchemaArray<T[N]>
        {
            using Element = T;
            static constexpr u32 COUNT = static_cast<u32>(N);
        };

        template <class T, sizet N>
        struct SchemaArray<std::array<T, N>>
        {
            using Element = T;
            static constexpr u32 COUNT = static_cast<u32>(N);
        };

        // Member names follow the m_ convention; the schema names the value.
        constexpr std::string_view schemaFieldName(std::string_view member)
        {
            return member.starts_with("m_") ? member.substr(2) : member;
        }

        template <class Member>
        FieldDesc makeSchemaField(const std::string_view member, const sizet offset, const FieldOptions& options,
                                  const u32 schemaDefault)
        {
            const u32 flags = options.flags;
            using Element = typename SchemaArray<Member>::Element;
            static_assert(SchemaFieldTraits<Element>::supported,
                          "SPITE_SCHEMA: unsupported field type. Use a supported kind, give the type its own "
                          "SPITE_SCHEMA, specialize spite::SchemaFieldTraits, or leave the field out.");

            FieldDesc field;
            field.name = schemaFieldName(member);
            field.offset = static_cast<u32>(offset);
            field.kind = SchemaFieldTraits<Element>::kind;
            constexpr u32 persistence = fieldFlags::PERSISTED | fieldFlags::OBSERVED;
            field.flags = (flags & persistence) != 0 ? flags : flags | schemaDefault;
            field.arrayCount = SchemaArray<Member>::COUNT;
            field.elementSize = static_cast<u32>(sizeof(Element));
            field.unit = options.unit;
            field.min = options.min;
            field.max = options.max;
            field.step = options.step;
            field.assetType = options.assetType;
            if constexpr (std::is_enum_v<Element> && requires { spiteEnumNames(Element{}); })
            {
                static_assert(sizeof(Element) <= 4, "SPITE_ENUM_NAMES: an enum of more than 32 bits has no names");
                field.enumNames = spiteEnumNames(Element{});
            }
            if constexpr (SchemaFieldTraits<Element>::kind == FieldKind::Struct)
            {
                field.structSchema = &Element::spiteSchema();
            }
            if constexpr (SchemaFieldTraits<Element>::kind == FieldKind::StructList)
            {
                field.structSchema = &Element::value_type::spiteSchema();
                field.listOps = &STRUCT_LIST_OPS<Element>;
            }
            return field;
        }
    } // namespace detail
} // namespace spite

// SPITE_SCHEMA(Type, a, b, SPITE_FIELD(c, OBSERVED | READONLY)) inside Type's
// body declares `static const StructSchema& spiteSchema()`. Fields are listed
// explicitly; kinds come from SchemaFieldTraits. SPITE_FIELD's second argument is
// flags and/or metadata joined by | (fieldUnit, fieldRange, fieldStep: see
// FieldOptions). SPITE_SCHEMA fields default to
// PERSISTED, SPITE_OBSERVED_SCHEMA fields (stats, events, derived state) to
// OBSERVED. Works under both the traditional and the conforming preprocessor.
#define SPITE_SCHEMA(...) SPITE_DETAIL_EXPAND(SPITE_DETAIL_SCHEMA(::spite::fieldFlags::PERSISTED, false, __VA_ARGS__))
#define SPITE_OBSERVED_SCHEMA(...)                                                                                     \
    SPITE_DETAIL_EXPAND(SPITE_DETAIL_SCHEMA(::spite::fieldFlags::OBSERVED, false, __VA_ARGS__))
// A fieldless component that is saved in scenes and copied to the Play clone by its presence alone.
#define SPITE_PERSISTED_TAG_SCHEMA(Type)                                                                               \
    SPITE_DETAIL_EXPAND(SPITE_DETAIL_SCHEMA(::spite::fieldFlags::PERSISTED, true, Type))
#define SPITE_FIELD(name, flags) (name, flags)
// Names an enum's values for tools (the Inspector's dropdown, the `inspector` query): SPITE_ENUM_NAMES(Mode, Off, Low)
// at namespace scope after the enum, in the enum's namespace, or SPITE_ENUM_NAMES_IN_CLASS(Mode, Off, Low) in the
// class that declares it (before its SPITE_SCHEMA). It defines the function the schema finds by argument-dependent
// lookup; up to 32 names. Two names of one value: tools show the first. A flags enum (a combination of bits) reads
// "N (unknown)" for any combination the names do not list.
#define SPITE_ENUM_NAMES(Type, ...)                                                                                    \
    inline std::span<const ::spite::EnumName> spiteEnumNames(Type)                                                     \
    {                                                                                                                  \
        SPITE_DETAIL_EXPAND(SPITE_DETAIL_ENUM_NAME_LIST(Type, __VA_ARGS__))                                            \
    }
#define SPITE_ENUM_NAMES_IN_CLASS(Type, ...)                                                                           \
    friend std::span<const ::spite::EnumName> spiteEnumNames(Type)                                                     \
    {                                                                                                                  \
        SPITE_DETAIL_EXPAND(SPITE_DETAIL_ENUM_NAME_LIST(Type, __VA_ARGS__))                                            \
    }
#define SPITE_DETAIL_ENUM_NAME_LIST(Type, ...)                                                                         \
    using SpiteEnumType = Type;                                                                                        \
    static constexpr ::spite::EnumName names[] = {SPITE_DETAIL_FOR_EACH(SPITE_DETAIL_ENUM_NAME, Type, __VA_ARGS__)};   \
    return names;
#define SPITE_DETAIL_ENUM_NAME(name) {static_cast<std::int64_t>(SpiteEnumType::name), #name},

#define SPITE_DETAIL_SCHEMA(defaultFlags, persistedTag, ...)                                                           \
    static const ::spite::StructSchema& spiteSchema()                                                                  \
    {                                                                                                                  \
        using SpiteSchemaType = SPITE_DETAIL_FIRST(__VA_ARGS__);                                                       \
        using namespace ::spite::fieldFlags;                                                                           \
        [[maybe_unused]] constexpr ::spite::u32 spiteSchemaDefault = defaultFlags;                                     \
        static const ::spite::FieldDesc spiteFields[] = {                                                              \
            SPITE_DETAIL_FOR_EACH(SPITE_DETAIL_FIELD, __VA_ARGS__)::spite::FieldDesc{}};                               \
        static const ::spite::StructSchema spiteSchemaValue{                                                           \
            SPITE_DETAIL_STRINGIZE(SPITE_DETAIL_FIRST(__VA_ARGS__)),                                                   \
            static_cast<::spite::u32>(sizeof(SpiteSchemaType)),                                                        \
            std::span<const ::spite::FieldDesc>(spiteFields, std::size(spiteFields) - 1), persistedTag};               \
        return spiteSchemaValue;                                                                                       \
    }

#define SPITE_DETAIL_EXPAND(x) x
#define SPITE_DETAIL_STRINGIZE(x) SPITE_DETAIL_STRINGIZE_I(x)
#define SPITE_DETAIL_STRINGIZE_I(x) #x
#define SPITE_DETAIL_CAT(a, b) SPITE_DETAIL_CAT_I(a, b)
#define SPITE_DETAIL_CAT_I(a, b) a##b
#define SPITE_DETAIL_FIRST(...) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FIRST_I(__VA_ARGS__, ~))
#define SPITE_DETAIL_FIRST_I(first, ...) first
#define SPITE_DETAIL_REM(...) __VA_ARGS__

// IS_PAREN: 1 for a SPITE_FIELD(...) item, 0 for a bare member name.
#define SPITE_DETAIL_PROBE(...) ~, 1
#define SPITE_DETAIL_SECOND(...) SPITE_DETAIL_EXPAND(SPITE_DETAIL_SECOND_I(__VA_ARGS__))
#define SPITE_DETAIL_SECOND_I(a, b, ...) b
#define SPITE_DETAIL_IS_PAREN(x) SPITE_DETAIL_SECOND(SPITE_DETAIL_PROBE x, 0, ~)

#define SPITE_DETAIL_FIELD(item) SPITE_DETAIL_CAT(SPITE_DETAIL_FIELD_, SPITE_DETAIL_IS_PAREN(item))(item)
#define SPITE_DETAIL_FIELD_0(name)                                                                                     \
    ::spite::detail::makeSchemaField<decltype(SpiteSchemaType::name)>(#name, offsetof(SpiteSchemaType, name), 0u,      \
                                                                      spiteSchemaDefault),
#define SPITE_DETAIL_FIELD_1(pair) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FIELD_FLAGGED(SPITE_DETAIL_REM pair))
#define SPITE_DETAIL_FIELD_FLAGGED(...) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FIELD_FLAGGED_I(__VA_ARGS__))
#define SPITE_DETAIL_FIELD_FLAGGED_I(name, flags)                                                                      \
    ::spite::detail::makeSchemaField<decltype(SpiteSchemaType::name)>(                                                 \
        #name, offsetof(SpiteSchemaType, name), ::spite::FieldOptions(flags), spiteSchemaDefault),

// FOR_EACH over the fields after the leading type argument (0..40 fields).
#define SPITE_DETAIL_FOR_EACH(m, ...)                                                                                  \
    SPITE_DETAIL_EXPAND(SPITE_DETAIL_FOR_EACH_PICK(                                                                    \
        __VA_ARGS__, SPITE_DETAIL_FE56, SPITE_DETAIL_FE55, SPITE_DETAIL_FE54, SPITE_DETAIL_FE53, SPITE_DETAIL_FE52,    \
        SPITE_DETAIL_FE51, SPITE_DETAIL_FE50, SPITE_DETAIL_FE49, SPITE_DETAIL_FE48, SPITE_DETAIL_FE47,                 \
        SPITE_DETAIL_FE46, SPITE_DETAIL_FE45, SPITE_DETAIL_FE44, SPITE_DETAIL_FE43, SPITE_DETAIL_FE42,                 \
        SPITE_DETAIL_FE41, SPITE_DETAIL_FE40, SPITE_DETAIL_FE39, SPITE_DETAIL_FE38, SPITE_DETAIL_FE37,                 \
        SPITE_DETAIL_FE36, SPITE_DETAIL_FE35, SPITE_DETAIL_FE34, SPITE_DETAIL_FE33, SPITE_DETAIL_FE32,                 \
        SPITE_DETAIL_FE31, SPITE_DETAIL_FE30, SPITE_DETAIL_FE29, SPITE_DETAIL_FE28, SPITE_DETAIL_FE27,                 \
        SPITE_DETAIL_FE26, SPITE_DETAIL_FE25, SPITE_DETAIL_FE24, SPITE_DETAIL_FE23, SPITE_DETAIL_FE22,                 \
        SPITE_DETAIL_FE21, SPITE_DETAIL_FE20, SPITE_DETAIL_FE19, SPITE_DETAIL_FE18, SPITE_DETAIL_FE17,                 \
        SPITE_DETAIL_FE16, SPITE_DETAIL_FE15, SPITE_DETAIL_FE14, SPITE_DETAIL_FE13, SPITE_DETAIL_FE12,                 \
        SPITE_DETAIL_FE11, SPITE_DETAIL_FE10, SPITE_DETAIL_FE9, SPITE_DETAIL_FE8, SPITE_DETAIL_FE7, SPITE_DETAIL_FE6,  \
        SPITE_DETAIL_FE5, SPITE_DETAIL_FE4, SPITE_DETAIL_FE3, SPITE_DETAIL_FE2, SPITE_DETAIL_FE1,                      \
        SPITE_DETAIL_FE0)(m, __VA_ARGS__))
#define SPITE_DETAIL_FOR_EACH_PICK(_0, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17,     \
                                   _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, _33,     \
                                   _34, _35, _36, _37, _38, _39, _40, _41, _42, _43, _44, _45, _46, _47, _48, _49,     \
                                   _50, _51, _52, _53, _54, _55, _56, N, ...)                                          \
    N
#define SPITE_DETAIL_FE0(m, type)
#define SPITE_DETAIL_FE1(m, type, x) m(x)
#define SPITE_DETAIL_FE2(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE1(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE3(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE2(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE4(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE3(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE5(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE4(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE6(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE5(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE7(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE6(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE8(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE7(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE9(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE8(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE10(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE9(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE11(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE10(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE12(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE11(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE13(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE12(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE14(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE13(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE15(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE14(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE16(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE15(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE17(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE16(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE18(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE17(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE19(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE18(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE20(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE19(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE21(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE20(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE22(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE21(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE23(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE22(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE24(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE23(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE25(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE24(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE26(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE25(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE27(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE26(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE28(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE27(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE29(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE28(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE30(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE29(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE31(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE30(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE32(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE31(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE33(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE32(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE34(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE33(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE35(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE34(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE36(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE35(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE37(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE36(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE38(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE37(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE39(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE38(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE40(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE39(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE41(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE40(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE42(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE41(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE43(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE42(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE44(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE43(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE45(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE44(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE46(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE45(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE47(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE46(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE48(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE47(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE49(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE48(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE50(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE49(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE51(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE50(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE52(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE51(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE53(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE52(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE54(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE53(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE55(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE54(m, type, __VA_ARGS__))
#define SPITE_DETAIL_FE56(m, type, x, ...) m(x) SPITE_DETAIL_EXPAND(SPITE_DETAIL_FE55(m, type, __VA_ARGS__))
