#pragma once

#include <span>
#include <string>
#include <string_view>

#include "asset/AssetExport.hpp"
#include "asset/TextureFormat.hpp"
#include "base/Expected.hpp"
#include "base/Platform.hpp"

namespace spite
{
    enum class ImportUpAxis : u8
    {
        Y, // the engine's: nothing changes
        Z // the model is turned -90 degrees about X
    };

    enum class ImportNormals : u8
    {
        Flat, // meshes without normals get one per face
        Smooth, // meshes without normals get angle-weighted vertex normals
        None // meshes without normals keep none
    };

    // The importer options a model bake reads from its sidecar (<source file>.import.json, next to the source);
    // each default is what the importer did before options existed, so a model without a sidecar bakes the same
    // geometry and materials; its textures are compressed (`textures`). Meshes that carry normals keep them whatever
    // `normals` says.
    struct ModelImportSettings
    {
        double scale = 1.0; // uniform; vertex positions and node translations
        ImportUpAxis upAxis = ImportUpAxis::Y; // applied to the root node
        ImportNormals normals = ImportNormals::Flat;
        // how the model's textures are stored: BC7 and BC5 blocks (compressed), opaque colour as BC1 (fast) or RGBA8;
        // the one option whose default changes what a model without a sidecar bakes
        TextureCompression textures = TextureCompression::Auto;
        // A rigged model's clips: the rate a CUBICSPLINE curve is resampled at, ozz has no cubic
        // interpolation, and whether scale tracks are dropped (every joint keeps its rest scale: a smaller clip for a
        // rig that never scales).
        double clipSampleRate = 60.0;
        bool stripScaleTracks = false;
        // What the runtime may do with a rigged model's pose. Culling is static, so the bake widens a
        // skinned model's bounds only for what is declared here: a model without either flag keeps its tight bounds,
        // and the runtime refuses IK nodes (usesIk) on a model that does not declare them.
        bool usesLayers = false; // animation layers mix per-joint poses of different clips
        bool usesIk = false; // AnimationIkComponent nodes bend the pose
        bool preserveAlphaGeometry = false; // keep level-0 geometry for masked/blended submeshes
        double alphaLodErrorScale = 1.0; // scale masked/blended LOD errors without changing other submeshes

        bool operator==(const ModelImportSettings&) const = default;
    };

    // One option as the importSettings query describes it.
    struct ImportOptionDesc
    {
        std::string_view name;
        std::string_view type; // number | enum | bool
        std::string_view doc;
        std::string_view defaultValue; // the text a sidecar or importModel arg writes
        double min = 0.0; // number
        double max = 0.0;
        std::span<const std::string_view> choices; // enum
    };

    SPITE_ASSET_API std::span<const ImportOptionDesc> modelImportOptions();

    // An option's value in settings, in the member its type reads: number, text (an enum's choice name) or flag.
    struct ImportOptionValue
    {
        double number = 0.0;
        std::string_view text;
        bool flag = false;
    };
    SPITE_ASSET_API ImportOptionValue importOptionValue(const ModelImportSettings& settings,
                                                        const ImportOptionDesc& option);

    // The text a sidecar and the importSettings query name an enum option's value by.
    SPITE_ASSET_API std::string_view importUpAxisName(ImportUpAxis axis);
    SPITE_ASSET_API std::string_view importNormalsName(ImportNormals normals);
    SPITE_ASSET_API std::string_view importTexturesName(TextureCompression textures);

    // The sidecar of a source file, both UTF-8.
    SPITE_ASSET_API std::string modelImportSidecarPath(std::string_view source);

    // Applies the members of a JSON object over settings; an error text (empty on success, settings unchanged) names
    // the offending member and the valid ones.
    SPITE_ASSET_API std::string applyModelImportSettings(ModelImportSettings& settings, std::string_view json);

    SPITE_ASSET_API std::string modelImportSettingsJson(const ModelImportSettings& settings);

    // The settings of source's sidecar over the defaults; the defaults when there is no sidecar, an error when the
    // sidecar cannot be read or holds an unknown or out-of-range option. Safe from any thread.
    SPITE_ASSET_API Expected<ModelImportSettings> loadModelImportSettings(std::string_view source);

    // Writes source's sidecar (atomically); every option, so the file documents what the bake used. Settings equal
    // to the defaults delete the sidecar instead: no sidecar means the defaults.
    SPITE_ASSET_API Expected<void> saveModelImportSettings(std::string_view source,
                                                           const ModelImportSettings& settings);
} // namespace spite
