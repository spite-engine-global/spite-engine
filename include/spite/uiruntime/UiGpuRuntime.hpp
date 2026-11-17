#pragma once

#include "uiruntime/UiGpuRuntimeExport.hpp"
#include "rendering/RenderPassModule.hpp"
#include "rendering/ShaderManager.hpp"

namespace spite::ui_view_module
{
    using UiPipelineSpecFactory = GraphicsPipelineSpec (*)(Format);
    SPITE_UI_GPU_RUNTIME_API void setup(RenderModuleBuilder& builder, UiPipelineSpecFactory specFactory);
    SPITE_UI_GPU_RUNTIME_API void setupTarget(RenderModuleBuilder& builder, UiPipelineSpecFactory specFactory);
} // namespace spite::ui_view_module
