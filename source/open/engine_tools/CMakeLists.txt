cmake_minimum_required(VERSION 3.25)
project(SpiteEngineTools LANGUAGES CXX)
find_package(Spite CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_library(SpiteEngineTools SHARED
    AnimationCommands.cpp
    ConfigCommands.cpp
    InputCommands.cpp
    LocalizationCommands.cpp
    SceneCommands.cpp
    StyleCommands.cpp
    UiDisplayCommands.cpp
    UiInspectCommands.cpp
    ViewModeCommands.cpp
    WindowCommands.cpp
    LocalizationContent.cpp
    AssetCommands.cpp
    FlipbookCard.cpp
    PrimitiveMesh.cpp
)
target_compile_features(SpiteEngineTools PRIVATE cxx_std_23)
target_compile_definitions(SpiteEngineTools PRIVATE SPITE_ENGINE_TOOLS_BUILD_DLL)
spite_module_vendor(SpiteEngineTools stb_image_write)
target_link_libraries(SpiteEngineTools PRIVATE
    Spite::Base
    Spite::Profiling
    Spite::Ecs
    Spite::World
    Spite::Animation
    Spite::AnimationAuthoring
    Spite::Asset
    Spite::AssetAuthoring
    Spite::AssetRuntime
    Spite::Audio
    Spite::Physics
    Spite::Application
    Spite::Control
    Spite::Serialization
    Spite::Scene
    Spite::Ui
    Spite::UiView
    Spite::Localization
    Spite::Rendering
    Spite::RenderRuntime
    nlohmann_json::nlohmann_json
)
spite_configure_module(SpiteEngineTools)
if(WIN32)
    target_sources(SpiteEngineTools PRIVATE AnimationControllerWatch.cpp)
endif()
