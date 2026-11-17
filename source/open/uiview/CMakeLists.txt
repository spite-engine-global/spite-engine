cmake_minimum_required(VERSION 3.25)
project(SpiteUiView LANGUAGES CXX)
find_package(Spite CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(harfbuzz CONFIG REQUIRED)
find_package(msdfgen CONFIG REQUIRED)
add_library(SpiteUiView SHARED
    UiFont.cpp
    UiPainter.cpp
    UiStyleSheet.cpp
    UiTextBlock.cpp
    UiTextCase.cpp
    UiViewContext.cpp
)
target_compile_features(SpiteUiView PRIVATE cxx_std_23)
target_compile_definitions(SpiteUiView PRIVATE SPITE_UIVIEW_BUILD_DLL)
target_link_libraries(SpiteUiView PRIVATE
    Spite::Base
    Spite::Profiling
    Spite::Ecs
    Spite::Ui
    Spite::UiRuntime
    nlohmann_json::nlohmann_json
    harfbuzz::harfbuzz
    msdfgen::msdfgen-ext
)
spite_configure_module(SpiteUiView)
