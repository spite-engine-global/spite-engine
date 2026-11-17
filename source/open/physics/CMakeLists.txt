cmake_minimum_required(VERSION 3.25)
project(SpitePhysics LANGUAGES CXX)
find_package(Spite CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_library(SpitePhysics SHARED
    PhysicsWorld.cpp
)
target_compile_features(SpitePhysics PRIVATE cxx_std_23)
target_compile_definitions(SpitePhysics PRIVATE SPITE_PHYSICS_BUILD_DLL)
target_link_libraries(SpitePhysics PRIVATE
    Spite::Base
    Spite::Profiling
    Spite::World
    Spite::Ecs
    Spite::Jolt
    nlohmann_json::nlohmann_json
)
spite_configure_module(SpitePhysics)
