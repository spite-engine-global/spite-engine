#pragma once

#include <concepts>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include "application/ApplicationExport.hpp"
#include "base/InternedString.hpp"
#include "base/Platform.hpp"
#include "base/Assert.hpp"
#include "base/Schema.hpp"

namespace spite
{
    class IFormatWriter;

    using ConfigSectionId = InternedString;

    // Spaces per level of the config files the codecs write: the checked-in files' layout, so a save is a diff of
    // the values only.
    inline constexpr int CONFIG_FILE_INDENT = 4;

    // Type-erased UTF-8 JSON codecs for a persisted section. The store treats
    // the text as opaque (a save only matches the replaced file's line endings
    // and final newline): encode produces the file content, decode merges a
    // parsed file on top of the caller-provided base value and returns the
    // decoded value (nullptr on failure). Empty text decodes to a copy of the
    // base. Sections with empty codecs are in-memory only (never loaded/saved).
    struct ConfigCodecs
    {
        std::function<std::string(const void*)> encode;
        std::function<std::shared_ptr<void>(const void* base, std::string_view text)> decode;
    };

    enum class ConfigApplyMode : u8
    {
        StartupOnly,
        LiveAtFrameBoundary
    };

    enum class ConfigPersistenceLayer : u8
    {
        Project,
        User,
        Memory // never read from or written to disk
    };

    enum class ConfigCommitResult : u8
    {
        Applied, // nothing waits: the value equals the staged one, or the published one (a pending change is dropped)
        Deferred,
        RestartRequired,
        Rejected,
        NotFound,
        TypeMismatch
    };

    // How one field takes a change when the section's apply mode does not say it: some fields of a startup-only
    // section are read every frame (live), some of a live one size a resource at startup.
    enum class ConfigFieldApply : u8
    {
        Section,
        Live,
        Startup
    };

    // One value of an enum field: its integer and the name a reader and a form show.
    struct ConfigChoice
    {
        i32 value = 0;
        std::string_view name;
    };

    inline constexpr double CONFIG_UNBOUNDED = std::numeric_limits<double>::infinity();

    // What a reader (the `config` query, a settings form) needs about a leaf field of a section's schema that the
    // schema does not carry. The table is keyed by dotted path ("shadows.maxDistance"); a path naming a
    // nested struct documents the group. min and max are declared only where the code enforces or physics implies
    // them: the type's own bounds (an unsigned never below 0) are not repeated. Strings point at static storage:
    // the string_views and the span of a section registered by a module that hot-reloads point into that module's
    // DLL, so the section's handle must be reset before the DLL unloads.
    struct ConfigFieldMeta
    {
        std::string_view path;
        std::string_view doc;
        std::string_view unit{};
        double min = -CONFIG_UNBOUNDED;
        double max = CONFIG_UNBOUNDED;
        std::span<const ConfigChoice> choices{}; // non-empty: the field is an enum and its value is a choice name
        std::string_view root{}; // non-empty: the string is a path relative to this root ("content")
        bool directory = false; // with root: the path names a directory (may be "." or end in a separator), not a file
        // with root, a file: why the file the text names cannot be used (missing, malformed), empty when it can; a
        // setConfig refuses a file this rejects. Points into the registering module like the strings above.
        std::string (*fileProblem)(std::string_view path) = nullptr;
        ConfigFieldApply apply = ConfigFieldApply::Section;
    };

    template <typename T>
    struct ConfigSection
    {
        ConfigSectionId sectionId{};
        std::filesystem::path relativePath{};
        T defaultValue{};
        ConfigApplyMode applyMode = ConfigApplyMode::StartupOnly;
        ConfigPersistenceLayer persistenceLayer = ConfigPersistenceLayer::Project;
        bool userOnly = false; // never read a project-layer value for machine-specific settings
        ConfigCodecs codecs{};
        std::function<bool(const T&)> validate{};
        std::function<void(const T&)> apply{};
        std::function<void(const T&)> changed{};
        // Adjusts the value read from the files (and the defaults when none exists) before it is validated and
        // published: a run-wide override of a file's value that setConfig can still change afterwards.
        std::function<void(T&)> overrideLoaded{};
        std::string_view doc{}; // what the section configures; static storage
        // The table for T's SPITE_SCHEMA fields (T with spiteSchema() is described by it automatically).
        std::span<const ConfigFieldMeta> fields{};
        // For a T without a schema: writes its value as the members of one object.
        std::function<void(const T&, IFormatWriter&)> writeValues{};
    };

    // A file one section or declared file reads or writes, as describe() reports it.
    struct ConfigFileState
    {
        std::string layer; // "project" or "user"
        std::string
            file; // "<layer>/<path under the layer's root>", '/' separated, UTF-8; absolute for a file elsewhere
        bool exists = false;
        bool target = false; // the file a save writes
    };

    // One section as a reader sees it (ConfigStore::describe): the staged value (a commit not yet applied counts) and
    // the defaults as type-erased shared state `schema` or `writeValues` read.
    struct ConfigSectionInfo
    {
        ConfigSectionId id{};
        std::string_view doc{};
        ConfigApplyMode applyMode = ConfigApplyMode::StartupOnly;
        ConfigPersistenceLayer layer = ConfigPersistenceLayer::Project; // where a save writes; Memory: nowhere
        std::vector<ConfigFileState> files; // in read order, a later one overriding the earlier
        bool dirty = false; // the staged value is not what the file holds
        bool pending = false; // a commit that has not applied yet
        std::string readOnlyReason; // non-empty: a declared file the store only lists (no value, no schema)
        const StructSchema* schema = nullptr; // null: opaque (writeValues) or a declared file
        std::span<const ConfigFieldMeta> fields{};
        std::shared_ptr<const void> value;
        std::shared_ptr<const void> published; // the value in force (value when nothing is pending); null: a file
        std::shared_ptr<const void> defaults;
        // the value the file holds (what was last loaded or saved) when describe asked for it; null otherwise, and for
        // a section without codecs
        std::shared_ptr<const void> saved;
        std::function<void(const void*, IFormatWriter&)> writeValues;
    };

    class ConfigStore;

    template <typename T>
    class ConfigEdit
    {
        ConfigStore* m_store = nullptr;
        ConfigSectionId m_sectionId{};
        T m_value{};

        ConfigEdit(ConfigStore& store, ConfigSectionId sectionId, const T& value) :
            m_store(&store), m_sectionId(sectionId), m_value(value)
        {
        }

        friend class ConfigStore;

    public:
        T& get() { return m_value; }
        const T& get() const { return m_value; }
        [[nodiscard]] ConfigCommitResult commit();
    };

    class SPITE_APPLICATION_API ConfigStore
    {
    public:
        // Nested classes are not exported implicitly when the enclosing class
        // is: this one owns the section lifetime, so it needs its own marker.
        class SPITE_APPLICATION_API SectionHandle
        {
            ConfigStore* m_store = nullptr;
            ConfigSectionId m_sectionId{};
            u64 m_generation = 0;

            SectionHandle(ConfigStore& store, ConfigSectionId sectionId, u64 generation) :
                m_store(&store), m_sectionId(sectionId), m_generation(generation)
            {
            }

            friend class ConfigStore;

        public:
            SectionHandle() = default;
            ~SectionHandle();
            SectionHandle(const SectionHandle&) = delete;
            SectionHandle& operator=(const SectionHandle&) = delete;
            SectionHandle(SectionHandle&& other) noexcept;
            SectionHandle& operator=(SectionHandle&& other) noexcept;

            [[nodiscard]] bool isValid() const;
            void reset();
        };

    private:
        struct SectionRecord
        {
            ConfigSectionId sectionId{};
            std::filesystem::path relativePath{};
            ConfigApplyMode applyMode = ConfigApplyMode::StartupOnly;
            ConfigPersistenceLayer persistenceLayer = ConfigPersistenceLayer::Project;
            bool userOnly = false;
            std::type_index type = typeid(void);
            std::shared_ptr<void> defaults{};
            std::shared_ptr<void> published{};
            std::shared_ptr<void> pending{};
            ConfigCodecs codecs{};
            std::function<bool(const void*)> validate{};
            std::function<void(const void*)> apply{};
            std::function<void(const void*)> changed{};
            std::function<void(void*)> overrideLoaded{};
            std::string_view doc{};
            std::span<const ConfigFieldMeta> fields{};
            const StructSchema* schema = nullptr;
            std::function<void(const void*, IFormatWriter&)> writeValues{};
            std::function<std::shared_ptr<void>(const void*)> clone{};
            std::string readOnlyReason{}; // set: declared by declareFile, never loaded, saved or edited
            std::string baseline{}; // the file text the value last loaded or saved encodes to: what `dirty` compares
            u64 generation = 0;
            bool dirty = false;
        };

        std::filesystem::path m_projectRoot;
        std::filesystem::path m_userRoot;
        std::filesystem::path m_fallbackProjectRoot;
        std::unordered_map<ConfigSectionId, SectionRecord> m_sections;
        mutable std::mutex m_mutex;
        std::function<void(ConfigSectionId)> m_changePublisher{};
        u64 m_nextGeneration = 1;

        [[nodiscard]] std::filesystem::path resolvePathUnlocked(const SectionRecord& record) const;
        [[nodiscard]] bool loadSectionUnlocked(SectionRecord& record, std::shared_ptr<void>& outValue) const;
        [[nodiscard]] bool saveSectionUnlocked(SectionRecord& record);
        [[nodiscard]] bool unregisterSection(ConfigSectionId sectionId, u64 generation);
        [[nodiscard]] ConfigCommitResult commitUntyped(ConfigSectionId sectionId, std::type_index type,
                                                       std::shared_ptr<void> value);

    public:
        ConfigStore(std::filesystem::path projectRoot, std::filesystem::path userRoot);
        // Read the fallback first, then project and user overrides; saves always target projectRoot or userRoot.
        ConfigStore(std::filesystem::path projectRoot, std::filesystem::path userRoot,
                    std::filesystem::path fallbackProjectRoot);
        ConfigStore(const ConfigStore&) = delete;
        ConfigStore& operator=(const ConfigStore&) = delete;

        void setChangePublisher(std::function<void(ConfigSectionId)> publisher);

        template <typename T>
        [[nodiscard]] SectionHandle registerSection(const ConfigSection<T>& section)
        {
            if (!section.sectionId.isValid() || section.relativePath.empty())
            {
                return {};
            }

            SectionRecord record{};
            record.sectionId = section.sectionId;
            record.relativePath = section.relativePath;
            record.applyMode = section.applyMode;
            record.persistenceLayer = section.persistenceLayer;
            record.userOnly = section.userOnly;
            record.type = typeid(T);
            record.defaults = std::make_shared<T>(section.defaultValue);
            record.published = std::make_shared<T>(section.defaultValue);
            record.codecs = section.codecs;
            if (record.codecs.encode)
            {
                record.baseline = record.codecs.encode(record.published.get());
            }
            record.validate = [validate = section.validate](const void* value)
            { return value != nullptr && (!validate || validate(*static_cast<const T*>(value))); };
            record.apply = [apply = section.apply](const void* value)
            {
                if (apply)
                {
                    apply(*static_cast<const T*>(value));
                }
            };
            record.changed = [changed = section.changed](const void* value)
            {
                if (changed)
                {
                    changed(*static_cast<const T*>(value));
                }
            };
            if (section.overrideLoaded)
            {
                record.overrideLoaded = [override = section.overrideLoaded](void* value)
                { override(*static_cast<T*>(value)); };
            }
            record.clone = [](const void* value) -> std::shared_ptr<void>
            { return std::make_shared<T>(*static_cast<const T*>(value)); };
            record.doc = section.doc;
            record.fields = section.fields;
            if constexpr (requires {
                              { T::spiteSchema() } -> std::same_as<const StructSchema&>;
                          })
            {
                record.schema = &T::spiteSchema();
            }
            if (section.writeValues)
            {
                record.writeValues = [write = section.writeValues](const void* value, IFormatWriter& out)
                { write(*static_cast<const T*>(value), out); };
            }

            std::lock_guard lock(m_mutex);
            if (m_sections.contains(section.sectionId))
            {
                return {};
            }

            record.generation = m_nextGeneration++;
            const u64 generation = record.generation;
            m_sections.emplace(section.sectionId, std::move(record));
            return SectionHandle(*this, section.sectionId, generation);
        }

        template <typename T>
        [[nodiscard]] const T& get(ConfigSectionId sectionId) const
        {
            std::lock_guard lock(m_mutex);
            const auto it = m_sections.find(sectionId);
            SASSERTM(it != m_sections.end() && it->second.type == typeid(T), "Invalid config section access.");
            return *static_cast<const T*>(it->second.published.get());
        }

        template <typename T>
        [[nodiscard]] std::optional<ConfigEdit<T>> beginEdit(ConfigSectionId sectionId)
        {
            std::lock_guard lock(m_mutex);
            const auto it = m_sections.find(sectionId);
            if (it == m_sections.end() || it->second.type != typeid(T))
            {
                return std::nullopt;
            }
            const void* value = it->second.pending ? it->second.pending.get() : it->second.published.get();
            return ConfigEdit<T>(*this, sectionId, *static_cast<const T*>(value));
        }

        [[nodiscard]] bool loadAll();
        [[nodiscard]] bool loadSection(ConfigSectionId sectionId);
        [[nodiscard]] bool reload(ConfigSectionId sectionId) { return loadSection(sectionId); }
        void applyPending();
        // every dirty section, or only those that save to `only` (the run's end saves the user layer: a runtime edit
        // of a project file is the user's explicit save)
        [[nodiscard]] bool saveDirty(std::optional<ConfigPersistenceLayer> only = std::nullopt);
        [[nodiscard]] bool saveSection(ConfigSectionId sectionId);
        [[nodiscard]] bool saveIfDirty(ConfigSectionId sectionId);

        template <typename T>
        [[nodiscard]] ConfigCommitResult commit(ConfigSectionId sectionId, const T& value)
        {
            return commitUntyped(sectionId, typeid(T), std::make_shared<T>(value));
        }

        // commit for a tool that works through the schema and holds no T: `change` edits a copy of the staged value
        // (outside the store's lock); a non-empty return refuses it, with nothing staged and Rejected returned.
        // Otherwise the copy is committed as commit does, and `committed` holds it (unchanged from then on, whatever
        // the result). NotFound: no such section, or a declared file.
        [[nodiscard]] ConfigCommitResult commitEdit(ConfigSectionId sectionId,
                                                    const std::function<std::string(void* value)>& change,
                                                    std::string& error, std::shared_ptr<const void>& committed);

        [[nodiscard]] bool hasSection(ConfigSectionId sectionId) const;

        // A file some module reads and writes itself, listed with the reason it is not a section here (an input
        // mapping, the editor's layout): `describe` reports it read-only and nothing loads, edits or saves it. file is
        // relative to the layer's root, or absolute. Empty id or file: an invalid handle; a taken id likewise.
        [[nodiscard]] SectionHandle declareFile(ConfigSectionId sectionId, std::filesystem::path file,
                                                ConfigPersistenceLayer layer, std::string reason);

        // Every section and declared file, by id (the `config` query's data): each staged value and its defaults, the
        // files read and the one saved. The values stay valid after a later commit. `withSaved` also decodes each
        // section's saved value (a parse per section, so only the callers that show it ask).
        [[nodiscard]] std::vector<ConfigSectionInfo> describe(bool withSaved = false) const;
    };

    template <typename T>
    ConfigCommitResult ConfigEdit<T>::commit()
    {
        return m_store ? m_store->commit(m_sectionId, m_value) : ConfigCommitResult::NotFound;
    }
} // namespace spite
