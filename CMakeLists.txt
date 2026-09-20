cmake_minimum_required(VERSION 3.26)
project(SoundControl VERSION 0.6.5 LANGUAGES C CXX)

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

add_library(SoundControlCore STATIC
    Sources/Core/AudioStream.cpp
    Sources/Core/AEC.cpp
    Sources/Core/MicHealth.cpp
    Sources/Core/REWParser.cpp
    Sources/Core/DSP.cpp
    Sources/Display/DisplayTools.cpp
)
target_include_directories(SoundControlCore PUBLIC Sources/Core Sources/Display)
if(APPLE)
    find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
    target_link_libraries(SoundControlCore PUBLIC "${ACCELERATE_FRAMEWORK}")
endif()
if(MSVC)
    target_compile_options(SoundControlCore PRIVATE /W4 /permissive-)
else()
    target_compile_options(SoundControlCore PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(SoundControlTests Tests/CoreTests.cpp)
if(APPLE)
    target_sources(SoundControlTests PRIVATE Sources/Mic/MicShared.c)
endif()
target_include_directories(SoundControlTests PRIVATE Sources/Mic Sources/Display)
target_link_libraries(SoundControlTests PRIVATE SoundControlCore)

add_executable(SoundControlValidate Tools/Validate.cpp)
target_link_libraries(SoundControlValidate PRIVATE SoundControlCore)

add_executable(SoundControlMicHealthTests Tests/MicHealthTests.cpp)
target_link_libraries(SoundControlMicHealthTests PRIVATE SoundControlCore)

add_executable(SoundControlBenchmark Tools/Benchmark.cpp)
target_link_libraries(SoundControlBenchmark PRIVATE SoundControlCore)

add_executable(SoundControlAECBenchmark Tools/AECBenchmark.cpp)
target_link_libraries(SoundControlAECBenchmark PRIVATE SoundControlCore)

find_package(Threads REQUIRED)
add_executable(SoundControlStreamTests Tests/AudioStreamTests.cpp)
target_link_libraries(SoundControlStreamTests PRIVATE SoundControlCore Threads::Threads)

enable_testing()
add_test(NAME SoundControlStreamTests COMMAND SoundControlStreamTests)
add_test(NAME SoundControlCoreTests COMMAND SoundControlTests)
add_test(NAME SoundControlMicHealthTests COMMAND SoundControlMicHealthTests)

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

add_executable(SoundControlMicHealthUITests Tests/MicHealthUITests.mm Sources/Mic/MicHealthUI.mm)
target_include_directories(SoundControlMicHealthUITests PRIVATE Sources/Mic)
target_compile_options(SoundControlMicHealthUITests PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(SoundControlMicHealthUITests PRIVATE "${FOUNDATION_FRAMEWORK}")

add_executable(SoundControlAudioProbe Tools/AudioProbe.mm)
target_compile_options(SoundControlAudioProbe PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(SoundControlAudioProbe PRIVATE
    "${COREAUDIO_FRAMEWORK}"
    "${FOUNDATION_FRAMEWORK}"
)

set_source_files_properties(Resources/32RTX950.edid.base64 PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
)
set_source_files_properties(Resources/SoundControl.icns PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
)
add_executable(SoundControlEngine MACOSX_BUNDLE
    Sources/Engine/main.mm
    Sources/Mic/MicShared.c
    Sources/Mic/MicHealthUI.mm
    Sources/Display/DisplayController.mm
    Resources/32RTX950.edid.base64
)
set_target_properties(SoundControlEngine PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/EngineInfo.plist"
)
target_include_directories(SoundControlEngine PRIVATE Sources/Core Sources/Mic Sources/Display)
target_compile_options(SoundControlEngine PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(SoundControlEngine PRIVATE
    SoundControlCore
    "${APPKIT_FRAMEWORK}"
    "${AVFOUNDATION_FRAMEWORK}"
    "${CARBON_FRAMEWORK}"
    "${COREAUDIO_FRAMEWORK}"
    "${COREGRAPHICS_FRAMEWORK}"
    "${FOUNDATION_FRAMEWORK}"
    "${IOKIT_FRAMEWORK}"
)

add_library(SoundControlMic MODULE
    Sources/Driver/SoundControlMic.c
    Sources/Mic/MicShared.c
)
set_target_properties(SoundControlMic PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION driver
    PREFIX ""
    OUTPUT_NAME "SoundControlMic"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/MicDriverInfo.plist.in"
)
target_include_directories(SoundControlMic PRIVATE
    Sources/Mic
    "${APPLE_NULL_AUDIO_SOURCE_DIR}"
)
target_compile_options(SoundControlMic PRIVATE -fblocks -Wno-deprecated-declarations)
target_link_libraries(SoundControlMic PRIVATE
    "${COREAUDIO_FRAMEWORK}"
    "${COREFOUNDATION_FRAMEWORK}"
)

add_executable(SoundControl MACOSX_BUNDLE Sources/App/main.mm Sources/Mic/MicHealthUI.mm Resources/SoundControl.icns)
set_target_properties(SoundControl PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppInfo.plist.in"
)
target_include_directories(SoundControl PRIVATE Sources/Core Sources/Display Sources/Mic)
target_compile_options(SoundControl PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(SoundControl PRIVATE
    SoundControlCore
    "${APPKIT_FRAMEWORK}"
    "${COREAUDIO_FRAMEWORK}"
    "${FOUNDATION_FRAMEWORK}"
    "${UNIFORMTYPEIDENTIFIERS_FRAMEWORK}"
)
add_dependencies(SoundControl SoundControlEngine)
add_custom_target(SoundControlPackage ALL
    # Sign the standalone helper first.  The installer copies this exact
    # identity out of the parent bundle so TCC permissions survive rebuilds.
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
            --identifier io.griplabs.soundcontrol.engine
            --requirements "${CMAKE_CURRENT_SOURCE_DIR}/Resources/SoundControlEngine.requirements"
            "$<TARGET_BUNDLE_DIR:SoundControlEngine>"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_DIR:SoundControl>/Contents/Helpers"
    COMMAND "${CMAKE_COMMAND}" -E rm -f
            "$<TARGET_BUNDLE_DIR:SoundControl>/Contents/Helpers/SoundControlEngine"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "$<TARGET_BUNDLE_DIR:SoundControl>/Contents/Helpers/SoundControlEngine.app"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "$<TARGET_BUNDLE_DIR:SoundControlEngine>"
            "$<TARGET_BUNDLE_DIR:SoundControl>/Contents/Helpers/SoundControlEngine.app"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "$<TARGET_BUNDLE_DIR:SoundControl>/Contents/Resources/SoundControlMic.driver"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "$<TARGET_BUNDLE_DIR:SoundControlMic>"
            "$<TARGET_BUNDLE_DIR:SoundControl>/Contents/Resources/SoundControlMic.driver"
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
            "$<TARGET_BUNDLE_DIR:SoundControl>/Contents/Resources/SoundControlMic.driver"
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
            --identifier io.griplabs.soundcontrol.engine
            --requirements "${CMAKE_CURRENT_SOURCE_DIR}/Resources/SoundControlEngine.requirements"
            "$<TARGET_BUNDLE_DIR:SoundControl>/Contents/Helpers/SoundControlEngine.app"
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
            --identifier io.griplabs.soundcontrol
            --requirements "${CMAKE_CURRENT_SOURCE_DIR}/Resources/SoundControl.requirements"
            "$<TARGET_BUNDLE_DIR:SoundControl>"
    DEPENDS SoundControl SoundControlEngine SoundControlMic
)

add_test(NAME SoundControlMicHealthUITests COMMAND SoundControlMicHealthUITests)
