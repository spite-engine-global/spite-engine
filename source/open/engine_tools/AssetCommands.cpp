#include "ecs/World.hpp"
#include "engine/AssetCommands.hpp"
#include "FlipbookCard.hpp"
#include "engine/PrimitiveMesh.hpp"
#include "SceneLayerPlacement.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <future>
#include <fstream>
#include <limits>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "audio/AudioWorld.hpp"
#include "asset/AssetImportingFunctions.hpp"
#include "asset/LoadBakedFunctions.hpp"
#include "asset/ModelImportSettings.hpp"
#include "asset/OceanProfile.hpp"
#include "base/ContentRoot.hpp"
#include "base/MathFunctions.hpp"
#include "base/io/FileSystem.hpp"
#include "assetruntime/AssetRuntime.hpp"
#include "assetruntime/RenderableComponents.hpp"
#include "control/ControlServer.hpp"
#include "ecs/ComponentRegistry.hpp"
#include "ecs/Chunk.hpp"
#include "serialization/FormatWalk.hpp"
#include "serialization/JsonFormat.hpp"
#include "engine/SceneCommands.hpp"
#include "scene/SceneStream.hpp"
#include "ui/UiComponents.hpp"
#include "renderruntime/LightComponents.hpp"
#include "renderruntime/RenderingComponents.hpp"
#include "world/EntityName.hpp"
#include "world/EntityTransformManager.hpp"
#include "world/TransformComponents.hpp"

namespace spite
{
    namespace
    {
        constexpr std::string_view OWNER = "engine";

        struct AssetKind
        {
            std::string_view name;
            AssetType type;
            bool loadable = true; // loadAsset takes it: a skeleton or clip is baked data the animation module reads
        };
        constexpr std::string_view SPRITE_ATLAS_KIND = "spriteAtlas";
        constexpr AssetKind KINDS[] = {{"model", AssetType::MESH},
                                       {"material", AssetType::MATERIAL},
                                       {"texture", AssetType::TEXTURE},
                                       {"environment", AssetType::ENVIRONMENT},
                                       {"oceanProfile", AssetType::OCEAN_PROFILE},
                                       {"skeleton", AssetType::SKELETON, false},
                                       {"clip", AssetType::ANIMATION, false}};

        using AssetFields = std::span<const std::string>;

        bool assetField(const AssetFields fields, const std::string_view name)
        {
            return fields.empty() || std::ranges::contains(fields, name);
        }

        const char* statusName(const AssetRuntime::AssetStatus status)
        {
            switch (status)
            {
            case AssetRuntime::AssetStatus::Loading:
                return "loading";
            case AssetRuntime::AssetStatus::Ready:
                return "ready";
            case AssetRuntime::AssetStatus::Failed:
                return "failed";
            default:
                return "unloaded";
            }
        }

        // a texture's status over its colour spaces and the spaces that hold it (the listing's "spaces")
        void writeSpaces(const AssetRuntime::TextureState& state, IFormatWriter& out)
        {
            constexpr std::string_view SPACE_NAMES[] = {"srgb", "linear"};
            u32 count = 0;
            for (const AssetRuntime::AssetStatus status : state.spaces)
            {
                count += status != AssetRuntime::AssetStatus::Unloaded;
            }
            out.beginArray("spaces", count);
            for (sizet i = 0; i < state.spaces.size(); ++i)
            {
                if (state.spaces[i] != AssetRuntime::AssetStatus::Unloaded)
                {
                    out.writeString("", SPACE_NAMES[i]);
                }
            }
            out.endArray();
        }

        std::string kindArg(const ControlArgs& args, const AssetKind*& out, const bool loadable = true)
        {
            const std::optional<std::string> type = args.string("type");
            if (!type)
            {
                return "missing arg 'type' (model, material, texture, environment or oceanProfile)";
            }
            const auto it = std::ranges::find(KINDS, std::string_view(*type), &AssetKind::name);
            if (it == std::end(KINDS) || (loadable && !it->loadable))
            {
                return "unknown asset type '" + *type + "' (model, material, texture, environment or oceanProfile)";
            }
            out = &*it;
            return {};
        }

        std::string nameArg(const ControlArgs& args, const std::string_view key, std::string& out)
        {
            std::optional<std::string> name = args.string(key);
            if (!name || name->empty())
            {
                return "missing arg '" + std::string(key) + "'";
            }
            out = std::move(*name);
            return {};
        }

        bool inManifest(const AssetManifest& manifest, const AssetType type, const InternedString& name)
        {
            switch (type)
            {
            case AssetType::MESH:
                return manifest.hasModel(name);
            case AssetType::MATERIAL:
                return manifest.hasMaterial(name);
            case AssetType::ENVIRONMENT:
                return manifest.hasEnvironment(name);
            case AssetType::OCEAN_PROFILE:
                return manifest.getOceanProfileEntry(name).has_value();
            default:
                return manifest.hasTexture(name);
            }
        }

        // -- material fields -------------------------------------------------------------

        const StructSchema& fieldsSchema() { return BakedMaterialWriteData::spiteSchema(); }

        std::string fieldsJson(const BakedMaterialWriteData& fields)
        {
            JsonFragmentWriter writer;
            writeValue(fieldsSchema(), &fields, writer, FieldSet::All);
            std::string text;
            writer.dump(text);
            return text;
        }

        void writeFields(const BakedMaterialWriteData& fields, const std::string_view key, IFormatWriter& out)
        {
            out.beginObject(key);
            writeValue(fieldsSchema(), &fields, out, FieldSet::All);
            out.endObject();
        }

        std::string fieldNames()
        {
            std::string names;
            for (const FieldDesc& field : fieldsSchema().fields)
            {
                names += (names.empty() ? "" : ", ") + std::string(field.name);
            }
            return names;
        }

        // What the fields arg names must hold to: a finite number inside the schema's range (a set refuses the same)
        // and an enum's named value.
        std::string checkFieldValues(const BakedMaterialWriteData& data, const nlohmann::json& named)
        {
            const auto* base = reinterpret_cast<const std::byte*>(&data);
            for (const FieldDesc& field : fieldsSchema().fields)
            {
                if (!named.contains(field.name))
                {
                    continue; // a stored value is kept as it is
                }
                const std::byte* member = base + field.offset;
                if (field.kind == FieldKind::F32 || field.kind == FieldKind::Vec3 || field.kind == FieldKind::Vec4)
                {
                    const sizet lanes = field.kind == FieldKind::F32 ? 1 : field.kind == FieldKind::Vec3 ? 3 : 4;
                    for (sizet i = 0; i < lanes; ++i)
                    {
                        f32 value;
                        std::memcpy(&value, member + i * sizeof(f32), sizeof(f32));
                        if (!std::isfinite(value))
                        {
                            return std::format("'{}' is not a finite number", field.name);
                        }
                        if (field.name != "terrainDetailMetres" && (value < field.min || value > field.max))
                        {
                            const auto bound = [](const double limit)
                            { return std::isfinite(limit) ? std::format("{}", limit) : std::string("unbounded"); };
                            return std::format("{} is outside the range of '{}' ({} to {})", value, field.name,
                                               bound(field.min), bound(field.max));
                        }
                    }
                }
                else if (field.kind == FieldKind::U32 && !field.enumNames.empty())
                {
                    u32 value;
                    std::memcpy(&value, member, sizeof(value));
                    if (std::ranges::none_of(field.enumNames, [value](const EnumName& named)
                                             { return named.value == static_cast<std::int64_t>(value); }))
                    {
                        std::string choices;
                        for (const EnumName& named : field.enumNames)
                        {
                            choices += (choices.empty() ? "" : ", ") + std::string(named.name) + "=" +
                                std::to_string(named.value);
                        }
                        return std::format("{} is not a value of '{}' ({})", value, field.name, choices);
                    }
                }
            }
            return {};
        }

        // Applies the "fields" arg over inout: the members it names, each a
        // schema field, texture names naming baked textures.
        std::string readFields(const AssetManifest& manifest, const ControlArgs& args, BakedMaterialWriteData& inout)
        {
            const std::optional<std::string> text = args.json("fields");
            if (!text)
            {
                return {};
            }
            const nlohmann::json parsed = nlohmann::json::parse(*text, nullptr, false);
            if (!parsed.is_object())
            {
                return "'fields' must be an object of material fields";
            }
            for (const auto& [key, value] : parsed.items())
            {
                if (std::ranges::none_of(fieldsSchema().fields,
                                         [&key](const FieldDesc& field) { return field.name == key; }))
                {
                    return "unknown material field '" + key + "' (" + fieldNames() + ")";
                }
            }
            const auto sharedScale = parsed.find("terrainDetailMetres");
            if (sharedScale != parsed.end() && sharedScale->is_number() &&
                !validTerrainLayerScale(sharedScale->get<double>(), false))
                return "invalid terrain detail scale";
            for (const char* name : {"terrainGrassDetailMetres", "terrainSoilDetailMetres", "terrainRockDetailMetres"})
            {
                const auto field = parsed.find(name);
                if (field != parsed.end() && field->is_number() && !validTerrainLayerScale(field->get<double>()))
                    return "invalid terrain detail scale";
            }
            std::string error;
            JsonFragmentReader reader(*text);
            if (!readValue(fieldsSchema(), &inout, reader, error))
            {
                return "bad fields: " + error;
            }
            if (std::string refused = checkFieldValues(inout, parsed); !refused.empty())
            {
                return refused;
            }
            for (const MaterialTextureSlot slot : MATERIAL_TEXTURE_SLOTS)
            {
                InternedString& texture = inout.textureName(slot);
                if (texture.isValid() && texture.toView().empty())
                {
                    texture = InternedString{};
                }
                if (texture.isValid() && !manifest.hasTexture(texture))
                {
                    return "no texture '" + std::string(texture.toView()) + "' in the manifest";
                }
            }
            for (const TerrainTextureSlot slot : TERRAIN_TEXTURE_SLOTS)
            {
                InternedString& texture = inout.terrainTextureName(slot);
                if (texture.isValid() && texture.toView().empty())
                    texture = InternedString{};
                if (texture.isValid() && !manifest.hasTexture(texture))
                    return "no terrain texture '" + std::string(texture.toView()) + "' in the manifest";
            }
            if (!inout.validTerrain())
                return "terrain needs ten textures, a base-color proxy, opaque alpha, no ordinary normal map and a "
                       "valid detail scale";
            return {};
        }

        constexpr sizet MAX_BAKE_NAME = 64;

        // "__" names are the engine's own.
        std::string engineMaterialName(const std::string& name)
        {
            return name.starts_with("__") ? "material names starting with '__' are reserved" : std::string{};
        }

        // A new asset's name becomes its bake file name, on a case-insensitive
        // file system that reserves device names.
        std::string bakeFileNameError(const std::string_view kind, const std::string& name)
        {
            if (name.size() > MAX_BAKE_NAME)
            {
                return std::string(kind) + " name is " + std::to_string(name.size()) + " characters; the limit is " +
                    std::to_string(MAX_BAKE_NAME);
            }
            if (!std::ranges::all_of(
                    name, [](const char c)
                    { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'; }) ||
                name.find("..") != std::string::npos || name.ends_with('.'))
            {
                return std::string(kind) + " name '" + name +
                    "' must use letters, digits, '_', '-' and '.' only, and not end with '.'";
            }
            // Windows device names, with any extension
            const std::string stem = AssetManifest::bakeFileKey(std::string_view(name).substr(0, name.find('.')));
            const bool numbered = stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) &&
                stem[3] >= '1' && stem[3] <= '9';
            if (numbered || stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
            {
                return std::string(kind) + " name '" + name + "' is a reserved device name";
            }
            return {};
        }

        std::string newMaterialNameArg(const AssetManifest& manifest, const ControlArgs& args, std::string& out)
        {
            if (std::string error = nameArg(args, "name", out); !error.empty())
            {
                return error;
            }
            if (std::string error = engineMaterialName(out); !error.empty())
            {
                return error;
            }
            if (std::string error = bakeFileNameError("material", out); !error.empty())
            {
                return error;
            }
            const std::string key = AssetManifest::bakeFileKey(out);
            for (const auto& [existing, entry] : manifest.getMaterials())
            {
                if (AssetManifest::bakeFileKey(existing.toView()) == key)
                {
                    return existing.toView() == out ? "material '" + out + "' exists; updateMaterial edits it"
                                                    : "material name '" + out + "' clashes with material '" +
                            std::string(existing.toView()) + "' (bake file names ignore case)";
                }
            }
            return {};
        }

        std::string readMaterial(const AssetRuntime& assets, const std::string& name, BakedMaterialWriteData& out)
        {
            const Expected<BakedMaterialWriteData> fields =
                readBakedMaterialDescriptor(InternedString(name.c_str()), assets.manifest());
            if (!fields)
            {
                return "no material '" + name + "'";
            }
            out = *fields;
            return {};
        }

        struct FlipbookInspection
        {
            SpiteTextureHeader header{};
            LocalBoundingBoxComponent bounds{};
            float worldWidth = 0.0f, worldHeight = 0.0f;
        };

        std::string validateFlipbookPlacement(const World& world, const AssetRuntime& assets, const Entity entity,
                                              const FlipbookComponent& effect, FlipbookInspection* inspection = nullptr)
        {
            if (!validFlipbook(effect))
                return "invalid metadata: use positive grid dimensions, an in-grid frame count, and finite rate and "
                       "time";
            if (!world.hasComponent<MaterialComponent>(entity) || !world.hasComponent<MeshComponent>(entity))
                return "flipbook needs a mesh and material; instantiate a textured card model";
            if (world.hasComponent<UiIdentityComponent>(entity) || !world.hasComponent<TransformComponent>(entity))
                return "flipbook needs a scene entity with an editable transform";
            if (!world.hasComponent<ObjectStaticInfoComponent>(entity) ||
                !world.hasComponent<LocalBoundingBoxComponent>(entity) ||
                !world.hasComponent<TransformMatrixComponent>(entity))
                return "card asset is not ready; wait for its mesh to load and retry";
            const auto& info = world.getConstComponent<ObjectStaticInfoComponent>(entity);
            if (info.packedUvOffset != 0 || info.packedUvExtent != 0)
                return "card UVs must stay within [0,1]; export a full-atlas card";
            const auto& binding = world.getConstComponent<MaterialComponent>(entity);
            BakedMaterialWriteData material;
            if (std::string error = readMaterial(assets, std::string(binding.materialHandle.name.toView()), material);
                !error.empty())
                return error;
            if (material.albedoTextureName.toView().empty())
                return "material needs an albedo atlas texture";
            if (material.uvOffset != Vec2(0.0f) || material.uvScale != Vec2(1.0f) || material.uvRotation != 0.0f)
                return "material texture transform must be identity for flipbook playback";
            if (effect.cameraFacing &&
                (material.alphaMode == MaterialAlphaMode::Opaque || material.alphaMode == MaterialAlphaMode::Mask))
                return "camera-facing cards need a blend, premultiplied, or additive material";
            if (effect.cameraFacing && material.castsShadow)
                return "camera-facing cards cannot cast shadows; disable castsShadow on the material";
            if (const auto valid = validateBakedTexture(material.albedoTextureName, assets.manifest()); !valid)
                return "atlas texture is missing or malformed: " + std::string(valid.error().message);
            const auto path = assets.manifest().getTextureBakedPath(material.albedoTextureName);
            std::ifstream input(resolveContentPath(path.toView()), std::ios::binary);
            SpiteTextureHeader header{};
            if (!input.read(reinterpret_cast<char*>(&header), sizeof(header)))
                return "cannot read the atlas texture header";
            if (header.width < effect.columns || header.height < effect.rows)
                return "atlas is smaller than the requested grid; use at least one pixel per cell";
            const auto& bounds = world.getConstComponent<LocalBoundingBoxComponent>(entity);
            const float width = bounds.max.x - bounds.min.x, height = bounds.max.y - bounds.min.y;
            const float depth = bounds.max.z - bounds.min.z;
            const Mat4& matrix = world.getConstComponent<TransformMatrixComponent>(entity).matrix;
            if (!(width > 0.0f && height > 0.0f && depth <= std::max(width, height) * 0.001f &&
                  width * length(Vec3(matrix[0])) > 0.0f && height * length(Vec3(matrix[1])) > 0.0f))
                return "card must be a nondegenerate local XY plane with nonzero world size";
            if (bounds.min.x > 0.0f || bounds.max.x < 0.0f || bounds.min.y > 0.0f || bounds.max.y < 0.0f)
                return "card pivot (local origin) must lie within its XY bounds";
            if (inspection)
            {
                inspection->header = header;
                inspection->bounds = bounds;
                inspection->worldWidth = width * length(Vec3(matrix[0]));
                inspection->worldHeight = height * length(Vec3(matrix[1]));
            }
            return {};
        }

        std::string writeMaterialRecording(ControlServer& control, AssetRuntime& assets, const std::string& name,
                                           const BakedMaterialWriteData& fields, const std::string_view inverse,
                                           const std::string& inverseArgs)
        {
            if (const Expected<void> written = assets.writeMaterial(InternedString(name.c_str()), fields); !written)
            {
                return "cannot write material '" + name + "': " + written.error().message;
            }
            control.recordInverse(inverse, inverseArgs);
            return {};
        }

        std::string inverseArgs(const std::string& name, const BakedMaterialWriteData* fields)
        {
            nlohmann::json args{{"name", name}};
            if (fields)
            {
                args["fields"] = nlohmann::json::parse(fieldsJson(*fields));
            }
            return args.dump();
        }

        // -- handlers --------------------------------------------------------------------

        // an environment's source, format and extent: the fields of the listing and of importEnvironment's reply
        void writeEnvironment(const AssetManifest::EnvironmentEntry& entry, IFormatWriter& out,
                              const AssetFields fields = {})
        {
            if (assetField(fields, "source"))
                out.writeString("source", entry.source.toView());
            if (assetField(fields, "format"))
                out.writeString("format", bakedEnvironmentFormatName(entry.format));
            if (assetField(fields, "width"))
                out.writeU32("width", entry.width);
            if (assetField(fields, "height"))
                out.writeU32("height", entry.height);
            if (assetField(fields, "mips"))
                out.writeU32("mips", entry.mipLevels);
            if (assetField(fields, "bytes"))
                out.writeU64("bytes", entry.pixelBytes);
        }

        // a skeleton's model and joint count, a clip's skeleton, model, length and track count: the fields of the
        // listing
        void writeSkeleton(const AssetManifest::SkeletonEntry& entry, IFormatWriter& out, const AssetFields fields)
        {
            if (assetField(fields, "model"))
                out.writeString("model", entry.model.toView());
            if (assetField(fields, "joints"))
                out.writeU32("joints", entry.jointCount);
        }

        void writeClip(const AssetManifest::AnimationEntry& entry, IFormatWriter& out, const AssetFields fields)
        {
            if (assetField(fields, "skeleton"))
                out.writeString("skeleton", entry.skeleton.toView());
            if (assetField(fields, "model"))
                out.writeString("model", entry.model.toView());
            if (assetField(fields, "duration"))
                out.writeF64("duration", entry.duration);
            if (assetField(fields, "tracks"))
                out.writeU32("tracks", entry.trackCount);
        }

        void writePlacementMetadata(const AssetManifest::ModelEntry& model, IFormatWriter& out,
                                    const AssetFields fields)
        {
            const std::filesystem::path source = pathFromUtf8(model.source.toView());
            if (source.empty() || source.is_absolute() ||
                std::ranges::any_of(source, [](const auto& part) { return part == ".."; }))
            {
                if (assetField(fields, "forward"))
                    out.writeString("forward", "unknown");
                if (assetField(fields, "connectionPointsSupported"))
                    out.writeBool("connectionPointsSupported", false);
                return;
            }
            auto sidecar = contentRoot() / source;
            sidecar += ".placement.json";
            const auto text = readTextFile(sidecar);
            const auto data = text ? nlohmann::json::parse(*text, nullptr, false) : nlohmann::json();
            if (!data.is_object())
            {
                if (assetField(fields, "forward"))
                    out.writeString("forward", "unknown");
                if (assetField(fields, "connectionPointsSupported"))
                    out.writeBool("connectionPointsSupported", false);
                return;
            }
            const auto forward = data.find("forward");
            if (assetField(fields, "forward"))
                out.writeString(
                    "forward", forward != data.end() && forward->is_string() ? forward->get<std::string>() : "unknown");
            const auto points = data.find("connectionPoints");
            const bool supported = points != data.end() && points->is_array();
            if (assetField(fields, "connectionPointsSupported"))
                out.writeBool("connectionPointsSupported", supported);
            if (!supported || !assetField(fields, "connectionPoints"))
                return;
            const auto vector = [](const nlohmann::json& value)
            {
                return value.is_array() && value.size() == 3 &&
                    std::ranges::all_of(
                           value, [](const auto& component)
                           { return component.is_number() && std::isfinite(component.template get<double>()); });
            };
            std::vector<const nlohmann::json*> valid;
            for (const auto& point : *points)
            {
                if (!point.is_object())
                    continue;
                const auto position = point.find("position"), direction = point.find("direction");
                if (position != point.end() && direction != point.end() && vector(*position) && vector(*direction))
                    valid.push_back(&point);
            }
            out.beginArray("connectionPoints", static_cast<u32>(valid.size()));
            for (const auto* item : valid)
            {
                const auto& point = *item;
                const auto position = point.find("position"), direction = point.find("direction");
                out.beginObject("");
                if (const auto name = point.find("name"); name != point.end() && name->is_string())
                    out.writeString("name", name->get<std::string>());
                for (const auto& [key, values] :
                     {std::pair{"position", &*position}, std::pair{"direction", &*direction}})
                {
                    out.beginArray(key, 3);
                    for (const auto& value : *values)
                        out.writeF64("", value.get<double>());
                    out.endArray();
                }
                out.endObject();
            }
            out.endArray();
        }

        void writeModelBounds(const InternedString& name, const AssetManifest& manifest, IFormatWriter& out,
                              const AssetFields fields)
        {
            const auto loaded = loadBakedModelDescriptor(name, manifest);
            if (!loaded)
            {
                if (assetField(fields, "boundsError"))
                    out.writeString("boundsError", loaded.error().message);
                return;
            }
            const auto& model = **loaded;
            if (model.rootNodeIndex >= model.nodes.size())
                return;
            const f32 inf = std::numeric_limits<f32>::infinity();
            Vec3 low{inf, inf, inf}, high{-inf, -inf, -inf};
            bool found = false;
            std::vector<std::pair<u32, Mat4>> pending{{model.rootNodeIndex, Mat4::identity()}};
            std::vector<bool> visited(model.nodes.size(), false);
            while (!pending.empty())
            {
                const auto [index, parent] = pending.back();
                pending.pop_back();
                if (index >= model.nodes.size() || visited[index])
                    continue;
                visited[index] = true;
                const auto& node = model.nodes[index];
                const auto& t = node.localTransform;
                const Mat4 world = parent * trs(t.position, t.rotation, t.scale);
                for (const u32 mesh : node.meshIndices)
                {
                    if (mesh >= model.submeshes.size())
                        continue;
                    const auto& box = model.submeshes[mesh].localBounds;
                    for (u32 corner = 0; corner < 8; ++corner)
                    {
                        const Vec3 point =
                            multiplyPoint(world,
                                          Vec3{corner & 1 ? box.max.x : box.min.x, corner & 2 ? box.max.y : box.min.y,
                                               corner & 4 ? box.max.z : box.min.z});
                        low = {std::min(low.x, point.x), std::min(low.y, point.y), std::min(low.z, point.z)};
                        high = {std::max(high.x, point.x), std::max(high.y, point.y), std::max(high.z, point.z)};
                        found = true;
                    }
                }
                for (const u32 child : node.childrenIndices)
                    pending.emplace_back(child, world);
            }
            if (!found)
                return;
            if (assetField(fields, "bounds"))
            {
                out.beginObject("bounds");
                out.beginArray("min", 3);
                out.writeF32("", low.x);
                out.writeF32("", low.y);
                out.writeF32("", low.z);
                out.endArray();
                out.beginArray("max", 3);
                out.writeF32("", high.x);
                out.writeF32("", high.y);
                out.writeF32("", high.z);
                out.endArray();
                out.endObject();
            }
            if (assetField(fields, "groundOffset"))
                out.writeF32("groundOffset", -low.y);
        }

        std::string listAssets(const AssetRuntime& assets, const ControlArgs& args, IFormatWriter& out)
        {
            const AssetKind* only = nullptr;
            bool atlases = true;
            const std::optional<std::string> type = args.string("type");
            if (type)
            {
                atlases = *type == SPRITE_ATLAS_KIND;
                if (!atlases)
                {
                    if (std::string error = kindArg(args, only, false); !error.empty())
                    {
                        return error;
                    }
                }
            }
            const auto text = args.string("text");
            const auto offsetArg = args.integer("offset");
            const auto limitArg = args.integer("limit");
            if (limitArg && *limitArg > 1000)
                return "asset limit must be between 0 and 1000";
            std::vector<std::string> fields;
            if (const auto projection = args.json("fields"))
            {
                constexpr std::string_view names[]{"name",
                                                   "type",
                                                   "status",
                                                   "refs",
                                                   "spaces",
                                                   "baked",
                                                   "source",
                                                   "authoringSource",
                                                   "format",
                                                   "width",
                                                   "height",
                                                   "mips",
                                                   "bytes",
                                                   "fileBytes",
                                                   "frameCount",
                                                   "tileMetres",
                                                   "framesPerSecond",
                                                   "maxDisplacementMetres",
                                                   "loop",
                                                   "skeleton",
                                                   "clips",
                                                   "materials",
                                                   "model",
                                                   "joints",
                                                   "duration",
                                                   "tracks",
                                                   "forward",
                                                   "connectionPointsSupported",
                                                   "connectionPoints",
                                                   "bounds",
                                                   "boundsError",
                                                   "groundOffset",
                                                   "texture",
                                                   "sprites"};
                const auto parsed = nlohmann::json::parse(*projection, nullptr, false);
                if (!parsed.is_array() || parsed.empty() || parsed.size() > std::size(names))
                    return "asset fields must be a nonempty array of supported field names";
                for (const auto& field : parsed)
                {
                    if (!field.is_string())
                        return "asset fields must be strings";
                    const auto& name = field.get_ref<const std::string&>();
                    if (!std::ranges::contains(names, std::string_view(name)))
                        return "unknown asset field '" + name + "'";
                    fields.push_back(name);
                }
            }
            const auto want = [&fields](const std::string_view field) { return assetField(fields, field); };
            const bool bounds = args.boolean("bounds").value_or(false) &&
                (want("bounds") || want("boundsError") || want("groundOffset"));
            const bool placement = args.boolean("placement").value_or(false) &&
                (want("forward") || want("connectionPointsSupported") || want("connectionPoints"));
            const auto lower = [](const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
            const u64 offset = offsetArg.value_or(0);
            const u64 limit = limitArg.value_or(std::numeric_limits<u64>::max());
            u64 total = 0, written = 0;
            const auto take = [&](const std::string_view name)
            {
                if (text && !text->empty() && std::ranges::search(name, *text, {}, lower, lower).empty())
                    return false;
                if (total++ < offset || written == limit)
                    return false;
                ++written;
                return true;
            };
            const AssetManifest& manifest = assets.manifest();
            out.beginArray("assets", 0);
            for (const AssetKind& kind : KINDS)
            {
                if (only ? only != &kind : type.has_value())
                {
                    continue;
                }
                std::vector<std::pair<std::string_view, InternedString>> rows;
                const auto collect = [&rows](const auto& entries)
                {
                    for (const auto& [name, entry] : entries)
                    {
                        rows.emplace_back(name.toView(), entry.baked);
                    }
                };
                switch (kind.type)
                {
                case AssetType::MESH:
                    collect(manifest.getModels());
                    break;
                case AssetType::MATERIAL:
                    collect(manifest.getMaterials());
                    break;
                case AssetType::ENVIRONMENT:
                    collect(manifest.getEnvironments());
                    break;
                case AssetType::OCEAN_PROFILE:
                    collect(manifest.getOceanProfiles());
                    break;
                case AssetType::SKELETON:
                    collect(manifest.getSkeletons());
                    break;
                case AssetType::ANIMATION:
                    collect(manifest.getAnimations());
                    break;
                default:
                    collect(manifest.getTextures());
                    break;
                }
                std::ranges::sort(rows, {}, [](const auto& row) { return row.first; });
                for (const auto& [name, baked] : rows)
                {
                    if (!take(name))
                        continue;
                    const InternedString interned(std::string(name).c_str());
                    const AssetHandle handle{interned, kind.type, 0};
                    out.beginObject({});
                    if (want("name"))
                        out.writeString("name", name);
                    if (want("type"))
                        out.writeString("type", kind.name);
                    if (kind.type == AssetType::TEXTURE)
                    {
                        if (want("status") || want("refs") || want("spaces"))
                        {
                            const AssetRuntime::TextureState state = assets.textureState(interned);
                            if (want("status"))
                                out.writeString("status", statusName(state.status));
                            if (want("refs"))
                                out.writeU32("refs", state.refs);
                            if (want("spaces"))
                                writeSpaces(state, out);
                        }
                    }
                    else
                    {
                        if (want("status"))
                            out.writeString("status", statusName(assets.status(handle)));
                        if (want("refs"))
                            out.writeU32("refs", assets.registry().getRefCount(handle));
                    }
                    if (want("baked"))
                        out.writeString("baked", baked.toView());
                    if (kind.type == AssetType::ENVIRONMENT)
                    {
                        writeEnvironment(*manifest.getEnvironmentEntry(interned), out, fields);
                    }
                    if (kind.type == AssetType::OCEAN_PROFILE)
                    {
                        const auto entry = *manifest.getOceanProfileEntry(interned);
                        if (want("width"))
                            out.writeU32("width", entry.width);
                        if (want("height"))
                            out.writeU32("height", entry.height);
                        if (want("frameCount"))
                            out.writeU32("frameCount", entry.frameCount);
                        if (want("tileMetres"))
                            out.writeF32("tileMetres", entry.tileMetres);
                        if (want("framesPerSecond"))
                            out.writeF32("framesPerSecond", entry.framesPerSecond);
                        if (want("maxDisplacementMetres"))
                            out.writeF32("maxDisplacementMetres", entry.maxDisplacementMetres);
                        if (want("loop"))
                            out.writeBool("loop", entry.loop);
                    }
                    if (const auto texture =
                            kind.type == AssetType::TEXTURE ? manifest.getTextureEntry(interned) : std::nullopt)
                    {
                        if (want("format"))
                            out.writeString("format",
                                            bakedTextureFormatName(texture->format != 0
                                                                       ? texture->format
                                                                       : BAKED_TEXTURE_FORMAT_R8G8B8A8_SRGB));
                        if (want("bytes"))
                            out.writeU64("bytes", texture->pixelBytes);
                        if (want("fileBytes"))
                            out.writeU64("fileBytes",
                                         texture->fileBytes ? texture->fileBytes
                                                            : texture->pixelBytes + sizeof(SpiteTextureHeader));
                    }
                    if (kind.type == AssetType::SKELETON)
                    {
                        writeSkeleton(*manifest.getSkeletonEntry(interned), out, fields);
                    }
                    if (kind.type == AssetType::ANIMATION)
                    {
                        writeClip(*manifest.getAnimationEntry(interned), out, fields);
                    }
                    if (kind.type == AssetType::MESH)
                    {
                        if (placement || want("source") || want("authoringSource") || want("skeleton") || want("clips"))
                        {
                            const auto model = *manifest.getModelEntry(interned);
                            if (want("source"))
                                out.writeString("source", model.source.toView());
                            if (want("authoringSource"))
                                out.writeString("authoringSource", model.authoringSource.toView());
                            if (placement)
                                writePlacementMetadata(model, out, fields);
                            if (model.skeleton.isValid())
                            {
                                if (want("skeleton"))
                                    out.writeString("skeleton", model.skeleton.toView());
                                if (want("clips"))
                                {
                                    out.beginArray("clips", static_cast<u32>(model.clips.size()));
                                    for (const InternedString& clip : model.clips)
                                        out.writeString("", clip.toView());
                                    out.endArray();
                                }
                            }
                        }
                        if (bounds)
                            writeModelBounds(interned, manifest, out, fields);
                        if (want("materials"))
                        {
                            const std::vector<InternedString> materials = manifest.getModelMaterials(interned);
                            out.beginArray("materials", static_cast<u32>(materials.size()));
                            for (const InternedString& material : materials)
                                out.writeString("", material.toView());
                            out.endArray();
                        }
                    }
                    out.endObject();
                }
            }
            if (atlases)
            {
                const auto entries = manifest.getSpriteAtlases();
                std::vector<std::pair<std::string_view, AssetManifest::SpriteAtlasEntry>> rows(entries.size());
                std::ranges::transform(entries, rows.begin(),
                                       [](const auto& row) { return std::pair(row.first.toView(), row.second); });
                std::ranges::sort(rows, {}, [](const auto& row) { return row.first; });
                for (const auto& [name, entry] : rows)
                {
                    if (!take(name))
                        continue;
                    out.beginObject({});
                    if (want("name"))
                        out.writeString("name", name);
                    if (want("type"))
                        out.writeString("type", SPRITE_ATLAS_KIND);
                    if (want("status") || want("spaces"))
                    {
                        const AssetRuntime::TextureState page = assets.textureState(entry.texture);
                        if (want("status"))
                            out.writeString("status", statusName(page.status));
                        if (want("spaces"))
                            writeSpaces(page, out);
                    }
                    if (want("baked"))
                        out.writeString("baked", entry.baked.toView());
                    if (want("texture"))
                        out.writeString("texture", entry.texture.toView());
                    if (want("sprites"))
                        out.writeU32("sprites", entry.spriteCount);
                    if (want("width"))
                        out.writeU32("width", entry.pageWidth);
                    if (want("height"))
                        out.writeU32("height", entry.pageHeight);
                    out.endObject();
                }
            }
            out.endArray();
            if (text || offsetArg || limitArg || !fields.empty())
            {
                out.writeU64("total", total);
                if (written != 0 && written < total - offset)
                    out.writeU64("nextOffset", offset + written);
            }
            return {};
        }

        std::string material(const AssetRuntime& assets, const ControlArgs& args, IFormatWriter& out)
        {
            std::string name;
            if (std::string error = nameArg(args, "name", name); !error.empty())
            {
                return error;
            }
            BakedMaterialWriteData fields;
            if (std::string error = readMaterial(assets, name, fields); !error.empty())
            {
                return error;
            }
            out.writeString("name", name);
            out.writeString("status",
                            statusName(assets.status({InternedString(name.c_str()), AssetType::MATERIAL, 0})));
            writeFields(fields, "fields", out);
            return {};
        }

        std::string loadAsset(AssetRuntime& assets, const ControlArgs& args, IFormatWriter& out)
        {
            const AssetKind* kind = nullptr;
            std::string name;
            if (std::string error = kindArg(args, kind); !error.empty())
            {
                return error;
            }
            if (std::string error = nameArg(args, "name", name); !error.empty())
            {
                return error;
            }
            const InternedString interned(name.c_str());
            if (!inManifest(assets.manifest(), kind->type, interned))
            {
                return "no " + std::string(kind->name) + " '" + name + "' in the manifest";
            }
            u32 subAssets = 1;
            if (kind->type == AssetType::MESH)
            {
                const auto descriptor = loadBakedModelDescriptor(interned, assets.manifest());
                if (!descriptor)
                {
                    return "cannot read model '" + name + "': " + descriptor.error().message;
                }
                subAssets = static_cast<u32>((*descriptor)->submeshes.size());
            }
            const std::optional<u64> priority = args.integer("priority"); // clamped to u32
            // a texture already held in a colour space (a normal map loads linear) is not loaded again in sRGB
            const bool held = kind->type == AssetType::TEXTURE &&
                assets.textureState(interned).status != AssetRuntime::AssetStatus::Unloaded;
            for (u32 i = 0; i < (held ? 0 : subAssets); ++i)
            {
                const AssetHandle handle{interned, kind->type, i};
                priority && kind->type != AssetType::OCEAN_PROFILE
                    ? assets.prefetchAsset(handle, static_cast<u32>(std::min<u64>(*priority, ~0u)), subAssets)
                    : assets.load(handle);
            }
            out.writeString("name", name);
            out.writeString("type", kind->name);
            out.writeU32("subAssets", subAssets);
            if (priority && kind->type != AssetType::OCEAN_PROFILE)
            {
                out.writeU32("prefetchQueued", assets.residencyStats().prefetchQueued);
            }
            return {};
        }

        // The optional "parent" arg: a live, non-UI entity with a transform.
        std::string parentArg(const World& world, const ControlArgs& args, Entity& out)
        {
            out = Entity::undefined();
            if (!args.json("parent"))
            {
                return {};
            }
            const std::optional<u64> id = args.integer("parent");
            if (!id)
            {
                return "'parent' must be an entity id";
            }
            out = Entity(*id);
            if (!world.isEntityAlive(out) || world.hasComponent<UiIdentityComponent>(out) ||
                !world.hasComponent<TransformComponent>(out))
            {
                return "parent " + std::to_string(*id) + " is not a live scene entity with a TransformComponent";
            }
            return {};
        }

        struct ReimportChange
        {
            std::string object;
            std::string property;
            u64 entity = 0;
            std::string baseline;
            std::string current;
            std::string source;
            bool conflict = false;
        };

        struct ReimportPreview
        {
            u64 generation = 0;
            u64 assetRevision = 0;
            u64 sceneRevision = 0;
            std::vector<ReimportChange> changes;
        };

        struct PreparedVersions
        {
            std::shared_ptr<AssetRuntime::PreparedModel> candidate;
            std::shared_ptr<AssetRuntime::PreparedModel> baseline;
        };

        struct PreparationJob
        {
            std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
            std::string model;
            std::future<Expected<PreparedVersions>> work;
            std::shared_ptr<const AssetRuntime::PreparationSnapshot> snapshot;
            std::future<void> cleanup;
            std::future<Expected<std::string>> sceneWork;
            std::string candidateScene;
            std::string sceneError;
            bool sceneSettled = false;
            u64 sceneStateGeneration = 0, sceneManifestGeneration = 0, sceneSchemaRevision = 0;
            std::shared_ptr<AssetRuntime::PreparedModel> prepared;
            std::shared_ptr<AssetRuntime::PreparedModel> baseline;
            std::string error;
            bool settled = false;

            void poll()
            {
                if (sceneWork.valid() && sceneWork.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                {
                    sceneSettled = true;
                    try
                    {
                        auto result = sceneWork.get();
                        if (!cancelled->load())
                        {
                            if (result)
                                candidateScene = std::move(*result);
                            else
                                sceneError = result.error().message;
                        }
                    }
                    catch (const std::exception& exception)
                    {
                        sceneError = exception.what();
                    }
                    catch (...)
                    {
                        sceneError = "scene preparation failed with an unknown exception";
                    }
                }
                if (cleanup.valid() && cleanup.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                    cleanup.get();
                if (settled || !work.valid() || work.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                    return;
                settled = true;
                try
                {
                    auto result = work.get();
                    if (cancelled->load())
                    {
                        if (result)
                        {
                            prepared = std::move(result->candidate);
                            baseline = std::move(result->baseline);
                            discard();
                        }
                        return;
                    }
                    if (result)
                    {
                        prepared = std::move(result->candidate);
                        baseline = std::move(result->baseline);
                    }
                    else
                        error = result.error().message;
                }
                catch (const std::exception& exception)
                {
                    error = exception.what();
                }
                catch (...)
                {
                    error = "model preparation failed with an unknown exception";
                }
            }
            void discard()
            {
                if (!prepared && !baseline)
                    return;
                auto held = prepared;
                auto heldBaseline = baseline;
                std::promise<void> release;
                auto ready = release.get_future();
                cleanup = std::async(
                    std::launch::async,
                    [held = std::move(held), heldBaseline = std::move(heldBaseline), ready = std::move(ready)]() mutable
                    {
                        ready.wait();
                        held.reset();
                        heldBaseline.reset();
                    });
                prepared.reset();
                baseline.reset();
                release.set_value();
            }
            const char* status() const
            {
                if (cancelled->load())
                    return "cancelled";
                if (!settled)
                    return "preparing";
                return prepared ? "ready" : "failed";
            }
        };
        struct PreparationJobs
        {
            u64 next = 0;
            std::unordered_map<u64, std::shared_ptr<PreparationJob>> jobs;
        };
        struct FlipbookCardJob
        {
            std::future<FlipbookCardResult> work;
            FlipbookCardResult result;
            std::string path;
            bool settled = false;

            void poll(bool wait)
            {
                if (settled || !work.valid())
                    return;
                if (!wait && work.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                    return;
                try
                {
                    result = work.get();
                }
                catch (const std::exception& error)
                {
                    result.error = error.what();
                }
                catch (...)
                {
                    result.error = "flipbook card preparation failed";
                }
                settled = true;
            }
        };
        struct FlipbookCardJobs
        {
            u64 next = 0;
            std::unordered_map<u64, std::shared_ptr<FlipbookCardJob>> jobs;
        };
        struct ExternalAudioJob
        {
            std::future<AudioProbeResult> work;
            AudioProbeResult result;
            std::string name;
            bool settled = false;
        };
        struct ExternalAudioJobs
        {
            u64 next = 0;
            std::unordered_map<u64, std::shared_ptr<ExternalAudioJob>> jobs;
        };
        struct ExternalImageResult
        {
            std::string error;
            InternedString name;
            AssetManifest::TextureEntry texture;
            AssetManifest::EnvironmentEntry environment;
        };
        struct ExternalImageJob
        {
            std::future<ExternalImageResult> work;
            ExternalImageResult result;
            std::filesystem::path stage;
            std::filesystem::path stagedFile;
            std::filesystem::path textureDir, materialDir, environmentDir;
            bool environment = false;
            bool settled = false;

            void cleanup()
            {
                std::error_code error;
                if (stage.empty() || std::filesystem::weakly_canonical(stage, error) != stage || error)
                    return;
                const auto inside = [&](const std::filesystem::path& path)
                {
                    const auto lexical = path.lexically_normal().lexically_relative(stage);
                    if (lexical.empty() || lexical == "." || *lexical.begin() == "..")
                        return false;
                    std::error_code check;
                    const auto canonical = std::filesystem::weakly_canonical(path, check);
                    if (check)
                        return false;
                    const auto relative = canonical.lexically_relative(stage);
                    return !relative.empty() && relative != "." && *relative.begin() != "..";
                };
                if (!stagedFile.empty() && inside(stagedFile))
                    std::filesystem::remove(stagedFile, error);
                const auto manifest = stage / "asset_manifest.json";
                if (inside(manifest))
                    std::filesystem::remove(manifest, error);
                for (const auto& part : {textureDir, materialDir, environmentDir})
                {
                    if (part.is_absolute())
                        continue;
                    auto directory = (stage / part).lexically_normal();
                    while (inside(directory))
                    {
                        std::filesystem::remove(directory, error);
                        directory = directory.parent_path();
                    }
                }
                std::filesystem::remove(stage, error);
            }
            ~ExternalImageJob()
            {
                if (work.valid())
                {
                    try
                    {
                        const auto remaining = work.get();
                        const auto baked = environment ? remaining.environment.baked : remaining.texture.baked;
                        if (baked.isValid())
                            stagedFile = pathFromUtf8(baked.toView());
                    }
                    catch (...)
                    {
                    }
                }
                cleanup();
            }
        };
        struct ExternalImageJobs
        {
            u64 next = 0;
            std::unordered_map<u64, std::shared_ptr<ExternalImageJob>> jobs;
        };
        bool belongsToImportedInstance(const World& world, EntityTransformManager& hierarchy, const Entity root,
                                       const Entity entity, const ImportedSourceComponent& source)
        {
            if (source.instanceKey.isValid())
                return source.instanceKey == world.getConstComponent<ImportedSourceComponent>(root).instanceKey;
            // Legacy scenes lack the explicit owner: only descendants without an intervening instance are safe.
            for (Entity parent = entity; parent.isValid(); parent = hierarchy.currentParentOf(parent))
            {
                if (parent == root)
                    return true;
                if (world.hasComponent<ImportedSourceComponent>(parent) &&
                    world.getConstComponent<ImportedSourceComponent>(parent).instanceRoot)
                    return false;
            }
            return false;
        }

        std::string collectImportedInstance(const World& world, EntityTransformManager& hierarchy, const Entity root,
                                            std::unordered_map<std::string, Entity>& entities,
                                            std::unordered_set<u64>* owned = nullptr)
        {
            const auto& rootSource = world.getConstComponent<ImportedSourceComponent>(root);
            const ComponentID id = getComponentId<ImportedSourceComponent>();
            bool duplicateSource = false;
            for (const Chunk* chunk : world.getChunksContaining(Aspect(id)))
                for (const Entity entity : chunk->entities())
                {
                    const auto& source = world.getConstComponent<ImportedSourceComponent>(entity);
                    if (source.document != rootSource.document ||
                        !belongsToImportedInstance(world, hierarchy, root, entity, source))
                        continue;
                    if (entity != root && source.instanceRoot)
                        return "duplicate imported instance identity";
                    if (owned)
                        owned->insert(entity.id());
                    if (source.generatedMesh || !source.object.isValid())
                        continue;
                    duplicateSource |= !entities.emplace(std::string(source.object.toView()), entity).second;
                }
            return duplicateSource ? "duplicate source identity in instance" : std::string{};
        }

        std::string previewInstance(ControlServer& control, AssetRuntime& assets, const ControlArgs& args,
                                    ReimportPreview& preview, const AssetRuntime::PreparedModel* prepared = nullptr)
        {
            const World* world = control.world(args);
            if (!world)
                return control.unknownWorld(args);
            const auto id = args.integer("id");
            const auto modelName = args.string("model");
            if (!id || !modelName)
                return "id and model are required";
            const Entity root(*id);
            if (!world->isEntityAlive(root) || !world->hasComponent<ImportedSourceComponent>(root) ||
                !world->getConstComponent<ImportedSourceComponent>(root).instanceRoot)
                return "id is not an imported instance root";
            if (!world->services().contains<EntityTransformManager>())
                return "world has no hierarchy";
            const auto modelKey = InternedString(*modelName);
            const auto entry = assets.baker().getManifest().getModelEntry(modelKey);
            if (!entry)
                return "model is not in the manifest";
            const auto& rootSource = world->getConstComponent<ImportedSourceComponent>(root);
            if ((entry->authoringSource.isValid() ? entry->authoringSource : entry->source) != rootSource.document)
                return "model source does not match the instance document";
            if (prepared &&
                (prepared->manifestGeneration != assets.manifest().generation() || !prepared->hierarchy ||
                 prepared->hierarchy->name != modelKey))
                return "prepared asset baseline is stale or belongs to a different model";
            const Expected<std::shared_ptr<ModelHierarchyDescriptor>> descriptor = prepared
                ? Expected<std::shared_ptr<ModelHierarchyDescriptor>>(prepared->hierarchy)
                : loadBakedModelDescriptor(modelKey, assets.baker().getManifest());
            if (!descriptor)
                return descriptor.error().message;
            const Expected<std::vector<u8>> instantiated = prepared
                ? Expected<std::vector<u8>>(prepared->instantiatedNodes)
                : assets.instantiatedNodeMask(modelKey);
            if (!instantiated)
                return instantiated.error().message;
            if (prepared)
            {
                const auto fingerprint = prepared->baselineModelFingerprint.toView();
                const auto parsed =
                    std::from_chars(fingerprint.data(), fingerprint.data() + fingerprint.size(), preview.assetRevision);
                if (parsed.ec != std::errc{} || parsed.ptr != fingerprint.data() + fingerprint.size())
                    return "prepared asset baseline fingerprint is missing";
            }
            else
            {
                const auto bytes = readBinaryFile(resolveContentPath(entry->baked.toView()));
                if (!bytes)
                    return bytes.error().message;
                preview.assetRevision = 1469598103934665603ull;
                for (const std::byte byte : *bytes)
                    preview.assetRevision = (preview.assetRevision ^ std::to_integer<u8>(byte)) * 1099511628211ull;
            }
            preview.generation = control.stateGeneration();

            auto& hierarchy = world->services().write<EntityTransformManager>().get();
            const auto document = collectWorldScene(const_cast<World&>(*world), hierarchy);
            if (!document)
                return document.error().message;
            std::string snapshot;
            emitSceneJson(*document, snapshot);
            preview.sceneRevision = 1469598103934665603ull;
            for (const unsigned char byte : snapshot)
                preview.sceneRevision = (preview.sceneRevision ^ byte) * 1099511628211ull;
            std::unordered_map<std::string, Entity> existing;
            if (std::string error = collectImportedInstance(*world, hierarchy, root, existing); !error.empty())
                return error;

            const auto& nodes = (*descriptor)->nodes;
            std::vector<i32> parents(nodes.size(), -1);
            for (u32 i = 0; i < nodes.size(); ++i)
                for (const u32 child : nodes[i].childrenIndices)
                    if (child < nodes.size())
                        parents[child] = static_cast<i32>(i);
            std::unordered_map<std::string, u32> incoming;
            for (u32 i = 0; i < nodes.size(); ++i)
            {
                const auto& node = nodes[i];
                if (i >= instantiated->size() || !(*instantiated)[i])
                    continue;
                if (!node.sourceObject.isValid())
                    continue;
                if (!incoming.emplace(std::string(node.sourceObject.toView()), i).second)
                    return "duplicate source identity in model";
            }
            const auto add = [&](const std::string& object, const std::string& property, const Entity entity,
                                 const auto& before, const auto& current, const auto& next)
            {
                if (before == next)
                    return;
                preview.changes.push_back({object, property, entity.id(), nlohmann::json(before).dump(),
                                           nlohmann::json(current).dump(), nlohmann::json(next).dump(),
                                           current != before && current != next});
            };
            const auto vec = [](const Vec3 v) { return std::array<float, 3>{v.x, v.y, v.z}; };
            const auto quat = [](const Quat q) { return std::array<float, 4>{q.x, q.y, q.z, q.w}; };
            for (const auto& [object, index] : incoming)
            {
                const auto& node = nodes[index];
                const auto old = existing.find(object);
                if (!node.sourceObjectStable ||
                    (old != existing.end() && !world->getConstComponent<ImportedSourceComponent>(old->second).stable))
                {
                    if (old != existing.end() && node.meshIndices.size() <= 1)
                    {
                        const auto& base = world->getConstComponent<ImportedSourceComponent>(old->second);
                        const i32 p = parents[index];
                        const InternedString parentId = p >= 0 ? nodes[p].sourceObject : InternedString{};
                        AssetHandle mesh, material;
                        if (node.meshIndices.size() == 1)
                        {
                            const u32 submesh = node.meshIndices[0];
                            mesh = {modelKey, AssetType::MESH, submesh};
                            if (submesh < (*descriptor)->submeshes.size())
                            {
                                const u32 materialIndex = (*descriptor)->submeshes[submesh].materialIndex;
                                if (materialIndex < (*descriptor)->materialNames.size())
                                    material = {(*descriptor)->materialNames[materialIndex], AssetType::MATERIAL, 0};
                            }
                        }
                        const auto& value = node.imported;
                        if (base.sourceName == node.name && base.parentObject == parentId &&
                            base.sourcePosition == node.localTransform.position &&
                            base.sourceRotation == node.localTransform.rotation &&
                            base.sourceScale == node.localTransform.scale && base.sourceMesh == mesh &&
                            base.sourceMaterial == material && base.sourceCamera == value.camera &&
                            base.sourceLight == value.light && base.sourceFov == value.fov &&
                            base.sourceNear == value.nearPlane && base.sourceFar == value.farPlane &&
                            base.sourceColor == Vec3(value.color[0], value.color[1], value.color[2]) &&
                            base.sourceIntensity == value.intensity && base.sourceRange == value.range &&
                            base.sourceInnerCone == value.innerCone && base.sourceOuterCone == value.outerCone)
                            continue;
                    }
                    preview.changes.push_back(
                        {object, "unstable_identity", old == existing.end() ? 0 : old->second.id(), {}, {}, {}, true});
                    continue;
                }
                if (old == existing.end())
                {
                    const ComponentID sourceId = getComponentId<ImportedSourceComponent>();
                    for (const Chunk* chunk : world->getChunksContaining(Aspect(sourceId)))
                        for (const Entity candidate : chunk->entities())
                        {
                            const auto& legacy = world->getConstComponent<ImportedSourceComponent>(candidate);
                            if (!legacy.instanceKey.isValid() && !legacy.generatedMesh &&
                                legacy.document == rootSource.document && legacy.object == node.sourceObject)
                                return "ambiguous legacy source identity outside the instance";
                        }
                    preview.changes.push_back({object, "add", 0, {}, {}, std::string(node.name.toView()), false});
                    continue;
                }
                const Entity entity = old->second;
                if (!world->hasComponent<TransformComponent>(entity))
                    return "imported entity has no transform";
                const auto& baseline = world->getConstComponent<ImportedSourceComponent>(entity);
                const auto& transform = world->getConstComponent<TransformComponent>(entity);
                add(object, "name", entity, std::string(baseline.sourceName.toView()),
                    std::string(entityName(*world, entity)), std::string(node.name.toView()));
                add(object, "position", entity, vec(baseline.sourcePosition), vec(transform.position),
                    vec(node.localTransform.position));
                add(object, "rotation", entity, quat(baseline.sourceRotation), quat(transform.rotation),
                    quat(node.localTransform.rotation));
                add(object, "scale", entity, vec(baseline.sourceScale), vec(transform.scale),
                    vec(node.localTransform.scale));
                const i32 p = parents[index];
                const std::string newParent = p >= 0 ? std::string(nodes[p].sourceObject.toView()) : "";
                const Entity currentParent = hierarchy.currentParentOf(entity);
                const std::string oldParent(baseline.parentObject.toView());
                if (oldParent != newParent)
                {
                    nlohmann::json current;
                    std::string scopedParent;
                    if (!currentParent.isValid())
                        current = {{"kind", "world"}};
                    else if (currentParent == root)
                    {
                        current = {{"kind", "instanceRoot"}};
                        scopedParent = rootSource.object.toView();
                    }
                    else
                    {
                        const auto match = std::ranges::find_if(existing, [&](const auto& entry)
                                                                { return entry.second == currentParent; });
                        if (match != existing.end())
                        {
                            scopedParent = match->first;
                            current = {{"kind", "source"}, {"object", scopedParent}};
                        }
                        else
                            current = {{"kind", "local"}, {"entity", currentParent.id()}};
                    }
                    const bool scoped =
                        currentParent == root || (currentParent.isValid() && current.value("kind", "") == "source");
                    preview.changes.push_back({object, "parent", entity.id(), nlohmann::json(oldParent).dump(),
                                               current.dump(), nlohmann::json(newParent).dump(),
                                               !scoped || (scopedParent != oldParent && scopedParent != newParent)});
                }
                const auto handleValue = [](const AssetHandle& handle) -> nlohmann::json
                {
                    if (!handle.isValid())
                        return nullptr;
                    return {{"name", handle.name.toView()},
                            {"assetType", static_cast<u32>(handle.assetType)},
                            {"subAssetIndex", handle.subAssetIndex}};
                };
                if (node.meshIndices.size() > 1)
                    preview.changes.push_back({object, "multi_mesh", entity.id(), {}, {}, {}, true});
                else
                {
                    AssetHandle nextMesh, nextMaterial, currentMesh, currentMaterial;
                    if (node.meshIndices.size() == 1)
                    {
                        const u32 mesh = node.meshIndices[0];
                        nextMesh = {modelKey, AssetType::MESH, mesh};
                        if (mesh < (*descriptor)->submeshes.size())
                        {
                            const u32 material = (*descriptor)->submeshes[mesh].materialIndex;
                            if (material < (*descriptor)->materialNames.size())
                                nextMaterial = {(*descriptor)->materialNames[material], AssetType::MATERIAL, 0};
                        }
                    }
                    if (world->hasComponent<MeshComponent>(entity))
                        currentMesh = world->getConstComponent<MeshComponent>(entity).meshHandle;
                    if (world->hasComponent<MaterialComponent>(entity))
                        currentMaterial = world->getConstComponent<MaterialComponent>(entity).materialHandle;
                    add(object, "mesh", entity, handleValue(baseline.sourceMesh), handleValue(currentMesh),
                        handleValue(nextMesh));
                    add(object, "material", entity, handleValue(baseline.sourceMaterial), handleValue(currentMaterial),
                        handleValue(nextMaterial));
                }
                nlohmann::json beforeCamera = nullptr, currentCamera = nullptr, nextCamera = nullptr;
                if (baseline.sourceCamera)
                    beforeCamera = {baseline.sourceFov, baseline.sourceNear, baseline.sourceFar};
                if (world->hasComponent<CameraComponent>(entity))
                {
                    const auto& camera = world->getConstComponent<CameraComponent>(entity);
                    currentCamera = {camera.fov, camera.nearPlane, camera.farPlane};
                }
                if (node.imported.camera)
                    nextCamera = {node.imported.fov, node.imported.nearPlane, node.imported.farPlane};
                add(object, "camera", entity, beforeCamera, currentCamera, nextCamera);
                if (nextCamera.is_null() && world->hasComponent<CameraComponent>(entity) && !preview.changes.empty() &&
                    preview.changes.back().object == object && preview.changes.back().property == "camera")
                {
                    const auto& camera = world->getConstComponent<CameraComponent>(entity);
                    if (camera.exposure != 0.f || camera.autoExposure)
                        preview.changes.back().conflict = true;
                }
                nlohmann::json beforeLight = nullptr, currentLight = nullptr, nextLight = nullptr;
                if (baseline.sourceLight)
                    beforeLight = {baseline.sourceLight, vec(baseline.sourceColor), baseline.sourceIntensity,
                                   baseline.sourceRange, baseline.sourceInnerCone,  baseline.sourceOuterCone};
                if (world->hasComponent<DirectionalLightComponent>(entity))
                {
                    const auto& light = world->getConstComponent<DirectionalLightComponent>(entity);
                    currentLight = {1,
                                    vec(light.color),
                                    light.intensity,
                                    baseline.sourceRange,
                                    baseline.sourceInnerCone,
                                    baseline.sourceOuterCone};
                }
                else if (world->hasComponent<PointLightComponent>(entity))
                {
                    const auto& light = world->getConstComponent<PointLightComponent>(entity);
                    currentLight = {2,
                                    vec(light.color),
                                    light.intensity,
                                    light.radius,
                                    baseline.sourceInnerCone,
                                    baseline.sourceOuterCone};
                }
                else if (world->hasComponent<SpotLightComponent>(entity))
                {
                    const auto& light = world->getConstComponent<SpotLightComponent>(entity);
                    currentLight = {
                        3, vec(light.color), light.intensity, light.radius, light.innerConeAngle, light.outerConeAngle};
                }
                if (node.imported.light)
                    nextLight = {
                        node.imported.light,
                        std::array<float, 3>{node.imported.color[0], node.imported.color[1], node.imported.color[2]},
                        node.imported.intensity,
                        node.imported.range,
                        node.imported.innerCone,
                        node.imported.outerCone};
                add(object, "light", entity, beforeLight, currentLight, nextLight);
                if ((!nextLight.is_array() || (beforeLight.is_array() && nextLight[0] != beforeLight[0])) &&
                    !preview.changes.empty() && preview.changes.back().object == object &&
                    preview.changes.back().property == "light")
                {
                    const bool shadows = world->hasComponent<DirectionalLightComponent>(entity)
                        ? world->getConstComponent<DirectionalLightComponent>(entity).castShadows
                        : world->hasComponent<PointLightComponent>(entity)
                        ? world->getConstComponent<PointLightComponent>(entity).castShadows
                        : world->hasComponent<SpotLightComponent>(entity) &&
                            world->getConstComponent<SpotLightComponent>(entity).castShadows;
                    if (shadows)
                        preview.changes.back().conflict = true;
                }
            }
            for (const auto& [object, entity] : existing)
                if (!incoming.contains(object))
                {
                    u32 localChildren = 0;
                    std::vector<Entity> descendants;
                    hierarchy.collectSubtree(entity, descendants);
                    for (const Entity child : descendants)
                        if (hierarchy.currentParentOf(child) == entity &&
                            (!world->hasComponent<ImportedSourceComponent>(child) ||
                             world->getConstComponent<ImportedSourceComponent>(child).instanceRoot))
                            ++localChildren;
                    const Entity removed[] = {entity};
                    const nlohmann::json details{{"localChildren", localChildren},
                                                 {"externalReferences", hasExternalSceneReferences(*world, removed)}};
                    preview.changes.push_back({object, "remove", entity.id(), {}, details.dump(), {}, true});
                }
            std::ranges::sort(
                preview.changes, [](const auto& a, const auto& b)
                { return std::tie(a.object, a.property, a.entity) < std::tie(b.object, b.property, b.entity); });
            return {};
        }

        std::string previewInstanceQuery(ControlServer& control, AssetRuntime& assets, const ControlArgs& args,
                                         IFormatWriter& out, const AssetRuntime::PreparedModel* prepared = nullptr)
        {
            ReimportPreview preview;
            if (std::string error = previewInstance(control, assets, args, preview, prepared); !error.empty())
                return error;
            out.writeU64("generation", preview.generation);
            out.writeString("assetRevision", std::format("{:016x}", preview.assetRevision));
            out.writeString("sceneRevision", std::format("{:016x}", preview.sceneRevision));
            out.beginArray("changes", static_cast<u32>(preview.changes.size()));
            for (const auto& change : preview.changes)
            {
                out.beginObject({});
                out.writeString("object", change.object);
                out.writeString("property", change.property);
                if (change.entity)
                    out.writeU64("entity", change.entity);
                out.writeString("baseline", change.baseline);
                out.writeString("current", change.current);
                out.writeString("source", change.source);
                out.writeBool("conflict", change.conflict);
                out.endObject();
            }
            out.endArray();
            return {};
        }

        std::string provenanceJson(const ImportedSourceComponent& value)
        {
            JsonFragmentWriter writer;
            writeValue(ImportedSourceComponent::spiteSchema(), &value, writer);
            std::string text;
            writer.dump(text);
            return text;
        }

        std::string setImportedBaseline(ControlServer& control, const ControlArgs& args, IFormatWriter&,
                                        const bool applying)
        {
            if (!applying && !control.replayingHistory())
                return "internal reconciliation command";
            const World* world = control.world(args);
            if (!world)
                return control.unknownWorld(args);
            const auto id = args.integer("id");
            const auto value = args.json("value");
            if (!id || !value)
                return "baseline needs id and value";
            const Entity entity(*id);
            if (!world->isEntityAlive(entity) || !world->hasComponent<ImportedSourceComponent>(entity))
                return "imported entity is gone";
            auto previous = world->getComponent<ImportedSourceComponent>(entity);
            auto next = previous;
            std::string error;
            JsonFragmentReader reader(*value);
            if (!readValue(ImportedSourceComponent::spiteSchema(), &next, reader, error))
                return "invalid baseline: " + error;
            world->getComponent<ImportedSourceComponent>(entity) = next;
            control.recordInverse(
                "setImportedBaseline",
                nlohmann::json{{"id", *id}, {"value", nlohmann::json::parse(provenanceJson(previous))}}.dump());
            return {};
        }

        std::string createImportedNodes(ControlServer& control, const ControlArgs& args, IFormatWriter&,
                                        const bool applying)
        {
            if (!applying && !control.replayingHistory())
                return "internal reconciliation command";
            const World* world = control.world(args);
            if (!world)
                return control.unknownWorld(args);
            const auto id = args.integer("id");
            const auto text = args.json("nodes");
            if (!id || !text)
                return "creation needs id and nodes";
            const Entity root(*id);
            if (!world->isEntityAlive(root) || !world->hasComponent<ImportedSourceComponent>(root))
                return "imported instance is gone";
            auto& hierarchy = world->services().write<EntityTransformManager>().get();
            const auto list = nlohmann::json::parse(*text, nullptr, false);
            if (!list.is_array())
                return "nodes must be an array";
            std::vector<ImportedSourceComponent> values;
            for (const auto& item : list)
            {
                if (!item.is_object())
                    return "invalid source node";
                ImportedSourceComponent value;
                JsonFragmentReader reader(item.dump());
                std::string error;
                if (!readValue(ImportedSourceComponent::spiteSchema(), &value, reader, error) ||
                    !value.object.isValid() || !value.stable || value.instanceRoot || value.generatedMesh)
                    return "invalid source node: " + error;
                values.push_back(value);
            }
            const auto movesText = args.json("moves");
            const auto moves = movesText ? nlohmann::json::parse(*movesText, nullptr, false) : nlohmann::json::array();
            if (!moves.is_array())
                return "moves must be an array";
            std::unordered_map<std::string, Entity> mapped;
            if (std::string error = collectImportedInstance(*world, hierarchy, root, mapped); !error.empty())
                return error;
            std::unordered_set<std::string> available;
            for (const auto& [name, entity] : mapped)
                available.insert(name);
            for (const auto& value : values)
            {
                const std::string object(value.object.toView());
                const std::string parent(value.parentObject.toView());
                if (available.contains(object) || (!parent.empty() && !available.contains(parent)))
                    return "source addition has a duplicate or unresolved parent";
                available.insert(object);
            }
            for (const auto& move : moves)
                if (!move.is_object() || !move.contains("object") || !move["object"].is_string() ||
                    !move.contains("parent") || !move["parent"].is_string() ||
                    !mapped.contains(move["object"].get<std::string>()) ||
                    !available.contains(move["parent"].get<std::string>()))
                    return "invalid source relocation";

            struct Rollback
            {
                const World& world;
                EntityTransformManager& hierarchy;
                std::vector<Entity> created;
                std::vector<std::pair<Entity, Entity>> moved;
                bool committed = false;
                ~Rollback()
                {
                    if (committed)
                        return;
                    for (auto it = moved.rbegin(); it != moved.rend(); ++it)
                        if (it->second.isValid())
                            hierarchy.attach(it->first, it->second);
                        else
                            hierarchy.detach(it->first);
                    for (auto it = created.rbegin(); it != created.rend(); ++it)
                        if (world.isEntityAlive(*it))
                            world.destroyEntity(*it);
                }
            } rollback{*world, hierarchy};
            std::unordered_set<std::string> newObjects;
            for (u32 i = 0; i < values.size(); ++i)
            {
                const auto& source = values[i];
                const std::string key(source.object.toView()), parent(source.parentObject.toView());
                const Entity entity = world->createEntity();
                rollback.created.push_back(entity);
                world->addComponent<TransformComponent>(entity,
                                                        TransformComponent{.position = source.sourcePosition,
                                                                           .rotation = source.sourceRotation,
                                                                           .scale = source.sourceScale});
                world->addComponent<TransformMatrixComponent>(entity);
                if (source.sourceName.isValid())
                    world->addComponent<EntityNameComponent>(entity, EntityNameComponent{.name = source.sourceName});
                auto ownedSource = source;
                ownedSource.instanceKey = world->getComponent<ImportedSourceComponent>(root).instanceKey;
                world->addComponent<ImportedSourceComponent>(entity, ownedSource);
                if (source.sourceMesh.isValid())
                    world->addComponent<MeshComponent>(entity, MeshComponent{.meshHandle = source.sourceMesh});
                if (source.sourceMaterial.isValid())
                    world->addComponent<MaterialComponent>(entity,
                                                           MaterialComponent{.materialHandle = source.sourceMaterial});
                if (source.sourceCamera)
                    world->addComponent<CameraComponent>(entity,
                                                         CameraComponent{.fov = source.sourceFov,
                                                                         .nearPlane = source.sourceNear,
                                                                         .farPlane = source.sourceFar});
                if (source.sourceLight == 1)
                    world->addComponent<DirectionalLightComponent>(
                        entity,
                        DirectionalLightComponent{
                            .color = source.sourceColor, .intensity = source.sourceIntensity, .castShadows = false});
                if (source.sourceLight == 2)
                    world->addComponent<PointLightComponent>(entity,
                                                             PointLightComponent{.color = source.sourceColor,
                                                                                 .intensity = source.sourceIntensity,
                                                                                 .radius = source.sourceRange});
                if (source.sourceLight == 3)
                    world->addComponent<SpotLightComponent>(
                        entity,
                        SpotLightComponent{.color = source.sourceColor,
                                           .intensity = source.sourceIntensity,
                                           .radius = source.sourceRange,
                                           .innerConeAngle = source.sourceInnerCone,
                                           .outerConeAngle = source.sourceOuterCone});
                const Entity target = parent.empty() ? root : mapped.at(parent);
                if (!hierarchy.attach(entity, target))
                    return "cannot attach source addition";
                mapped.emplace(key, entity);
                newObjects.insert(key);
                control.recordCreated(i, entity);
            }
            for (const auto& move : moves)
            {
                const Entity entity = mapped.at(move["object"].get<std::string>());
                const Entity parent = mapped.at(move["parent"].get<std::string>());
                const Entity previous = hierarchy.currentParentOf(entity);
                if (!hierarchy.attach(entity, parent))
                    return "cannot reparent to source addition";
                rollback.moved.emplace_back(entity, previous);
            }
            for (auto it = rollback.moved.rbegin(); it != rollback.moved.rend(); ++it)
            {
                nlohmann::json inverse{{"id", it->first.id()}};
                if (it->second.isValid())
                    inverse["parent"] = it->second.id();
                control.recordInverse("reparent", inverse.dump());
            }
            for (u32 i = 0; i < values.size(); ++i)
                if (!newObjects.contains(std::string(values[i].parentObject.toView())))
                    control.recordInverse("deleteEntity", nlohmann::json{{"id", rollback.created[i].id()}}.dump());
            rollback.committed = true;
            return {};
        }

        std::string applyInstance(ControlServer& control, AssetRuntime& assets, const ControlArgs& args,
                                  IFormatWriter& out, bool& applying,
                                  const AssetRuntime::PreparedModel* prepared = nullptr,
                                  nlohmann::json* planned = nullptr)
        {
            ReimportPreview preview;
            if (std::string error = previewInstance(control, assets, args, preview, prepared); !error.empty())
                return error;
            const auto sceneRevision = args.string("sceneRevision");
            const auto assetRevision = args.string("assetRevision");
            if (!sceneRevision || !assetRevision || *sceneRevision != std::format("{:016x}", preview.sceneRevision) ||
                *assetRevision != std::format("{:016x}", preview.assetRevision))
                return "preview is stale; preview again";
            const auto text = args.json("resolutions");
            const auto resolutions = text ? nlohmann::json::parse(*text, nullptr, false) : nlohmann::json::array();
            if (!resolutions.is_array())
                return "resolutions must be an array";
            std::unordered_map<std::string, std::string> choices;
            for (const auto& resolution : resolutions)
            {
                if (!resolution.is_object() || !resolution.contains("object") || !resolution["object"].is_string() ||
                    !resolution.contains("property") || !resolution["property"].is_string() ||
                    !resolution.contains("choice") || !resolution["choice"].is_string())
                    return "invalid resolution";
                const std::string choice = resolution["choice"];
                if (choice != "source" && choice != "local")
                    return "resolution choice must be source or local";
                const std::string key = nlohmann::json::array({resolution["object"], resolution["property"]}).dump();
                if (!choices.emplace(key, choice).second)
                    return "duplicate resolution";
            }
            for (const auto& [key, choice] : choices)
                if (std::ranges::none_of(
                        preview.changes, [&](const auto& change)
                        { return key == nlohmann::json::array({change.object, change.property}).dump(); }))
                    return "resolution does not match the preview";
            const World* world = control.world(args);
            auto& hierarchy = world->services().write<EntityTransformManager>().get();
            const Entity root(*args.integer("id"));
            std::unordered_map<std::string, Entity> entities;
            std::unordered_set<u64> ownedEntities;
            if (std::string error = collectImportedInstance(*world, hierarchy, root, entities, &ownedEntities);
                !error.empty())
                return error;
            nlohmann::json commands = nlohmann::json::array();
            std::unordered_map<u64, ImportedSourceComponent> baselines;
            std::vector<std::pair<Entity, bool>> removals;
            std::unordered_set<u64> plannedReparents;
            std::unordered_set<u64> localParentChoices;
            std::unordered_set<std::string> additions;
            nlohmann::json moves = nlohmann::json::array();
            for (const auto& change : preview.changes)
                if (change.property == "add")
                    additions.insert(change.object);
            const auto componentCommand =
                [&](const u64 id, const char* component, const bool exists, const nlohmann::json& value)
            {
                if (exists)
                    commands.push_back(
                        {{"command", "removeComponent"}, {"args", {{"id", id}, {"component", component}}}});
                if (!value.is_null())
                    commands.push_back({{"command", "addComponent"},
                                        {"args", {{"id", id}, {"component", component}, {"value", value}}}});
            };
            for (const auto& change : preview.changes)
            {
                const auto choice = choices.find(nlohmann::json::array({change.object, change.property}).dump());
                if (change.conflict && choice == choices.end())
                    return "unresolved conflict: " + change.object + "." + change.property;
                if (change.property == "remove")
                {
                    const Entity entity(change.entity);
                    if (entity == root)
                        return "cannot remove the instance root";
                    const bool remove = choice != choices.end() && choice->second == "source";
                    removals.emplace_back(entity, remove);
                    continue;
                }
                if (change.property == "add")
                {
                    if (choice != choices.end() && choice->second != "source")
                        return "a source addition cannot be suppressed without a persistent identity";
                    continue;
                }
                if (change.property != "name" && change.property != "position" && change.property != "rotation" &&
                    change.property != "scale" && change.property != "parent" && change.property != "mesh" &&
                    change.property != "material" && change.property != "camera" && change.property != "light")
                    return "reconciliation operation is not yet supported: " + change.property;
                const bool takeSource = choice == choices.end() || choice->second == "source";
                const Entity entity(change.entity);
                auto [at, inserted] =
                    baselines.try_emplace(change.entity, world->getConstComponent<ImportedSourceComponent>(entity));
                auto& baseline = at->second;
                const auto next = nlohmann::json::parse(change.source, nullptr, false);
                if (next.is_discarded())
                    return "invalid source value in preview";
                if (change.property == "name")
                    baseline.sourceName = InternedString(next.get<std::string>());
                else if (change.property == "position")
                    baseline.sourcePosition = Vec3(next[0].get<float>(), next[1].get<float>(), next[2].get<float>());
                else if (change.property == "rotation")
                    baseline.sourceRotation =
                        Quat(next[0].get<float>(), next[1].get<float>(), next[2].get<float>(), next[3].get<float>());
                else if (change.property == "scale")
                    baseline.sourceScale = Vec3(next[0].get<float>(), next[1].get<float>(), next[2].get<float>());
                else if (change.property == "parent")
                    baseline.parentObject = InternedString(next.get<std::string>());
                else if (change.property == "mesh" || change.property == "material")
                {
                    AssetHandle handle;
                    if (!next.is_null())
                        handle = {InternedString(next["name"].get<std::string>()),
                                  static_cast<AssetType>(next["assetType"].get<u32>()),
                                  next["subAssetIndex"].get<u32>()};
                    if (change.property == "mesh")
                        baseline.sourceMesh = handle;
                    else
                        baseline.sourceMaterial = handle;
                    if (takeSource)
                    {
                        const bool mesh = change.property == "mesh";
                        const char* component = mesh ? "MeshComponent" : "MaterialComponent";
                        const bool exists = mesh ? world->hasComponent<MeshComponent>(entity)
                                                 : world->hasComponent<MaterialComponent>(entity);
                        nlohmann::json value = nullptr;
                        if (!next.is_null())
                            value = {{mesh ? "meshHandle" : "materialHandle", next}};
                        componentCommand(entity.id(), component, exists, value);
                    }
                    continue;
                }
                else if (change.property == "camera")
                {
                    baseline.sourceCamera = next.is_null() ? 0 : 1;
                    if (!next.is_null())
                    {
                        baseline.sourceFov = next[0].get<float>();
                        baseline.sourceNear = next[1].get<float>();
                        baseline.sourceFar = next[2].get<float>();
                    }
                    else
                        baseline.sourceFov = baseline.sourceNear = baseline.sourceFar = 0.f;
                    if (takeSource)
                    {
                        const bool exists = world->hasComponent<CameraComponent>(entity);
                        if (exists && !next.is_null())
                        {
                            for (u32 i = 0; i < 3; ++i)
                            {
                                const char* fields[] = {"CameraComponent.fov", "CameraComponent.nearPlane",
                                                        "CameraComponent.farPlane"};
                                commands.push_back(
                                    {{"command", "set"},
                                     {"args", {{"id", entity.id()}, {"path", fields[i]}, {"value", next[i]}}}});
                            }
                        }
                        else
                        {
                            nlohmann::json value = nullptr;
                            if (!next.is_null())
                                value = {{"fov", next[0]}, {"nearPlane", next[1]}, {"farPlane", next[2]}};
                            componentCommand(entity.id(), "CameraComponent", exists, value);
                        }
                    }
                    continue;
                }
                else if (change.property == "light")
                {
                    baseline.sourceLight = next.is_null() ? 0 : next[0].get<u32>();
                    if (!next.is_null())
                    {
                        baseline.sourceColor =
                            Vec3(next[1][0].get<float>(), next[1][1].get<float>(), next[1][2].get<float>());
                        baseline.sourceIntensity = next[2].get<float>();
                        baseline.sourceRange = next[3].get<float>();
                        baseline.sourceInnerCone = next[4].get<float>();
                        baseline.sourceOuterCone = next[5].get<float>();
                    }
                    else
                    {
                        baseline.sourceColor = Vec3{};
                        baseline.sourceIntensity = baseline.sourceRange = 0.f;
                        baseline.sourceInnerCone = baseline.sourceOuterCone = 0.f;
                    }
                    if (takeSource)
                    {
                        const u32 current = world->hasComponent<DirectionalLightComponent>(entity) ? 1
                            : world->hasComponent<PointLightComponent>(entity)                     ? 2
                            : world->hasComponent<SpotLightComponent>(entity)                      ? 3
                                                                                                   : 0;
                        const u32 type = baseline.sourceLight;
                        const char* names[] = {"", "DirectionalLightComponent", "PointLightComponent",
                                               "SpotLightComponent"};
                        if (current && current != type)
                            componentCommand(entity.id(), names[current], true, nullptr);
                        if (type && current != type)
                        {
                            nlohmann::json value{{"color", next[1]}, {"intensity", next[2]}, {"castShadows", false}};
                            if (type >= 2)
                                value["radius"] = next[3];
                            if (type == 3)
                            {
                                value["innerConeAngle"] = next[4];
                                value["outerConeAngle"] = next[5];
                            }
                            componentCommand(entity.id(), names[type], false, value);
                        }
                        else if (type)
                        {
                            const auto setLight = [&](const char* field, const nlohmann::json& value)
                            {
                                commands.push_back({{"command", "set"},
                                                    {"args",
                                                     {{"id", entity.id()},
                                                      {"path", std::string(names[type]) + "." + field},
                                                      {"value", value}}}});
                            };
                            setLight("color", next[1]);
                            setLight("intensity", next[2]);
                            if (type >= 2)
                                setLight("radius", next[3]);
                            if (type == 3)
                            {
                                setLight("innerConeAngle", next[4]);
                                setLight("outerConeAngle", next[5]);
                            }
                        }
                    }
                    continue;
                }
                if (!takeSource)
                {
                    if (change.property == "parent")
                        localParentChoices.insert(entity.id());
                    continue;
                }
                if (change.property == "parent")
                {
                    const std::string parentId = next.get<std::string>();
                    const auto parent = entities.find(parentId);
                    if (!parentId.empty() && parent == entities.end() && additions.contains(parentId))
                    {
                        moves.push_back({{"object", change.object}, {"parent", parentId}});
                        plannedReparents.insert(entity.id());
                        continue;
                    }
                    if (!parentId.empty() && parent == entities.end())
                        return "new source parent is not present";
                    const Entity target = parentId.empty() ? root : parent->second;
                    if (entity == root)
                        return "cannot reparent an instance root";
                    commands.push_back(
                        {{"command", "reparent"}, {"args", {{"id", entity.id()}, {"parent", target.id()}}}});
                    plannedReparents.insert(entity.id());
                }
                else
                {
                    const std::string path = change.property == "name" ? "EntityNameComponent.name"
                                                                       : "TransformComponent." + change.property;
                    commands.push_back(
                        {{"command", "set"}, {"args", {{"id", entity.id()}, {"path", path}, {"value", next}}}});
                }
            }
            if (!additions.empty())
            {
                const InternedString modelKey(*args.string("model"));
                const auto entry = assets.baker().getManifest().getModelEntry(modelKey);
                const Expected<std::shared_ptr<ModelHierarchyDescriptor>> descriptor = prepared
                    ? Expected<std::shared_ptr<ModelHierarchyDescriptor>>(prepared->hierarchy)
                    : loadBakedModelDescriptor(modelKey, assets.baker().getManifest());
                if (!entry || !descriptor)
                    return "cannot load the source hierarchy";
                std::unordered_set<u32> skinned;
                if (prepared)
                    skinned.insert(prepared->skinnedSubmeshes.begin(), prepared->skinnedSubmeshes.end());
                else if (entry->skin.isValid())
                {
                    const auto skin = loadBakedSkin(modelKey, assets.baker().getManifest());
                    if (!skin)
                        return skin.error().message;
                    for (const auto& submesh : skin->submeshes)
                        skinned.insert(submesh.submesh);
                }
                const auto& nodes = (*descriptor)->nodes;
                std::vector<i32> parents(nodes.size(), -1);
                for (u32 i = 0; i < nodes.size(); ++i)
                    for (const u32 child : nodes[i].childrenIndices)
                        if (child < nodes.size())
                            parents[child] = static_cast<i32>(i);
                std::unordered_set<std::string> available;
                for (const auto& [key, entity] : entities)
                    available.insert(key);
                nlohmann::json created = nlohmann::json::array();
                auto pending = additions;
                while (!pending.empty())
                {
                    bool progressed = false;
                    for (u32 i = 0; i < nodes.size(); ++i)
                    {
                        const auto& node = nodes[i];
                        const std::string object(node.sourceObject.toView());
                        if (!pending.contains(object))
                            continue;
                        const i32 p = parents[i];
                        const InternedString parent = p >= 0 ? nodes[p].sourceObject : InternedString{};
                        if (parent.isValid() && !available.contains(std::string(parent.toView())))
                            continue;
                        if (node.meshIndices.size() > 1 ||
                            (!node.meshIndices.empty() && skinned.contains(node.meshIndices[0])))
                            return "source additions with multiple or skinned meshes are not supported";
                        ImportedSourceComponent value;
                        value.document = (*descriptor)->sourceDocument;
                        value.object = node.sourceObject;
                        value.parentObject = parent;
                        value.sourceName = node.name;
                        value.sourcePosition = node.localTransform.position;
                        value.sourceRotation = node.localTransform.rotation;
                        value.sourceScale = node.localTransform.scale;
                        value.stable = node.sourceObjectStable;
                        if (!node.meshIndices.empty())
                        {
                            const u32 mesh = node.meshIndices[0];
                            if (mesh >= (*descriptor)->submeshes.size())
                                return "invalid source mesh";
                            value.sourceMesh = {modelKey, AssetType::MESH, mesh};
                            const u32 material = (*descriptor)->submeshes[mesh].materialIndex;
                            if (material < (*descriptor)->materialNames.size())
                                value.sourceMaterial = {(*descriptor)->materialNames[material], AssetType::MATERIAL, 0};
                        }
                        value.sourceCamera = node.imported.camera;
                        value.sourceLight = node.imported.light;
                        value.sourceFov = node.imported.fov;
                        value.sourceNear = node.imported.nearPlane;
                        value.sourceFar = node.imported.farPlane;
                        value.sourceColor =
                            Vec3(node.imported.color[0], node.imported.color[1], node.imported.color[2]);
                        value.sourceIntensity = node.imported.intensity;
                        value.sourceRange = node.imported.range;
                        value.sourceInnerCone = node.imported.innerCone;
                        value.sourceOuterCone = node.imported.outerCone;
                        created.push_back(nlohmann::json::parse(provenanceJson(value)));
                        available.insert(object);
                        pending.erase(object);
                        progressed = true;
                    }
                    if (!progressed)
                        return "source addition has an unresolved parent";
                }
                commands.insert(
                    commands.begin(),
                    nlohmann::json{
                        {"command", "createImportedNodes"},
                        {"args", {{"id", root.id()}, {"nodes", std::move(created)}, {"moves", std::move(moves)}}}});
            }
            // Classify the whole deletion set before scheduling any hierarchy changes. A generated mesh is owned by
            // its source object even when it has been moved elsewhere; local or nested-instance descendants are not.
            std::unordered_map<u64, Entity> doomed;
            const InternedString modelName(*args.string("model"));
            for (const auto& [entity, remove] : removals)
            {
                const auto& source = world->getConstComponent<ImportedSourceComponent>(entity);
                if (remove)
                    doomed.emplace(entity.id(), entity);
                for (const u64 ownedId : ownedEntities)
                {
                    const Entity child(ownedId);
                    const auto& item = world->getConstComponent<ImportedSourceComponent>(child);
                    if (!item.generatedMesh || item.object != source.object || item.document != source.document ||
                        item.instanceKey != source.instanceKey || item.sourceMesh.name != modelName)
                        continue;
                    if (remove)
                        doomed.emplace(child.id(), child);
                    else
                        componentCommand(child.id(), "ImportedSourceComponent", true, nullptr);
                }
                if (!remove)
                    componentCommand(entity.id(), "ImportedSourceComponent", true, nullptr);
            }
            std::vector<Entity> doomedEntities;
            doomedEntities.reserve(doomed.size());
            for (const auto& [id, entity] : doomed)
                doomedEntities.push_back(entity);
            if (!doomedEntities.empty() && hasExternalSceneReferences(*world, doomedEntities))
                return "source removal has incoming references; resolve them before applying";
            for (const u64 id : localParentChoices)
                if (doomed.contains(hierarchy.currentParentOf(Entity(id)).id()))
                    return "a local parent choice would retain a removed source parent";

            // Move each surviving direct child out of the deletion set before deleting its non-overlapping roots.
            // The destination is found in the original relation graph, so removal order cannot leave a dead parent.
            for (const Entity entity : doomedEntities)
            {
                Entity destination = hierarchy.currentParentOf(entity);
                while (destination.isValid() && doomed.contains(destination.id()))
                    destination = hierarchy.currentParentOf(destination);
                std::vector<Entity> descendants;
                hierarchy.collectSubtree(entity, descendants);
                for (const Entity child : descendants)
                    if (hierarchy.currentParentOf(child) == entity && !doomed.contains(child.id()) &&
                        !plannedReparents.contains(child.id()))
                    {
                        nlohmann::json reparentArgs{{"id", child.id()}};
                        if (destination.isValid())
                            reparentArgs["parent"] = destination.id();
                        commands.push_back({{"command", "reparent"}, {"args", std::move(reparentArgs)}});
                    }
            }
            for (const Entity entity : doomedEntities)
                if (!doomed.contains(hierarchy.currentParentOf(entity).id()))
                    commands.push_back({{"command", "deleteEntity"}, {"args", {{"id", entity.id()}}}});
            for (const auto& [id, baseline] : baselines)
                commands.push_back(
                    {{"command", "setImportedBaseline"},
                     {"args", {{"id", id}, {"value", nlohmann::json::parse(provenanceJson(baseline))}}}});
            if (planned)
            {
                *planned = std::move(commands);
                return {};
            }
            if (commands.empty())
            {
                out.writeU32("applied", 0);
                return {};
            }
            struct ApplyScope
            {
                bool& flag;
                ApplyScope(bool& f) : flag(f) { flag = true; }
                ~ApplyScope() { flag = false; }
            } scope(applying);
            return control.executeUndoableBatch(control.worldName(args), commands.dump(), out);
        }

        // Execute plans against an isolated world. The caller captures the snapshot and plans on the frame thread;
        // this function owns every object it touches and is suitable for a worker.
        Expected<std::string> prepareSceneCopy(std::string snapshot, nlohmann::json plans,
                                               const std::filesystem::path& sceneRoot)
        {
            ServiceLocator services;
            World world = World::createForCopy(1u, &services);
            struct WorldOwner
            {
                World& world;
                ~WorldOwner() { world.destroy(); }
            } owner{world};
            auto transforms = std::make_unique<EntityTransformManager>(world, false);
            // This copy executes commands but never runs systems. Initializing systems here would register their
            // components globally and mutate the live host's schema revision during a private preparation.
            ControlServer control;
            control.addWorld("scene", world);
            SceneRecoveryAccess recovery;
            registerSceneCommandsForCopy(control, sceneRoot, &recovery);
            bool applying = true;
            control.registerUndoableCommand("setImportedBaseline", OWNER,
                                            [&control, &applying](const ControlArgs& args, IFormatWriter& out)
                                            { return setImportedBaseline(control, args, out, applying); });
            control.registerUndoableCommand("createImportedNodes", OWNER,
                                            [&control, &applying](const ControlArgs& args, IFormatWriter& out)
                                            { return createImportedNodes(control, args, out, applying); });
            std::unordered_map<u64, u64> mapping;
            const auto restored = recovery.restoreWithMapping(snapshot, mapping);
            if (!restored)
                return std::unexpected(restored.error());
            for (auto& commands : plans)
            {
                for (auto& command : commands)
                {
                    auto& args = command["args"];
                    for (const char* key : {"id", "parent"})
                        if (args.contains(key))
                        {
                            const auto found = mapping.find(args[key].get<u64>());
                            if (found == mapping.end())
                                return std::unexpected(
                                    makeError(ErrorCode::CorruptedData, "scene plan refers to an unknown entity"));
                            args[key] = found->second;
                        }
                }
                if (!commands.empty())
                {
                    JsonFragmentWriter result;
                    if (const auto error = control.executeUndoableBatch("scene", commands.dump(), result);
                        !error.empty())
                        return std::unexpected(makeError(ErrorCode::AssetSaveFailed, error));
                }
            }
            return recovery.capture();
        }

        std::string primitiveModelName(const std::string_view kind) { return "primitive-" + std::string(kind); }

        // The generated GLB of a primitive goes under the import root once and imports like any model; a later spawn
        // finds the model in the manifest.
        std::string ensurePrimitiveModel(AssetRuntime& assets, const std::filesystem::path& importRoot,
                                         const std::string_view kind)
        {
            const std::string model = primitiveModelName(kind);
            if (assets.manifest().hasModel(InternedString(model.c_str())))
                return {};
            const std::filesystem::path file = importRoot / "primitives" / (std::string(kind) + ".glb");
            std::error_code error;
            if (!std::filesystem::exists(file, error))
            {
                const std::vector<std::byte> bytes = primitiveGlb(kind);
                // written whole beside the target and renamed over it: a failed or cut write never leaves a file
                // that every later spawn would import
                const std::filesystem::path temp = file.parent_path() / (std::string(kind) + ".glb.tmp");
                bool written = !bytes.empty();
                if (written)
                {
                    std::filesystem::create_directories(file.parent_path(), error);
                    std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
                    stream.write(reinterpret_cast<const char*>(bytes.data()),
                                 static_cast<std::streamsize>(bytes.size()));
                    stream.close();
                    written = !stream.fail();
                    error.clear();
                    if (written)
                        std::filesystem::rename(temp, file, error);
                    written = written && !error;
                    if (!written)
                        std::filesystem::remove(temp, error);
                }
                if (!written)
                    return "cannot write the primitive's source '" + utf8Path(file) + "'";
            }
            const auto imported = assets.importModelNow(utf8Path(file), InternedString(model.c_str()),
                                                        utf8Path(importRoot), std::nullopt);
            return imported ? std::string{}
                            : "cannot import the primitive '" + utf8Path(file) + "': " + imported.error().message;
        }

        // kind non-empty: createPrimitive, which places the primitive's generated model (imported on first use) the
        // way instantiateModel places a named one
        std::string instantiateModel(ControlServer& control, AssetRuntime& assets, const ControlArgs& args,
                                     IFormatWriter& out, const std::filesystem::path& importRoot = {},
                                     const std::string_view kind = {})
        {
            const World* world = control.world(args);
            if (!world)
            {
                return control.unknownWorld(args);
            }
            std::string model;
            if (!kind.empty())
                model = primitiveModelName(kind);
            else if (std::string error = nameArg(args, "model", model); !error.empty())
            {
                return error;
            }
            Entity parent;
            if (std::string error = parentArg(*world, args, parent); !error.empty())
            {
                return error;
            }
            const auto requestedLayer = args.integer("layer");
            const bool editScene =
                control.worldName(args) == "scene" && world->services().contains<SceneLayerPlacement>();
            if (requestedLayer && (parent.isValid() || !editScene))
                return "layer can only be specified for a new root in the edit world";
            std::optional<u32> layer;
            if (editScene && !parent.isValid())
            {
                layer = world->services().read<SceneLayerPlacement>()->resolve(requestedLayer);
                if (!layer)
                    return "no such scene layer (sceneLayers lists them)";
            }
            // vetted before anything is built, so applying it cannot fail
            const std::optional<std::string> transform = args.json("transform");
            if (transform)
            {
                TransformComponent scratch;
                std::string error;
                JsonFragmentReader reader(*transform);
                if (!nlohmann::json::parse(*transform, nullptr, false).is_object() ||
                    !readValue(TransformComponent::spiteSchema(), &scratch, reader, error))
                {
                    return "bad transform (an object of position, rotation, scale): " + error;
                }
            }
            // names are labels, not keys: instances may share one
            const std::string name = args.string("name").value_or(kind.empty() ? model : primitiveLabel(kind));
            if (name.empty())
            {
                return std::string("name must not be empty");
            }
            if (!kind.empty())
                if (std::string error = ensurePrimitiveModel(assets, importRoot, kind); !error.empty())
                    return error;

            const Expected<AssetRuntime::InstantiatedModel> instance =
                assets.instantiateModelNow(*world, InternedString(model.c_str()), parent);
            if (!instance)
            {
                return "cannot instantiate '" + model + "': " + instance.error().message;
            }
            const Entity root = instance->root;
            if (layer && *layer != 0)
                world->addComponent<SceneLayerComponent>(root, SceneLayerComponent{.layer = *layer});
            setEntityName(*world, root, name);
            if (transform)
            {
                std::string error;
                JsonFragmentReader reader(*transform);
                (void)readValue(TransformComponent::spiteSchema(),
                                world->getComponentDataById(root, getComponentId<TransformComponent>()), reader, error);
            }
            control.recordCreated(0, root);
            if (layer)
            {
                control.touchLayer(*layer);
                nlohmann::json forward{{"name", name}, {"layer", *layer}};
                forward[kind.empty() ? "model" : "kind"] = kind.empty() ? model : std::string(kind);
                if (transform)
                    forward["transform"] = nlohmann::json::parse(*transform);
                control.recordForward(forward.dump());
            }
            control.recordInverse("deleteEntity", R"({"id":)" + std::to_string(root.id()) + "}");
            out.writeU64("id", root.id());
            out.writeString("name", name);
            out.writeString("model", model);
            if (parent.isValid())
            {
                out.writeU64("parent", parent.id());
            }
            out.writeU32("entities", instance->entities);
            return {};
        }

        std::string createMaterial(ControlServer& control, AssetRuntime& assets, const ControlArgs& args,
                                   IFormatWriter& out)
        {
            std::string name;
            if (std::string error = newMaterialNameArg(assets.manifest(), args, name); !error.empty())
            {
                return error;
            }
            BakedMaterialWriteData fields;
            if (std::string error = readFields(assets.manifest(), args, fields); !error.empty())
            {
                return error;
            }
            if (std::string error =
                    writeMaterialRecording(control, assets, name, fields, "deleteMaterial", inverseArgs(name, nullptr));
                !error.empty())
            {
                return error;
            }
            out.writeString("name", name);
            writeFields(fields, "fields", out);
            return {};
        }

        std::string updateMaterial(ControlServer& control, AssetRuntime& assets, const ControlArgs& args,
                                   IFormatWriter& out)
        {
            std::string name;
            if (std::string error = nameArg(args, "name", name); !error.empty())
            {
                return error;
            }
            if (!args.json("fields"))
            {
                return std::string("missing arg 'fields'");
            }
            BakedMaterialWriteData previous;
            if (std::string error = readMaterial(assets, name, previous); !error.empty())
            {
                return error;
            }
            BakedMaterialWriteData fields = previous;
            if (std::string error = readFields(assets.manifest(), args, fields); !error.empty())
            {
                return error;
            }
            if (std::string error = writeMaterialRecording(control, assets, name, fields, "updateMaterial",
                                                           inverseArgs(name, &previous));
                !error.empty())
            {
                return error;
            }
            out.writeString("name", name);
            writeFields(fields, "fields", out);
            writeFields(previous, "previous", out);
            return {};
        }

        std::string deleteMaterial(ControlServer& control, AssetRuntime& assets, const ControlArgs& args,
                                   IFormatWriter& out)
        {
            std::string name;
            if (std::string error = nameArg(args, "name", name); !error.empty())
            {
                return error;
            }
            if (std::string error = engineMaterialName(name); !error.empty())
            {
                return error;
            }
            BakedMaterialWriteData fields;
            if (std::string error = readMaterial(assets, name, fields); !error.empty())
            {
                return error;
            }
            const InternedString interned(name.c_str());
            for (const auto& [model, entry] : assets.manifest().getModels())
            {
                if (std::ranges::find(entry.materials, interned) != entry.materials.end())
                {
                    return "model '" + std::string(model.toView()) + "' references material '" + name + "'";
                }
            }
            // a loaded material holds its load reference; any more are bindings
            const AssetHandle handle{interned, AssetType::MATERIAL, 0};
            if (const u32 refs = assets.registry().getRefCount(handle); refs > 1)
            {
                return "material '" + name + "' is bound by " + std::to_string(refs - 1) + " entities";
            }
            if (const Expected<void> removed = assets.removeMaterial(interned); !removed)
            {
                return "cannot delete material '" + name + "': " + removed.error().message;
            }
            control.recordInverse("createMaterial", inverseArgs(name, &fields));
            out.writeString("name", name);
            writeFields(fields, "fields", out);
            return {};
        }

        // -- import ----------------------------------------------------------------------

        using ModelImport = AssetRuntime::ModelImport;

        // The "path" arg resolved under the import root to an existing file.
        std::string importPath(const ControlServer& control, const std::filesystem::path& root, const ControlArgs& args,
                               std::filesystem::path& out)
        {
            const std::optional<std::string> path = args.string("path");
            if (!path)
            {
                return "missing arg 'path'";
            }
            if (std::string error = control.resolveFile("import", *path, out); !error.empty())
            {
                return error;
            }
            std::error_code error;
            if (!std::filesystem::is_regular_file(out, error))
            {
                return "no file '" + *path + "' under the import root '" + utf8Path(root) + "'";
            }
            return {};
        }

        // source (UTF-8, as the import pipeline holds it) as importModel takes it: relative to root; empty outside it
        std::string importArgPath(const std::filesystem::path& root, const std::string& source)
        {
            const std::filesystem::path relative = pathFromUtf8(source).lexically_relative(root);
            return relative.empty() || *relative.begin() == ".." ? std::string{} : utf8Path(relative);
        }

        const char* importStateName(const ModelImport::State state)
        {
            switch (state)
            {
            case ModelImport::State::Queued:
                return "queued";
            case ModelImport::State::Baking:
                return "baking";
            case ModelImport::State::Done:
                return "done";
            case ModelImport::State::Failed:
                return "failed";
            default:
                return "cancelled";
            }
        }

        void writeNames(const std::string_view key, const std::vector<InternedString>& names, IFormatWriter& out)
        {
            out.beginArray(key, static_cast<u32>(names.size()));
            for (const InternedString& name : names)
            {
                out.writeString("", name.toView());
            }
            out.endArray();
        }

        void writeImport(const std::filesystem::path& root, const ModelImport& record, IFormatWriter& out)
        {
            out.writeU32("import", record.id);
            out.writeString("name", record.name.toView());
            const std::string path = importArgPath(root, record.source);
            out.writeString("path", path);
            if (path.empty())
                out.writeString("source", record.source);
            out.writeString("status", importStateName(record.state));
            out.writeBool("previews", record.generatePreviews);
            if (!record.error.empty())
            {
                out.writeString("error", record.error);
            }
            out.beginArray("diagnostics", static_cast<u32>(record.diagnostics.size()));
            for (const auto& diagnostic : record.diagnostics)
            {
                out.beginObject("");
                out.writeString("code", diagnostic.code);
                out.writeString("extension", diagnostic.extension);
                out.writeBool("required", diagnostic.required);
                out.writeString("policy",
                                diagnostic.policy.empty()
                                    ? (diagnostic.required ? "reject" : "ignore_and_use_core_fallback")
                                    : diagnostic.policy);
                out.endObject();
            }
            out.endArray();
            writeNames("materials", record.materials, out);
            writeNames("textures", record.textures, out);
            out.writeF64("bakeMs", record.bakeMs);
            if (record.reimport)
            {
                out.writeBool("reimport", true);
            }
        }

        u32 lastImportId(const AssetRuntime& assets)
        {
            u32 last = 0;
            for (const auto& record : assets.imports())
                last = std::max(last, record.id);
            return last;
        }

        void writeFailedImport(const AssetRuntime& assets, const std::filesystem::path& root, const u32 previous,
                               const InternedString name, IFormatWriter& out)
        {
            for (const auto& record : assets.imports())
                if (record.id > previous && record.name == name && record.state == ModelImport::State::Failed)
                    writeImport(root, record, out);
        }

        // -- import settings -----------------------------------------------------------------

        void writeSettings(const ModelImportSettings& settings, const std::string_view key, IFormatWriter& out)
        {
            out.beginObject(key);
            for (const ImportOptionDesc& option : modelImportOptions())
            {
                const ImportOptionValue value = importOptionValue(settings, option);
                if (option.type == "number")
                {
                    out.writeF64(option.name, value.number);
                }
                else if (option.type == "bool")
                {
                    out.writeBool(option.name, value.flag);
                }
                else
                {
                    out.writeString(option.name, value.text);
                }
            }
            out.endObject();
        }

        // A source's sidecar settings (the defaults without one) with the "settings" arg's members over them; given
        // says the arg was passed, so the sidecar is to be written.
        std::string settingsArg(const ControlArgs& args, const std::string& source, ModelImportSettings& previous,
                                ModelImportSettings& next, bool& given)
        {
            const Expected<ModelImportSettings> loaded = loadModelImportSettings(source);
            if (!loaded)
            {
                return loaded.error().message + "; fix or delete the file";
            }
            previous = next = *loaded;
            const std::optional<std::string> text = args.json("settings");
            given = text.has_value();
            return given ? applyModelImportSettings(next, *text) : std::string{};
        }

        // The sidecar of source lies under the import root, as the source does (no link leads out of it).
        std::string checkSidecar(const ControlServer& control, const std::filesystem::path& root,
                                 const std::string& source)
        {
            std::filesystem::path sidecar;
            return control.resolveFile("import", importArgPath(root, modelImportSidecarPath(source)), sidecar);
        }

        std::string modelSource(const ControlServer& control, const std::filesystem::path& root,
                                const AssetManifest::ModelEntry& model, std::filesystem::path& source,
                                std::filesystem::path& sourceRoot)
        {
            const std::string text(model.source.toView());
            const std::string relative = importArgPath(root, text);
            sourceRoot = root;
            if (!relative.empty())
            {
                if (auto error = control.resolveFile("import", relative, source); !error.empty())
                    return error;
                return checkSidecar(control, root, text);
            }
            std::error_code error;
            source = std::filesystem::canonical(pathFromUtf8(text), error);
            if (error || !model.sourceRoot.isValid())
                return "model source is outside the import root";
            sourceRoot = std::filesystem::canonical(pathFromUtf8(model.sourceRoot.toView()), error);
            if (error || source.parent_path() != sourceRoot)
                return "model source is outside its recorded source directory";
            const auto sidecar =
                std::filesystem::weakly_canonical(pathFromUtf8(modelImportSidecarPath(utf8Path(source))), error);
            if (error || sidecar.parent_path() != sourceRoot)
                return "model sidecar is outside the source directory";
            return {};
        }

        std::string importSettings(const ControlServer& control, const AssetRuntime& assets,
                                   const std::filesystem::path& root, const ControlArgs& args, IFormatWriter& out)
        {
            std::string source;
            std::optional<AssetManifest::ModelEntry> baked;
            const std::optional<std::string> path = args.string("path");
            const std::optional<std::string> model = args.string("model");
            if (path && model)
            {
                return std::string("pass 'path' or 'model', not both");
            }
            if (path)
            {
                std::filesystem::path file;
                if (std::string error = importPath(control, root, args, file); !error.empty())
                {
                    return error;
                }
                source = utf8Path(file);
            }
            else if (model)
            {
                const std::optional<AssetManifest::ModelEntry> entry =
                    assets.manifest().getModelEntry(InternedString(model->c_str()));
                if (!entry)
                {
                    return "no model '" + *model + "' in the manifest";
                }
                baked = entry;
                source = std::string(entry->source.toView());
            }
            out.beginArray("options", 0);
            for (const ImportOptionDesc& option : modelImportOptions())
            {
                out.beginObject({});
                out.writeString("name", option.name);
                out.writeString("type", option.type);
                out.writeString("doc", option.doc);
                if (option.type == "number")
                {
                    out.writeF64("default", std::stod(std::string(option.defaultValue)));
                    out.writeF64("min", option.min);
                    out.writeF64("max", option.max);
                }
                else if (option.type == "bool")
                {
                    out.writeBool("default", option.defaultValue == "true");
                }
                else
                {
                    out.writeString("default", option.defaultValue);
                    out.beginArray("choices", static_cast<u32>(option.choices.size()));
                    for (const std::string_view choice : option.choices)
                    {
                        out.writeString("", choice);
                    }
                    out.endArray();
                }
                out.endObject();
            }
            out.endArray();
            if (source.empty())
            {
                return {};
            }
            std::filesystem::path checkedSource;
            if (baked)
            {
                std::filesystem::path sourceRoot;
                if (std::string error = modelSource(control, root, *baked, checkedSource, sourceRoot); !error.empty())
                    return error;
            }
            else
            {
                if (std::string error = control.resolveFile("import", importArgPath(root, source), checkedSource);
                    !error.empty())
                    return error;
                if (std::string error = checkSidecar(control, root, source); !error.empty())
                    return error;
            }
            const Expected<ModelImportSettings> settings = loadModelImportSettings(source);
            if (!settings)
            {
                return settings.error().message;
            }
            out.writeString("path", importArgPath(root, source));
            out.writeString("sidecar", importArgPath(root, modelImportSidecarPath(source)));
            std::error_code ignored;
            out.writeBool("sidecarExists",
                          std::filesystem::exists(pathFromUtf8(modelImportSidecarPath(source)), ignored));
            writeSettings(*settings, "settings", out);
            if (baked)
            {
                writeSettings(baked->importSettings, "bakedWith", out);
                out.writeBool("needsReimport", *settings != baked->importSettings);
            }
            return {};
        }

        // The sidecar of a model's source gets the settings now, no bake: the model then needs a reimport.
        std::string setImportSettings(ControlServer& control, AssetRuntime& assets, const std::filesystem::path& root,
                                      const ControlArgs& args, IFormatWriter& out)
        {
            std::string name;
            if (std::string error = nameArg(args, "name", name); !error.empty())
            {
                return error;
            }
            const std::optional<AssetManifest::ModelEntry> model =
                assets.manifest().getModelEntry(InternedString(name.c_str()));
            if (!model)
            {
                return "no model '" + name + "' in the manifest";
            }
            const std::string sourceText(model->source.toView());
            const std::string path = importArgPath(root, sourceText);
            std::filesystem::path source;
            std::filesystem::path sourceRoot;
            if (std::string error = modelSource(control, root, *model, source, sourceRoot); !error.empty())
            {
                return error;
            }
            std::error_code ignored;
            if (!std::filesystem::is_regular_file(source, ignored))
            {
                return "the source '" + path + "' of model '" + name + "' is gone";
            }
            ModelImportSettings previous;
            ModelImportSettings settings;
            bool given = false;
            if (std::string error = settingsArg(args, sourceText, previous, settings, given); !error.empty())
            {
                return error;
            }
            writeSettings(settings, "settings", out);
            writeSettings(previous, "previous", out);
            out.writeBool("needsReimport", settings != model->importSettings);
            if (settings == previous)
            {
                return {}; // nothing to write and nothing to undo
            }
            if (const Expected<void> saved = assets.saveImportSettings(sourceText, settings); !saved)
            {
                return "cannot write the settings of '" + name + "': " + saved.error().message;
            }
            control.recordInverse(
                "setImportSettings",
                nlohmann::json{{"name", name}, {"settings", nlohmann::json::parse(modelImportSettingsJson(previous))}}
                    .dump());
            return {};
        }

        std::string importModel(ControlServer& control, AssetRuntime& assets, const std::filesystem::path& root,
                                const ControlArgs& args, IFormatWriter& out, bool external = false)
        {
            std::filesystem::path source;
            std::filesystem::path sourceRoot = root;
            if (external)
            {
                const auto path = args.string("path");
                if (!path || !pathFromUtf8(*path).is_absolute())
                    return "path must be an absolute model file path";
                std::error_code error;
                source = std::filesystem::canonical(pathFromUtf8(*path), error);
                if (error || !std::filesystem::is_regular_file(source, error))
                    return "cannot open model source '" + *path + "'";
                sourceRoot = source.parent_path();
                std::string extension = utf8Path(source.extension());
                std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return std::tolower(c); });
                if (extension != ".gltf" && extension != ".glb")
                    return "source must be a glTF or GLB file";
                const auto sidecar =
                    std::filesystem::weakly_canonical(pathFromUtf8(modelImportSidecarPath(utf8Path(source))), error);
                if (error || sidecar.parent_path() != sourceRoot)
                    return "model sidecar is outside the source directory";
            }
            else if (std::string error = importPath(control, root, args, source); !error.empty())
            {
                return error;
            }
            const std::string name = args.string("name").value_or(utf8Path(source.stem()));
            if (name.empty())
            {
                return std::string("name must not be empty");
            }
            if (std::string error = bakeFileNameError("model", name); !error.empty())
            {
                return args.string("name")
                    ? error
                    : error + "; pass a name, for example '" + AssetManifest::safeBakeName(name) + "'";
            }
            const InternedString interned(name.c_str());
            const std::string sourceText = utf8Path(source);
            ModelImportSettings previous;
            ModelImportSettings settings;
            bool given = false;
            if (!external)
                if (std::string error = checkSidecar(control, root, sourceText); !error.empty())
                    return error;
            if (std::string error = settingsArg(args, sourceText, previous, settings, given); !error.empty())
            {
                return error;
            }
            const std::optional<ModelImportSettings> written = given ? std::optional(settings) : std::nullopt;
            nlohmann::json forward{{"path", *args.string("path")},
                                   {"name", name},
                                   {"wait", true},
                                   {"previews", args.boolean("previews").value_or(true)}};
            if (given)
            {
                forward["settings"] = nlohmann::json::parse(modelImportSettingsJson(settings));
            }
            if (args.boolean("wait").value_or(false))
            {
                const u32 previousId = lastImportId(assets);
                const Expected<ModelImport> imported = assets.importModelNow(
                    sourceText, interned, utf8Path(sourceRoot), written, args.boolean("previews").value_or(true));
                if (!imported)
                {
                    writeFailedImport(assets, root, previousId, interned, out);
                    return "cannot import '" + sourceText + "': " + imported.error().message;
                }
                writeImport(root, *imported, out);
            }
            else
            {
                const Expected<u32> started = assets.importModel(sourceText, interned, utf8Path(sourceRoot), written,
                                                                 args.boolean("previews").value_or(true));
                if (!started)
                {
                    return "cannot import '" + sourceText + "': " + started.error().message;
                }
                const std::vector<ModelImport> imports = assets.imports();
                writeImport(root, *std::ranges::find(imports, *started, &ModelImport::id), out);
                // a redo or replay bakes before the commands after it run
                control.recordForward(forward.dump());
            }
            writeSettings(settings, "settings", out);
            control.recordInverse("removeModel", nlohmann::json{{"name", name}}.dump());
            return {};
        }

        std::string importEnvironment(ControlServer& control, AssetRuntime& assets, const ControlArgs& args,
                                      IFormatWriter& out)
        {
            const std::optional<std::string> path = args.string("path");
            if (!path)
            {
                return "missing arg 'path'";
            }
            std::filesystem::path source;
            std::error_code error;
            if (!control.resolveFile("import", *path, source).empty() ||
                !std::filesystem::is_regular_file(source, error))
            {
                if (std::string failure = control.resolveDirectory("import", *path, source); !failure.empty())
                {
                    return failure;
                }
            }
            const std::string name = args.string("name").value_or(utf8Path(source.stem()));
            if (name.empty())
            {
                return std::string("name must not be empty");
            }
            if (std::string failure = bakeFileNameError("environment", name); !failure.empty())
            {
                return args.string("name")
                    ? failure
                    : failure + "; pass a name, for example '" + AssetManifest::safeBakeName(name) + "'";
            }
            EnvironmentFormat format = EnvironmentFormat::Rgba16Float;
            if (const std::optional<std::string> text = args.string("format"))
            {
                const std::string key = AssetManifest::bakeFileKey(*text);
                if (key == "rgb9e5")
                    format = EnvironmentFormat::Rgb9E5;
                else if (key == "r11g11b10")
                    format = EnvironmentFormat::R11G11B10;
                else if (key != "rgba16f")
                    return "unknown environment format '" + *text + "' (rgba16f, rgb9e5 or r11g11b10)";
            }
            const Expected<AssetManifest::EnvironmentEntry> imported =
                assets.importEnvironment(utf8Path(source), InternedString(name.c_str()), format);
            if (!imported)
            {
                return "cannot import '" + *path + "': " + imported.error().message;
            }
            out.writeString("name", name);
            out.writeString("baked", imported->baked.toView());
            writeEnvironment(*imported, out);
            return {};
        }

        std::string reimportModel(ControlServer& control, AssetRuntime& assets, const std::filesystem::path& root,
                                  const ControlArgs& args, IFormatWriter& out)
        {
            std::string name;
            if (std::string error = nameArg(args, "name", name); !error.empty())
            {
                return error;
            }
            const InternedString interned(name.c_str());
            const std::optional<AssetManifest::ModelEntry> model = assets.manifest().getModelEntry(interned);
            if (!model)
            {
                return "no model '" + name + "' in the manifest";
            }
            const std::string sourceText(model->source.toView());
            const std::string path = importArgPath(root, sourceText);
            std::filesystem::path source;
            std::filesystem::path sourceRoot;
            if (std::string error = modelSource(control, root, *model, source, sourceRoot); !error.empty())
            {
                return error;
            }
            std::error_code ignored;
            if (!std::filesystem::is_regular_file(source, ignored))
            {
                return "the source '" + path + "' of model '" + name + "' is gone";
            }
            ModelImportSettings previous;
            ModelImportSettings settings;
            bool given = false;
            if (std::string error = settingsArg(args, sourceText, previous, settings, given); !error.empty())
            {
                return error;
            }
            const std::optional<ModelImportSettings> written = given ? std::optional(settings) : std::nullopt;
            const nlohmann::json next = nlohmann::json::parse(modelImportSettingsJson(settings));
            // Undo restores the bake, not the sidecar: the settings the model has now, or will have once its pending
            // reimport (which this one replaces) bakes
            ModelImportSettings restore = model->importSettings;
            const std::vector<ModelImport> pending = assets.imports();
            if (const auto job = std::ranges::find_if(pending.rbegin(), pending.rend(),
                                                      [&interned](const ModelImport& record)
                                                      {
                                                          return record.name == interned && record.reimport &&
                                                              (record.state == ModelImport::State::Queued ||
                                                               record.state == ModelImport::State::Baking);
                                                      });
                job != pending.rend())
            {
                restore = job->settings;
            }
            if (args.boolean("wait").value_or(false))
            {
                const u32 previousId = lastImportId(assets);
                const Expected<ModelImport> imported = assets.reimportModelNow(interned, utf8Path(sourceRoot), written,
                                                                               args.boolean("previews").value_or(true));
                if (!imported)
                {
                    writeFailedImport(assets, root, previousId, interned, out);
                    return "cannot reimport '" + name + "': " + imported.error().message;
                }
                writeImport(root, *imported, out);
            }
            else
            {
                const Expected<u32> started = assets.reimportModel(interned, utf8Path(sourceRoot), written,
                                                                   args.boolean("previews").value_or(true));
                if (!started)
                {
                    return "cannot reimport '" + name + "': " + started.error().message;
                }
                const std::vector<ModelImport> imports = assets.imports();
                writeImport(root, *std::ranges::find(imports, *started, &ModelImport::id), out);
                // a redo or replay bakes before the commands after it run
                control.recordForward(nlohmann::json{{"name", name},
                                                     {"settings", next},
                                                     {"wait", true},
                                                     {"previews", args.boolean("previews").value_or(true)}}
                                          .dump());
            }
            writeSettings(settings, "settings", out);
            writeSettings(previous, "previous", out);
            control.recordInverse("reimportModel",
                                  nlohmann::json{{"name", name},
                                                 {"settings", nlohmann::json::parse(modelImportSettingsJson(restore))},
                                                 {"wait", true}}
                                      .dump());
            return {};
        }

        std::string listImports(const AssetRuntime& assets, const std::filesystem::path& root, const ControlArgs& args,
                                IFormatWriter& out)
        {
            const std::optional<u64> only = args.integer("import");
            if (args.json("import") && !only)
            {
                return std::string("'import' must be an import id");
            }
            const std::vector<ModelImport> imports = assets.imports();
            if (only && std::ranges::find(imports, *only, &ModelImport::id) == imports.end())
            {
                return "no import " + std::to_string(*only);
            }
            out.beginArray("imports", 0);
            for (const ModelImport& record : imports)
            {
                if (!only || record.id == *only)
                {
                    out.beginObject({});
                    writeImport(root, record, out);
                    out.endObject();
                }
            }
            out.endArray();
            return {};
        }

        std::string removeModel(ControlServer& control, AssetRuntime& assets, const std::filesystem::path& root,
                                const ControlArgs& args, IFormatWriter& out)
        {
            std::string name;
            if (std::string error = nameArg(args, "name", name); !error.empty())
            {
                return error;
            }
            const InternedString interned(name.c_str());
            // the source the inverse re-imports: a pending or failed import's, else the manifest's
            const std::vector<ModelImport> imports = assets.imports();
            const auto latest = std::ranges::find(imports.rbegin(), imports.rend(), interned, &ModelImport::name);
            const std::optional<AssetManifest::ModelEntry> model = assets.manifest().getModelEntry(interned);
            if (!model && (latest == imports.rend() || latest->state == ModelImport::State::Done))
            {
                return "no model '" + name + "' in the manifest";
            }
            const std::string source = model ? std::string(model->source.toView()) : latest->source;
            const std::string path = importArgPath(root, source);
            if (!model && path.empty() && !pathFromUtf8(source).is_absolute())
            {
                return "model '" + name + "' was baked from '" + source +
                    "', not from a file under the import root; removing it could not be undone";
            }

            AssetRuntime::RemovedModel removed{.source = source};
            const bool pending =
                !model && (latest->state == ModelImport::State::Queued || latest->state == ModelImport::State::Baking);
            // a failed or cancelled import left nothing to remove; undoing it still re-imports
            if (model || pending)
            {
                Expected<AssetRuntime::RemovedModel> result = assets.removeModel(interned);
                if (!result)
                {
                    return "cannot remove model '" + name + "': " + result.error().message;
                }
                removed = std::move(*result);
            }
            if (model)
                control.recordInverse("restoreDeletedModel", nlohmann::json{{"id", removed.recoveryId}}.dump());
            else
                control.recordInverse(
                    path.empty() ? "importExternalModel" : "importModel",
                    nlohmann::json{{"path", path.empty() ? source : path}, {"name", name}, {"wait", true}}.dump());
            out.writeString("name", name);
            out.writeString("path", path);
            out.writeString("recoveryId", removed.recoveryId);
            out.writeBool("cancelled", removed.cancelled);
            writeNames("materials", removed.materials, out);
            writeNames("textures", removed.textures, out);
            return {};
        }
    } // namespace

    void registerAssetCommands(ControlServer& control, AssetRuntime& assets, std::filesystem::path importRoot)
    {
        registerAssetCommands(control, assets, std::move(importRoot), nullptr, {});
    }

    void registerAssetCommands(ControlServer& control, AssetRuntime& assets, std::filesystem::path importRoot,
                               SceneRecoveryAccess* recovery,
                               std::function<std::shared_ptr<void>()> acquireSceneCopyLease)
    {
        control.setRoot("import", importRoot, "model sources: importModel");
        constexpr std::string_view TYPE_DOC = "model, material, texture, environment or oceanProfile";
        constexpr std::string_view LIST_TYPE_DOC =
            "model, material, texture, environment, oceanProfile, skeleton, clip or spriteAtlas (status is its page's)";
        static constexpr std::string_view LIST_TYPE_CHOICES[] = {
            "model", "material", "texture", "environment", "oceanProfile", "skeleton", "clip", SPRITE_ATLAS_KIND};
        static constexpr std::string_view TYPE_CHOICES[] = {"model", "material", "texture", "environment",
                                                            "oceanProfile"};
        constexpr ArgDesc MATERIAL_ARG = requiredArg("name", ArgKind::String, "the material");
        constexpr ArgDesc FIELDS_ARG = optionalArg(
            "fields", ArgKind::Json,
            "an object of material fields (the material query lists them): albedoFactor, metallicFactor, "
            "roughnessFactor, emissiveFactor, emissiveStrength, occlusionStrength, normalScale, thinLeafTransmission, "
            "alphaMode "
            "(0 Opaque, 1 Mask, 2 Blend, 3 Premultiplied, 4 Additive), alphaCutoff, doubleSided, castsShadow "
            "(a Blend material casts a shadow into the sun's cascades and the light maps, dithered by its "
            "alpha; default false, ignored by the other modes), unlit (the surface shows its base colour plus "
            "the emission with no lighting, shadows, ambient or reflections; default false), and per "
            "texture slot albedo, normal, "
            "metallicRoughness, occlusion, emissive: <slot>TextureName, <slot>TextureSpace (0 SRGB, 1 "
            "LINEAR) and <slot>WrapU, <slot>WrapV (0 Repeat, 1 MirroredRepeat, 2 ClampToEdge); the "
            "texture transform uvOffset [x,y], uvScale [x,y], uvRotation (radians, counter-clockwise, "
            "KHR_texture_transform in the image's top-left, v-down space); textureFilter (0 Smooth: "
            "trilinear anisotropic, 1 Linear and 2 Nearest: the base level only)");
        constexpr ArgDesc IMPORT_SETTINGS_ARG =
            optionalArg("settings", ArgKind::Json,
                        "importer options (importSettings lists them) written to the source's sidecar over its "
                        "current ones",
                        "the sidecar's, else the defaults");
        control.registerQuery("imports", OWNER,
                              "model imports: queued, baking, done, failed or cancelled; diagnostics is an array "
                              "whose code identifies the issue, extension names the extension or source property, "
                              "required says whether it rejects the import, and policy describes the fallback",
                              {optionalArg("import", ArgKind::Integer, "one import's id", "every import")},
                              [&assets, importRoot](const ControlArgs& args, IFormatWriter& out)
                              { return listImports(assets, importRoot, args, out); });
        control.registerQuery(
            "previewImportedInstance", OWNER,
            "compares an imported instance with the already rebaked model without changing the scene; "
            "generation is the control state generation, and assetRevision and sceneRevision are "
            "hex tokens for apply. changes lists object, property, optional entity, baseline, "
            "current, source and conflict; baseline, current and source are JSON text for property "
            "changes. An add's source is its name; a removal's current is JSON with localChildren "
            "and externalReferences. A current parent describes instanceRoot, source, local or "
            "world. Rebaking is a separate operation",
            {requiredArg("id", ArgKind::Integer, "imported instance root"),
             requiredArg("model", ArgKind::String, "the rebaked model")},
            [&control, &assets](const ControlArgs& args, IFormatWriter& out)
            { return previewInstanceQuery(control, assets, args, out); });
        const auto preparations = std::make_shared<PreparationJobs>();
        const auto flipbookCards = std::make_shared<FlipbookCardJobs>();
        control.registerCommand(
            "prepareFlipbookCard", OWNER,
            "asynchronously converts an atlas to a GLB card under the import root; returns job and status, or path, "
            "width, height and fractionalCells when complete; importModel consumes path. Existing files are never "
            "replaced",
            {requiredPath("path", "import", "atlas image under the import root (PNG, JPEG or TGA)"),
             requiredArg("name", ArgKind::String, "new card file name"),
             requiredArg("metadata", ArgKind::Json, "FlipbookComponent fields including grid and frame rate"),
             optionalArg("size", ArgKind::Json, "card width and height in world units", "[1,1]"),
             optionalArg("pivot", ArgKind::Json, "normalized pivot within the card", "[0.5,0]"),
             optionalArg("emissive", ArgKind::Boolean, "use the atlas as emission as well as albedo", "false"),
             optionalArg("wait", ArgKind::Boolean, "wait for preparation instead of polling flipbookCard", "false")},
            [&control, flipbookCards](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                const auto path = args.string("path");
                const auto name = args.string("name");
                const auto metadata = args.json("metadata");
                if (!path || !name || name->empty() || !metadata)
                    return "path, name and metadata are required";
                if (std::string error = bakeFileNameError("card", *name); !error.empty())
                    return error;
                FlipbookComponent effect;
                std::string error;
                JsonFragmentReader reader(*metadata);
                if (!nlohmann::json::parse(*metadata, nullptr, false).is_object() ||
                    !readValue(FlipbookComponent::spiteSchema(), &effect, reader, error) || !validFlipbook(effect))
                    return "invalid metadata: provide positive grid dimensions, an in-grid frame count, and finite "
                           "rate and time";
                FlipbookCardRequest request{.columns = effect.columns, .rows = effect.rows};
                const auto vectorArg = [&](const char* key, Vec2& value) -> bool
                {
                    const auto text = args.json(key);
                    if (!text)
                        return true;
                    const auto parsed = nlohmann::json::parse(*text, nullptr, false);
                    if (!parsed.is_array() || parsed.size() != 2 || !parsed[0].is_number() || !parsed[1].is_number())
                        return false;
                    const double x = parsed[0].get<double>(), y = parsed[1].get<double>();
                    const auto finite = [](double value)
                    { return (std::bit_cast<u64>(value) & 0x7ff0000000000000ull) != 0x7ff0000000000000ull; };
                    if (!finite(x) || !finite(y) || std::abs(x) > 1000000 || std::abs(y) > 1000000)
                        return false;
                    value = Vec2(static_cast<float>(x), static_cast<float>(y));
                    return true;
                };
                if (!vectorArg("size", request.size) || request.size.x <= 0.0f || request.size.y <= 0.0f ||
                    !vectorArg("pivot", request.pivot) || request.pivot.x < 0.0f || request.pivot.x > 1.0f ||
                    request.pivot.y < 0.0f || request.pivot.y > 1.0f)
                    return "size must be two positive finite values and pivot two values in [0,1]";
                request.emissive = args.boolean("emissive").value_or(false);
                std::filesystem::path source, output;
                if (std::string failure = control.resolveFile("import", *path, source); !failure.empty())
                    return failure;
                if (!std::filesystem::is_regular_file(source))
                    return "atlas source does not exist";
                const std::string relative = *name + ".flipbook.glb";
                if (std::string failure = control.resolveFile("import", relative, output); !failure.empty())
                    return failure;
                if (std::filesystem::exists(output))
                    return "card output already exists; choose a new name";
                u32 activeJobs = 0;
                for (const auto& [id, other] : flipbookCards->jobs)
                {
                    other->poll(false);
                    if (!other->settled && other->path == relative)
                        return "card output is already being prepared";
                    activeJobs += !other->settled ? 1u : 0u;
                }
                if (activeJobs >= 2)
                    return "two flipbook cards are already being prepared; retry when one finishes";
                const u64 id = ++flipbookCards->next;
                auto job = std::make_shared<FlipbookCardJob>();
                job->path = relative;
                job->work = std::async(std::launch::async, [source, output, request, id]
                                       { return prepareFlipbookCard(source, output, request, id); });
                flipbookCards->jobs.emplace(id, job);
                job->poll(args.boolean("wait").value_or(false));
                out.writeU64("job", id);
                out.writeString("status", !job->settled ? "preparing" : job->result.error.empty() ? "ready" : "failed");
                if (job->settled && !job->result.error.empty())
                    return job->result.error;
                if (job->settled)
                {
                    out.writeString("path", job->path);
                    out.writeU32("width", job->result.width);
                    out.writeU32("height", job->result.height);
                    out.writeBool("fractionalCells",
                                  job->result.width % request.columns != 0 || job->result.height % request.rows != 0);
                }
                return {};
            });
        control.registerQuery(
            "flipbookCard", OWNER, "preparation job status: status, path, width, height or an actionable error",
            {requiredArg("job", ArgKind::Integer, "id returned by prepareFlipbookCard")},
            [flipbookCards](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                const auto id = args.integer("job");
                if (!id || !flipbookCards->jobs.contains(*id))
                    return "unknown flipbook card job";
                auto& job = *flipbookCards->jobs.at(*id);
                job.poll(false);
                out.writeString("status", !job.settled ? "preparing" : job.result.error.empty() ? "ready" : "failed");
                if (job.settled && !job.result.error.empty())
                    out.writeString("error", job.result.error);
                if (job.settled && job.result.error.empty())
                {
                    out.writeString("path", job.path);
                    out.writeU32("width", job.result.width);
                    out.writeU32("height", job.result.height);
                }
                return {};
            });
        const auto startPreparation =
            [&assets, preparations](const std::string& modelName, const std::filesystem::path& source,
                                    const std::filesystem::path& sourceRoot, const ModelImportSettings& settings,
                                    std::string overlay, IFormatWriter& out) -> std::string
        {
            for (auto& [id, job] : preparations->jobs)
                job->poll();
            std::erase_if(preparations->jobs,
                          [](const auto& pair)
                          {
                              return pair.second->settled && !pair.second->cleanup.valid() &&
                                  (pair.second->cancelled->load() || !pair.second->prepared);
                          });
            const auto running =
                std::ranges::count_if(preparations->jobs, [](const auto& pair) { return !pair.second->settled; });
            if (running >= 4)
                return "too many preparations are still running";
            for (const auto& [id, job] : preparations->jobs)
                if (!job->cancelled->load() &&
                    AssetManifest::bakeFileKey(job->model) == AssetManifest::bakeFileKey(modelName) &&
                    (!job->settled || job->prepared))
                    return "a preparation for this model already exists";
            if (preparations->jobs.size() >= 64)
                return "too many preparation records";
            auto snapshot = assets.captureModelPreparation(utf8Path(source), InternedString(modelName),
                                                           utf8Path(sourceRoot), settings, true, std::move(overlay));
            if (!snapshot)
                return snapshot.error().message;
            auto job = std::make_shared<PreparationJob>();
            job->model = modelName;
            const auto captured = *snapshot;
            const bool hasBaseline = assets.manifest().getModelEntry(InternedString(modelName)).has_value();
            job->snapshot = captured;
            job->work = std::async(
                std::launch::async,
                [captured, hasBaseline, cancellation = job->cancelled]() -> Expected<PreparedVersions>
                {
                    PreparedVersions versions;
                    if (hasBaseline)
                    {
                        auto baseline = AssetRuntime::capturePublishedModel(*captured);
                        if (!baseline)
                            return std::unexpected(baseline.error());
                        versions.baseline = std::move(*baseline);
                    }
                    if (cancellation->load())
                        return std::unexpected(makeError(ErrorCode::Cancelled, "preparation cancelled"));
                    auto candidate = AssetRuntime::prepareModel(*captured);
                    if (!candidate)
                        return std::unexpected(candidate.error());
                    versions.candidate = std::move(*candidate);
                    if (versions.baseline)
                    {
                        if (versions.baseline->baselineModelFingerprint != versions.candidate->baselineModelFingerprint)
                            return std::unexpected(
                                makeError(ErrorCode::AssetSaveFailed, "published model changed during preparation"));
                        if (const auto valid = AssetRuntime::verifyPublishedModel(*captured, *versions.baseline);
                            !valid)
                            return std::unexpected(valid.error());
                    }
                    if (cancellation->load())
                        return std::unexpected(makeError(ErrorCode::Cancelled, "preparation cancelled"));
                    return versions;
                });
            const u64 id = ++preparations->next;
            preparations->jobs.emplace(id, job);
            out.writeU64("preparation", id);
            out.writeString("status", "preparing");
            return {};
        };
        control.registerCommand(
            "prepareModelUpdate", OWNER,
            "prepare the current glTF model source on a worker without publishing assets, changing its sidecar or "
            "editing the scene",
            {requiredArg("model", ArgKind::String, "existing model name"),
             optionalArg("settings", ArgKind::Json, "options over the source sidecar", "sidecar or defaults")},
            [&control, &assets, importRoot, startPreparation](const ControlArgs& args, IFormatWriter& out)
            {
                const auto modelName = *args.string("model");
                const auto model = assets.manifest().getModelEntry(InternedString(modelName));
                if (!model || !model->source.isValid())
                    return std::string("model has no imported source");
                auto relative = importArgPath(importRoot, std::string(model->source.toView()));
                if (relative.empty() && model->authoringSource.isValid())
                {
                    const auto identity = pathFromUtf8(model->authoringSource.toView());
                    if (!identity.is_absolute() && isGltfFile(identity))
                        relative = utf8Path(identity);
                }
                std::filesystem::path checkedSource;
                std::filesystem::path sourceRoot = importRoot;
                if (relative.empty())
                {
                    if (const std::string error = modelSource(control, importRoot, *model, checkedSource, sourceRoot);
                        !error.empty())
                        return error;
                }
                else if (const std::string error = control.resolveFile("import", relative, checkedSource);
                         !error.empty())
                    return error;
                auto settings = model->importSettings;
                if (const auto text = args.json("settings"))
                    if (const auto error = applyModelImportSettings(settings, *text); !error.empty())
                        return error;
                return startPreparation(modelName, checkedSource, sourceRoot, settings,
                                        args.json("settings").value_or(""), out);
            });
        control.registerCommand(
            "prepareModelImport", OWNER, "prepare a new glTF model privately without publishing it",
            {requiredPath("path", "import", "a file under the import root"),
             optionalArg("name", ArgKind::String, "the model's name", "the file's stem"),
             optionalArg("settings", ArgKind::Json, "options over the source sidecar", "sidecar or defaults")},
            [&control, &assets, importRoot, startPreparation](const ControlArgs& args, IFormatWriter& out)
            {
                std::filesystem::path source;
                if (const std::string error = importPath(control, importRoot, args, source); !error.empty())
                    return error;
                const auto name = args.string("name").value_or(utf8Path(source.stem()));
                for (const auto& [existing, entry] : assets.manifest().getModels())
                    if (AssetManifest::bakeFileKey(existing.toView()) == AssetManifest::bakeFileKey(name))
                        return std::string("model already exists; prepare an update instead");
                ModelImportSettings settings;
                if (const auto text = args.json("settings"))
                    if (const auto error = applyModelImportSettings(settings, *text); !error.empty())
                        return error;
                return startPreparation(name, source, importRoot, settings, args.json("settings").value_or(""), out);
            });
        control.registerQuery(
            "modelPreparation", OWNER,
            "inspect a private model preparation. status describes progress, settled means all workers have stopped, "
            "and error explains a failure. A ready result lists affected instances, sourceDocument, sourceFingerprint, "
            "baselineModelFingerprint and manifestGeneration. rootNode indexes nodes, whose entries include "
            "sourceObject, "
            "stable identity, local position, rotation and scale, and indices for children and meshes. submeshes "
            "entries "
            "include materialIndex. Staged entries describe materials, textures, skeletons and animations. diagnostics "
            "report code, extension, required and policy for compatibility issues.",
            {requiredArg("preparation", ArgKind::Integer, "preparation id")},
            [&control, &assets, preparations](const ControlArgs& args, IFormatWriter& out)
            {
                const auto found = preparations->jobs.find(*args.integer("preparation"));
                if (found == preparations->jobs.end())
                    return std::string("unknown preparation");
                auto& job = *found->second;
                job.poll();
                out.writeString("status", job.status());
                out.writeBool("settled", job.settled && !job.cleanup.valid() && !job.sceneWork.valid());
                out.writeString("model", job.model);
                if (!job.error.empty())
                    out.writeString("error", job.error);
                if (job.prepared)
                {
                    std::vector<u64> instances;
                    const auto model = assets.manifest().getModelEntry(InternedString(job.model));
                    if (const World* world = control.world(args); world && model)
                    {
                        const auto document = model->authoringSource.isValid() ? model->authoringSource : model->source;
                        for (const Chunk* chunk :
                             world->getChunksContaining(Aspect(getComponentId<ImportedSourceComponent>())))
                            for (const Entity entity : chunk->entities())
                            {
                                const auto& provenance = world->getConstComponent<ImportedSourceComponent>(entity);
                                if (provenance.instanceRoot && provenance.document == document)
                                    instances.push_back(entity.id());
                            }
                    }
                    std::ranges::sort(instances);
                    out.beginArray("instances", static_cast<u32>(instances.size()));
                    for (const auto instance : instances)
                        out.writeU64("", instance);
                    out.endArray();
                    out.writeString("sourceFingerprint", job.prepared->sourceFingerprint.toView());
                    out.writeString("baselineModelFingerprint", job.prepared->baselineModelFingerprint.toView());
                    out.writeU64("manifestGeneration", job.prepared->manifestGeneration);
                    out.writeString("sourceDocument", job.prepared->hierarchy->sourceDocument.toView());
                    writeSettings(job.prepared->settings, "settings", out);
                    out.beginArray("submeshes", static_cast<u32>(job.prepared->hierarchy->submeshes.size()));
                    for (const auto& submesh : job.prepared->hierarchy->submeshes)
                    {
                        out.beginObject({});
                        out.writeString("name", submesh.name.toView());
                        out.writeU64("materialIndex", submesh.materialIndex);
                        out.endObject();
                    }
                    out.endArray();
                    writeNames("modelMaterials", job.prepared->hierarchy->materialNames, out);
                    out.writeU64("rootNode", job.prepared->hierarchy->rootNodeIndex);
                    out.beginArray("nodes", static_cast<u32>(job.prepared->hierarchy->nodes.size()));
                    for (const auto& node : job.prepared->hierarchy->nodes)
                    {
                        out.beginObject({});
                        out.writeString("name", node.name.toView());
                        out.writeString("sourceObject", node.sourceObject.toView());
                        out.writeBool("stable", node.sourceObjectStable);
                        const auto& transform = node.localTransform;
                        out.beginArray("position", 3);
                        out.writeF64("", transform.position.x);
                        out.writeF64("", transform.position.y);
                        out.writeF64("", transform.position.z);
                        out.endArray();
                        out.beginArray("rotation", 4);
                        out.writeF64("", transform.rotation.x);
                        out.writeF64("", transform.rotation.y);
                        out.writeF64("", transform.rotation.z);
                        out.writeF64("", transform.rotation.w);
                        out.endArray();
                        out.beginArray("scale", 3);
                        out.writeF64("", transform.scale.x);
                        out.writeF64("", transform.scale.y);
                        out.writeF64("", transform.scale.z);
                        out.endArray();
                        out.beginArray("children", static_cast<u32>(node.childrenIndices.size()));
                        for (const auto child : node.childrenIndices)
                            out.writeU64("", child);
                        out.endArray();
                        out.beginArray("meshes", static_cast<u32>(node.meshIndices.size()));
                        for (const auto mesh : node.meshIndices)
                            out.writeU64("", mesh);
                        out.endArray();
                        out.endObject();
                    }
                    out.endArray();
                    const auto writeEntries = [&out](const std::string_view key, const auto& entries)
                    {
                        out.beginArray(key, static_cast<u32>(entries.size()));
                        for (const auto& [name, entry] : entries)
                        {
                            out.beginObject({});
                            out.writeString("name", name.toView());
                            out.writeString("baked", entry.baked.toView());
                            out.endObject();
                        }
                        out.endArray();
                    };
                    writeEntries("materials", job.prepared->materials);
                    writeEntries("textures", job.prepared->textures);
                    writeEntries("skeletons", job.prepared->skeletons);
                    writeEntries("animations", job.prepared->animations);
                    writeNames("sharedMaterials", job.prepared->sharedMaterials, out);
                    writeNames("sharedTextures", job.prepared->sharedTextures, out);
                    out.beginArray("diagnostics", static_cast<u32>(job.prepared->diagnostics.size()));
                    for (const auto& diagnostic : job.prepared->diagnostics)
                    {
                        out.beginObject({});
                        out.writeString("code", diagnostic.code);
                        out.writeString("extension", diagnostic.extension);
                        out.writeBool("required", diagnostic.required);
                        out.writeString("policy", diagnostic.policy);
                        out.endObject();
                    }
                    out.endArray();
                }
                return std::string{};
            });
        control.registerQuery("previewModelPreparation", OWNER,
                              "compare a prepared model with an imported instance without publishing it",
                              {requiredArg("preparation", ArgKind::Integer, "preparation id"),
                               requiredArg("id", ArgKind::Integer, "imported instance root"),
                               requiredArg("model", ArgKind::String, "model name")},
                              [&control, &assets, preparations](const ControlArgs& args, IFormatWriter& out)
                              {
                                  const auto found = preparations->jobs.find(*args.integer("preparation"));
                                  if (found == preparations->jobs.end())
                                      return std::string("unknown preparation");
                                  auto& job = *found->second;
                                  job.poll();
                                  if (!job.prepared || job.cancelled->load())
                                      return std::string("preparation is not ready");
                                  return previewInstanceQuery(control, assets, args, out, job.prepared.get());
                              });
        control.registerQuery("validateModelPreparationResolutions", OWNER,
                              "validate conflict resolutions against a prepared candidate without changing the scene; "
                              "planned counts the commands the selected resolution would require",
                              {requiredArg("preparation", ArgKind::Integer, "preparation id"),
                               requiredArg("id", ArgKind::Entity, "imported instance root"),
                               requiredArg("model", ArgKind::String, "model name"),
                               requiredArg("sceneRevision", ArgKind::String, "scene revision from preview"),
                               requiredArg("assetRevision", ArgKind::String, "asset revision from preview"),
                               optionalArg("resolutions", ArgKind::Json, "array of object, property, choice", "[]")},
                              [&control, &assets, preparations](const ControlArgs& args, IFormatWriter& out)
                              {
                                  const auto found = preparations->jobs.find(*args.integer("preparation"));
                                  if (found == preparations->jobs.end())
                                      return std::string("unknown preparation");
                                  auto& job = *found->second;
                                  job.poll();
                                  if (!job.prepared || job.cancelled->load())
                                      return std::string("preparation is not ready");
                                  nlohmann::json commands;
                                  bool applying = false;
                                  const auto error = applyInstance(control, assets, args, out, applying,
                                                                   job.prepared.get(), &commands);
                                  if (error.empty())
                                      out.writeU32("planned", static_cast<u32>(commands.size()));
                                  return error;
                              });
        control.registerViewCommand(
            "prepareModelScene", OWNER, "prepare an isolated scene copy for every staged model instance",
            {requiredArg("preparation", ArgKind::Integer, "preparation id"),
             requiredArg("model", ArgKind::String, "model name"),
             requiredArg("sceneRevision", ArgKind::String, "scene revision from preview"),
             requiredArg("assetRevision", ArgKind::String, "asset revision from preview"),
             requiredArg("instances", ArgKind::Json, "array of id and resolutions for every instance")},
            [&control, &assets, preparations, recovery, acquireSceneCopyLease](const ControlArgs& args,
                                                                               IFormatWriter& out)
            {
                const auto found = preparations->jobs.find(*args.integer("preparation"));
                if (found == preparations->jobs.end())
                    return std::string("unknown preparation");
                auto& job = *found->second;
                job.poll();
                if (!job.prepared || job.cancelled->load())
                    return std::string("preparation is not ready");
                if (!recovery || !recovery->capture)
                    return std::string("scene capture is unavailable");
                if (job.sceneWork.valid())
                    return std::string("scene preparation is already running");
                const World* world = control.world(args);
                if (!world)
                    return control.unknownWorld(args);
                const auto modelName = *args.string("model");
                const auto model = assets.manifest().getModelEntry(InternedString(modelName));
                if (!model)
                    return std::string("model is not in the manifest");
                const auto document = model->authoringSource.isValid() ? model->authoringSource : model->source;
                for (const auto& [name, entry] : assets.manifest().getModels())
                    if (name != InternedString(modelName) &&
                        (entry.authoringSource.isValid() ? entry.authoringSource : entry.source) == document)
                        return std::string("multiple models share this source; instance ownership is ambiguous");
                std::unordered_set<u64> instances;
                for (const Chunk* chunk : world->getChunksContaining(Aspect(getComponentId<ImportedSourceComponent>())))
                    for (const Entity entity : chunk->entities())
                    {
                        const auto& source = world->getConstComponent<ImportedSourceComponent>(entity);
                        if (source.instanceRoot && source.document == document)
                            instances.insert(entity.id());
                    }
                if (instances.empty())
                    return std::string("scene preparation needs at least one imported instance");
                const auto values = nlohmann::json::parse(*args.json("instances"), nullptr, false);
                if (!values.is_array() || values.size() != instances.size())
                    return std::string("every imported instance requires a resolution entry");
                auto plans = nlohmann::json::array();
                for (const auto& value : values)
                {
                    if (!value.is_object() || !value.contains("id") || !value["id"].is_number_unsigned() ||
                        !value.contains("resolutions") || !value["resolutions"].is_array() ||
                        instances.erase(value["id"].get<u64>()) != 1)
                        return std::string("invalid or duplicate instance resolution entry");
                    nlohmann::json planArgs{{"world", control.worldName(args)},
                                            {"id", value["id"]},
                                            {"model", modelName},
                                            {"sceneRevision", *args.string("sceneRevision")},
                                            {"assetRevision", *args.string("assetRevision")},
                                            {"resolutions", value["resolutions"]}};
                    nlohmann::json commands;
                    bool applying = false;
                    if (const auto error = applyInstance(control, assets, ControlArgs(&planArgs), out, applying,
                                                         job.prepared.get(), &commands);
                        !error.empty())
                        return error;
                    plans.push_back(std::move(commands));
                }
                if (!acquireSceneCopyLease)
                    return std::string("module lifetime guard is unavailable");
                auto lease = acquireSceneCopyLease();
                if (!lease)
                    return std::string("scene preparation requires no loaded runtime modules or module change");
                const auto snapshot = recovery->capture();
                if (!snapshot)
                    return snapshot.error().message;
                job.candidateScene.clear();
                job.sceneError.clear();
                job.sceneSettled = false;
                job.sceneStateGeneration = control.stateGeneration();
                job.sceneManifestGeneration = assets.manifest().generation();
                job.sceneSchemaRevision = getComponentRegistryRevision();
                job.sceneWork = std::async(
                    std::launch::async,
                    [scene = *snapshot, plans = std::move(plans), root = control.root("scene"),
                     lease = std::move(lease), prepared = job.prepared, baseline = job.baseline,
                     captured = job.snapshot]() mutable -> Expected<std::string>
                    {
                        auto held = std::move(lease); // released when the worker exits, before its future is polled
                        if (const auto valid = AssetRuntime::verifyPreparationInputs(*prepared); !valid)
                            return std::unexpected(valid.error());
                        if (baseline)
                            if (const auto valid = AssetRuntime::verifyPublishedModel(*captured, *baseline); !valid)
                                return std::unexpected(valid.error());
                        auto result = prepareSceneCopy(std::move(scene), std::move(plans), root);
                        if (!result)
                            return result;
                        if (const auto valid = AssetRuntime::verifyPreparationInputs(*prepared); !valid)
                            return std::unexpected(valid.error());
                        if (baseline)
                            if (const auto valid = AssetRuntime::verifyPublishedModel(*captured, *baseline); !valid)
                                return std::unexpected(valid.error());
                        return result;
                    });
                out.writeString("status", "preparing");
                return std::string{};
            });
        control.registerQuery("modelScenePreparation", OWNER, "inspect an isolated scene preparation",
                              {requiredArg("preparation", ArgKind::Integer, "preparation id")},
                              [&control, &assets, preparations](const ControlArgs& args, IFormatWriter& out)
                              {
                                  const auto found = preparations->jobs.find(*args.integer("preparation"));
                                  if (found == preparations->jobs.end())
                                      return std::string("unknown preparation");
                                  auto& job = *found->second;
                                  job.poll();
                                  if (job.cancelled->load())
                                      out.writeString("status", "cancelled");
                                  else if (!job.sceneSettled)
                                      out.writeString("status", job.sceneWork.valid() ? "preparing" : "notStarted");
                                  else if (!job.sceneError.empty())
                                  {
                                      out.writeString("status", "failed");
                                      out.writeString("error", job.sceneError);
                                  }
                                  else if (control.stateGeneration() != job.sceneStateGeneration ||
                                           assets.manifest().generation() != job.sceneManifestGeneration ||
                                           getComponentRegistryRevision() != job.sceneSchemaRevision)
                                      out.writeString("status", "stale");
                                  else
                                  {
                                      out.writeString("status", "ready");
                                      out.writeString("scene", job.candidateScene);
                                  }
                                  return std::string{};
                              });
        control.registerCommand("cancelModelPreparation", OWNER, "discard a private model preparation",
                                {requiredArg("preparation", ArgKind::Integer, "preparation id")},
                                [preparations](const ControlArgs& args, IFormatWriter& out)
                                {
                                    const auto found = preparations->jobs.find(*args.integer("preparation"));
                                    if (found == preparations->jobs.end())
                                        return std::string("unknown preparation");
                                    auto& job = *found->second;
                                    job.poll();
                                    job.discard();
                                    job.cancelled->store(true);
                                    job.candidateScene.clear();
                                    job.poll();
                                    out.writeString("status", "cancelled");
                                    return std::string{};
                                });
        const auto applyingReimport = std::make_shared<bool>(false);
        control.registerUndoableCommand(
            "setImportedBaseline", OWNER, "internal history operation for imported source baselines",
            {requiredArg("id", ArgKind::Entity, "imported entity"), requiredArg("value", ArgKind::Json, "baseline")},
            [&control, applyingReimport](const ControlArgs& args, IFormatWriter& out)
            { return setImportedBaseline(control, args, out, *applyingReimport); });
        control.registerUndoableCommand(
            "createImportedNodes", OWNER, "internal history operation for source additions",
            {requiredArg("id", ArgKind::Entity, "imported instance root"),
             requiredArg("nodes", ArgKind::Json, "source nodes in parent-first order"),
             optionalArg("moves", ArgKind::Json, "existing objects to attach to new nodes", "[]")},
            [&control, applyingReimport](const ControlArgs& args, IFormatWriter& out)
            { return createImportedNodes(control, args, out, *applyingReimport); });
        control.registerCommand(
            "applyImportedInstance", OWNER,
            "applies the preview as one undoable batch after checking sceneRevision and assetRevision; each "
            "conflicting "
            "change requires a resolutions entry naming object, property and choice: source applies it, local "
            "keeps "
            "the local value while advancing its baseline. For removal, local keeps the entity as local and source "
            "removes it while retaining children; incoming references must be resolved first. Changed unstable IDs "
            "and unsupported multi-mesh or skinned additions are refused. No changes returns applied as zero; "
            "otherwise results contains the batch replies. Asset rebaking is a separate operation",
            {requiredArg("id", ArgKind::Entity, "imported instance root"),
             requiredArg("model", ArgKind::String, "already rebaked model"),
             requiredArg("sceneRevision", ArgKind::String, "scene revision from preview"),
             requiredArg("assetRevision", ArgKind::String, "asset revision from preview"),
             optionalArg("resolutions", ArgKind::Json, "array of object, property, choice (source or local)", "[]")},
            [&control, &assets, applyingReimport](const ControlArgs& args, IFormatWriter& out)
            { return applyInstance(control, assets, args, out, *applyingReimport); });
        control.registerUndoableViewCommand(
            "importModel", OWNER, "bakes a glTF file into a model with its materials and textures",
            {requiredPath("path", "import", "a file under the import root"),
             optionalArg("name", ArgKind::String, "the model's name", "the file's stem"), IMPORT_SETTINGS_ARG,
             optionalArg("wait", ArgKind::Boolean, "bake before replying instead of in the background", "false"),
             optionalArg("previews", ArgKind::Boolean, "generate optional previews", "true")},
            [&control, &assets, importRoot](const ControlArgs& args, IFormatWriter& out)
            { return importModel(control, assets, importRoot, args, out); });
        control.registerUndoableViewCommand(
            "importExternalModel", OWNER, "bakes an absolute glTF source with dependencies beside it",
            {requiredPath("path", HOST_ROOT, "absolute glTF or GLB source path"),
             optionalArg("name", ArgKind::String, "the model's name", "the file's stem"), IMPORT_SETTINGS_ARG,
             optionalArg("wait", ArgKind::Boolean, "bake before replying instead of in the background", "false"),
             optionalArg("previews", ArgKind::Boolean, "generate optional previews", "true")},
            [&control, &assets, importRoot](const ControlArgs& args, IFormatWriter& out)
            { return importModel(control, assets, importRoot, args, out, true); });
        const auto imageImports = std::make_shared<ExternalImageJobs>();
        control.registerCommand(
            "importExternalImage", OWNER, "bakes an external image as a texture or HDR environment on a worker",
            {requiredPath("path", HOST_ROOT, "absolute image source path")},
            [&assets, imageImports](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                const auto source = pathFromUtf8(*args.string("path"));
                if (!source.is_absolute())
                    return "path must be absolute";
                std::string ext = utf8Path(source.extension());
                std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return std::tolower(c); });
                const bool environment = ext == ".hdr";
                if (!environment && ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".bmp" &&
                    ext != ".tga" && ext != ".gif" && ext != ".psd" && ext != ".pic" && ext != ".ppm" &&
                    ext != ".pgm" && ext != ".pnm")
                    return "unsupported image format";
                std::error_code error;
                if (!std::filesystem::is_regular_file(source, error))
                    return "cannot open image source";
                const std::string name = AssetManifest::safeBakeName(utf8Path(source.stem()));
                if (name.empty())
                    return "image source has no usable name";
                if (imageImports->jobs.size() >= 64)
                {
                    std::erase_if(imageImports->jobs, [](const auto& entry) { return entry.second->settled; });
                    if (imageImports->jobs.size() >= 64)
                        return "too many image imports";
                }
                const u64 id = ++imageImports->next;
                const auto base =
                    resolveContentPath(pathFromUtf8(assets.baker().config().outputDirectory)) / ".external-images";
                std::filesystem::create_directories(base, error);
                if (error)
                    return "cannot create image staging directory: " + error.message();
                auto stage = base / std::to_string(id);
                while (std::filesystem::exists(stage, error) && !error)
                    stage = base / std::to_string(++imageImports->next);
                if (error || !std::filesystem::create_directory(stage, error))
                    return "cannot create image staging directory: " + error.message();
                stage = std::filesystem::canonical(stage, error);
                if (error)
                    return "cannot resolve image staging directory: " + error.message();
                auto job = std::make_shared<ExternalImageJob>();
                job->stage = stage;
                job->environment = environment;
                AssetBakerConfig original = assets.baker().config();
                job->textureDir = pathFromUtf8(original.textureSubdir);
                job->materialDir = pathFromUtf8(original.materialSubdir);
                job->environmentDir = pathFromUtf8(original.environmentSubdir);
                IOManager* io = &assets.io();
                job->work = std::async(
                    std::launch::async,
                    [io, original, source, stage, name, environment]() mutable -> ExternalImageResult
                    {
                        ExternalImageResult result;
                        const std::string directory = utf8Path(stage);
                        original.outputDirectory = directory.c_str();
                        AssetBaker baker(*io);
                        if (auto initialized = baker.initialize(original); !initialized)
                        {
                            result.error = initialized.error().message;
                            return result;
                        }
                        const InternedString sourceName(utf8Path(source));
                        const BakeResult baked = environment
                            ? baker.bakeEnvironment(sourceName, InternedString(name), EnvironmentFormat::Rgba16Float)
                            : baker.bakeTexture(sourceName);
                        if (!baked.success)
                        {
                            result.error = std::string(baked.errorMessage.toView());
                            return result;
                        }
                        result.name = baked.name;
                        if (environment)
                            result.environment = *baker.getManifest().getEnvironmentEntry(baked.name);
                        else
                            result.texture = *baker.getManifest().getTextureEntry(baked.name);
                        return result;
                    });
                imageImports->jobs.emplace(id, job);
                out.writeU64("import", id);
                return {};
            });
        control.registerQuery("externalImageImport", OWNER, "inspect an external image import",
                              {requiredArg("import", ArgKind::Integer, "image import id")},
                              [imageImports](const ControlArgs& args, IFormatWriter& out) -> std::string
                              {
                                  const auto found = imageImports->jobs.find(*args.integer("import"));
                                  if (found == imageImports->jobs.end())
                                      return "unknown image import";
                                  const auto& job = *found->second;
                                  const bool ready = !job.settled &&
                                      job.work.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                                  out.writeString("status",
                                                  job.settled ? (job.result.error.empty() ? "done" : "failed")
                                                      : ready ? "ready"
                                                              : "baking");
                                  if (job.result.name.isValid())
                                      out.writeString("name", job.result.name.toView());
                                  if (!job.result.error.empty())
                                      out.writeString("error", job.result.error);
                                  return {};
                              });
        control.registerCommand(
            "publishExternalImage", OWNER, "publish a ready external image import",
            {requiredArg("import", ArgKind::Integer, "image import id")},
            [&assets, imageImports](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                const auto found = imageImports->jobs.find(*args.integer("import"));
                if (found == imageImports->jobs.end())
                    return "unknown image import";
                auto& job = *found->second;
                if (!job.settled && job.work.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                    return "image import is still baking";
                if (!job.settled && job.work.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                {
                    try
                    {
                        job.result = job.work.get();
                    }
                    catch (const std::exception& error)
                    {
                        job.result.error = error.what();
                    }
                    catch (...)
                    {
                        job.result.error = "image bake failed";
                    }
                    auto& result = job.result;
                    const InternedString stagedPath = job.environment ? result.environment.baked : result.texture.baked;
                    if (stagedPath.isValid())
                        job.stagedFile = pathFromUtf8(stagedPath.toView());
                    if (result.error.empty())
                    {
                        AssetManifest& manifest = assets.baker().getManifest();
                        const std::string key = AssetManifest::bakeFileKey(result.name.toView());
                        if (job.environment)
                        {
                            for (const auto& [name, entry] : manifest.getEnvironments())
                                if (AssetManifest::bakeFileKey(name.toView()) == key)
                                    result.error = "environment name already exists";
                        }
                        else
                        {
                            for (const auto& [name, entry] : manifest.getTextures())
                                if (AssetManifest::bakeFileKey(name.toView()) == key)
                                    result.error = "texture name already exists";
                            if (manifest.findTextureByHash(result.texture.contentHash).isValid())
                                result.error = "texture content already exists";
                        }
                        if (result.error.empty())
                        {
                            InternedString& baked = job.environment ? result.environment.baked : result.texture.baked;
                            const auto from = pathFromUtf8(baked.toView());
                            job.stagedFile = from;
                            const auto& config = assets.baker().config();
                            const auto target = resolveContentPath(pathFromUtf8(config.outputDirectory)) /
                                pathFromUtf8(job.environment ? config.environmentSubdir : config.textureSubdir) /
                                from.filename();
                            std::error_code error;
                            std::filesystem::create_directories(target.parent_path(), error);
                            if (!error)
                                std::filesystem::create_hard_link(from, target, error);
                            if (error)
                                result.error = "cannot publish image: " + error.message();
                            else
                            {
                                baked = InternedString(utf8Path(target));
                                if (job.environment)
                                    manifest.overwriteEnvironment(result.name, result.environment);
                                else
                                    manifest.tryAddTexture(result.name, result.texture);
                                if (!assets.baker().saveManifest())
                                {
                                    if (job.environment)
                                        manifest.removeEnvironment(result.name);
                                    else
                                        manifest.removeTexture(result.name);
                                    std::filesystem::remove(target, error);
                                    result.error = "cannot save the asset manifest";
                                }
                            }
                        }
                    }
                    job.cleanup();
                    job.settled = true;
                }
                out.writeString("status", !job.settled ? "baking" : job.result.error.empty() ? "done" : "failed");
                if (job.result.name.isValid())
                    out.writeString("name", job.result.name.toView());
                if (!job.result.error.empty())
                    out.writeString("error", job.result.error);
                return {};
            });
        const auto audioImports = std::make_shared<ExternalAudioJobs>();
        control.registerCommand(
            "importExternalAudio", OWNER, "copies an external WAV, MP3 or FLAC clip into the project audio folder",
            {requiredPath("path", HOST_ROOT, "absolute audio source path")},
            [audioImports](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                const std::string path = *args.string("path");
                const auto source = pathFromUtf8(path);
                if (!source.is_absolute())
                    return "path must be absolute";
                std::string extension = utf8Path(source.extension());
                std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return std::tolower(c); });
                if (extension != ".wav" && extension != ".mp3" && extension != ".flac")
                    return "audio source must be WAV, MP3 or FLAC";
                std::error_code error;
                if (!std::filesystem::is_regular_file(source, error))
                    return "cannot open audio source '" + path + "'";
                const std::string stem = AssetManifest::safeBakeName(utf8Path(source.stem()));
                if (stem.empty())
                    return "audio source has no usable name";
                if (audioImports->jobs.size() >= 64)
                {
                    std::erase_if(audioImports->jobs, [](const auto& entry) { return entry.second->settled; });
                    if (audioImports->jobs.size() >= 64)
                        return "too many audio imports";
                }
                const std::string name = stem + extension;
                const auto destination = contentRoot() / "assets/audio" / pathFromUtf8(name);
                if (std::filesystem::exists(destination, error))
                    return "audio clip already exists: " + name;
                const u64 id = ++audioImports->next;
                auto job = std::make_shared<ExternalAudioJob>();
                job->name = name;
                job->work = std::async(
                    std::launch::async,
                    [source, destination, id]() -> AudioProbeResult
                    {
                        AudioProbeResult result;
                        std::error_code error;
                        std::filesystem::create_directories(destination.parent_path(), error);
                        if (error)
                        {
                            result.error = "cannot create audio directory: " + error.message();
                            return result;
                        }
                        auto staging = destination;
                        staging += ".import-" + std::to_string(id);
                        if (!std::filesystem::create_directory(staging, error))
                        {
                            result.error = "cannot create audio staging directory: " +
                                (error ? error.message() : "already exists");
                            return result;
                        }
                        struct Cleanup
                        {
                            std::filesystem::path path;
                            ~Cleanup()
                            {
                                std::error_code ignored;
                                std::filesystem::remove_all(path, ignored);
                            }
                        } cleanup{staging};
                        const auto temporary = staging / destination.filename();
                        if (!std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::none, error))
                        {
                            result.error = "cannot copy audio source: " + error.message();
                            return result;
                        }
                        result = probeAudio(temporary);
                        if (!result.error.empty())
                            return result;
                        std::filesystem::create_hard_link(temporary, destination, error);
                        if (error)
                            result.error =
                                "cannot publish audio clip (destination may already exist): " + error.message();
                        return result;
                    });
                audioImports->jobs.emplace(id, job);
                out.writeU64("import", id);
                out.writeString("name", name);
                return {};
            });
        control.registerQuery(
            "externalAudioImport", OWNER, "inspect audio import status, channels, sampleRate, frames and error",
            {requiredArg("import", ArgKind::Integer, "audio import id")},
            [audioImports](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                const auto found = audioImports->jobs.find(*args.integer("import"));
                if (found == audioImports->jobs.end())
                    return "unknown audio import";
                auto& job = *found->second;
                if (!job.settled && job.work.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                {
                    try
                    {
                        job.result = job.work.get();
                    }
                    catch (const std::exception& error)
                    {
                        job.result.error = error.what();
                    }
                    catch (...)
                    {
                        job.result.error = "audio import failed";
                    }
                    job.settled = true;
                }
                out.writeString("name", job.name);
                out.writeString("status", !job.settled ? "copying" : job.result.error.empty() ? "done" : "failed");
                if (!job.result.error.empty())
                    out.writeString("error", job.result.error);
                if (job.settled && job.result.error.empty())
                {
                    out.writeU64("channels", job.result.channels);
                    out.writeU64("sampleRate", job.result.sampleRate);
                    out.writeU64("frames", job.result.frames);
                }
                return {};
            });
        static constexpr std::string_view ENVIRONMENT_FORMATS[] = {"rgba16f", "rgb9e5", "r11g11b10"};
        control.registerCommand(
            "importEnvironment", OWNER,
            "bakes an HDR environment into an environment asset: an equirect image (2:1; a Radiance .hdr keeps its "
            "range), a cube cross image (4:3 or 3:4) or a folder holding the six faces px, nx, py, ny, pz, nz; "
            "every "
            "layout bakes to the same equirect asset with a mip chain. Bakes before replying, replacing an "
            "environment "
            "of the same name (a loaded copy stays until released); assets lists it with source, format, size and "
            "mips",
            {requiredPath("path", "import", "an image or a face folder under the import root"),
             optionalArg("name", ArgKind::String, "the environment's name", "the file's stem"),
             optionalArg("format", ArgKind::String,
                         "the texel format: rgba16f (half floats), rgb9e5 (shared exponent) or r11g11b10", "rgba16f",
                         ENVIRONMENT_FORMATS)},
            [&control, &assets](const ControlArgs& args, IFormatWriter& out)
            { return importEnvironment(control, assets, args, out); });
        control.registerUndoableViewCommand(
            "reimportModel", OWNER, "bakes an imported model again from its source and replaces it",
            {requiredArg("name", ArgKind::String, "the model"), IMPORT_SETTINGS_ARG,
             optionalArg("wait", ArgKind::Boolean, "bake before replying instead of in the background", "false"),
             optionalArg("previews", ArgKind::Boolean, "generate optional previews", "true")},
            [&control, &assets, importRoot](const ControlArgs& args, IFormatWriter& out)
            { return reimportModel(control, assets, importRoot, args, out); });
        control.registerUndoableViewCommand(
            "setImportSettings", OWNER,
            "writes importer options to an imported model's sidecar without baking: the model then needs a "
            "reimport",
            {requiredArg("name", ArgKind::String, "the model"),
             requiredArg("settings", ArgKind::Json, "importer options (importSettings lists them) over the sidecar's")},
            [&control, &assets, importRoot](const ControlArgs& args, IFormatWriter& out)
            { return setImportSettings(control, assets, importRoot, args, out); });
        control.registerQuery(
            "importSettings", OWNER,
            "the importer options with defaults, ranges and docs; with a source or model, its "
            "sidecar's settings (a model also bakedWith, the settings of its bake, and needsReimport)",
            {optionalPath("path", "import", "a model file under the import root", "none"),
             optionalArg("model", ArgKind::String, "an imported model: its source's settings", "none")},
            [&control, &assets, importRoot](const ControlArgs& args, IFormatWriter& out)
            { return importSettings(control, assets, importRoot, args, out); });
        control.registerUndoableViewCommand(
            "removeModel", OWNER,
            "Removes a model and saves its baked files, or cancels its import. recoveryId identifies the saved "
            "snapshot for restoreDeletedModel (empty for cancellation); materials and textures are retained.",
            {requiredArg("name", ArgKind::String, "the model")},
            [&control, &assets, importRoot](const ControlArgs& args, IFormatWriter& out)
            { return removeModel(control, assets, importRoot, args, out); });
        control.registerUndoableViewCommand(
            "restoreDeletedModel", OWNER,
            "Restores saved baked files without source content: id is the recovery record, name is the restored "
            "model, "
            "source is its original source path, and restored confirms successful restoration.",
            {requiredArg("id", ArgKind::String, "the recovery id")},
            [&control, &assets](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                std::string id;
                if (std::string error = nameArg(args, "id", id); !error.empty())
                    return error;
                auto restored = assets.restoreRemovedModel(id);
                if (!restored)
                    return "cannot restore model: " + restored.error().message;
                control.recordInverse("removeModel", nlohmann::json{{"name", restored->name}}.dump());
                out.writeString("id", restored->id);
                out.writeString("name", restored->name);
                out.writeString("source", restored->source);
                out.writeBool("restored", restored->restored);
                return {};
            });
        control.registerQuery(
            "deletedModels", OWNER,
            "Saved recovery records for the configured bake root in deletedModels: each has id (for "
            "restoreDeletedModel), name "
            "(model), source (original source path) and restored (whether that snapshot was restored). "
            "Snapshots are retained; unrestored records whose model name is present are omitted.",
            {},
            [&assets](const ControlArgs&, IFormatWriter& out) -> std::string
            {
                auto records = assets.listDeletedModels();
                if (!records)
                    return "cannot list deleted models: " + records.error().message;
                out.beginArray("deletedModels", 0);
                for (const auto& record : *records)
                {
                    out.beginObject({});
                    out.writeString("id", record.id);
                    out.writeString("name", record.name);
                    out.writeString("source", record.source);
                    out.writeBool("restored", record.restored);
                    out.endObject();
                }
                out.endArray();
                return {};
            });
        control.registerQuery(
            "assets", OWNER,
            "the manifest's assets with load status and references (a texture over its colour "
            "spaces: the furthest status, all references, and the spaces holding it); a model's "
            "authoringSource identifies its original document when available. bounds=true reports min and max, "
            "groundOffset, or boundsError; "
            "placement=true reads <source>.placement.json with forward and connectionPoints "
            "(each point has name, position and direction in model space); connectionPointsSupported reports "
            "availability; ocean profiles report width and height samples, frameCount, tileMetres, "
            "framesPerSecond, maxDisplacementMetres and loop. text, offset, limit or fields also report total and "
            "nextOffset when a page has more matches",
            {optionalArg("type", ArgKind::String, LIST_TYPE_DOC, "every type", LIST_TYPE_CHOICES),
             optionalArg("bounds", ArgKind::Boolean, "read model-space baked bounds and ground offset", "false"),
             optionalArg("placement", ArgKind::Boolean, "read optional placement metadata sidecars", "false"),
             optionalArg("text", ArgKind::String, "substring in asset names, ASCII case-insensitive"),
             optionalArg("offset", ArgKind::Integer, "skip this many matching rows", "0"),
             optionalArg("limit", ArgKind::Integer, "rows to return, 0..1000; 0 only counts", "all"),
             optionalArg("fields", ArgKind::Json, "nonempty array of existing top-level fields to return")},
            [&assets](const ControlArgs& args, IFormatWriter& out) { return listAssets(assets, args, out); });
        control.registerQuery(
            "bindingReadiness", OWNER,
            "Observed mesh and material binding states in the selected world. Counts cover all bindings; rows are "
            "limited and truncated reports omitted rows. Derived component presence is not frame completion; "
            "graph execution and readback are not checked.",
            {WORLD_ARG, optionalArg("limit", ArgKind::Integer, "maximum binding rows, 0..1000", "100")},
            [&control, &assets](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                const World* world = control.world(args);
                if (!world)
                    return control.unknownWorld(args);
                const u64 limit = args.integer("limit").value_or(100);
                if (limit > 1000)
                    return "binding limit must be between 0 and 1000";
                out.writeString("frameCompletion", "notChecked");
                if (!world->services().contains<GpuAssetFactory>())
                {
                    out.writeString("availability", "unavailable");
                    return {};
                }
                struct Row
                {
                    Entity entity;
                    AssetHandle handle;
                    const char* component;
                    AssetRuntime::AssetStatus status;
                };
                std::vector<Row> rows;
                u64 total = 0, ready = 0, loading = 0, failed = 0, unknown = 0;
                const auto collect = [&](const ComponentID componentId, const bool mesh)
                {
                    for (const Chunk* chunk : world->getChunksContaining(Aspect(componentId)))
                        for (const Entity entity : chunk->entities())
                        {
                            if (world->hasComponent<InactiveComponent>(entity))
                                continue;
                            const AssetHandle handle = mesh
                                ? world->getConstComponent<MeshComponent>(entity).meshHandle
                                : world->getConstComponent<MaterialComponent>(entity).materialHandle;
                            const auto status = assets.status(handle);
                            ++total;
                            switch (status)
                            {
                            case AssetRuntime::AssetStatus::Ready:
                                ++ready;
                                break;
                            case AssetRuntime::AssetStatus::Loading:
                                ++loading;
                                break;
                            case AssetRuntime::AssetStatus::Failed:
                                ++failed;
                                break;
                            default:
                                ++unknown;
                                break;
                            }
                            if (rows.size() < limit)
                                rows.push_back({entity, handle, mesh ? "MeshComponent" : "MaterialComponent", status});
                        }
                };
                collect(getComponentId<MeshComponent>(), true);
                collect(getComponentId<MaterialComponent>(), false);
                out.writeString("availability", "available");
                out.writeU64("total", total);
                out.writeU64("ready", ready);
                out.writeU64("loading", loading);
                out.writeU64("failed", failed);
                out.writeU64("unknown", unknown);
                out.writeBool("truncated", total > rows.size());
                out.beginArray("bindings", rows.size());
                for (const Row& row : rows)
                {
                    out.beginObject({});
                    out.writeU64("entity", row.entity.id());
                    out.writeString("component", row.component);
                    out.beginObject("handle");
                    out.writeString("name", row.handle.name.toView());
                    out.writeU32("assetType", static_cast<u32>(row.handle.assetType));
                    out.writeU32("subAssetIndex", row.handle.subAssetIndex);
                    out.endObject();
                    out.writeString("status",
                                    row.status == AssetRuntime::AssetStatus::Unloaded ? "unknown"
                                                                                      : statusName(row.status));
                    if (row.status == AssetRuntime::AssetStatus::Failed)
                        out.writeString("reason", "asset load/upload failed; allocation cause unavailable");
                    out.writeBool("meshletComponent", world->hasComponent<MeshletComponent>(row.entity));
                    out.writeBool("objectStaticInfoComponent",
                                  world->hasComponent<ObjectStaticInfoComponent>(row.entity));
                    out.endObject();
                }
                out.endArray();
                return {};
            });
        control.registerQuery("material", OWNER, "a material's fields", {MATERIAL_ARG},
                              [&assets](const ControlArgs& args, IFormatWriter& out)
                              { return material(assets, args, out); });
        control.registerQuery(
            "flipbook", OWNER,
            "validates a card's atlas, playback and mesh; returns frame, width, height, fractionalCells, "
            "samplingNote, pivot, worldSize, localMin and localMax; a camera-facing card must use a blended "
            "non-shadowing material and a full-atlas XY mesh with an identity material UV transform",
            {requiredArg("id", ArgKind::Entity, "a renderable with FlipbookComponent")},
            [&control, &assets](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                const World* world = control.world(args);
                if (!world)
                {
                    return control.unknownWorld(args);
                }
                const auto id = args.integer("id");
                if (!id || !world->isEntityAlive(Entity(*id)))
                {
                    return "id must name a live scene entity";
                }
                const Entity entity(*id);
                if (!world->hasComponent<FlipbookComponent>(entity))
                {
                    return "add FlipbookComponent to a textured card first";
                }
                const auto& effect = world->getConstComponent<FlipbookComponent>(entity);
                FlipbookInspection inspection;
                if (std::string failure = validateFlipbookPlacement(*world, assets, entity, effect, &inspection);
                    !failure.empty())
                {
                    return failure;
                }
                const auto& header = inspection.header;
                const auto& bounds = inspection.bounds;
                const float width = bounds.max.x - bounds.min.x;
                const float height = bounds.max.y - bounds.min.y;
                const float worldWidth = inspection.worldWidth;
                const float worldHeight = inspection.worldHeight;
                out.writeU32("frame", flipbookFrame(effect));
                out.writeU32("width", header.width);
                out.writeU32("height", header.height);
                out.writeBool("fractionalCells",
                              header.width % effect.columns != 0 || header.height % effect.rows != 0);
                out.writeString(
                    "samplingNote",
                    "Mipmaps and filtering can sample neighbouring frames; pad atlas cells to avoid bleed.");
                out.beginArray("pivot", 2);
                out.writeF32("", -bounds.min.x / width);
                out.writeF32("", -bounds.min.y / height);
                out.endArray();
                out.beginArray("worldSize", 2);
                out.writeF32("", worldWidth);
                out.writeF32("", worldHeight);
                out.endArray();
                out.beginArray("localMin", 3);
                out.writeF32("", bounds.min.x);
                out.writeF32("", bounds.min.y);
                out.writeF32("", bounds.min.z);
                out.endArray();
                out.beginArray("localMax", 3);
                out.writeF32("", bounds.max.x);
                out.writeF32("", bounds.max.y);
                out.writeF32("", bounds.max.z);
                out.endArray();
                return {};
            });
        const auto applyMaterialBinding = [&control, &assets](const ControlArgs& args, IFormatWriter& out,
                                                              const std::optional<AssetHandle>& next) -> std::string
        {
            const World* world = control.world(args);
            if (!world)
                return control.unknownWorld(args);
            const auto id = args.integer("id");
            if (!id || !world->isEntityAlive(Entity(*id)))
                return "id must name a live entity";
            const Entity entity(*id);
            const auto* existing = world->tryGetConstComponent<MaterialComponent>(entity);
            const std::optional<AssetHandle> previous =
                existing ? std::optional(existing->materialHandle) : std::nullopt;
            if (previous && previous->assetType != AssetType::UNKNOWN && previous->assetType != AssetType::MATERIAL)
                return "existing material binding has an invalid asset type";
            if (previous == next)
            {
                out.writeBool("changed", false);
                return {};
            }
            const bool applied =
                next ? assets.bindMaterial(*world, entity, *next) : assets.clearMaterial(*world, entity);
            if (!applied)
                return "world is not attached to the asset runtime or the material handle is invalid";
            nlohmann::json saved = nullptr;
            if (previous)
                saved = {{"name", previous->name.toView()},
                         {"assetType", static_cast<u32>(previous->assetType)},
                         {"subAssetIndex", previous->subAssetIndex}};
            control.recordInverse("restoreMaterialBinding",
                                  nlohmann::json{{"id", entity.id()}, {"handle", saved}}.dump());
            out.writeU64("id", entity.id());
            out.writeBool("changed", true);
            return {};
        };
        control.registerUndoableCommand(
            "assignMaterial", OWNER,
            "assigns an existing material to an entity in the selected world; undo restores its binding",
            {requiredArg("id", ArgKind::Entity, "entity"), requiredArg("material", ArgKind::String, "material name")},
            [&control, &assets, applyMaterialBinding](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                const auto name = args.string("material");
                if (!name || name->empty() ||
                    (!control.replayingHistory() && !assets.manifest().getMaterialEntry(InternedString(*name))))
                    return "material is not in the manifest";
                return applyMaterialBinding(args, out, AssetHandle{InternedString(*name), AssetType::MATERIAL, 0});
            });
        control.registerUndoableCommand(
            "restoreMaterialBinding", OWNER, "internal material binding undo",
            {requiredArg("id", ArgKind::Entity, "entity"),
             requiredArg("handle", ArgKind::Json, "previous handle or null")},
            [&control, applyMaterialBinding](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                if (!control.replayingHistory())
                    return "restoreMaterialBinding is reserved for undo";
                const auto value = args.json("handle");
                if (!value)
                    return "missing handle";
                const auto parsed = nlohmann::json::parse(*value, nullptr, false);
                if (parsed.is_null())
                    return applyMaterialBinding(args, out, std::nullopt);
                if (!parsed.is_object())
                    return "invalid handle";
                AssetHandle handle;
                std::string error;
                JsonFragmentReader reader(*value);
                if (!readValue(AssetHandle::spiteSchema(), &handle, reader, error))
                    return "invalid handle: " + error;
                return applyMaterialBinding(args, out, handle);
            });
        control.registerUndoableCommand(
            "placeFlipbook", OWNER,
            "validates a ready textured card and adds playback as one undoable action; returns id and frame",
            {requiredArg("id", ArgKind::Entity, "card entity"),
             requiredArg("metadata", ArgKind::Json, "FlipbookComponent fields including grid and frame rate")},
            [&control, &assets](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                const World* world = control.world(args);
                if (!world)
                    return control.unknownWorld(args);
                const auto id = args.integer("id");
                if (!id || !world->isEntityAlive(Entity(*id)))
                    return "id must name a live scene entity";
                const Entity entity(*id);
                if (world->hasComponent<FlipbookComponent>(entity))
                    return "entity already has FlipbookComponent; edit it with set";
                const auto metadata = args.json("metadata");
                if (!metadata || !nlohmann::json::parse(*metadata, nullptr, false).is_object())
                    return "metadata must be an object of FlipbookComponent fields";
                FlipbookComponent effect;
                std::string error;
                JsonFragmentReader reader(*metadata);
                if (!readValue(FlipbookComponent::spiteSchema(), &effect, reader, error))
                    return "invalid metadata: " + error;
                if (std::string failure = validateFlipbookPlacement(*world, assets, entity, effect); !failure.empty())
                    return failure;
                world->addComponent<FlipbookComponent>(entity, effect);
                control.recordInverse("removeComponent",
                                      nlohmann::json{{"id", entity.id()}, {"component", "FlipbookComponent"}}.dump());
                out.writeU64("id", entity.id());
                out.writeU32("frame", flipbookFrame(effect));
                return {};
            });
        control.registerCommand(
            "loadAsset", OWNER,
            "starts loading an asset, every submesh of a model; a texture loads in the sRGB space unless it is "
            "already "
            "loading or loaded in any space (assets lists its spaces)",
            {requiredArg("type", ArgKind::String, TYPE_DOC, TYPE_CHOICES),
             requiredArg("name", ArgKind::String, "the asset"),
             optionalArg("priority", ArgKind::Integer,
                         "queues the load as streaming demand, admitted by priority while the GPU budget allows",
                         "load at once")},
            [&assets](const ControlArgs& args, IFormatWriter& out) { return loadAsset(assets, args, out); });
        control.registerQuery(
            "oceanProfileBinding", OWNER,
            "an entity's ocean profile status and name; nativeFallback indicates procedural playback, "
            "cpuRefs and gpuRefs report CPU and GPU reference counts",
            {WORLD_ARG, requiredArg("id", ArgKind::Entity, "water entity")},
            [&control, &assets](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                const World* world = control.world(args);
                if (!world)
                    return control.unknownWorld(args);
                const auto id = args.integer("id");
                if (!id || !world->isEntityAlive(Entity(*id)))
                    return "id must name a live scene entity";
                const auto* binding = world->tryGetConstComponent<OceanProfileAssetBinding>(Entity(*id));
                const auto* rendered = world->tryGetConstComponent<OceanProfileRenderState>(Entity(*id));
                std::string_view status = "native";
                AssetHandle cpu, gpu;
                if (binding)
                {
                    cpu = binding->profileHandle;
                    gpu = {cpu.name, AssetType::OCEAN_PROFILE, 1};
                    const bool valid =
                        cpu.isValid() && cpu.assetType == AssetType::OCEAN_PROFILE && cpu.subAssetIndex == 0;
                    const bool failed = !valid || assets.status(cpu) == AssetRuntime::AssetStatus::Failed ||
                        (world->services().contains<GpuAssetFactory>() &&
                         world->services().read<GpuAssetFactory>()->uploadFailed(gpu));
                    status = failed ? "failed" : (rendered && rendered->address ? "ready" : "loading");
                }
                out.writeString("status", status);
                out.writeBool("nativeFallback", status != "ready");
                out.writeString("name", cpu.name.toView());
                out.writeU32("cpuRefs", cpu.isValid() ? assets.registry().getRefCount(cpu) : 0);
                out.writeU32("gpuRefs", gpu.isValid() ? assets.registry().getRefCount(gpu) : 0);
                return {};
            });
        control.registerCommand("unloadOceanProfile", OWNER, "releases an ocean profile loaded by loadAsset",
                                {requiredArg("name", ArgKind::String, "the ocean profile")},
                                [&assets](const ControlArgs& args, IFormatWriter& out) -> std::string
                                {
                                    std::string name;
                                    if (std::string error = nameArg(args, "name", name); !error.empty())
                                        return error;
                                    const AssetHandle handle{InternedString(name.c_str()), AssetType::OCEAN_PROFILE, 0};
                                    if (assets.status(handle) == AssetRuntime::AssetStatus::Loading)
                                        return "ocean profile is still loading";
                                    assets.release(handle);
                                    out.writeString("name", name);
                                    return {};
                                });
        struct OceanRecoveryState
        {
            u64 next = 1;
            std::unordered_map<u64, std::pair<InternedString, AssetManifest::OceanProfileEntry>> rows;
        };
        const auto oceanRecovery = std::make_shared<OceanRecoveryState>();
        control.registerUndoableCommand(
            "importOceanProfile", OWNER,
            "imports a packed ocean profile under the import root; returns name and baked path",
            {requiredPath("path", "import", "a packed .spoc file"), requiredArg("name", ArgKind::String, "asset name"),
             optionalArg("recoveryId", ArgKind::Integer, "internal history id")},
            [&control, &assets, oceanRecovery](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                std::string name;
                if (std::string error = nameArg(args, "name", name); !error.empty())
                    return error;
                if (std::string error = bakeFileNameError("ocean profile", name); !error.empty())
                    return error;
                const auto recoveryId = args.integer("recoveryId");
                if (recoveryId && !control.replayingHistory())
                    return "recoveryId is internal";
                if (recoveryId)
                {
                    const auto saved = oceanRecovery->rows.find(*recoveryId);
                    if (saved == oceanRecovery->rows.end() || saved->second.first != InternedString(name.c_str()))
                        return "no matching ocean profile recovery";
                    const auto restored = assets.restoreOceanProfile(saved->second.first, saved->second.second);
                    if (!restored)
                        return restored.error().message;
                    control.recordInverse("removeOceanProfile",
                                          nlohmann::json{{"name", name}, {"recoveryId", *recoveryId}}.dump());
                    out.writeString("name", name);
                    out.writeString("baked", saved->second.second.baked.toView());
                    return {};
                }
                const auto path = args.string("path");
                if (!path)
                    return "missing arg 'path'";
                std::filesystem::path source;
                if (std::string error = control.resolveFile("import", *path, source); !error.empty())
                    return error;
                const auto imported = assets.importOceanProfile(utf8Path(source), InternedString(name.c_str()));
                if (!imported)
                    return imported.error().message;
                const u64 id = oceanRecovery->next++;
                oceanRecovery->rows.emplace(id, std::make_pair(InternedString(name.c_str()), *imported));
                control.recordForward(nlohmann::json{{"path", *path}, {"name", name}, {"recoveryId", id}}.dump());
                control.recordInverse("removeOceanProfile", nlohmann::json{{"name", name}, {"recoveryId", id}}.dump());
                out.writeString("name", name);
                out.writeString("baked", imported->baked.toView());
                return {};
            });
        control.registerUndoableCommand(
            "removeOceanProfile", OWNER, "removes an ocean profile while retaining its recovery bytes",
            {requiredArg("name", ArgKind::String, "asset name"),
             optionalArg("recoveryId", ArgKind::Integer, "internal history id")},
            [&control, &assets, oceanRecovery](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                std::string name;
                if (std::string error = nameArg(args, "name", name); !error.empty())
                    return error;
                const InternedString key(name.c_str());
                const auto recoveryId = args.integer("recoveryId");
                if (recoveryId && !control.replayingHistory())
                    return "recoveryId is internal";
                if (recoveryId)
                {
                    const auto saved = oceanRecovery->rows.find(*recoveryId);
                    const auto current = assets.manifest().getOceanProfileEntry(key);
                    if (saved == oceanRecovery->rows.end() || saved->second.first != key || !current ||
                        current->contentHash != saved->second.second.contentHash ||
                        current->source != saved->second.second.source || current->baked != saved->second.second.baked)
                        return "ocean profile changed since history was recorded";
                }
                const auto removed = assets.removeOceanProfile(key);
                if (!removed)
                    return removed.error().message;
                const u64 id = recoveryId ? *recoveryId : oceanRecovery->next++;
                if (!recoveryId)
                    oceanRecovery->rows.emplace(id, std::make_pair(key, *removed));
                control.recordInverse("restoreOceanProfile", nlohmann::json{{"recoveryId", id}}.dump());
                out.writeString("name", name);
                return {};
            });
        control.registerUndoableCommand(
            "restoreOceanProfile", OWNER, "restores a removed ocean profile from retained recovery bytes",
            {requiredArg("recoveryId", ArgKind::Integer, "internal history id")},
            [&control, &assets, oceanRecovery](const ControlArgs& args, IFormatWriter& out) -> std::string
            {
                if (!control.replayingHistory())
                    return "restoreOceanProfile is only available to undo history";
                const auto id = args.integer("recoveryId");
                const auto it = id ? oceanRecovery->rows.find(*id) : oceanRecovery->rows.end();
                if (it == oceanRecovery->rows.end())
                    return "no retained ocean profile recovery";
                const auto restored = assets.restoreOceanProfile(it->second.first, it->second.second);
                if (!restored)
                    return restored.error().message;
                control.recordInverse("removeOceanProfile",
                                      nlohmann::json{{"name", it->second.first.toView()}, {"recoveryId", *id}}.dump());
                out.writeString("name", it->second.first.toView());
                return {};
            });
        const auto bind =
            [&control, &assets](std::string (*fn)(ControlServer&, AssetRuntime&, const ControlArgs&, IFormatWriter&))
        {
            return [&control, &assets, fn](const ControlArgs& args, IFormatWriter& out)
            { return fn(control, assets, args, out); };
        };
        control.registerUndoableCommand(
            "instantiateModel", OWNER, "instantiates a baked model as an entity hierarchy",
            {requiredArg("model", ArgKind::String, "the model"),
             optionalArg("name", ArgKind::String, "the root entity's name", "the model's name"),
             optionalArg("parent", ArgKind::Entity, "a scene entity with a TransformComponent", "none: a root"),
             optionalArg("layer", ArgKind::Integer, "the scene to place a root in (sceneLayers)", "the active scene"),
             optionalArg("transform", ArgKind::Json, "an object of position, rotation, scale")},
            [&control, &assets](const ControlArgs& args, IFormatWriter& out)
            { return instantiateModel(control, assets, args, out); });
        const auto transformPreflight = [](const ControlArgs& args) -> std::string
        {
            if (const auto transform = args.json("transform"))
            {
                TransformComponent scratch;
                std::string error;
                JsonFragmentReader reader(*transform);
                if (!nlohmann::json::parse(*transform, nullptr, false).is_object() ||
                    !readValue(TransformComponent::spiteSchema(), &scratch, reader, error))
                    return "bad transform (an object of position, rotation, scale): " + error;
            }
            return {};
        };
        control.setCommandPreflight("instantiateModel", OWNER,
                                    [&assets, transformPreflight](const ControlArgs& args) -> std::string
                                    {
                                        const auto model = args.string("model");
                                        if (model && !assets.manifest().hasModel(InternedString(model->c_str())))
                                            return "model '" + *model + "' is not in the asset manifest";
                                        return transformPreflight(args);
                                    });
        static const std::string KIND_DOC = []
        {
            std::string doc = "the shape, each fitting a unit box (the capsule is two tall):";
            for (const std::string_view kind : primitiveKinds())
                doc += " " + std::string(kind);
            return doc;
        }();
        control.registerUndoableCommand(
            "createPrimitive", OWNER,
            "creates a mesh primitive as an entity hierarchy at the origin (or at transform); its model "
            "primitive-<kind> is generated and baked on first use and stays in the manifest after an undo",
            {requiredArg("kind", ArgKind::String, KIND_DOC, primitiveKinds()),
             optionalArg("name", ArgKind::String, "the root entity's name", "the kind, capitalised"),
             optionalArg("parent", ArgKind::Entity, "a scene entity with a TransformComponent", "none: a root"),
             optionalArg("layer", ArgKind::Integer, "the scene to place a root in (sceneLayers)", "the active scene"),
             optionalArg("transform", ArgKind::Json, "an object of position, rotation, scale")},
            [&control, &assets, importRoot](const ControlArgs& args, IFormatWriter& out)
            { return instantiateModel(control, assets, args, out, importRoot, *args.string("kind")); });
        control.setCommandPreflight("createPrimitive", OWNER, transformPreflight);
        control.registerUndoableViewCommand(
            "createMaterial", OWNER, "creates a material",
            {requiredArg("name", ArgKind::String, "the new material's name"), FIELDS_ARG}, bind(&createMaterial));
        control.registerUndoableViewCommand(
            "updateMaterial", OWNER, "changes a material's fields; the result carries the previous ones",
            {MATERIAL_ARG, requiredArg(FIELDS_ARG.name, FIELDS_ARG.kind, FIELDS_ARG.doc),
             optionalArg("merge", ArgKind::String,
                         "a key: an update with the newest undo step's key, naming the same material and members, "
                         "folds into it (one undo step for a drag)")},
            bind(&updateMaterial));
        control.registerUndoableViewCommand("deleteMaterial", OWNER, "deletes a material no model or entity uses",
                                            {MATERIAL_ARG}, bind(&deleteMaterial));
    }
} // namespace spite
