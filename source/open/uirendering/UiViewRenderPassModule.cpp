#include "uirendering/UiRendering.hpp"
#include "uiruntime/UiGpuRuntime.hpp"
#include "rendering/ShaderPaths.hpp"

namespace spite
{
    namespace ui_view_module
    {
        GraphicsPipelineSpec qualificationSpec(Format format)
        {
            GraphicsPipelineSpec spec{};
            spec.depthTestEnable = false;
            spec.depthWriteEnable = false;
            spec.cullMode = CullMode::NONE;
            spec.colorAttachmentFormats = {format};
            spec.blendStates = {{
                .blendEnable = true,
                .srcColorBlendFactor = BlendFactor::SRC_ALPHA,
                .dstColorBlendFactor = BlendFactor::ONE_MINUS_SRC_ALPHA,
                .srcAlphaBlendFactor = BlendFactor::ONE,
                .dstAlphaBlendFactor = BlendFactor::ONE_MINUS_SRC_ALPHA,
            }};
            spec.shaders.push_back(
                {.path = toHashedString(SPITE_SHADER_PATH("ui_view.vert")), .stage = ShaderStage::VERTEX});
            spec.shaders.push_back(
                {.path = toHashedString(SPITE_SHADER_PATH("ui_view.frag")), .stage = ShaderStage::FRAGMENT});

            return spec;
        }

    } // namespace ui_view_module

    RenderPassModuleDefinition createUiViewRenderPassModule()
    {
        HighLevelRenderModuleDefinition definition;
        definition.passName = toHashedString("UiView");
        definition.implementation = RenderPassImplementation::Default;
        definition.priority = 0;
        definition.setup = [](RenderModuleBuilder& builder)
        { ui_view_module::setup(builder, ui_view_module::qualificationSpec); };
        return createRenderPassModule(definition);
    }

    RenderPassModuleDefinition createUiViewTargetRenderPassModule()
    {
        HighLevelRenderModuleDefinition definition;
        definition.passName = toHashedString("UiViewTarget");
        definition.implementation = RenderPassImplementation::Default;
        definition.priority = 0;
        definition.perView = true;
        definition.setup = [](RenderModuleBuilder& builder)
        { ui_view_module::setupTarget(builder, ui_view_module::qualificationSpec); };
        return createRenderPassModule(definition);
    }
} // namespace spite
