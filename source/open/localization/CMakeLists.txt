cmake_minimum_required(VERSION 3.25)
project(SpiteLocalization LANGUAGES CXX)
find_package(Spite CONFIG REQUIRED)
add_library(SpiteLocalization SHARED
    LocaleRules.cpp
    Localization.cpp
    Message.cpp
    PseudoLocale.cpp
)
target_compile_features(SpiteLocalization PRIVATE cxx_std_23)
target_compile_definitions(SpiteLocalization PRIVATE SPITE_LOCALIZATION_BUILD_DLL)
target_link_libraries(SpiteLocalization PRIVATE Spite::Base Spite::Profiling)
spite_configure_module(SpiteLocalization)
