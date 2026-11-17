#pragma once

namespace spite
{
    struct ModelImportSettings;

    struct AssetBakerConfig
    {
        const char* outputDirectory = "assets_baked";
        const char* textureSubdir = "textures";
        const char* materialSubdir = "materials";
        const char* atlasSubdir = "atlases";
        const char* environmentSubdir = "environments";
        bool forceRebake = false;
        // when set, a model's external files (buffers, images) must resolve
        // inside this directory; a model naming one outside fails to bake
        const char* sourceRoot = nullptr;
        // when set, a model bakes with these importer options instead of reading its source's sidecar (the
        // runtime's imports fix them when queued); must outlive the baker
        const ModelImportSettings* importSettings = nullptr;
        // worker threads a texture compression uses (0: one per hardware thread); the baked bytes never depend on it
        unsigned int encoderThreads = 0;
        // Private preparations fail instead of silently dropping an unreadable texture or material.
        bool strictModelDependencies = false;
    };
} // namespace spite
