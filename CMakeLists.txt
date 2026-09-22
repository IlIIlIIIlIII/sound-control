cmake_minimum_required(VERSION 3.26)
project(PersonalTools VERSION 0.6.5 LANGUAGES C CXX)

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
if(MINGW)
    # Portable binaries, including tests, must run without a compiler DLL PATH.
    add_link_options(-static)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(PersonalToolsCore STATIC
    Sources/Core/AudioStream.cpp
    Sources/Core/AEC.cpp
    Sources/Core/MicHealth.cpp
    Sources/Core/REWParser.cpp
    Sources/Core/DSP.cpp
    Sources/Display/DisplayTools.cpp
)
target_include_directories(PersonalToolsCore PUBLIC Sources/Core Sources/Display)
if(APPLE)
    find_library(ACCELERATE_FRAMEWORK Accelerate REQUIRED)
    target_link_libraries(PersonalToolsCore PUBLIC "${ACCELERATE_FRAMEWORK}")
endif()
if(MSVC)
    target_compile_options(PersonalToolsCore PRIVATE /W4 /permissive-)
else()
    target_compile_options(PersonalToolsCore PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(PersonalToolsTests Tests/CoreTests.cpp)
if(APPLE)
    target_sources(PersonalToolsTests PRIVATE Sources/Mic/MicShared.c)
endif()
target_include_directories(PersonalToolsTests PRIVATE Sources/Mic Sources/Display)
target_link_libraries(PersonalToolsTests PRIVATE PersonalToolsCore)

add_executable(PersonalToolsValidate Tools/Validate.cpp)
target_link_libraries(PersonalToolsValidate PRIVATE PersonalToolsCore)

add_executable(PersonalToolsMicHealthTests Tests/MicHealthTests.cpp)
target_link_libraries(PersonalToolsMicHealthTests PRIVATE PersonalToolsCore)

add_executable(PersonalToolsBenchmark Tools/Benchmark.cpp)
target_link_libraries(PersonalToolsBenchmark PRIVATE PersonalToolsCore)

add_executable(PersonalToolsAECBenchmark Tools/AECBenchmark.cpp)
target_link_libraries(PersonalToolsAECBenchmark PRIVATE PersonalToolsCore)

find_package(Threads REQUIRED)
add_executable(PersonalToolsStreamTests Tests/AudioStreamTests.cpp)
target_link_libraries(PersonalToolsStreamTests PRIVATE PersonalToolsCore Threads::Threads)

enable_testing()
add_test(NAME PersonalToolsStreamTests COMMAND PersonalToolsStreamTests)
add_test(NAME PersonalToolsCoreTests COMMAND PersonalToolsTests)
add_test(NAME PersonalToolsMicHealthTests COMMAND PersonalToolsMicHealthTests)

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

add_executable(PersonalToolsMicHealthUITests Tests/MicHealthUITests.mm Sources/Mic/MicHealthUI.mm)
target_include_directories(PersonalToolsMicHealthUITests PRIVATE Sources/Mic)
target_compile_options(PersonalToolsMicHealthUITests PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(PersonalToolsMicHealthUITests PRIVATE "${FOUNDATION_FRAMEWORK}")

add_executable(PersonalToolsAudioProbe Tools/AudioProbe.mm)
target_compile_options(PersonalToolsAudioProbe PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(PersonalToolsAudioProbe PRIVATE
    "${COREAUDIO_FRAMEWORK}"
    "${FOUNDATION_FRAMEWORK}"
)

set_source_files_properties(Resources/32RTX950.edid.base64 PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
)
set_source_files_properties(Resources/PersonalTools.icns PROPERTIES
    MACOSX_PACKAGE_LOCATION Resources
)
add_executable(PersonalToolsEngine MACOSX_BUNDLE
    Sources/Engine/main.mm
    Sources/Mic/MicShared.c
    Sources/Mic/MicHealthUI.mm
    Sources/Display/DisplayController.mm
    Resources/32RTX950.edid.base64
)
set_target_properties(PersonalToolsEngine PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/EngineInfo.plist"
)
target_include_directories(PersonalToolsEngine PRIVATE Sources/Core Sources/Mic Sources/Display)
target_compile_options(PersonalToolsEngine PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(PersonalToolsEngine PRIVATE
    PersonalToolsCore
    "${APPKIT_FRAMEWORK}"
    "${AVFOUNDATION_FRAMEWORK}"
    "${CARBON_FRAMEWORK}"
    "${COREAUDIO_FRAMEWORK}"
    "${COREGRAPHICS_FRAMEWORK}"
    "${FOUNDATION_FRAMEWORK}"
    "${IOKIT_FRAMEWORK}"
)

add_library(PersonalToolsMic MODULE
    Sources/Driver/PersonalToolsMic.c
    Sources/Mic/MicShared.c
)
set_target_properties(PersonalToolsMic PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION driver
    PREFIX ""
    OUTPUT_NAME "PersonalToolsMic"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/MicDriverInfo.plist.in"
)
target_include_directories(PersonalToolsMic PRIVATE
    Sources/Mic
    "${APPLE_NULL_AUDIO_SOURCE_DIR}"
)
target_compile_options(PersonalToolsMic PRIVATE -fblocks -Wno-deprecated-declarations)
target_link_libraries(PersonalToolsMic PRIVATE
    "${COREAUDIO_FRAMEWORK}"
    "${COREFOUNDATION_FRAMEWORK}"
)

add_executable(PersonalTools MACOSX_BUNDLE Sources/App/main.mm Sources/Mic/MicHealthUI.mm Resources/PersonalTools.icns)
set_target_properties(PersonalTools PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/AppInfo.plist.in"
)
target_include_directories(PersonalTools PRIVATE Sources/Core Sources/Display Sources/Mic)
target_compile_options(PersonalTools PRIVATE -fobjc-arc -Wall -Wextra)
target_link_libraries(PersonalTools PRIVATE
    PersonalToolsCore
    "${APPKIT_FRAMEWORK}"
    "${COREAUDIO_FRAMEWORK}"
    "${FOUNDATION_FRAMEWORK}"
    "${UNIFORMTYPEIDENTIFIERS_FRAMEWORK}"
)
add_dependencies(PersonalTools PersonalToolsEngine)
add_custom_target(PersonalToolsPackage ALL
    # Sign the standalone helper first.  The installer copies this exact
    # identity out of the parent bundle so TCC permissions survive rebuilds.
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
            --identifier io.griplabs.personaltools.engine
            --requirements "${CMAKE_CURRENT_SOURCE_DIR}/Resources/PersonalToolsEngine.requirements"
            "$<TARGET_BUNDLE_DIR:PersonalToolsEngine>"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_DIR:PersonalTools>/Contents/Helpers"
    COMMAND "${CMAKE_COMMAND}" -E rm -f
            "$<TARGET_BUNDLE_DIR:PersonalTools>/Contents/Helpers/PersonalToolsEngine"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "$<TARGET_BUNDLE_DIR:PersonalTools>/Contents/Helpers/PersonalToolsEngine.app"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "$<TARGET_BUNDLE_DIR:PersonalToolsEngine>"
            "$<TARGET_BUNDLE_DIR:PersonalTools>/Contents/Helpers/PersonalToolsEngine.app"
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
            "$<TARGET_BUNDLE_DIR:PersonalTools>/Contents/Resources/PersonalToolsMic.driver"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "$<TARGET_BUNDLE_DIR:PersonalToolsMic>"
            "$<TARGET_BUNDLE_DIR:PersonalTools>/Contents/Resources/PersonalToolsMic.driver"
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
            "$<TARGET_BUNDLE_DIR:PersonalTools>/Contents/Resources/PersonalToolsMic.driver"
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
            --identifier io.griplabs.personaltools.engine
            --requirements "${CMAKE_CURRENT_SOURCE_DIR}/Resources/PersonalToolsEngine.requirements"
            "$<TARGET_BUNDLE_DIR:PersonalTools>/Contents/Helpers/PersonalToolsEngine.app"
    COMMAND /usr/bin/codesign --force --sign - --timestamp=none
            --identifier io.griplabs.personaltools
            --requirements "${CMAKE_CURRENT_SOURCE_DIR}/Resources/PersonalTools.requirements"
            "$<TARGET_BUNDLE_DIR:PersonalTools>"
    DEPENDS PersonalTools PersonalToolsEngine PersonalToolsMic
)

add_test(NAME PersonalToolsMicHealthUITests COMMAND PersonalToolsMicHealthUITests)
