cmake_minimum_required(VERSION 3.25)
project(SpiteApplication LANGUAGES CXX)
find_package(Spite CONFIG REQUIRED)
find_package(SDL3 CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
add_library(SpiteApplication SHARED
    Application.cpp
    ApplicationConfigFields.cpp
    Clipboard.cpp
    ConfigStore.cpp
    EventDispatcher.cpp
    FrameClock.cpp
    FrameScheduler.cpp
    PlatformServices.cpp
    SdlWindowBackend.cpp
    Shell.cpp
    Window.cpp
    WindowService.cpp
    input/InputActionMap.cpp
    input/InputInjection.cpp
    input/InputSystem.cpp
    input/SdlInputDevice.cpp
    input/TextInputBridge.cpp
    input/VirtualGamepad.cpp
)
target_compile_features(SpiteApplication PRIVATE cxx_std_23)
target_compile_definitions(SpiteApplication PRIVATE SPITE_APPLICATION_BUILD_DLL)
target_link_libraries(SpiteApplication PRIVATE Spite::Base Spite::Profiling Spite::Serialization SDL3::SDL3-shared Vulkan::Headers)
spite_configure_module(SpiteApplication)
