#pragma once

#include "uirendering/UiRenderingExport.hpp"
#include "rendering/RenderPassModule.hpp"
#include "rendering/ShaderManager.hpp"

namespace spite
{
    SPITE_UI_RENDERING_API RenderPassModuleDefinition createUiViewRenderPassModule();
    SPITE_UI_RENDERING_API RenderPassModuleDefinition createUiViewTargetRenderPassModule();

    namespace ui_view_module
    {
        SPITE_UI_RENDERING_API GraphicsPipelineSpec qualificationSpec(Format format);
    }
} // namespace spite
