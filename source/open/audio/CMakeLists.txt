cmake_minimum_required(VERSION 3.25)
project(SpiteAudio LANGUAGES CXX)
find_package(Spite CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)
add_library(SpiteAudio SHARED
    AudioCommands.cpp
    AudioWorld.cpp
)
target_compile_features(SpiteAudio PRIVATE cxx_std_23)
target_compile_definitions(SpiteAudio PRIVATE SPITE_AUDIO_BUILD_DLL)
spite_module_vendor(SpiteAudio miniaudio)
target_link_libraries(SpiteAudio PRIVATE
    Spite::Base
    Spite::Profiling
    Spite::World
    Spite::Ecs
    Spite::Control
    Spite::Serialization
    nlohmann_json::nlohmann_json
    Threads::Threads
)
spite_configure_module(SpiteAudio)
if(WIN32)
else()
    target_link_libraries(SpiteAudio PRIVATE dl)
endif()
