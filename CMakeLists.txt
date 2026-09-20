cmake_minimum_required(VERSION 3.26)
project(MacTools VERSION 0.6.5 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0")
    set(CMAKE_OSX_ARCHITECTURES "arm64")
endif()
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(MacToolsCore STATIC
    Sources/Core/AudioStream.cpp
    Sources/Core/AEC.cpp
    Sources/Core/MicHealth.cpp
    Sources/Core/REWParser.cpp
    Sources/Core/DSP.cpp
    Sources/Display/DisplayTools.cpp
)
target_include_directories(MacToolsCore PUBLIC Sources/Core Sources/Display)
if(APPLE)
    find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
    target_link_libraries(MacToolsCore PUBLIC "${ACCELERATE_FRAMEWORK}")
endif()
if(MSVC)
    target_compile_options(MacToolsCore PRIVATE /W4 /permissive-)
else()
    target_compile_options(MacToolsCore PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(MacToolsTests Tests/CoreTests.cpp)
if(APPLE)
    target_sources(MacToolsTests PRIVATE Sources/Mic/MicShared.c)
endif()
target_include_directories(MacToolsTests PRIVATE Sources/Mic Sources/Display)
target_link_libraries(MacToolsTests PRIVATE MacToolsCore)

add_executable(MacToolsValidate Tools/Validate.cpp)
target_link_libraries(MacToolsValidate PRIVATE MacToolsCore)

add_executable(MacToolsMicHealthTests Tests/MicHealthTests.cpp)
target_link_libraries(MacToolsMicHealthTests PRIVATE MacToolsCore)

add_executable(MacToolsBenchmark Tools/Benchmark.cpp)
target_link_libraries(MacToolsBenchmark PRIVATE MacToolsCore)

add_executable(MacToolsAECBenchmark Tools/AECBenchmark.cpp)
target_link_libraries(MacToolsAECBenchmark PRIVATE MacToolsCore)

find_package(Threads REQUIRED)
add_executable(MacToolsStreamTests Tests/AudioStreamTests.cpp)
target_link_libraries(MacToolsStreamTests PRIVATE MacToolsCore Threads::Threads)

enable_testing()
add_test(NAME MacToolsStreamTests COMMAND MacToolsStreamTests)
add_test(NAME MacToolsCoreTests COMMAND MacToolsTests)
add_test(NAME MacToolsMicHealthTests COMMAND MacToolsMicHealthTests)

if(WIN32)
    add_subdirectory(Sources/Windows)
endif()

if(NOT APPLE)
    return()
endif()

find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
find_library(APPKIT_FRAMEWORK AppKit REQUIRED)
find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
find_library(CARBON_FRAMEWORK Carbon REQUIRED)
find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
find_library(UNIFORMTYPEIDENTIFIERS_FRAMEWORK UniformTypeIdentifiers REQUIRED)

set(APPLE_NULL_AUDIO_ARCHIVE "${CMAKE_BINARY_DIR}/CreatingAnAudioServerDriverPlugIn.zip")
set(APPLE_NULL_AUDIO_SOURCE_DIR "${CMAKE_BINARY_DIR}/AppleNullAudio")
if(NOT EXISTS "${APPLE_NULL_AUDIO_SOURCE_DIR}/NullAudio.c")
    file(DOWNLOAD
        "https://docs-assets.developer.apple.com/published/430ad6501f6f/CreatingAnAudioServerDriverPlugIn.zip"
        "${APPLE_NULL_AUDIO_ARCHIVE}"
        EXPECTED_HASH SHA256=860334dfa9e83f2afb749a890ca5a79fc7d3d572d4e116ff9242bec458be4cb0
        TLS_VERIFY ON
    )
    file(MAKE_DIRECTORY "${APPLE_NULL_AUDIO_SOURCE_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${APPLE_NULL_AUDIO_ARCHIVE}"
         DESTINATION "${APPLE_NULL_AUDIO_SOURCE_DIR}")
endif()

add_executable(MacToolsMicHealthUITests Tests/MicHealthUITests.mm Sources/Mic/MicHealthUI.mm)
target_include_directories(MacToolsMicHealthUITests PRIVATE Sources/Mic)
target_compile_options(MacToolsMicHealthUITests PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(MacToolsMicHealthUITests PRIVATE "${FOUNDATION_FRAMEWORK}")

add_executable(MacToolsAudioProbe Tools/AudioProbe.mm)
target_compile_options(MacToolsAudioProbe PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(MacToolsAudioProbe PRIVATE
    "${COREAUDIO_FRAMEWORK}"
    "${FOUNDATION_FRAMEWORK}"
)

set_source_files_properties(Resources/32RTX950.edid.base64 PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
)
set_source_files_properties(Resources/MacTools.icns PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
)
add_executable(MacToolsEngine MACOSX_BUNDLE
    Sources/Engine/main.mm
    Sources/Mic/MicShared.c
    Sources/Mic/MicHealthUI.mm
    Sources/Display/DisplayController.mm
    Resources/32RTX950.edid.base64
)
set_target_properties(MacToolsEngine PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/EngineInfo.plist"
)
target_include_directories(MacToolsEngine PRIVATE Sources/Core Sources/Mic Sources/Display)
target_compile_options(MacToolsEngine PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(MacToolsEngine PRIVATE
    MacToolsCore
    "${APPKIT_FRAMEWORK}"
    "${AVFOUNDATION_FRAMEWORK}"
    "${CARBON_FRAMEWORK}"
    "${COREAUDIO_FRAMEWORK}"
    "${COREGRAPHICS_FRAMEWORK}"
    "${FOUNDATION_FRAMEWORK}"
    "${IOKIT_FRAMEWORK}"
)

add_library(MacToolsMic MODULE
    Sources/Driver/MacSoundMic.c
    Sources/Mic/MicShared.c
)
set_target_properties(MacToolsMic PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION driver
    PREFIX ""
    OUTPUT_NAME "MacToolsMic"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/MicDriverInfo.plist.in"
)
target_include_directories(MacToolsMic PRIVATE
    Sources/Mic
    "${APPLE_NULL_AUDIO_SOURCE_DIR}"
)
target_compile_options(MacToolsMic PRIVATE -fblocks -Wno-deprecated-declarations)
target_link_libraries(MacToolsMic PRIVATE
    "${COREAUDIO_FRAMEWORK}"
    "${COREFOUNDATION_FRAMEWORK}"
)

add_executable(MacTools MACOSX_BUNDLE Sources/App/main.mm Sources/Mic/MicHealthUI.mm Resources/MacTools.icns)
set_target_properties(MacTools PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppInfo.plist.in"
)
target_include_directories(MacTools PRIVATE Sources/Core Sources/Display Sources/Mic)
target_compile_options(MacTools PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(MacTools PRIVATE
    MacToolsCore
    "${APPKIT_FRAMEWORK}"
    "${COREAUDIO_FRAMEWORK}"
    "${FOUNDATION_FRAMEWORK}"
    "${UNIFORMTYPEIDENTIFIERS_FRAMEWORK}"
)
add_dependencies(MacTools MacToolsEngine)
add_custom_target(MacToolsPackage ALL
    # Sign the standalone helper first.  The installer copies this exact
    # identity out of the parent bundle so TCC permissions survive rebuilds.
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
            --identifier io.griplabs.macsound.engine
            --requirements "${CMAKE_CURRENT_SOURCE_DIR}/Resources/MacToolsEngine.requirements"
            "$<TARGET_BUNDLE_DIR:MacToolsEngine>"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_DIR:MacTools>/Contents/Helpers"
    COMMAND "${CMAKE_COMMAND}" -E rm -f
            "$<TARGET_BUNDLE_DIR:MacTools>/Contents/Helpers/MacToolsEngine"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "$<TARGET_BUNDLE_DIR:MacTools>/Contents/Helpers/MacToolsEngine.app"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "$<TARGET_BUNDLE_DIR:MacToolsEngine>"
            "$<TARGET_BUNDLE_DIR:MacTools>/Contents/Helpers/MacToolsEngine.app"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "$<TARGET_BUNDLE_DIR:MacTools>/Contents/Resources/MacToolsMic.driver"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "$<TARGET_BUNDLE_DIR:MacToolsMic>"
            "$<TARGET_BUNDLE_DIR:MacTools>/Contents/Resources/MacToolsMic.driver"
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
            "$<TARGET_BUNDLE_DIR:MacTools>/Contents/Resources/MacToolsMic.driver"
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
            --identifier io.griplabs.macsound.engine
            --requirements "${CMAKE_CURRENT_SOURCE_DIR}/Resources/MacToolsEngine.requirements"
            "$<TARGET_BUNDLE_DIR:MacTools>/Contents/Helpers/MacToolsEngine.app"
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
            --identifier io.griplabs.macsound
            --requirements "${CMAKE_CURRENT_SOURCE_DIR}/Resources/MacTools.requirements"
            "$<TARGET_BUNDLE_DIR:MacTools>"
    DEPENDS MacTools MacToolsEngine MacToolsMic
)

add_test(NAME MacToolsMicHealthUITests COMMAND MacToolsMicHealthUITests)
