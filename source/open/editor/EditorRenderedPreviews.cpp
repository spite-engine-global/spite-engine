#include "EditorRenderedPreviews.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iomanip>
#include <sstream>

#include "asset/LoadBakedFunctions.hpp"
#include "asset/AssetImportingFunctions.hpp"
#include "asset/VertexTangents.hpp"
#include "asset/GpuAssetFactory.hpp"
#include "base/ContentRoot.hpp"
#include "base/io/FileSystem.hpp"
#include <vector>

#include "assetruntime/AssetRuntime.hpp"
#include "assetruntime/RenderableComponents.hpp"
#include "assetruntime/RenderStateComponents.hpp"
#include "base/ServiceLocator.hpp"
#include "ecs/World.hpp"
#include "engine/Engine.hpp"
#include "renderruntime/RenderRuntime.hpp"
#include "renderruntime/EnvironmentComponent.hpp"
#include "renderruntime/LightComponents.hpp"
#include "renderruntime/RenderingComponents.hpp"
#include "scene/SceneApply.hpp"
#include "scene/SceneStream.hpp"
#include "world/EntityTransformManager.hpp"
#include "world/TransformComponents.hpp"

namespace spite
{
    namespace
    {
        ProcessedSubmesh previewSphere()
        {
            constexpr u32 slices = 48, stacks = 24;
            constexpr float pi = 3.14159265358979323846f;
            ProcessedSubmesh mesh;
            mesh.localBounds = {Vec3{-1.0f}, Vec3{1.0f}};
            for (u32 y = 0; y <= stacks; ++y)
            {
                const float v = static_cast<float>(y) / stacks;
                const float theta = pi * v;
                for (u32 x = 0; x <= slices; ++x)
                {
                    const float u = static_cast<float>(x) / slices;
                    const float phi = 2.0f * pi * u;
                    const Vec3 n{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
                    const float inv = 1.0f / (std::abs(n.x) + std::abs(n.y) + std::abs(n.z));
                    float ox = n.x * inv, oy = n.y * inv;
                    if (n.z < 0.0f)
                    {
                        const float oldX = ox;
                        ox = (1.0f - std::abs(oy)) * (oldX >= 0.0f ? 1.0f : -1.0f);
                        oy = (1.0f - std::abs(oldX)) * (oy >= 0.0f ? 1.0f : -1.0f);
                    }
                    const auto px = static_cast<u16>(static_cast<i16>(ox * 32767.0f));
                    const auto py = static_cast<u16>(static_cast<i16>(oy * 32767.0f));
                    mesh.vertices.push_back(Vertex{n.x, n.y, n.z, static_cast<u32>(px) | (static_cast<u32>(py) << 16),
                                                   packUv(Vec2{u, v}, mesh.uvRange), 0});
                }
            }
            std::vector<u32> indices;
            for (u32 y = 0; y < stacks; ++y)
                for (u32 x = 0; x < slices; ++x)
                {
                    const u32 a = y * (slices + 1) + x, b = a + slices + 1;
                    if (y != 0)
                        indices.insert(indices.end(), {a, a + 1, b});
                    if (y + 1 != stacks)
                        indices.insert(indices.end(), {a + 1, b + 1, b});
                }
            generateVertexTangents(mesh.vertices, indices);
            buildMeshletHierarchy(mesh, indices);
            return mesh;
        }

        struct CacheResult
        {
            std::filesystem::path path;
            std::string error;
        };

        CacheResult dependencyPaths(const AssetManifest& manifest, InternedString name, bool material,
                                    std::vector<std::filesystem::path>& files)
        {
            auto add = [&](InternedString path)
            {
                if (!path.isValid())
                    return false;
                files.push_back(resolveContentPath(pathFromUtf8(path.toView())));
                return true;
            };
            std::vector<InternedString> materials;
            if (material)
                materials.push_back(name);
            else
            {
                const auto model = manifest.getModelEntry(name);
                if (!model || !add(model->baked))
                    return {{}, "Preview model missing"};
                materials = model->materials;
            }
            for (const auto item : materials)
            {
                const auto entry = manifest.getMaterialEntry(item);
                const auto fields = readBakedMaterialDescriptor(item, manifest);
                if (!entry || !fields || !add(entry->baked))
                    return {{}, "Preview material missing"};
                for (const auto slot : MATERIAL_TEXTURE_SLOTS)
                {
                    const auto texture = fields->textureName(slot);
                    if (texture.isValid() && !add(manifest.getTextureBakedPath(texture)))
                        return {{}, "Preview texture missing"};
                }
            }
            std::sort(files.begin(), files.end());
            files.erase(std::unique(files.begin(), files.end()), files.end());
            return {};
        }

        CacheResult cachePath(const std::vector<std::filesystem::path>& files, bool material,
                              std::string_view materialName, const std::filesystem::path& directory)
        {
            unsigned long long hash = 14695981039346656037ull;
            auto feed = [&](unsigned char c) { hash = (hash ^ c) * 1099511628211ull; };
            for (const char c :
                 std::string_view(material ? "spite-material-preview-procedural-sphere-v2" : "spite-model-preview-v1"))
                feed(c);
            if (material)
            {
                for (const char c : materialName)
                    feed(c);
                feed(0xff);
            }
            for (const auto& path : files)
            {
                std::ifstream file(path, std::ios::binary);
                if (!file)
                    return {{}, "Cannot read preview dependency"};
                char buffer[65536];
                while (file.read(buffer, sizeof(buffer)) || file.gcount())
                    for (std::streamsize i = 0; i < file.gcount(); ++i)
                        feed(static_cast<unsigned char>(buffer[i]));
                if (!file.eof())
                    return {{}, "Cannot read preview dependency"};
                feed(0xff);
            }
            std::ostringstream filename;
            filename << std::hex << std::setfill('0') << std::setw(16) << hash << ".png";
            return {directory / filename.str(), {}};
        }
    } // namespace

    struct EditorRenderedPreviews::State
    {
        explicit State(Engine& owner, u32 index) : engine(owner), viewIndex(index) {}
        ~State() { teardown(); }

        void teardown()
        {
            if (!world)
            {
                sphere.release();
                services.reset();
                return;
            }
            auto& view = engine.render().view(viewIndex);
            view.active = false;
            view.pose.reset();
            view.scene = 0;
            engine.detachWorld(world);
            transforms.reset();
            world.destroy();
            world = {};
            sphere.release();
            services.reset();
        }

        void fail(std::string message)
        {
            error = std::move(message);
            status = Status::Failed;
            teardown();
        }

        float fitDistance() const
        {
            const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
            const float halfFov = std::atan(std::tan(toRadians(22.5f)) * std::min(aspect, 1.0f));
            return radius / std::sin(halfFov) * 1.15f;
        }

        void updateCamera()
        {
            if (!world || !haveCamera)
                return;
            CameraPose pose;
            pose.transform.position = center +
                Vec3{std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)} * distance;
            pose.transform.rotation =
                multiply(fromAxisAngle({0.0f, 1.0f, 0.0f}, yaw), fromAxisAngle({1.0f, 0.0f, 0.0f}, -pitch));
            pose.camera.fov = toRadians(45.0f);
            pose.camera.nearPlane = std::max(0.001f, distance - radius * 1.5f);
            pose.camera.farPlane = distance + radius * 2.0f;
            engine.render().view(viewIndex).pose = pose;
        }

        Engine& engine;
        u32 viewIndex;
        Extent2D extent{256, 256};
        Vec3 center{0.0f};
        float radius = 1.0f;
        float yaw = 0.78539816f;
        float pitch = 0.55f;
        float distance = 1.0f;
        bool live = false;
        bool haveCamera = false;
        std::unique_ptr<ServiceLocator> services;
        World world;
        std::unique_ptr<EntityTransformManager> transforms;
        Entity root = Entity::undefined();
        std::vector<Entity> sceneRoots;
        bool scenePreview = false;
        AssetRef sphere;
        Status status = Status::Idle;
        std::filesystem::path path;
        std::string error;
        unsigned generation = 0;
        std::future<CacheResult> worker;
        std::vector<std::future<CacheResult>> retired;
        InternedString cachedName;
        bool cachedMaterial = false;
        bool cached = false;
        bool generateIfMissing = false;
        std::filesystem::path finalPath;
        std::filesystem::path cachedScene;
        std::vector<std::pair<InternedString, AssetManifest::MaterialEntry>> materialVersions;
    };

    EditorRenderedPreviews::EditorRenderedPreviews(Engine& engine, u32 viewIndex) :
        m_state(std::make_shared<State>(engine, viewIndex))
    {
    }
    EditorRenderedPreviews::~EditorRenderedPreviews() { cancel(); }

    void EditorRenderedPreviews::cancel()
    {
        ++m_state->generation;
        if (m_state->worker.valid())
            m_state->retired.push_back(std::move(m_state->worker));
        m_state->cached = false;
        m_state->live = false;
        m_state->haveCamera = false;
        m_state->scenePreview = false;
        m_state->sceneRoots.clear();
        m_state->finalPath.clear();
        m_state->cachedScene.clear();
        m_state->materialVersions.clear();
        m_state->teardown();
        m_state->status = Status::Idle;
        m_state->path.clear();
        m_state->error.clear();
    }

    void EditorRenderedPreviews::openModel(InternedString name)
    {
        cancel();
        m_state->live = true;
        startModel(name, {}, {});
    }

    void EditorRenderedPreviews::openMaterial(InternedString name)
    {
        cancel();
        m_state->live = true;
        startModel({}, {}, name);
    }

    void EditorRenderedPreviews::openScene(const std::filesystem::path& path)
    {
        cancel();
        auto& s = *m_state;
        s.live = true;
        s.scenePreview = true;
        if (s.engine.render().viewCount() <= s.viewIndex)
        {
            s.fail("Preview view unavailable");
            return;
        }
        const auto bytes = readBinaryFile(path);
        if (!bytes)
        {
            s.fail(bytes.error().message);
            return;
        }
        const auto* raw = reinterpret_cast<const u8*>(bytes->data());
        const bool binary = bytes->size() >= sizeof(SCENE_BINARY_MAGIC) &&
            std::memcmp(raw, SCENE_BINARY_MAGIC, sizeof(SCENE_BINARY_MAGIC)) == 0;
        auto document = binary ? parseSceneBinary(std::span(raw, bytes->size()))
                               : parseSceneJson(std::string_view(reinterpret_cast<const char*>(raw), bytes->size()));
        if (!document)
        {
            s.fail(document.error().message);
            return;
        }
        s.services = std::make_unique<ServiceLocator>(&s.engine.services());
        s.world = World::create(0, s.services.get());
        s.transforms = std::make_unique<EntityTransformManager>(s.world);
        const u32 scene = s.engine.attachWorld(s.world);
        s.world.initializeSystems();
        auto applied = applyScene(*document, s.world, *s.transforms, Entity::undefined());
        if (!applied)
        {
            s.fail(applied.error().message);
            return;
        }
        s.sceneRoots = std::move(applied->roots);
        auto& view = s.engine.render().view(s.viewIndex);
        view.scene = scene;
        view.extent = s.extent;
        view.renderScale = 1.0f;
        view.scalable = false;
        view.active = true;
        s.status = Status::Loading;
    }

    void EditorRenderedPreviews::requestCachedScene(const std::filesystem::path& source,
                                                    const std::filesystem::path& directory)
    {
        cancel();
        auto& s = *m_state;
        s.cachedScene = source;
        s.cached = true;
        s.generateIfMissing = true;
        s.status = Status::Loading;
        std::vector<std::string> versions;
        const auto& manifest = s.engine.assets().manifest();
        const auto collect = [&](const auto& entries)
        {
            for (const auto& [name, entry] : entries)
            {
                std::error_code ec;
                const auto path = resolveContentPath(pathFromUtf8(entry.baked.toView()));
                const auto modified = std::filesystem::last_write_time(path, ec);
                versions.push_back(std::string(name.toView()) + ":" + std::string(entry.baked.toView()) + ":" +
                                   (ec ? "missing" : std::to_string(modified.time_since_epoch().count())));
            }
        };
        collect(manifest.getModels());
        collect(manifest.getMaterials());
        collect(manifest.getTextures());
        collect(manifest.getEnvironments());
        std::sort(versions.begin(), versions.end());
        std::string revision = "scene-preview-v1";
        for (const auto& version : versions)
            revision += "\n" + version;
        s.worker = std::async(std::launch::async,
                              [source, directory, revision = std::move(revision)]
                              {
                                  try
                                  {
                                      return cachePath({source}, true, revision, directory);
                                  }
                                  catch (const std::exception& e)
                                  {
                                      return CacheResult{{}, e.what()};
                                  }
                              });
    }

    void EditorRenderedPreviews::setViewportExtent(Extent2D extent)
    {
        auto& s = *m_state;
        extent.width = std::max(1u, extent.width);
        extent.height = std::max(1u, extent.height);
        if (extent.width == s.extent.width && extent.height == s.extent.height)
            return;
        const float oldFit = s.fitDistance();
        s.extent = extent;
        if (s.live && s.world)
        {
            s.engine.render().view(s.viewIndex).extent = extent;
            if (s.haveCamera)
            {
                s.distance *= s.fitDistance() / oldFit;
                s.updateCamera();
            }
        }
    }

    void EditorRenderedPreviews::orbit(float yawDelta, float pitchDelta)
    {
        auto& s = *m_state;
        if (!s.live || !s.haveCamera)
            return;
        s.yaw += yawDelta;
        s.pitch = std::clamp(s.pitch + pitchDelta, -1.5f, 1.5f);
        s.updateCamera();
    }

    void EditorRenderedPreviews::zoom(float wheelDelta)
    {
        auto& s = *m_state;
        if (!s.live || !s.haveCamera)
            return;
        s.distance = std::clamp(s.distance * std::exp(-wheelDelta * 0.15f), s.radius * 1.05f, s.radius * 100.0f);
        s.updateCamera();
    }

    void EditorRenderedPreviews::requestModel(InternedString name, std::filesystem::path path)
    {
        cancel();
        startModel(name, std::move(path), {});
    }

    void EditorRenderedPreviews::requestMaterial(InternedString name, std::filesystem::path path)
    {
        cancel();
        startModel({}, std::move(path), name);
    }

    void EditorRenderedPreviews::startModel(InternedString name, std::filesystem::path path, InternedString material)
    {
        auto& s = *m_state;
        s.path = std::move(path);
        if (s.engine.render().viewCount() <= s.viewIndex)
        {
            s.fail("Preview view unavailable");
            return;
        }
        s.services = std::make_unique<ServiceLocator>(&s.engine.services());
        s.world = World::create(0, s.services.get());
        s.transforms = std::make_unique<EntityTransformManager>(s.world);
        const u32 scene = s.engine.attachWorld(s.world);
        s.world.initializeSystems();
        if (material.isValid())
        {
            const auto geometry = previewSphere();
            const InternedString privateName(std::string("__spite_material_preview_") + std::to_string(s.viewIndex));
            const auto handle = s.engine.services().get<GpuAssetFactory>().createSubmesh(geometry, 0, privateName);
            if (!handle.isValid())
            {
                s.fail("Preview geometry creation failed");
                return;
            }
            s.sphere = AssetRef(&s.engine.assets().registry(), handle);
            s.root = s.transforms->createNamedEntity("material preview sphere");
            s.world.addComponent<MeshComponent>(s.root, MeshComponent{.meshHandle = handle});
            s.world.addComponent<MaterialComponent>(
                s.root, MaterialComponent{.materialHandle = {material, AssetType::MATERIAL, 0}});
        }
        else
        {
            const auto model = s.engine.assets().instantiateModelNow(s.world, name, Entity::undefined());
            if (!model)
            {
                s.fail(model.error().message);
                return;
            }
            s.root = model->root;
            if (model->entities == 0 || s.root == Entity::undefined())
            {
                s.fail("Model is empty");
                return;
            }
        }
        const Entity sun = s.transforms->createNamedEntity("preview sun");
        s.world.getComponent<TransformComponent>(sun).rotation =
            multiply(fromAxisAngle({0.0f, 1.0f, 0.0f}, 0.5f), fromAxisAngle({1.0f, 0.0f, 0.0f}, -1.05f));
        s.world.addComponent<DirectionalLightComponent>(sun, DirectionalLightComponent{.intensity = 2.0f});
        const Entity environment = s.world.createEntity();
        s.world.addComponent<EnvironmentComponent>(
            environment, EnvironmentComponent{.mode = EnvironmentMode::Color, .tint = Vec3{0.18f}});
        auto& view = s.engine.render().view(s.viewIndex);
        view.scene = scene;
        view.extent = s.live ? s.extent : Extent2D{256, 256};
        view.renderScale = 1.0f;
        view.scalable = false;
        view.active = true;
        s.status = Status::Loading;
    }

    void EditorRenderedPreviews::requestCachedModel(InternedString name, std::filesystem::path directory,
                                                    bool generateIfMissing)
    {
        requestCached(name, std::move(directory), false, generateIfMissing);
    }

    void EditorRenderedPreviews::requestCachedMaterial(InternedString name, std::filesystem::path directory,
                                                       bool generateIfMissing)
    {
        requestCached(name, std::move(directory), true, generateIfMissing);
    }

    void EditorRenderedPreviews::requestCached(InternedString name, std::filesystem::path directory, bool material,
                                               bool generateIfMissing)
    {
        cancel();
        auto& s = *m_state;
        s.cachedName = name;
        s.cachedMaterial = material;
        s.cached = true;
        s.generateIfMissing = generateIfMissing;
        s.materialVersions.clear();
        const auto& manifest = s.engine.assets().manifest();
        std::vector<InternedString> materialNames;
        if (material)
            materialNames.push_back(name);
        else if (const auto model = manifest.getModelEntry(name))
            materialNames = model->materials;
        for (const auto materialName : materialNames)
            if (const auto entry = manifest.getMaterialEntry(materialName))
                s.materialVersions.emplace_back(materialName, *entry);
        s.status = Status::Loading;
        std::vector<std::filesystem::path> files;
        try
        {
            auto dependencies = dependencyPaths(s.engine.assets().manifest(), name, material, files);
            if (!dependencies.error.empty())
            {
                s.fail(std::move(dependencies.error));
                return;
            }
        }
        catch (const std::exception& e)
        {
            s.fail(e.what());
            return;
        }
        s.worker = std::async(std::launch::async,
                              [files = std::move(files), material, materialName = std::string(name.toView()),
                               directory = std::move(directory)]
                              {
                                  try
                                  {
                                      return cachePath(files, material, materialName, directory);
                                  }
                                  catch (const std::exception& e)
                                  {
                                      return CacheResult{{}, e.what()};
                                  }
                              });
    }

    void EditorRenderedPreviews::update()
    {
        auto& s = *m_state;
        std::erase_if(s.retired,
                      [](auto& job) { return job.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
        if (s.status != Status::Loading)
            return;
        if (s.worker.valid())
        {
            if (s.worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                return;
            auto result = s.worker.get();
            if (!result.error.empty())
            {
                s.fail(std::move(result.error));
                return;
            }
            s.finalPath = std::move(result.path);
            std::error_code ec;
            if (std::filesystem::is_regular_file(s.finalPath, ec))
            {
                s.path = s.finalPath;
                s.status = Status::Ready;
                return;
            }
            if (ec && ec != std::errc::no_such_file_or_directory)
            {
                s.fail("Cannot inspect preview cache");
                return;
            }
            ec.clear();
            if (!s.generateIfMissing)
            {
                s.status = Status::Missing;
                return;
            }
            std::filesystem::create_directories(s.finalPath.parent_path(), ec);
            if (ec)
            {
                s.fail("Cannot create preview cache");
                return;
            }
            auto temp = s.finalPath;
            temp += std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()) + L".tmp.png";
            if (!s.cachedScene.empty())
            {
                const auto source = s.cachedScene;
                const auto destination = s.finalPath;
                openScene(source);
                if (s.status == Status::Loading)
                {
                    s.cached = true;
                    s.live = false;
                    s.extent = {256, 256};
                    s.engine.render().view(s.viewIndex).extent = s.extent;
                    s.finalPath = destination;
                    s.path = temp;
                }
                return;
            }
            startModel(s.cachedMaterial ? InternedString{} : s.cachedName, temp,
                       s.cachedMaterial ? s.cachedName : InternedString{});
            return;
        }
        std::vector<Entity> entities;
        if (s.scenePreview)
        {
            for (const Entity root : s.sceneRoots)
            {
                std::vector<Entity> subtree;
                s.transforms->collectSubtree(root, subtree);
                entities.insert(entities.end(), subtree.begin(), subtree.end());
            }
        }
        else
            s.transforms->collectSubtree(s.root, entities);
        bool found = false;
        bool waiting = false;
        bool haveBounds = false;
        Vec3 lo{0.0f}, hi{0.0f};
        for (const Entity entity : entities)
        {
            if (s.scenePreview && s.world.hasComponent<InactiveComponent>(entity))
                continue;
            const auto* mesh = s.world.tryGetConstComponent<MeshComponent>(entity);
            if (!mesh)
                continue;
            found = true;
            const auto* material = s.world.tryGetConstComponent<MaterialComponent>(entity);
            if (!material && s.scenePreview)
                continue;
            if (!material)
            {
                s.fail("Model mesh has no material binding");
                return;
            }
            const auto meshStatus = s.engine.assets().status(mesh->meshHandle);
            const auto materialStatus = s.engine.assets().status(material->materialHandle);
            if (meshStatus == AssetRuntime::AssetStatus::Failed || materialStatus == AssetRuntime::AssetStatus::Failed)
            {
                s.fail("Model asset failed to load");
                return;
            }
            const auto* bounds = s.world.tryGetConstComponent<WorldBoundingBoxComponent>(entity);
            const auto* slot = s.world.tryGetConstComponent<GpuInstanceSlotComponent>(entity);
            if (meshStatus != AssetRuntime::AssetStatus::Ready || materialStatus != AssetRuntime::AssetStatus::Ready ||
                !bounds || !slot || slot->slot == INVALID_INSTANCE_SLOT ||
                !s.world.hasComponent<ObjectStaticInfoComponent>(entity))
            {
                waiting = true;
                continue;
            }
            if (!haveBounds)
            {
                lo = bounds->min;
                hi = bounds->max;
                haveBounds = true;
            }
            else
            {
                lo = min(lo, bounds->min);
                hi = max(hi, bounds->max);
            }
        }
        if (!found && !s.scenePreview)
        {
            s.fail("Model has no meshes");
            return;
        }
        if (waiting)
            return;
        if (!haveBounds && !s.scenePreview)
        {
            s.fail("Model has no bounds");
            return;
        }
        s.center = haveBounds ? (lo + hi) * 0.5f : Vec3{0.0f};
        s.radius = haveBounds ? std::max(length(hi - lo) * 0.5f, 0.01f) : 1.0f;
        s.yaw = 0.78539816f;
        s.pitch = 0.55f;
        if (!s.live)
            s.extent = {256, 256};
        s.distance = s.fitDistance();
        s.haveCamera = true;
        s.updateCamera();
        if (s.live)
        {
            s.status = Status::Ready;
            return;
        }
        s.status = Status::Capturing;
        const unsigned generation = s.generation;
        std::weak_ptr<State> weak = m_state;
        const auto temporaryPath = s.cached ? s.path : std::filesystem::path{};
        if (!s.engine.captureView(s.viewIndex, s.path, 3,
                                  [weak, generation, temporaryPath](std::string error)
                                  {
                                      auto state = weak.lock();
                                      if (!state || state->generation != generation ||
                                          state->status != Status::Capturing)
                                      {
                                          if (!temporaryPath.empty())
                                          {
                                              std::error_code ec;
                                              std::filesystem::remove(temporaryPath, ec);
                                          }
                                          return;
                                      }
                                      if (state)
                                      {
                                          const auto& manifest = state->engine.assets().manifest();
                                          for (const auto& [name, version] : state->materialVersions)
                                          {
                                              const auto current = manifest.getMaterialEntry(name);
                                              if (!current || current->baked != version.baked ||
                                                  current->contentHash != version.contentHash)
                                              {
                                                  if (!temporaryPath.empty())
                                                  {
                                                      std::error_code ec;
                                                      std::filesystem::remove(temporaryPath, ec);
                                                  }
                                                  state->status = Status::Idle;
                                                  state->teardown();
                                                  return;
                                              }
                                          }
                                          state->error = std::move(error);
                                          if (state->cached)
                                          {
                                              std::error_code ec;
                                              if (state->error.empty())
                                              {
                                                  std::filesystem::rename(state->path, state->finalPath, ec);
                                                  if (ec && !std::filesystem::is_regular_file(state->finalPath))
                                                      state->error = "Cannot publish preview";
                                              }
                                              std::filesystem::remove(state->path, ec);
                                              state->path = state->finalPath;
                                          }
                                          state->status = state->error.empty() ? Status::Ready : Status::Failed;
                                          state->teardown();
                                      }
                                  }))
            s.fail("Preview capture unavailable");
    }

    EditorRenderedPreviews::Status EditorRenderedPreviews::status() const { return m_state->status; }
    const std::filesystem::path& EditorRenderedPreviews::path() const { return m_state->path; }
    const std::string& EditorRenderedPreviews::error() const { return m_state->error; }
} // namespace spite
