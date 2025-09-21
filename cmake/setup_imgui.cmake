# ============================================================================
# setup_imgui.cmake (modern)
# Fetch Dear ImGui (docking) via FetchContent and define imgui target/backends.
# Exposes:
#   - use_imgui(<target>)
#   - imgui_enable_backends(<target> BACKENDS sdl3;vulkan)
# ============================================================================

if(DEFINED _SETUP_IMGUI_INCLUDED)
    return()
endif()
set(_SETUP_IMGUI_INCLUDED TRUE)

include(FetchContent)

set(IMGUI_VERSION "v1.92.2b-docking" CACHE STRING "Dear ImGui docking tag/commit")

# Fetch imgui sources
FetchContent_Declare(
    imgui_src
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        ${IMGUI_VERSION}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui_src)

# imgui source dir
set(IMGUI_SOURCE_DIR ${imgui_src_SOURCE_DIR})
set(IMGUI_INCLUDE_DIR ${IMGUI_SOURCE_DIR})

# Core sources
set(_IMGUI_CORE_SOURCES
    ${IMGUI_SOURCE_DIR}/imgui.cpp
    ${IMGUI_SOURCE_DIR}/imgui_demo.cpp
    ${IMGUI_SOURCE_DIR}/imgui_draw.cpp
    ${IMGUI_SOURCE_DIR}/imgui_tables.cpp
    ${IMGUI_SOURCE_DIR}/imgui_widgets.cpp
)

function(_define_imgui_target)
    if(TARGET imgui::imgui)
        return()
    endif()
    add_library(imgui STATIC ${_IMGUI_CORE_SOURCES})
    add_library(imgui::imgui ALIAS imgui)
    target_include_directories(imgui
        PUBLIC
            ${IMGUI_SOURCE_DIR}
            ${IMGUI_SOURCE_DIR}/backends
    )
    target_compile_features(imgui PUBLIC cxx_std_17)
    set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

function(use_imgui TARGET_NAME)
    if(NOT TARGET ${TARGET_NAME})
        message(FATAL_ERROR "use_imgui called with unknown target `${TARGET_NAME}`")
    endif()
    _define_imgui_target()
    target_link_libraries(${TARGET_NAME} PUBLIC imgui::imgui)
endfunction()

function(imgui_enable_backends TARGET_NAME)
    if(NOT TARGET ${TARGET_NAME})
        message(FATAL_ERROR "imgui_enable_backends called with unknown target `${TARGET_NAME}`")
    endif()
    if(NOT IMGUI_SOURCE_DIR)
        message(FATAL_ERROR "IMGUI_SOURCE_DIR is not defined. Include setup_imgui.cmake before enabling backends.")
    endif()

    cmake_parse_arguments(_IMGUI_BACKEND "" "" "BACKENDS" ${ARGN})
    if(NOT _IMGUI_BACKEND_BACKENDS)
        message(FATAL_ERROR "imgui_enable_backends requires BACKENDS to be specified")
    endif()

    _define_imgui_target()

    foreach(_backend IN LISTS _IMGUI_BACKEND_BACKENDS)
        string(TOLOWER "${_backend}" _backend_lower)
        if(_backend_lower STREQUAL "vulkan")
            target_sources(${TARGET_NAME} PRIVATE "${IMGUI_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
            if(NOT TARGET Vulkan::Vulkan)
                find_package(Vulkan REQUIRED)
            endif()
            target_link_libraries(${TARGET_NAME} PRIVATE Vulkan::Vulkan)
        elseif(_backend_lower STREQUAL "sdl3")
            target_sources(${TARGET_NAME} PRIVATE "${IMGUI_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp")
            if(COMMAND use_sdl3)
                use_sdl3(${TARGET_NAME})
            elseif(TARGET SDL3::SDL3)
                target_link_libraries(${TARGET_NAME} PRIVATE SDL3::SDL3)
            else()
                message(FATAL_ERROR "SDL3 backend requested but SDL3 is not configured. Include setup_sdl3.cmake and/or create SDL3::SDL3 target.")
            endif()
        else()
            message(FATAL_ERROR "Unknown Dear ImGui backend '${_backend}' requested")
        endif()
    endforeach()
endfunction()
