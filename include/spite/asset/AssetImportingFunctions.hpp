#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "asset/AssetData.hpp"
#include "asset/AssetExport.hpp"
#include "asset/ModelImportSettings.hpp"
#include "base/Expected.hpp"
#include "base/InternedString.hpp"

namespace spite
{

    /**
     * @brief Decodes an image file to RGBA8 with stb_image, opened through the UTF-8 to path conversion. Rows come
     * bottom-first (the importers' convention); the flip is local to the calling thread's decode.
     * @param path The file path, UTF-8.
     * @return The pixels (release with freeTexture), or nullptr when the file cannot be read or decoded.
     */
    SPITE_ASSET_API u8* decodeImageFile(InternedString path, int& width, int& height, int& channels);

    // Rejects an oversized encoded file or decoded image before allocating pixel storage.
    SPITE_ASSET_API u8* decodeImageFileBounded(InternedString path, int& width, int& height, int& channels, u32 maxSide,
                                               u64 maxPixels, u64 maxEncodedBytes);

    // Probe image dimensions without allocating decoded pixels. Errors own their diagnostic text.
    SPITE_ASSET_API Expected<void> probeImageFile(const std::filesystem::path& path, int& width, int& height);

    // Decode captured image bytes as top-first RGBA8, requiring the probed dimensions; release with freeTexture.
    SPITE_ASSET_API Expected<u8*> decodeImageTopFirst(std::span<const std::byte> bytes, int expectedWidth,
                                                      int expectedHeight, std::string_view source);

    /// Releases pixels returned by image decoding, on the allocator that made them.
    SPITE_ASSET_API void freeTexture(u8* pixels);

    /**
     * @brief Loads a texture from a file path using stb_image.
     * @param path The file path of the texture, UTF-8.
     * @return A shared pointer to the loaded texture data, or nullptr on failure.
     */
    SPITE_ASSET_API std::shared_ptr<TextureAsset> loadTexture(InternedString path);

    /**
     * @brief Imports a 3D model from a file using Assimp.
     *
     * This function reads a model file, processes its geometry with meshoptimizer,
     * extracts materials, and builds a node hierarchy.
     *
     * @param filename The file path of the model, UTF-8.
     * @param sourceRoot UTF-8. When not empty, every file the model reads (buffers) or
     *        names (images) must resolve inside this directory; one outside fails
     *        the import.
     * @param settings The importer options (scale, up axis, normals); the defaults reproduce the plain import.
     * @return An Expected containing either a shared pointer to the processed model data
     *         or an Error on failure.
     */
    SPITE_ASSET_API Expected<std::shared_ptr<ParsedModelData>> importModelAssimp(
        InternedString filename, std::string_view sourceRoot = {}, const ModelImportSettings& settings = {},
        std::vector<GltfExtensionDiagnostic>* diagnostics = nullptr);

    // Whether the path ends in .gltf or .glb, ignoring ASCII case.
    SPITE_ASSET_API bool isGltfFile(const std::filesystem::path& path);

    struct GltfExternalReference
    {
        std::string uri;
        std::string decoded;
    };
    // External buffer and image URIs declared by a glTF/GLB, before and after URI decoding.
    SPITE_ASSET_API Expected<std::vector<GltfExternalReference>> gltfExternalReferences(
        std::span<const std::byte> file);

    /**
     * @brief Builds a submesh's meshlets and their cluster LOD hierarchy from its vertices and triangle indices
     * with 64-vertex, 124-triangle clusters merged into groups with locked borders and simplified level by
     * level. Vertices at one position whose uv, handedness or normal differ (a seam) stay locked at every level
     * where the surface is flat there. Fills meshlets (level 0 first), the meshlet vertex and triangle
     * data; the result depends on the inputs alone (same bytes in every configuration and on every thread).
     * @param submesh Its vertices are read; the meshlet members are replaced.
     * @param indices Triangle list into submesh.vertices.
     * @param vertexLocks Empty, or one byte per vertex: a nonzero vertex keeps its place at every level (a border
     * another submesh shares, which would crack if the two simplified it differently).
     * @param skeleton The skeleton the submesh's skin stream binds to, or null: with a skin the simplifier weighs how
     * far each vertex moves when the skeleton's joints bend, so a bend its bind-pose geometry hides (a straight
     * tube) keeps its detail and its error.
     */
    SPITE_ASSET_API void buildMeshletHierarchy(ProcessedSubmesh& submesh, std::span<const u32> indices,
                                               std::span<const u8> vertexLocks = {},
                                               const RawSkeleton* skeleton = nullptr);

    /**
     * @brief The vertices of each submesh whose position another submesh has too (a material seam): the locks
     * buildMeshletHierarchy takes. A vertex is matched by its position alone, whatever its normal or uv.
     * @return Per submesh, one byte per vertex: 1 for a vertex on a seam.
     */
    SPITE_ASSET_API std::vector<std::vector<u8>> sharedVertexLocks(std::span<const ProcessedSubmesh> submeshes);
    // Compare every node instance in model space, within float roundoff; the result unions locks for shared mesh data.
    // comparedVertices, when given, receives how many placed vertices the pair search held (only those near another
    // instance are).
    SPITE_ASSET_API std::vector<std::vector<u8>> sharedVertexLocks(const ParsedMeshData& mesh,
                                                                   sizet* comparedVertices = nullptr);

} // namespace spite
