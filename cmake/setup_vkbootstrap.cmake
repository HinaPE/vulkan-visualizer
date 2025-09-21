# ============================================================================
# setup_vkbootstrap.cmake (modern)
# Fetch vk-bootstrap via FetchContent and expose vk-bootstrap::vk-bootstrap.
# Exposes: use_vkbootstrap(<target>)
# ============================================================================

if(DEFINED _SETUP_VKBOOTSTRAP_INCLUDED)
    return()
endif()
set(_SETUP_VKBOOTSTRAP_INCLUDED TRUE)

include(FetchContent)

set(VK_BOOTSTRAP_GIT_TAG "v1.4.321" CACHE STRING "vk-bootstrap git tag/commit")

# Silence warnings as errors in vk-bootstrap if any
set(VK_BOOTSTRAP_WERROR OFF CACHE BOOL "" FORCE)
set(VK_BOOTSTRAP_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    vk_bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
    GIT_TAG        ${VK_BOOTSTRAP_GIT_TAG}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(vk_bootstrap)

# Fallback: if upstream target name changes, try to create one
if(NOT TARGET vk-bootstrap::vk-bootstrap)
    if(TARGET vk-bootstrap)
        add_library(vk-bootstrap::vk-bootstrap ALIAS vk-bootstrap)
    elseif(EXISTS "${vk_bootstrap_SOURCE_DIR}/src/VkBootstrap.cpp")
        add_library(vk-bootstrap STATIC "${vk_bootstrap_SOURCE_DIR}/src/VkBootstrap.cpp")
        add_library(vk-bootstrap::vk-bootstrap ALIAS vk-bootstrap)
        target_include_directories(vk-bootstrap PUBLIC
            "${vk_bootstrap_SOURCE_DIR}/src"
            "${vk_bootstrap_SOURCE_DIR}/include"
        )
        if(NOT TARGET Vulkan::Vulkan)
            find_package(Vulkan REQUIRED)
        endif()
        target_link_libraries(vk-bootstrap PUBLIC Vulkan::Vulkan)
        target_compile_features(vk-bootstrap PUBLIC cxx_std_17)
        set_target_properties(vk-bootstrap PROPERTIES POSITION_INDEPENDENT_CODE ON)
    else()
        message(FATAL_ERROR "vk-bootstrap targets not found and source missing.")
    endif()
endif()

function(use_vkbootstrap TARGET_NAME)
    if(NOT TARGET ${TARGET_NAME})
        message(FATAL_ERROR "use_vkbootstrap called with unknown target `${TARGET_NAME}`")
    endif()
    if(NOT TARGET vk-bootstrap::vk-bootstrap)
        message(FATAL_ERROR "vk-bootstrap::vk-bootstrap not available. Include setup_vkbootstrap.cmake earlier.")
    endif()
    target_link_libraries(${TARGET_NAME} PUBLIC vk-bootstrap::vk-bootstrap)
endfunction()
