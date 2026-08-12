cmake_minimum_required(VERSION 3.22)

project(ToolchainTest VERSION 0.0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../external/JUCE" "${CMAKE_BINARY_DIR}/JUCE")

juce_add_gui_app(ToolchainTest
    PRODUCT_NAME "ToolchainTest"
    COMPANY_NAME "Test")

target_sources(ToolchainTest PRIVATE Main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/audio/Vst2Host.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/audio/PluginHost.cpp)

target_include_directories(ToolchainTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)

# JUCE sources are UTF-8; without this MSVC reads them as CP932 on a Japanese system.
target_compile_options(ToolchainTest PRIVATE /utf-8)

target_compile_definitions(ToolchainTest PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_ASIO=1
    JUCE_PLUGINHOST_VST3=1
    KVC_VST2_ENABLED=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_AU=0
    JUCE_MODAL_LOOPS_PERMITTED=0)

target_link_libraries(ToolchainTest PRIVATE
    juce::juce_gui_extra
    juce::juce_audio_devices
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
