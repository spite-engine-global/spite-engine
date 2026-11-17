#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "rendering/GraphicsDescs.hpp"
#include "rendering/RenderResourceHandles.hpp"
#include "rendering/RenderingExport.hpp"
#include "base/InternedString.hpp"
#include "base/Schema.hpp"
#include "base/StringInterner.hpp"

namespace spite
{
    class IBindlessResourceManager;
    class IRenderDevice;
    struct ShaderDescription;

    // Pipeline assembly vocabulary. A spec declares shaders and fixed-function
    // state only: every pipeline shares the bindless layout.
    struct PipelineShaderSpec
    {
        InternedString path;
        ShaderStage stage = ShaderStage::NONE;
        InternedString entryPoint = toHashedString("main");
        std::vector<InternedString> defines;
    };

    struct GraphicsPipelineSpec
    {
        std::vector<PipelineShaderSpec> shaders;

        std::vector<VertexInputBindingDesc> vertexBindings;
        std::vector<VertexInputAttributeDesc> vertexAttributes;

        PrimitiveTopology topology = PrimitiveTopology::TRIANGLE_LIST;
        PolygonMode polygonMode = PolygonMode::FILL;
        CullMode cullMode = CullMode::BACK;
        FrontFace frontFace = FrontFace::COUNTER_CLOCKWISE;
        bool depthTestEnable = true;
        bool depthWriteEnable = true;
        CompareOp depthCompareOp = CompareOp::LESS;
        bool depthBiasEnable = false;
        float depthBiasConstantFactor = 0.0f;
        float depthBiasSlopeFactor = 0.0f;
        // fragments keep their depth clamped to the viewport range instead of being clipped by the near/far planes
        bool depthClampEnable = false;
        std::vector<BlendStateDesc> blendStates;

        std::vector<Format> colorAttachmentFormats;
        Format depthAttachmentFormat = Format::UNDEFINED;
    };

    struct ComputePipelineSpec
    {
        PipelineShaderSpec shader;
    };

    // One shader module the device holds, as a table row.
    struct ShaderRow
    {
        std::string path;
        std::string stage;
        std::string entryPoint;
        std::string defines; // comma separated
        SPITE_OBSERVED_SCHEMA(ShaderRow, path, stage, entryPoint, defines)
    };

    // Shader hot reload compiles source/<name>.slang to binary/<name>.spv (+ .reflect.json, .deps), where the
    // pipelines load it from. An empty source: no hot reload.
    struct ShaderHotReloadDirectories
    {
        std::filesystem::path source;
        std::filesystem::path binary;
    };

    struct ShaderHotReloadStatus
    {
        ShaderHotReloadDirectories directories;
        bool watching = false; // the OS change notification is live; reloadShaders works regardless
    };

    // One hot reload: what each out-of-date source came to, by file name.
    struct ShaderReloadReport
    {
        std::vector<std::string> compiled; // written to the binary directory and swapped into loaded modules
        std::vector<std::string> failed; // "<file>: <error>": did not compile; the running shader stays
        std::vector<std::string> refused; // "<file>: <difference>": layout no longer matches the C++ build
        u32 pipelinesRebuilt = 0;
        u32 pipelinesKept = 0; // also built from a failed or refused shader, or refused by the driver
    };

    class ShaderHotReload;

    // Resolves specs into GPU pipelines on the shared bindless layout and
    // dedupes them through the device's pipeline cache.
    class SPITE_RENDERING_API ShaderManager
    {
    public:
        ShaderManager(const ShaderManager& other) = delete;
        ShaderManager(ShaderManager&& other) noexcept = delete;
        ShaderManager& operator=(const ShaderManager& other) = delete;
        ShaderManager& operator=(ShaderManager&& other) noexcept = delete;

        ShaderManager(IRenderDevice& device, IBindlessResourceManager& bindlessResourceManager,
                      const ShaderHotReloadDirectories& hotReload = {});
        ~ShaderManager();

        PipelineHandle getOrCreatePipeline(const GraphicsPipelineSpec& spec);
        PipelineHandle getOrCreateComputePipeline(const ComputePipelineSpec& spec);
        // One row per shader module, by path (the shaders table).
        [[nodiscard]] std::vector<ShaderRow> shaderRows() const;

        [[nodiscard]] bool hotReloadEnabled() const;
        [[nodiscard]] ShaderHotReloadStatus hotReloadStatus() const;
        // At a frame boundary, before the frame records: reloads once edited sources have settled (an OS change
        // notification; an idle frame reads no file).
        void pollHotReload();
        // Compiles every source newer than its outputs now; the pipelines built from them switch at the next
        // recorded frame under their existing handles. Frame thread, between frames; empty without hot reload.
        ShaderReloadReport reloadShaders();
        // The report of the last reload, a watcher's included, and how many ran: a failed or refused shader stays
        // listed until a reload finds the sources up to date.
        [[nodiscard]] const ShaderReloadReport& lastReloadReport() const { return m_lastReport; }
        [[nodiscard]] u32 reloadCount() const { return m_reloads; }

    private:
        IRenderDevice& m_device;
        IBindlessResourceManager& m_bindlessResourceManager;
        std::unique_ptr<ShaderHotReload> m_hotReload;
        ShaderReloadReport m_lastReport;
        u32 m_reloads = 0;
    };
} // namespace spite
