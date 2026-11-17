cmake_minimum_required(VERSION 3.25)
project(SpiteAssetAuthoring LANGUAGES CXX)
find_package(Spite CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_library(SpiteAssetAuthoring SHARED
    AssetBaker.cpp
    SpriteAtlasBaking.cpp
)
target_compile_features(SpiteAssetAuthoring PRIVATE cxx_std_23)
target_compile_definitions(SpiteAssetAuthoring PRIVATE SPITE_ASSET_AUTHORING_BUILD_DLL)
spite_module_vendor(SpiteAssetAuthoring stb_rect_pack)
target_link_libraries(SpiteAssetAuthoring PRIVATE
    Spite::Base
    Spite::Profiling
    Spite::Asset
    nlohmann_json::nlohmann_json
)
spite_configure_module(SpiteAssetAuthoring)
