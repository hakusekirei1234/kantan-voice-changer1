# =============================================================================
#  KantanVoiceChanger (簡単ボイチェン)
#  Realtime voice changer for Windows 11 - standalone GUI app, no plugin.
#
#  Toolchain : Visual Studio 2019 / MSVC v142 (14.29.30133) / Windows SDK 10.0.17763.0
#  JUCE      : 8.0.15, checked out at ./external/JUCE
#
#  Configure (the generator is NOT optional - VS2022 has no C++ workload here):
#    cmake -S . -B C:/build/kvc -G "Visual Studio 16 2019" -A x64 -T v142,host=x64 ^
#          -DCMAKE_SYSTEM_VERSION=10.0.17763.0
#  Build:
#    cmake --build C:/build/kvc --config Release --parallel
#
#  SAVE THIS FILE AS UTF-8 WITH BOM.
# =============================================================================

# 3.25 so CMP0091 (MSVC runtime library abstraction) and CMP0141 default to NEW.
cmake_minimum_required(VERSION 3.25)

# LANGUAGES C CXX is mandatory: JUCE requires a C compiler, CXX alone fails configure.
project(KantanVoiceChanger
        VERSION 1.0.1
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 静的 CRT。非技術者に「VC++ 再頒布パッケージを入れてください」と言わずに済ませる。
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
    add_compile_options(
        /utf-8      # ACP=932 のこの機械では必須。無いと JUCE 自身のソースが C4819 を大量に吐き、
                    # かつ日本語文字列リテラルが CP932 と誤認されて全て化ける。
        /bigobj
        /MP
        /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# -----------------------------------------------------------------------------
# JUCE
# -----------------------------------------------------------------------------
set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/JUCE" CACHE PATH
    "Path to a JUCE 8.0.15 checkout. Must be a pure-ASCII path: juceaide crashes otherwise.")

if(NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "JUCE not found at '${JUCE_DIR}'.\n"
        "git clone --branch 8.0.15 --depth 1 https://github.com/juce-framework/JUCE.git \"${JUCE_DIR}\"")
endif()

add_subdirectory("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/JUCE" EXCLUDE_FROM_ALL)

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------
option(KVC_ENABLE_ASIO
       "Enable ASIO device support. JUCE 8.0.11+ bundles the Steinberg ASIO headers, so this \
costs nothing at build time - but PUBLISHING a closed-source binary with ASIO requires a signed \
Steinberg ASIO License Agreement. Build with -DKVC_ENABLE_ASIO=OFF to ship without it." ON)

option(KVC_ASIO_USE_EXTERNAL_SDK
       "Use an external Steinberg ASIO SDK instead of JUCE's bundled headers." OFF)

# VST3 ホスティングは常時 ON。VST3 SDK は JUCE 同梱で、追加のライセンス手続きも要らない。
#
# VST2 は別扱い。JUCE の VST2 ホストは Steinberg の VST2 SDK ヘッダ
# (pluginterfaces/vst2.x/aeffect.h) を要求するが、JUCE はこれを同梱しておらず、
# Steinberg は VST2 SDK の新規ライセンスを発行していない。そこで JUCE の実装は使わず、
# ABI 定義を自前で持つ最小ホストを src/audio/Vst2Host.cpp に置く（Ardour / LMMS と同じ方式）。
# Pitchproof が VST2 でしか配布されていないため必要になった機能。
#
# ★公開配布時の判断: VST2 ホストの配布はライセンス上グレー。
#   公開版は -DKVC_ENABLE_VST2=OFF でビルドすること。
option(KVC_ENABLE_VST2
       "Host VST2 plugins (e.g. Pitchproof) via our own clean-room implementation. \
Legally grey for public distribution - build the public release with OFF." ON)

set(KVC_ASIO_SDK_DIR "" CACHE PATH
    "Path to the ASIO SDK 'common' folder containing iasiodrv.h (only used when \
KVC_ASIO_USE_EXTERNAL_SDK=ON).")

# JUCE 8.0.11 以降はヘッダを同梱している。実際に在るかを configure 時に確認し、
# 無ければ外部 SDK を要求する（黙って JUCE_ASIO=1 のままにするとリンク直前で落ちる）。
set(KVC_BUNDLED_ASIO_HEADER
    "${JUCE_DIR}/modules/juce_audio_devices/native/asio/iasiodrv.h")

if(KVC_ENABLE_ASIO AND NOT KVC_ASIO_USE_EXTERNAL_SDK)
    if(NOT EXISTS "${KVC_BUNDLED_ASIO_HEADER}")
        message(WARNING
            "KVC_ENABLE_ASIO=ON but this JUCE checkout has no bundled ASIO headers at\n"
            "  ${KVC_BUNDLED_ASIO_HEADER}\n"
            "Disabling ASIO. Re-enable with -DKVC_ASIO_USE_EXTERNAL_SDK=ON -DKVC_ASIO_SDK_DIR=<sdk>/common")
        set(KVC_ENABLE_ASIO OFF CACHE BOOL "" FORCE)
    endif()
endif()

if(KVC_ENABLE_ASIO AND KVC_ASIO_USE_EXTERNAL_SDK)
    if(NOT KVC_ASIO_SDK_DIR OR NOT EXISTS "${KVC_ASIO_SDK_DIR}/iasiodrv.h")
        message(FATAL_ERROR
            "KVC_ASIO_USE_EXTERNAL_SDK=ON but iasiodrv.h was not found under "
            "KVC_ASIO_SDK_DIR='${KVC_ASIO_SDK_DIR}'.")
    endif()
endif()

# -----------------------------------------------------------------------------
# kvc_config - JUCE module configuration macros.
#
# JUCE modules are INTERFACE libraries whose .cpp files are compiled INTO the
# consuming target, so these definitions are what actually configure them.
# -----------------------------------------------------------------------------
add_library(kvc_config INTERFACE)

target_compile_definitions(kvc_config INTERFACE
    # --- audio backends -------------------------------------------------------
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_ASIO=$<BOOL:${KVC_ENABLE_ASIO}>
    JUCE_ASIO_USE_EXTERNAL_SDK=$<BOOL:${KVC_ASIO_USE_EXTERNAL_SDK}>
    JUCE_USE_WINRT_MIDI=0
    JUCE_USE_WINDOWS_MIDI_SERVICES=0

    # --- audio formats --------------------------------------------------------
    JUCE_USE_FLAC=0
    JUCE_USE_OGGVORBIS=0
    JUCE_USE_MP3AUDIOFORMAT=1        # デコードのみ。書き出しは Media Foundation。
    JUCE_USE_LAME_AUDIO_FORMAT=0

    # --- plugin hosting --------------------------------------------------------
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0        # JUCE の VST2 ホストは使わない（Steinberg SDK を要求するため）
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_PLUGINHOST_ARA=0
    JUCE_CUSTOM_VST3_SDK=0

    # --- trim ------------------------------------------------------------------
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0

    # --- hygiene ---------------------------------------------------------------
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=0)    # 全ダイアログは launchAsync 系のみ

if(KVC_ENABLE_ASIO AND KVC_ASIO_USE_EXTERNAL_SDK)
    target_include_directories(kvc_config INTERFACE "${KVC_ASIO_SDK_DIR}")
endif()

# -----------------------------------------------------------------------------
# kvc_core - the pure DSP / engine sources.
#
# MUST be INTERFACE, never STATIC: JUCE modules attach their sources via
# target_sources(INTERFACE), so a STATIC core linking JUCE PUBLIC-ly would
# compile the module objects twice and produce duplicate symbols. INTERFACE lets
# a future juce_add_plugin target reuse exactly these sources.
# -----------------------------------------------------------------------------
add_library(kvc_core INTERFACE)

target_sources(kvc_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/DriftRing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/BeepGenerator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/PitchTracker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/PitchFormantShifter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/Wola.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/NoiseSuppressor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/VoiceEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core/VoiceAnalyser.cpp)
# NOTE: src/core/PeakRing.h and src/core/Parameters.h are header-only by design.

target_include_directories(kvc_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# -----------------------------------------------------------------------------
# The application.
#
# juce_add_gui_app, NOT juce_add_plugin: StandalonePluginHolder owns exactly one
# AudioDeviceManager and cannot express the three-device graph this app needs.
# PRODUCT_NAME / COMPANY_NAME / BUNDLE_ID are pure ASCII on purpose - juceaide
# mis-decodes non-ASCII CMake output under codepage 932. Japanese text lives only
# in C++ string literals, which are safe because we compile /utf-8.
# -----------------------------------------------------------------------------
juce_add_gui_app(KantanVoiceChanger
    PRODUCT_NAME      "KantanVoiceChanger"
    VERSION           ${PROJECT_VERSION}
    COMPANY_NAME      "KantanVoice"
    COMPANY_COPYRIGHT "Copyright (c) 2026 KantanVoice"
    BUNDLE_ID         "com.kantanvoice.kantanvoicechanger"
    NEEDS_CURL        FALSE
    NEEDS_WEB_BROWSER FALSE
    NEEDS_WEBVIEW2    FALSE)

target_sources(KantanVoiceChanger PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Main.cpp

    ${CMAKE_CURRENT_SOURCE_DIR}/src/app/Settings.cpp

    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/DeviceCatalog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/AudioEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/Recorder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/Mp3Exporter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/PluginHost.cpp

    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/GlobalHotkey_Win32.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/RevealInExplorer.cpp

    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/WaveformDisplay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/DeviceSelectorPanel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/HotkeyCaptureDialog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/AdvancedSettingsDialog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/MainComponent.cpp)

target_include_directories(KantanVoiceChanger PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_options(KantanVoiceChanger PRIVATE /utf-8)

target_compile_definitions(KantanVoiceChanger PRIVATE
    KVC_VERSION_STRING="${PROJECT_VERSION}"
    KVC_ASIO_ENABLED=$<BOOL:${KVC_ENABLE_ASIO}>
    KVC_VST2_ENABLED=$<BOOL:${KVC_ENABLE_VST2}>)

if(KVC_ENABLE_VST2)
    target_sources(KantanVoiceChanger PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/audio/Vst2Host.cpp)
endif()

target_link_libraries(KantanVoiceChanger PRIVATE
    kvc_core
    kvc_config
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_gui_extra
    # VST3 ホスティングのために追加。同梱の VST3 SDK がコンパイルされるので
    # ビルド時間と exe サイズは増えるが、ライセンス上の追加手続きは要らない。
    # JUCE 8 では AudioPluginFormatManager::addDefaultFormats() が削除されているので、
    # PluginHost は VST3PluginFormat を明示的に addFormat する（そもそも VST3 だけ欲しい）。
    juce::juce_audio_processors
    # 意図的に juce_audio_utils は外したまま。必要な AudioTransportSource /
    # AudioFormatReaderSource は juce_audio_devices と juce_audio_formats 側に在る。
  PRIVATE
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# Media Foundation (MP3 書き出し) + Shell (エクスプローラで選択表示) + hotkey。
target_link_libraries(KantanVoiceChanger PRIVATE
    mf mfplat mfreadwrite mfuuid propsys shlwapi ole32 uuid user32 shell32)

# Windows N 版には mf*.dll が無い。静的インポートのままだと main() に到達する前に
# 「DLL が見つかりません」で起動不能になるため、必ず遅延ロードにする。
# mfuuid/uuid は GUID 定義だけの静的ライブラリなので対象外。
target_link_options(KantanVoiceChanger PRIVATE
    /DELAYLOAD:mf.dll
    /DELAYLOAD:mfplat.dll
    /DELAYLOAD:mfreadwrite.dll)
target_link_libraries(KantanVoiceChanger PRIVATE delayimp)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
             PROPERTY VS_STARTUP_PROJECT KantanVoiceChanger)

# -----------------------------------------------------------------------------
# Tests.
#
# A console app, not a GUI one: CI has no window station and no audio device.
# It links kvc_core only - nothing here touches juce_audio_devices, so the tests
# run identically on a headless runner and on the developer's machine.
#
#   cmake -S . -B build -DKVC_BUILD_TESTS=ON
#   cmake --build build --config Release --target kvc_tests --parallel 6
#   ctest --test-dir build -C Release --output-on-failure
# -----------------------------------------------------------------------------
option(KVC_BUILD_TESTS "Build the DSP test runner (console app)." OFF)

if(KVC_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(kvc_tests
        PRODUCT_NAME "kvc_tests"
        VERSION      ${PROJECT_VERSION})

    target_sources(kvc_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestMain.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/TestSupport.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/PitchFormantShifterTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/PitchTrackerTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/VoiceEngineTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/NoiseSuppressorTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/DriftRingTests.cpp)

    target_include_directories(kvc_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)

    target_compile_definitions(kvc_tests PRIVATE
        KVC_VERSION_STRING="${PROJECT_VERSION}")

    target_link_libraries(kvc_tests PRIVATE
        kvc_core
        kvc_config
        juce::juce_audio_basics
        juce::juce_dsp
      PRIVATE
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

    add_test(NAME kvc_dsp_tests COMMAND kvc_tests)
endif()
