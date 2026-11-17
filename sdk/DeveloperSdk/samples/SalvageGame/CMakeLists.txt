cmake_minimum_required(VERSION 3.25)
project(SalvageGame LANGUAGES CXX)

set(SPITE_SDK "" CACHE PATH "Path to the SPITE SDK")
find_package(Spite CONFIG REQUIRED PATHS "${SPITE_SDK}/cmake" NO_DEFAULT_PATH)
add_library(SalvageGame MODULE SalvageGame.cpp)
spite_configure_module(SalvageGame)
target_link_libraries(SalvageGame PRIVATE
    Spite::Base Spite::Ecs Spite::Application Spite::World
    Spite::Ui Spite::UiView Spite::Audio Spite::RenderRuntime)
