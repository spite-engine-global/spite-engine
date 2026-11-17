cmake_minimum_required(VERSION 3.25)
project(SpiteScene LANGUAGES CXX)
find_package(Spite CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_library(SpiteScene SHARED
    SceneApply.cpp
    SceneClone.cpp
    SceneCollect.cpp
    SceneStream.cpp
)
target_compile_features(SpiteScene PRIVATE cxx_std_23)
target_compile_definitions(SpiteScene PRIVATE SPITE_SCENE_BUILD_DLL)
target_link_libraries(SpiteScene PRIVATE Spite::Base Spite::Ecs Spite::Profiling Spite::Serialization Spite::World nlohmann_json::nlohmann_json)
spite_configure_module(SpiteScene)
