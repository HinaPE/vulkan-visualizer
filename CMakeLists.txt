cmake_minimum_required(VERSION 3.26)

# =========================================================
# Project metadata
# =========================================================
project(
        VulkanVisualizer
        VERSION 0.3.0
        LANGUAGES CXX
)

# =========================================================
# Options
# =========================================================
option(VULKAN_VISUALIZER_BUILD_EXAMPLE "Build the Vulkan visualizer example executable" OFF)

# =========================================================
# C++ standard & general compilation behavior
# =========================================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Position Independent Code (required for shared libs on several platforms)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# =========================================================
# CMake helper modules (install dirs, config helpers, external setup scripts)
# Each setup_*.cmake script is expected to define a use_<pkg>(target) function.
# =========================================================
include(cmake/setup_imgui.cmake)
include(cmake/setup_sdl3.cmake)
include(cmake/setup_vkbootstrap.cmake)
include(cmake/setup_vma.cmake)

# =========================================================
# Required external packages
# =========================================================
find_package(Vulkan REQUIRED)

# =========================================================
# Library target
# =========================================================
set(libname vulkan_visualizer)

set(${libname}_SOURCES
        src/vk_engine.cpp
)

add_library(${libname} STATIC
        ${${libname}_SOURCES}
)

# =========================================================
# Include directories
# BUILD_INTERFACE: during build
# INSTALL_INTERFACE: after install
# =========================================================
target_include_directories(${libname} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# =========================================================
# Link dependencies
# External helper macros attach their own include/link settings
# =========================================================
target_link_libraries(${libname} PUBLIC Vulkan::Vulkan)
use_imgui(${libname})
use_sdl3(${libname})
use_vkbootstrap(${libname})
use_vma(${libname})

# Enable ImGui backends (provided by setup_imgui.cmake)
imgui_enable_backends(${libname} BACKENDS sdl3 vulkan)

# =========================================================
# Compile features and optimization flags
# =========================================================
target_compile_features(${libname} PRIVATE cxx_std_23)

set(MSVC_RELEASE_OPTIONS
        /O2 /Ot /Oi /GL /fp:fast /arch:AVX2
)

set(GCC_CLANG_RELEASE_OPTIONS_BASE -O3 -ffast-math -fomit-frame-pointer)
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|X86_64|AMD64")
    list(APPEND GCC_CLANG_RELEASE_OPTIONS_BASE -march=native -mavx2 -mfma)
endif ()
set(GCC_CLANG_RELEASE_OPTIONS ${GCC_CLANG_RELEASE_OPTIONS_BASE})

target_compile_options(${libname} PRIVATE
        $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:MSVC>>:${MSVC_RELEASE_OPTIONS}>
        $<$<AND:$<CONFIG:Release>,$<NOT:$<CXX_COMPILER_ID:MSVC>>>:${GCC_CLANG_RELEASE_OPTIONS}>
)

# =========================================================
# Optional examples
# =========================================================
if (VULKAN_VISUALIZER_BUILD_EXAMPLE)
    add_subdirectory(examples)
endif ()
