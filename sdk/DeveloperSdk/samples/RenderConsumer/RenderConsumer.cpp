#include <filesystem>
#include <format>
#include <cstring>
#include <stdexcept>

#include "base/ContentRoot.hpp"
#include "control/ControlServer.hpp"
#include "engine/Module.hpp"

#include "rendering/IRenderer.hpp"
#include "rendering/IRenderResourceManager.hpp"
#include "rendering/RenderPassModule.hpp"
#include "serialization/FormatWalk.hpp"


namespace
{
    using namespace spite;

    constexpr auto VERTEX = "./shaders/bin/sdk_stripe.vert.spv";
    constexpr auto FRAGMENT = "./shaders/bin/sdk_stripe.frag.spv";
    constexpr auto HDR_VERTEX = "./shaders/bin/sdk_hdr.vert.spv";
    constexpr auto HDR_FRAGMENT = "./shaders/bin/sdk_hdr.frag.spv";
    BufferHandle vertices{};
    BufferHandle hdrColors[2]{};
    u32 view0Color = 0;
    const BufferDesc HDR_COLOR_DESC{.size = 4 * sizeof(float),
                                    .usage = BufferUsage::STORAGE_BUFFER | BufferUsage::SHADER_DEVICE_ADDRESS,
                                    .memoryUsage = MemoryUsage::CPU_TO_GPU};

    PipelineHandle stripePipeline(RenderPassExecutionContext& context, Format format, bool biased)
    {
        GraphicsPipelineSpec spec;
        spec.depthTestEnable = true;
        spec.depthWriteEnable = true;
        spec.depthCompareOp = CompareOp::GREATER;
        spec.depthBiasEnable = biased;
        spec.depthBiasConstantFactor = biased ? 16.0f : 0.0f;
        spec.depthAttachmentFormat = Format::D32_SFLOAT;
        spec.cullMode = CullMode::NONE;
        spec.colorAttachmentFormats = {format};
        spec.vertexBindings = {{.binding = 0, .stride = 2 * sizeof(float)}};
        spec.vertexAttributes = {{.location = 0, .binding = 0, .format = Format::R32G32_SFLOAT, .offset = 0}};
        spec.shaders = {
            {.path = InternedString(VERTEX), .stage = ShaderStage::VERTEX},
            {.path = InternedString(FRAGMENT), .stage = ShaderStage::FRAGMENT},
        };
        return context.services().write<ShaderManager>()->getOrCreatePipeline(spec);
    }

    bool load(const ModuleContext& context)
    {
        auto& services = context.scene.services();
        if (!services.contains<RenderPassSetupService>())
        {
            return true;
        }

        if (!std::filesystem::is_regular_file(resolveContentPath(VERTEX)) ||
            !std::filesystem::is_regular_file(resolveContentPath(FRAGMENT)) ||
            !std::filesystem::is_regular_file(resolveContentPath(HDR_VERTEX)) ||
            !std::filesystem::is_regular_file(resolveContentPath(HDR_FRAGMENT)))
            throw std::runtime_error(std::format("RenderConsumer requires shader files {}, {}, {} and {}", VERTEX,
                                                 FRAGMENT, HDR_VERTEX, HDR_FRAGMENT));

        constexpr float positions[] = {-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f};
        {
            auto resources = services.write<IRenderResourceManager>();
            vertices = resources->createBuffer({.size = sizeof(positions),
                                                .usage = BufferUsage::VERTEX_BUFFER,
                                                .memoryUsage = MemoryUsage::CPU_TO_GPU},
                                               "SdkStripeVertices");
            std::memcpy(resources->mapBuffer(vertices), positions, sizeof(positions));
            resources->unmapBuffer(vertices);
        }

        struct LoadGuard
        {
            ServiceLocator& services;
            bool committed = false;
            ~LoadGuard()
            {
                if (!committed)
                {
                    services.write<IRenderResourceManager>()->destroyBuffer(vertices);
                    vertices = {};
                    for (auto& color : hdrColors)
                    {
                        if (color.isValid())
                            services.write<IRenderResourceManager>()->destroyBuffer(color);
                        color = {};
                    }
                }
            }
        } guard{services};

        // Immutable presets isolate the picture-change signal from in-flight buffer writes.
        constexpr float colors[2][4] = {{8.0f, 0.05f, 0.05f, 1.0f}, {0.05f, 8.0f, 0.05f, 1.0f}};
        {
            auto resources = services.write<IRenderResourceManager>();
            for (u32 i = 0; i != 2; ++i)
            {
                hdrColors[i] = resources->createBuffer(HDR_COLOR_DESC, i == 0 ? "SdkHdrRed" : "SdkHdrGreen");
                std::memcpy(resources->mapBuffer(hdrColors[i]), colors[i], sizeof(colors[i]));
                resources->unmapBuffer(hdrColors[i]);
            }
        }
        view0Color = 0;
        context.control.registerCommand("renderConsumerSetHdrColor", context.owner.toView(),
                                        "select the HDR witness preset for view 0",
                                        {requiredArg("view", ArgKind::Integer, "only view 0 is supported"),
                                         requiredArg("color", ArgKind::Integer, "0 = red, 1 = green")},
                                        [&services](const ControlArgs& args, IFormatWriter& out) -> std::string
                                        {
                                            const auto view = args.integer("view");
                                            const auto color = args.integer("color");
                                            if (!view || *view != 0 || !color || *color > 1)
                                                return "view must be 0 and color must be 0 or 1";
                                            if (view0Color != *color)
                                            {
                                                view0Color = static_cast<u32>(*color);
                                                services.write<RenderPassSetupService>()->markViewContentChanged(0);
                                            }
                                            out.writeU32("view", 0);
                                            out.writeU32("color", view0Color);
                                            return {};
                                        });

        auto setup = services.write<RenderPassSetupService>();
        const InternedString name("SdkColorStripe");
        setup->registerModule(
            context.owner,
            createRenderPassModule({
                .passName = name,
                .perView = true,
                .placement = RenderPassPlacement::AfterComposite,
                .setup =
                    [name](RenderModuleBuilder& module)
                {
                    for (u32 witness = 0; witness != 3; ++witness)
                    {
                        const InternedString passName(witness == 0       ? "SdkColorStripe"
                                                          : witness == 1 ? "SdkBiasControl"
                                                                         : "SdkBiasApplied");
                        module.addGraphicsPass(
                            passName,
                            [witness](RGBuilder& builder)
                            {
                                builder.asGraphicsPassMatchingAttachment();
                                builder.writeColorAttachment(viewTargetName(0), 0, RGUsage::ColorAttachmentWrite,
                                                             {.loadOp = AttachmentLoadOp::LOAD});
                                const auto depth = toHashedString("SdkBiasDepth");
                                if (witness == 0)
                                    builder.createPerFrameTexture(depth,
                                                                  {.format = Format::D32_SFLOAT,
                                                                   .usage = TextureUsage::DEPTH_STENCIL_ATTACHMENT,
                                                                   .extent = TextureExtent::DISPLAY});
                                builder.writeDepth(
                                    depth, RGUsage::DepthStencilAttachmentWrite,
                                    witness == 0 ? RGWriteAttachmentInfo{.loadOp = AttachmentLoadOp::CLEAR,
                                                                         .clearValue = ClearDepthStencilValue{0.0f, 0}}
                                                 : RGWriteAttachmentInfo{.loadOp = AttachmentLoadOp::LOAD});
                            },
                            [witness, pipeline = PipelineHandle{},
                             format = Format::UNDEFINED](RenderPassExecutionContext& context) mutable
                            {
                                const Format current = context.services().read<IRenderer>()->getSwapchainFormat();
                                if (current != format)
                                {
                                    pipeline = stripePipeline(context, current, witness == 2);
                                    format = current;
                                }
                                return pipeline;
                            },
                            [witness](RenderPassExecutionContext& context)
                            {
                                context.bindVertexBuffer(vertices);
                                context.draw(3, 1, 0, witness);
                            });
                    }
                },
            }));

        setup->registerModule(
            context.owner,
            createRenderPassModule({
                .passName = InternedString("SdkHdrWitness"),
                .perView = true,
                .placement = RenderPassPlacement::AfterTransparent,
                .setup =
                    [](RenderModuleBuilder& module)
                {
                    GraphicsPipelineSpec spec;
                    spec.depthTestEnable = false;
                    spec.depthWriteEnable = false;
                    spec.cullMode = CullMode::NONE;
                    spec.colorAttachmentFormats = {Format::R16G16B16A16_SFLOAT};
                    spec.shaders = {
                        {.path = InternedString(HDR_VERTEX), .stage = ShaderStage::VERTEX},
                        {.path = InternedString(HDR_FRAGMENT), .stage = ShaderStage::FRAGMENT},
                    };
                    const auto pipeline = module.declareGraphicsPipeline(InternedString("SdkHdrWitness"), spec);
                    module.addFullscreenPass(
                        InternedString("SdkHdrWitness"),
                        [](RGBuilder& builder)
                        {
                            builder.importBuffer(toHashedString("SdkHdrRed"), hdrColors[0], HDR_COLOR_DESC);
                            builder.importBuffer(toHashedString("SdkHdrGreen"), hdrColors[1], HDR_COLOR_DESC);
                            builder.read(toHashedString("SdkHdrRed"), RGUsage::StorageBufferRead);
                            builder.read(toHashedString("SdkHdrGreen"), RGUsage::StorageBufferRead);
                            builder.asGraphicsPassMatchingAttachment();
                            builder.writeColorAttachment(toHashedString("SceneColor"), 0, RGUsage::ColorAttachmentWrite,
                                                         {.loadOp = AttachmentLoadOp::LOAD});
                        },
                        pipeline,
                        [](RenderPassExecutionContext& context)
                        {
                            context.pushConstants(
                                context.bufferAddress(hdrColors[context.view() == 0 ? view0Color : 0]));
                        });
                },
            }));

        setup->finalize(services);
        guard.committed = true;
        return true;
    }

    void unload(const ModuleContext& context)
    {
        for (auto& color : hdrColors)
        {
            if (color.isValid())
                context.scene.services().write<IRenderResourceManager>()->destroyBuffer(color);
            color = {};
        }
        if (vertices.isValid())
        {
            context.scene.services().write<IRenderResourceManager>()->destroyBuffer(vertices);
            vertices = {};
        }
    }
} // namespace

SPITE_MODULE_EXPORT const spite::ModuleDescriptor* SPITE_MODULE_ENTRY()
{
    static const spite::ModuleDescriptor descriptor{.name = "RenderConsumer", .load = &load, .unload = &unload};
    return &descriptor;
}
