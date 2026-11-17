#include "base/WritableLayout.hpp"
#include "editor/EditorPreviewJobs.hpp"
#include "EditorAssetPreviews.hpp"
#include "EditorPreviewHistory.hpp"
#include "EditorRenderedPreviews.hpp"
#include "asset/LoadBakedFunctions.hpp"
#include "assetruntime/AssetRuntime.hpp"
#include "base/ContentRoot.hpp"
#include "base/ServiceLocator.hpp"
#include "base/io/FileSystem.hpp"
#include "control/ControlServer.hpp"
#include "engine/Engine.hpp"
#include "serialization/FormatWalk.hpp"
#include <algorithm>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace spite
{
    namespace
    {
        constexpr auto OWNER = "editorPreviewJobs";
    }
    struct EditorPreviewJobs::State : EditorPreviewHistory
    {
        Engine& engine;
        std::filesystem::path directory = (writableDirectory(WritableFolder::Cache, {}, false) / "previews");
        EditorRenderedPreviews rendered;
        EditorAssetPreviews textures{directory};
        std::unordered_set<u32> oldImports;
        std::unordered_map<InternedString, AssetManifest::MaterialEntry> materials;
        std::vector<AssetRuntime::MaterialPreviewVersion> suppressedVersions, enabledVersions;
        u64 importGeneration = 0, manifestGeneration = 0, nextJob = 0, revision = 0;

        explicit State(Engine& e) : engine(e), rendered(e)
        {
            importGeneration = e.assets().importsGeneration();
            const auto& manifest = e.assets().manifest();
            manifestGeneration = manifest.generation();
            for (const auto& [name, entry] : manifest.getMaterials())
                materials.emplace(name, entry);
            for (const auto& item : e.assets().imports())
                oldImports.insert(item.id);
        }
        void add(u64 job, std::string type, InternedString name, InternedString baked)
        {
            if (!name.isValid() || !baked.isValid())
                return;
            if (std::ranges::any_of(
                    items, [&](const Item& i)
                    { return i.type == type && i.name == name && (i.status == "queued" || i.status == "active"); }))
                return;
            items.push_back({job, std::move(type), name, resolveContentPath(pathFromUtf8(baked.toView()))});
            ++requested;
        }
        u64 enqueue(std::string_view type, std::string_view name)
        {
            const u64 job = ++nextJob;
            const auto& manifest = engine.assets().manifest();
            const auto matches = [&](std::string_view t, InternedString n)
            { return (type == "all" || type == t) && (name.empty() || n.toView() == name); };
            for (const auto& [n, entry] : manifest.getModels())
                if (matches("model", n))
                    add(job, "model", n, entry.baked);
            for (const auto& [n, entry] : manifest.getMaterials())
                if (matches("material", n) && !n.toView().starts_with("__"))
                    add(job, "material", n, entry.baked);
            for (const auto& [n, entry] : manifest.getTextures())
                if (matches("texture", n))
                    add(job, "texture", n, entry.baked);
            return job;
        }
        void imports()
        {
            auto& assets = engine.assets();
            if (importGeneration == assets.importsGeneration())
                return;
            importGeneration = assets.importsGeneration();
            const auto& manifest = assets.manifest();
            for (const auto& record : assets.imports())
            {
                if (oldImports.contains(record.id) || record.state != AssetRuntime::ModelImport::State::Done)
                    continue;
                oldImports.insert(record.id);
                auto& versions = record.generatePreviews ? enabledVersions : suppressedVersions;
                versions.insert(versions.end(), record.previewMaterialVersions.begin(),
                                record.previewMaterialVersions.end());
                if (!record.generatePreviews)
                    continue;
                const u64 job = ++nextJob;
                const auto model = manifest.getModelEntry(record.name);
                if (!model)
                    continue;
                add(job, "model", record.name, model->baked);
                auto materials = model->materials;
                materials.insert(materials.end(), record.materials.begin(), record.materials.end());
                for (auto name : materials)
                {
                    const auto entry = manifest.getMaterialEntry(name);
                    if (!entry)
                        continue;
                    if (!name.toView().starts_with("__"))
                        add(job, "material", name, entry->baked);
                    if (const auto fields = readBakedMaterialDescriptor(name, manifest))
                        for (const auto slot : MATERIAL_TEXTURE_SLOTS)
                        {
                            const auto texture = fields->textureName(slot);
                            add(job, "texture", texture, manifest.getTextureBakedPath(texture));
                        }
                }
                for (auto name : record.textures)
                    add(job, "texture", name, manifest.getTextureBakedPath(name));
            }
        }
        void materialChanges()
        {
            const auto& manifest = engine.assets().manifest();
            if (manifestGeneration == manifest.generation())
                return;
            manifestGeneration = manifest.generation();
            std::unordered_map<InternedString, AssetManifest::MaterialEntry> current;
            std::unordered_set<InternedString> changed;
            for (const auto& [name, entry] : manifest.getMaterials())
            {
                current.emplace(name, entry);
                const auto previous = materials.find(name);
                const auto matches = [&](const auto& version)
                {
                    return version.name == name && version.baked == entry.baked &&
                        version.contentHash == entry.contentHash;
                };
                const bool suppressed =
                    std::ranges::any_of(suppressedVersions, matches) && !std::ranges::any_of(enabledVersions, matches);
                if (!suppressed && !name.toView().starts_with("__") &&
                    (previous == materials.end() || previous->second.baked != entry.baked ||
                     previous->second.contentHash != entry.contentHash))
                    changed.insert(name);
            }
            materials = std::move(current);
            if (changed.empty())
                return;
            const u64 job = ++nextJob;
            auto refresh = [&](std::string_view type, InternedString name, InternedString baked)
            {
                for (sizet i = 0; i < items.size(); ++i)
                {
                    auto& item = items[i];
                    if (item.type != type || item.name != name)
                        continue;
                    if (item.status == "active")
                    {
                        rendered.cancel();
                        item.status = "superseded";
                        active = static_cast<sizet>(-1);
                    }
                    else if (item.status == "queued")
                    {
                        item.baked = resolveContentPath(pathFromUtf8(baked.toView()));
                        return;
                    }
                }
                add(job, std::string(type), name, baked);
            };
            for (const auto name : changed)
                if (const auto entry = manifest.getMaterialEntry(name))
                    refresh("material", name, entry->baked);
            for (const auto& [name, entry] : manifest.getModels())
                if (std::ranges::any_of(entry.materials,
                                        [&](InternedString material) { return changed.contains(material); }))
                    refresh("model", name, entry.baked);
        }
        void update()
        {
            imports();
            materialChanges();
            suppressedVersions.clear();
            enabledVersions.clear();
            pruneHistory();
            if (active != static_cast<sizet>(-1))
            {
                auto& item = items[active];
                bool done = false, success = false;
                if (item.type == "texture")
                {
                    textures.update();
                    auto result = textures.status(std::string(item.name.toView()));
                    done = result.status == EditorAssetPreviews::Status::Ready ||
                        result.status == EditorAssetPreviews::Status::Error ||
                        result.status == EditorAssetPreviews::Status::Missing;
                    success = result.status == EditorAssetPreviews::Status::Ready;
                    item.error = result.error;
                }
                else
                {
                    rendered.update();
                    auto status = rendered.status();
                    done = status == EditorRenderedPreviews::Status::Ready ||
                        status == EditorRenderedPreviews::Status::Failed ||
                        status == EditorRenderedPreviews::Status::Missing ||
                        status == EditorRenderedPreviews::Status::Idle;
                    success = status == EditorRenderedPreviews::Status::Ready;
                    item.error = rendered.error();
                }
                if (!done)
                    return;
                item.status = success ? "completed" : "failed";
                if (!success)
                    ++failed;
                if (!success && item.error.empty())
                    item.error = "Preview unavailable";
                if (success)
                    ++revision;
                if (item.type == "texture")
                    textures.cancel(std::string(item.name.toView()));
                else
                    rendered.cancel();
                active = static_cast<sizet>(-1);
                pruneHistory();
            }
            for (sizet i = 0; i < items.size(); ++i)
            {
                auto& item = items[i];
                if (item.status != "queued")
                    continue;
                active = i;
                item.status = "active";
                if (item.type == "texture")
                    textures.request(std::string(item.name.toView()), item.baked);
                else if (item.type == "model")
                    rendered.requestCachedModel(item.name, directory, true);
                else
                    rendered.requestCachedMaterial(item.name, directory, true);
                break;
            }
        }
    };

    EditorPreviewJobs::EditorPreviewJobs(Engine& engine) : m_state(std::make_unique<State>(engine))
    {
        engine.services().registerService(this);
        engine.control().registerViewCommand(
            "generateAssetPreviews", OWNER,
            "Enqueue saved asset previews: accepted confirms the request, jobId identifies its batch, "
            "count is the number of newly queued assets (zero if none match or all are already pending).",
            {optionalArg("type", ArgKind::String, "all, model, material or texture", "all"),
             optionalArg("name", ArgKind::String, "asset name"),
             optionalArg("mode", ArgKind::String, "missing", "missing")},
            [this](const ControlArgs& args, IFormatWriter& out)
            {
                const auto type = args.string("type").value_or("all");
                const auto name = args.string("name").value_or("");
                const auto mode = args.string("mode").value_or("missing");
                if (type != "all" && type != "model" && type != "material" && type != "texture")
                    return std::string("Invalid preview type");
                if (mode != "missing")
                    return std::string("Invalid preview mode");
                const auto before = m_state->items.size();
                const auto id = m_state->enqueue(type, name);
                out.writeBool("accepted", true);
                out.writeU64("jobId", id);
                out.writeU64("count", m_state->items.size() - before);
                return std::string{};
            });
        engine.control().registerQuery(
            "assetPreviewJobs", OWNER,
            "Saved preview jobs: revision counts successful preview completions; items holds "
            "all queued/active assets and the newest 128 terminal entries in enqueue order. "
            "Each item has jobId (batch), name (asset), type (model/material/texture), status "
            "(queued, active, completed, failed or superseded), and error (failure message).",
            {},
            [this](const ControlArgs&, IFormatWriter& out)
            {
                out.writeU64("revision", m_state->revision);
                out.beginArray("items", static_cast<u32>(m_state->items.size()));
                for (const auto& item : m_state->items)
                {
                    out.beginObject("");
                    out.writeU64("jobId", item.job);
                    out.writeString("name", item.name.toView());
                    out.writeString("type", item.type);
                    out.writeString("status", item.status);
                    out.writeString("error", item.error);
                    out.endObject();
                }
                out.endArray();
                return std::string{};
            });
    }
    EditorPreviewJobs::~EditorPreviewJobs()
    {
        m_state->engine.control().unregisterOwner(OWNER);
        m_state->engine.services().unregisterService<EditorPreviewJobs>();
    }
    void EditorPreviewJobs::update() { m_state->update(); }
    std::uint64_t EditorPreviewJobs::revision() const { return m_state->revision; }
    void EditorPreviewJobs::enqueue(std::string_view type, std::string_view name) { m_state->enqueue(type, name); }
    bool EditorPreviewJobs::busy() const
    {
        return std::ranges::any_of(m_state->items,
                                   [](const auto& item) { return item.status == "queued" || item.status == "active"; });
    }
    int EditorPreviewJobs::report(std::ostream& out) const
    {
        for (const auto& item : m_state->items)
        {
            if (item.status == "failed")
            {
                out << item.type << " " << item.name.toView() << ": " << item.error << '\n';
            }
        }
        out << "Asset previews: " << m_state->requested << " requested, " << m_state->failed << " failed\n";
        return m_state->failed ? 1 : 0;
    }

} // namespace spite
