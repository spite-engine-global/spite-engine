#include "application/ConfigStore.hpp"

#include <algorithm>

#include "base/Logging.hpp"
#include "base/io/FileSystem.hpp"

namespace spite
{
    ConfigStore::ConfigStore(std::filesystem::path projectRoot, std::filesystem::path userRoot) :
        m_projectRoot(std::move(projectRoot)), m_userRoot(std::move(userRoot))
    {
    }

    void ConfigStore::setChangePublisher(std::function<void(ConfigSectionId)> publisher)
    {
        std::lock_guard lock(m_mutex);
        m_changePublisher = std::move(publisher);
    }

    ConfigStore::ConfigStore(std::filesystem::path projectRoot, std::filesystem::path userRoot,
                             std::filesystem::path fallbackProjectRoot) :
        ConfigStore(std::move(projectRoot), std::move(userRoot))
    {
        if (fallbackProjectRoot.lexically_normal() != m_projectRoot.lexically_normal())
            m_fallbackProjectRoot = std::move(fallbackProjectRoot);
    }

    ConfigStore::SectionHandle::~SectionHandle() { reset(); }

    ConfigStore::SectionHandle::SectionHandle(SectionHandle&& other) noexcept :
        m_store(other.m_store), m_sectionId(other.m_sectionId), m_generation(other.m_generation)
    {
        other.m_store = nullptr;
        other.m_sectionId = {};
        other.m_generation = 0;
    }

    ConfigStore::SectionHandle& ConfigStore::SectionHandle::operator=(SectionHandle&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            m_store = other.m_store;
            m_sectionId = other.m_sectionId;
            m_generation = other.m_generation;
            other.m_store = nullptr;
            other.m_sectionId = {};
            other.m_generation = 0;
        }
        return *this;
    }

    bool ConfigStore::SectionHandle::isValid() const { return m_store != nullptr && m_generation != 0; }

    void ConfigStore::SectionHandle::reset()
    {
        if (m_store)
        {
            m_store->unregisterSection(m_sectionId, m_generation);
            m_store = nullptr;
            m_sectionId = {};
            m_generation = 0;
        }
    }

    std::filesystem::path ConfigStore::resolvePathUnlocked(const SectionRecord& record) const
    {
        const std::filesystem::path& root =
            record.persistenceLayer == ConfigPersistenceLayer::Project ? m_projectRoot : m_userRoot;
        return root / record.relativePath;
    }

    bool ConfigStore::loadSectionUnlocked(SectionRecord& record, std::shared_ptr<void>& outValue) const
    {
        if (!record.codecs.decode)
        {
            return false;
        }

        // Project layer loads first, user layer overrides it; each decode
        // merges the file on top of the previous result, starting at the
        // registered defaults.
        std::shared_ptr<void> value = record.codecs.decode(record.defaults.get(), {});
        std::vector<std::filesystem::path> loaded;
        const std::filesystem::path* roots[] = {&m_fallbackProjectRoot, &m_projectRoot, &m_userRoot};
        for (int layer = 0; layer != 3; ++layer)
        {
            if (roots[layer]->empty() || (record.userOnly && layer < 2) ||
                (record.persistenceLayer == ConfigPersistenceLayer::Memory && layer == 2))
                continue;
            const auto path = *roots[layer] / record.relativePath;
            std::error_code error;
            if (!std::filesystem::exists(path, error))
            {
                continue;
            }

            const auto text = readTextFile(path);
            if (!text || text->empty())
            {
                SERROR_LOG("ConfigStore failed to read {}: {}", utf8Path(path),
                           text ? "empty file" : text.error().message);
                return false;
            }

            std::shared_ptr<void> decoded = record.codecs.decode(value.get(), *text);
            if (!decoded)
            {
                SERROR_LOG("ConfigStore failed to decode {}.", utf8Path(path));
                return false;
            }
            value = std::move(decoded);
            loaded.push_back(path);
        }

        if (record.overrideLoaded)
        {
            record.overrideLoaded(value.get());
        }
        if (!record.validate(value.get()))
        {
            // path::string() throws for a name the ANSI page cannot hold: only the rejection builds the text, as UTF-8
            std::string files;
            for (const std::filesystem::path& path : loaded)
            {
                files += (files.empty() ? "" : ", ") + utf8Path(path);
            }
            SWARNING_LOG("ConfigStore section {} rejected: the value read from {} fails the section's validator; "
                         "the section keeps its previous value.",
                         record.sectionId.toView(), files.empty() ? "no file" : files);
            return false;
        }
        outValue = std::move(value);
        return true;
    }

    bool ConfigStore::loadSection(ConfigSectionId sectionId)
    {
        std::shared_ptr<void> value;
        std::function<void(const void*)> apply;
        {
            std::lock_guard lock(m_mutex);
            const auto it = m_sections.find(sectionId);
            if (it == m_sections.end() || !loadSectionUnlocked(it->second, value))
            {
                return false;
            }
            it->second.published = value;
            it->second.pending.reset();
            it->second.dirty = false;
            if (it->second.codecs.encode)
            {
                it->second.baseline = it->second.codecs.encode(value.get());
            }
            apply = it->second.apply;
        }

        apply(value.get());
        return true;
    }

    bool ConfigStore::loadAll()
    {
        std::vector<ConfigSectionId> sectionIds;
        {
            std::lock_guard lock(m_mutex);
            sectionIds.reserve(m_sections.size());
            for (const auto& [sectionId, record] : m_sections)
            {
                if (record.readOnlyReason.empty())
                {
                    sectionIds.push_back(sectionId);
                }
            }
        }

        bool success = true;
        for (const ConfigSectionId sectionId : sectionIds)
        {
            success = loadSection(sectionId) && success;
        }
        return success;
    }

    ConfigStore::SectionHandle ConfigStore::declareFile(const ConfigSectionId sectionId, std::filesystem::path file,
                                                        const ConfigPersistenceLayer layer, std::string reason)
    {
        if (!sectionId.isValid() || file.empty() || reason.empty() || layer == ConfigPersistenceLayer::Memory)
        {
            return {};
        }
        SectionRecord record{};
        record.sectionId = sectionId;
        record.relativePath = std::move(file);
        record.persistenceLayer = layer;
        record.readOnlyReason = std::move(reason);

        std::lock_guard lock(m_mutex);
        if (m_sections.contains(sectionId))
        {
            return {};
        }
        record.generation = m_nextGeneration++;
        const u64 generation = record.generation;
        m_sections.emplace(sectionId, std::move(record));
        return SectionHandle(*this, sectionId, generation);
    }

    std::vector<ConfigSectionInfo> ConfigStore::describe(const bool withSaved) const
    {
        std::vector<ConfigSectionInfo> infos;
        std::lock_guard lock(m_mutex);
        infos.reserve(m_sections.size());
        for (const auto& [sectionId, record] : m_sections)
        {
            ConfigSectionInfo info;
            info.id = sectionId;
            info.doc = record.doc;
            info.applyMode = record.applyMode;
            info.layer = record.persistenceLayer;
            info.dirty = record.dirty;
            info.pending = record.pending != nullptr;
            info.readOnlyReason = record.readOnlyReason;
            info.schema = record.schema;
            info.fields = record.fields;
            info.value = record.pending ? record.pending : record.published;
            info.published = record.published;
            info.defaults = record.defaults;
            info.writeValues = record.writeValues;
            if (withSaved && record.codecs.decode && !record.baseline.empty())
            {
                info.saved = record.codecs.decode(record.defaults.get(), record.baseline);
            }

            const auto addFile =
                [&](const std::filesystem::path& root, const ConfigPersistenceLayer layer, const bool target)
            {
                const std::filesystem::path path = root / record.relativePath;
                ConfigFileState file;
                file.layer = layer == ConfigPersistenceLayer::Project ? "project" : "user";
                const auto text = [](const std::filesystem::path& value)
                {
                    const std::u8string utf8 = value.generic_u8string();
                    return std::string(utf8.begin(), utf8.end());
                };
                // a file declared by its absolute path shows relative to its layer's root when it lies under it
                std::filesystem::path under = record.relativePath;
                if (under.is_absolute())
                {
                    under = under.lexically_normal().lexically_relative(root.lexically_normal());
                }
                const bool inside = !under.empty() && !under.is_absolute() && *under.begin() != "..";
                file.file = inside && (layer == ConfigPersistenceLayer::User || m_fallbackProjectRoot.empty())
                    ? file.layer + "/" + text(under)
                    : text(path);
                std::error_code error;
                file.exists = std::filesystem::exists(path, error);
                file.target = target;
                info.files.push_back(std::move(file));
            };
            if (!record.readOnlyReason.empty())
            {
                addFile(record.persistenceLayer == ConfigPersistenceLayer::Project ? m_projectRoot : m_userRoot,
                        record.persistenceLayer, false);
            }
            else
            {
                if (!record.userOnly)
                {
                    if (!m_fallbackProjectRoot.empty())
                        addFile(m_fallbackProjectRoot, ConfigPersistenceLayer::Project, false);
                    addFile(m_projectRoot, ConfigPersistenceLayer::Project,
                            record.persistenceLayer == ConfigPersistenceLayer::Project);
                }
                if (record.persistenceLayer != ConfigPersistenceLayer::Memory)
                {
                    addFile(m_userRoot, ConfigPersistenceLayer::User,
                            record.persistenceLayer == ConfigPersistenceLayer::User);
                }
            }
            infos.push_back(std::move(info));
        }
        std::ranges::sort(infos, {}, [](const ConfigSectionInfo& info) { return info.id.toView(); });
        return infos;
    }

    ConfigCommitResult ConfigStore::commitUntyped(ConfigSectionId sectionId, const std::type_index type,
                                                  std::shared_ptr<void> value)
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_sections.find(sectionId);
        if (it == m_sections.end())
        {
            return ConfigCommitResult::NotFound;
        }
        if (it->second.type != type)
        {
            return ConfigCommitResult::TypeMismatch;
        }
        if (!it->second.validate(value.get()))
        {
            return ConfigCommitResult::Rejected;
        }

        SectionRecord& record = it->second;
        if (record.codecs.encode)
        {
            const std::string text = record.codecs.encode(value.get());
            const void* staged = record.pending ? record.pending.get() : record.published.get();
            if (text == record.codecs.encode(staged))
            {
                // nothing changes: a change already pending stays so, otherwise there is nothing to apply
                if (!record.pending)
                {
                    return ConfigCommitResult::Applied;
                }
                return record.applyMode == ConfigApplyMode::StartupOnly ? ConfigCommitResult::RestartRequired
                                                                        : ConfigCommitResult::Deferred;
            }
            // dirty: the staged value differs from what was last loaded or saved (as the file text it encodes to)
            record.dirty = text != record.baseline;
            // back to the value in force: the change waiting for a boundary or a restart is gone, not re-staged
            if (record.pending && text == record.codecs.encode(record.published.get()))
            {
                record.pending.reset();
                return ConfigCommitResult::Applied;
            }
        }
        else
        {
            record.dirty = true;
        }
        if (it->second.applyMode == ConfigApplyMode::StartupOnly)
        {
            it->second.pending = std::move(value);
            return ConfigCommitResult::RestartRequired;
        }

        it->second.pending = std::move(value);
        return ConfigCommitResult::Deferred;
    }

    ConfigCommitResult ConfigStore::commitEdit(const ConfigSectionId sectionId,
                                               const std::function<std::string(void* value)>& change,
                                               std::string& error, std::shared_ptr<const void>& committed)
    {
        std::shared_ptr<void> copy;
        std::type_index type = typeid(void);
        {
            std::lock_guard lock(m_mutex);
            const auto it = m_sections.find(sectionId);
            if (it == m_sections.end() || !it->second.clone) // a declared file has none
            {
                return ConfigCommitResult::NotFound;
            }
            const SectionRecord& record = it->second;
            copy = record.clone(record.pending ? record.pending.get() : record.published.get());
            type = record.type;
        }
        error = change(copy.get());
        if (!error.empty())
        {
            return ConfigCommitResult::Rejected;
        }
        committed = copy;
        return commitUntyped(sectionId, type, std::move(copy));
    }

    void ConfigStore::applyPending()
    {
        struct PendingApply
        {
            ConfigSectionId sectionId{};
            std::shared_ptr<void> value{};
            std::function<void(const void*)> apply{};
            std::function<void(const void*)> changed{};
        };

        std::vector<PendingApply> pending;
        {
            std::lock_guard lock(m_mutex);
            for (auto& [sectionId, record] : m_sections)
            {
                if (record.pending && record.applyMode == ConfigApplyMode::LiveAtFrameBoundary)
                {
                    pending.push_back({sectionId, std::move(record.pending), record.apply, record.changed});
                }
            }
        }

        for (const PendingApply& change : pending)
        {
            change.apply(change.value.get());
            std::function<void(ConfigSectionId)> changePublisher;
            {
                std::lock_guard lock(m_mutex);
                const auto it = m_sections.find(change.sectionId);
                if (it == m_sections.end() || it->second.pending)
                {
                    continue;
                }
                it->second.published = change.value;
                changePublisher = m_changePublisher;
            }
            change.changed(change.value.get());
            if (changePublisher)
            {
                changePublisher(change.sectionId);
            }
        }
    }

    bool ConfigStore::saveSectionUnlocked(SectionRecord& record)
    {
        if (!record.readOnlyReason.empty())
        {
            return false;
        }
        const void* staged = record.pending ? record.pending.get() : record.published.get();
        if (record.persistenceLayer == ConfigPersistenceLayer::Memory)
        {
            if (record.codecs.encode)
            {
                record.baseline = record.codecs.encode(staged);
            }
            record.dirty = false;
            return true;
        }
        if (!record.codecs.encode)
        {
            return false;
        }
        const std::filesystem::path path = resolvePathUnlocked(record);
        std::string text;
        try
        {
            text = record.codecs.encode(staged);
        }
        catch (const std::exception& e)
        {
            SERROR_LOG("ConfigStore failed to serialize {}: {}", utf8Path(path), e.what());
            return false;
        }

        // The codec's text, but for the replaced file's own line endings and final newline: a save of an unchanged
        // section leaves a checked-in file as it was (git shows no diff), a changed one a diff of its values.
        std::string content = text;
        if (const auto existing = readTextFile(path); existing && !existing->empty())
        {
            if (existing->back() == '\n' && !content.ends_with('\n'))
            {
                content += '\n';
            }
            if (existing->find("\r\n") != std::string::npos && content.find('\r') == std::string::npos)
            {
                std::string crlf;
                crlf.reserve(content.size() + content.size() / 16);
                for (const char c : content)
                {
                    if (c == '\n')
                    {
                        crlf += '\r';
                    }
                    crlf += c;
                }
                content = std::move(crlf);
            }
        }

        // through a temporary beside the file; the error text names the path as UTF-8: path::string() throws for a
        // config root outside the ANSI page
        if (const Expected<void> written = writeTextFileAtomically(path, content); !written)
        {
            SERROR_LOG("ConfigStore failed to write {}: {}", utf8Path(path), written.error().message);
            return false;
        }
        record.baseline = std::move(text);
        record.dirty = false;
        return true;
    }

    bool ConfigStore::saveSection(ConfigSectionId sectionId)
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_sections.find(sectionId);
        return it != m_sections.end() && saveSectionUnlocked(it->second);
    }

    bool ConfigStore::saveIfDirty(ConfigSectionId sectionId)
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_sections.find(sectionId);
        return it != m_sections.end() && (!it->second.dirty || saveSectionUnlocked(it->second));
    }

    bool ConfigStore::saveDirty(const std::optional<ConfigPersistenceLayer> only)
    {
        std::lock_guard lock(m_mutex);
        bool success = true;
        for (auto& [_, record] : m_sections)
        {
            if (record.dirty && (!only || record.persistenceLayer == *only))
            {
                success = saveSectionUnlocked(record) && success;
            }
        }
        return success;
    }

    bool ConfigStore::unregisterSection(ConfigSectionId sectionId, const u64 generation)
    {
        std::lock_guard lock(m_mutex);
        const auto it = m_sections.find(sectionId);
        if (it == m_sections.end() || it->second.generation != generation)
        {
            return false;
        }
        m_sections.erase(it);
        return true;
    }

    bool ConfigStore::hasSection(ConfigSectionId sectionId) const
    {
        std::lock_guard lock(m_mutex);
        return m_sections.contains(sectionId);
    }
} // namespace spite
