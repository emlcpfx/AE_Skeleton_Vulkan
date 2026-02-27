cmake_minimum_required(VERSION 3.20)

# macOS: build universal binary (Intel + Apple Silicon)
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "Build architectures for macOS")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS deployment target" FORCE)
endif()

project(SkeletonVulkan VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static linking for deployment (Windows)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

message(STATUS "===========================================")
message(STATUS "Skeleton Vulkan v1.0 - AE Plugin Template")
message(STATUS "===========================================")

# ===========================================
# After Effects SDK
# ===========================================
# Set AESDK_ROOT to your After Effects SDK path, or place it adjacent to this project
if(NOT DEFINED AESDK_ROOT)
    # Check common locations
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/AfterEffectsSDK")
        set(AESDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/AfterEffectsSDK" CACHE PATH "Path to After Effects SDK")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../AfterEffectsSDK")
        set(AESDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../AfterEffectsSDK" CACHE PATH "Path to After Effects SDK")
    else()
        # Search parent directories for common SDK folder names
        file(GLOB _aesdk_candidates
            "${CMAKE_CURRENT_SOURCE_DIR}/../*AfterEffectsSDK*"
            "${CMAKE_CURRENT_SOURCE_DIR}/../*/AfterEffectsSDK"
        )
        if(_aesdk_candidates)
            list(GET _aesdk_candidates 0 _aesdk_found)
            set(AESDK_ROOT "${_aesdk_found}" CACHE PATH "Path to After Effects SDK")
        else()
            set(AESDK_ROOT "" CACHE PATH "Path to After Effects SDK")
        endif()
    endif()
endif()

if(NOT EXISTS "${AESDK_ROOT}")
    message(FATAL_ERROR
        "After Effects SDK not found at: ${AESDK_ROOT}\n"
        "Set AESDK_ROOT to the SDK path:\n"
        "  cmake -DAESDK_ROOT=/path/to/AfterEffectsSDK ..")
endif()
message(STATUS "AE SDK: ${AESDK_ROOT}")

# ===========================================
# Vulkan SDK (optional - graceful fallback to CPU)
# ===========================================
if(NOT DEFINED ENV{VULKAN_SDK})
    if(APPLE)
        file(GLOB _vulkan_sdk_candidates "$ENV{HOME}/VulkanSDK/*/macOS")
    elseif(WIN32)
        file(GLOB _vulkan_sdk_candidates "C:/VulkanSDK/*")
    endif()
    if(_vulkan_sdk_candidates)
        list(SORT _vulkan_sdk_candidates)
        list(GET _vulkan_sdk_candidates -1 _vulkan_sdk_latest)
        set(ENV{VULKAN_SDK} "${_vulkan_sdk_latest}")
        message(STATUS "Auto-detected Vulkan SDK: ${_vulkan_sdk_latest}")
    endif()
endif()

find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    set(VULKAN_FOUND TRUE)
    message(STATUS "Vulkan SDK: ${Vulkan_LIBRARY}")
else()
    set(VULKAN_FOUND FALSE)
    message(STATUS "Vulkan SDK: NOT FOUND (CPU-only mode)")
endif()

# ===========================================
# Source files
# ===========================================
set(PLUGIN_SOURCES
    src/SkeletonVulkan.cpp
    src/SkeletonVulkan_Strings.cpp
    ${AESDK_ROOT}/Examples/Util/AEGP_SuiteHandler.cpp
    ${AESDK_ROOT}/Examples/Util/MissingSuiteError.cpp
    ${AESDK_ROOT}/Examples/Util/Smart_Utils.cpp
)

# Windows resource files (PiPL)
if(WIN32)
    list(APPEND PLUGIN_SOURCES
        resources/SkeletonVulkanPiPL.rc
    )
endif()

# Vulkan renderer sources
if(VULKAN_FOUND)
    list(APPEND PLUGIN_SOURCES
        src/gpu/VulkanRenderer.cpp
    )
    message(STATUS "Vulkan GPU rendering: ENABLED")
else()
    message(STATUS "Vulkan GPU rendering: DISABLED (CPU fallback only)")
endif()

# ===========================================
# Build plugin as shared library (.aex / .plugin)
# ===========================================
add_library(${PROJECT_NAME} MODULE ${PLUGIN_SOURCES})

# Platform-specific output
if(WIN32)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        OUTPUT_NAME "SkeletonVulkan"
        SUFFIX ".aex"
        PREFIX ""
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    )
elseif(APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "plugin"
        OUTPUT_NAME "SkeletonVulkan"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/Info.plist"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/Release"
    )
endif()

# ===========================================
# Include directories
# ===========================================
target_include_directories(${PROJECT_NAME} PRIVATE
    ${AESDK_ROOT}/Examples/Headers
    ${AESDK_ROOT}/Examples/Headers/SP
    ${AESDK_ROOT}/Examples/Util
    headers
    src
    src/gpu
)

# ===========================================
# Compiler definitions
# ===========================================
target_compile_definitions(${PROJECT_NAME} PRIVATE
    PF_DEEP_COLOR_AWARE=1
)

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        WIN32_LEAN_AND_MEAN
        MSWindows=1
        UNICODE
        _UNICODE
    )
elseif(APPLE)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        AE_OS_MAC=1
        __MACH__
    )
endif()

# ===========================================
# Platform libraries
# ===========================================
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        kernel32.lib
        user32.lib
        gdi32.lib
        advapi32.lib
    )
elseif(APPLE)
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -O3 -ffast-math)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "-framework CoreFoundation"
        "-framework ApplicationServices"
    )
endif()

# ===========================================
# Vulkan linking
# ===========================================
if(VULKAN_FOUND)
    target_include_directories(${PROJECT_NAME} PRIVATE ${Vulkan_INCLUDE_DIRS})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_VULKAN=1)

    if(APPLE)
        # Link Vulkan dynamically and embed in bundle
        target_link_libraries(${PROJECT_NAME} PRIVATE ${Vulkan_LIBRARY})
        set_target_properties(${PROJECT_NAME} PROPERTIES
            BUILD_RPATH "@loader_path"
            INSTALL_RPATH "@loader_path"
        )
        # Copy libvulkan + MoltenVK into the plugin bundle
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "$ENV{VULKAN_SDK}/lib/libvulkan.1.dylib"
                "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/MacOS/libvulkan.1.dylib"
            COMMAND install_name_tool -id "@loader_path/libvulkan.1.dylib"
                "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/MacOS/libvulkan.1.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy
                "$ENV{VULKAN_SDK}/lib/libMoltenVK.dylib"
                "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/MacOS/libMoltenVK.dylib"
            COMMAND codesign --force --sign -
                "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/MacOS/libvulkan.1.dylib"
            COMMAND codesign --force --sign -
                "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/MacOS/libMoltenVK.dylib"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Resources/vulkan/icd.d"
            COMMENT "Embedding Vulkan loader and MoltenVK in plugin bundle"
        )
        file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/MoltenVK_icd.json"
            CONTENT "{\"file_format_version\":\"1.0.0\",\"ICD\":{\"library_path\":\"../../MacOS/libMoltenVK.dylib\",\"api_version\":\"1.2.0\"}}"
        )
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_CURRENT_BINARY_DIR}/MoltenVK_icd.json"
                "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json"
        )
    else()
        # Windows: delay-load vulkan-1.dll so plugin loads even without Vulkan
        target_link_libraries(${PROJECT_NAME} PRIVATE ${Vulkan_LIBRARY} delayimp.lib)
        target_link_options(${PROJECT_NAME} PRIVATE "/DELAYLOAD:vulkan-1.dll")
        message(STATUS "Vulkan delay-loaded: ${Vulkan_LIBRARY}")
    endif()
endif()

# ===========================================
# PiPL resource compilation (Windows)
# ===========================================
# On Windows, the PiPL .r file must be preprocessed to .rc
# This is handled by a custom build step in the .rc file
# The .rc file should already be a pre-processed version
# See resources/SkeletonVulkanPiPL.rc

# ===========================================
# PiPL resource compilation (macOS)
# ===========================================
if(APPLE)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/SkeletonVulkanPiPL.rsrc
        COMMAND /usr/bin/Rez -o "${CMAKE_CURRENT_BINARY_DIR}/SkeletonVulkanPiPL.rsrc"
                -d SystemSevenOrLater=1 -useDF -script Roman -d __MACH__
                -d AE_OS_MAC=1
                -arch arm64 -arch x86_64
                -i "${AESDK_ROOT}/Examples/Headers"
                -i "${AESDK_ROOT}/Examples/Util"
                -i "${AESDK_ROOT}/Examples/Headers/SP"
                -i "${AESDK_ROOT}/Examples/Resources"
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/SkeletonVulkanPiPL.r"
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resources/SkeletonVulkanPiPL.r
        COMMENT "Compiling PiPL resource (macOS)"
    )
    target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/SkeletonVulkanPiPL.rsrc)

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy
            "${CMAKE_CURRENT_BINARY_DIR}/SkeletonVulkanPiPL.rsrc"
            "$<TARGET_BUNDLE_DIR:${PROJECT_NAME}>/Contents/Resources/SkeletonVulkanPiPL.rsrc"
        COMMENT "Copying PiPL resource into plugin bundle"
    )
endif()

# ===========================================
# Summary
# ===========================================
message(STATUS "-------------------------------------------")
if(WIN32)
    message(STATUS "Output: ${CMAKE_BINARY_DIR}/Release/SkeletonVulkan.aex")
elseif(APPLE)
    message(STATUS "Output: ${CMAKE_BINARY_DIR}/Release/SkeletonVulkan.plugin")
endif()
message(STATUS "===========================================")
