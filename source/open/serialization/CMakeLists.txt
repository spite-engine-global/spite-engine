cmake_minimum_required(VERSION 3.25)
project(SpiteSerialization LANGUAGES CXX)
find_package(Spite CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_library(SpiteSerialization SHARED
    BinaryFormat.cpp
    FormatWalk.cpp
    JsonFormat.cpp
)
target_compile_features(SpiteSerialization PRIVATE cxx_std_23)
target_compile_definitions(SpiteSerialization PRIVATE SPITE_SERIALIZATION_BUILD_DLL)
target_link_libraries(SpiteSerialization PRIVATE Spite::Base Spite::Profiling nlohmann_json::nlohmann_json)
spite_configure_module(SpiteSerialization)
