cmake_minimum_required(VERSION 3.25)
project(SpiteUi LANGUAGES CXX)
find_package(Spite CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_library(SpiteUi SHARED
    UiAccessibility.cpp
    UiBinding.cpp
    UiCanvas.cpp
    UiChart.cpp
    UiChartAxis.cpp
    UiColorPicker.cpp
    UiContext.cpp
    UiDock.cpp
    UiDocument.cpp
    UiDragDrop.cpp
    UiDropdown.cpp
    UiForm.cpp
    UiHierarchyManager.cpp
    UiIcons.cpp
    UiLayoutTokens.cpp
    UiMenu.cpp
    UiNavigation.cpp
    UiOverlay.cpp
    UiRichText.cpp
    UiRouter.cpp
    UiScope.cpp
    UiScrollView.cpp
    UiTable.cpp
    UiTextInput.cpp
    UiTreeView.cpp
    UiTween.cpp
    UiValueWidgets.cpp
)
target_compile_features(SpiteUi PRIVATE cxx_std_23)
target_compile_definitions(SpiteUi PRIVATE SPITE_UI_BUILD_DLL)
target_link_libraries(SpiteUi PRIVATE
    Spite::Base
    Spite::Profiling
    Spite::UiRuntime
    Spite::Ecs
    Spite::Localization
    nlohmann_json::nlohmann_json
)
spite_configure_module(SpiteUi)
