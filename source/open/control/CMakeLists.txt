cmake_minimum_required(VERSION 3.25)
project(SpiteControl LANGUAGES CXX)
find_package(Spite CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
add_library(SpiteControl SHARED
    CommandHistory.cpp
    ControlServer.cpp
    DirectoryListing.cpp
    EcsTables.cpp
    FindEntities.cpp
    LogHistory.cpp
    Snapshot.cpp
)
target_compile_features(SpiteControl PRIVATE cxx_std_23)
target_compile_definitions(SpiteControl PRIVATE SPITE_CONTROL_BUILD_DLL)
target_link_libraries(SpiteControl PRIVATE Spite::Base Spite::Ecs Spite::Profiling Spite::Serialization nlohmann_json::nlohmann_json unofficial::sqlite3::sqlite3)
spite_configure_module(SpiteControl)
