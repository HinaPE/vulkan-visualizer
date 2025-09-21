# ============================================================================
# setup_vma.cmake (modern)
# Fetch VMA via FetchContent (URL) and expose VMA::VMA header-only target.
# Exposes: use_vma(<target>)
# ============================================================================

if(DEFINED _SETUP_VMA_INCLUDED)
    return()
endif()
set(_SETUP_VMA_INCLUDED TRUE)

include(FetchContent)

set(VMA_VERSION "3.3.0" CACHE STRING "VMA version")
set(VMA_URL "https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v${VMA_VERSION}.tar.gz" CACHE STRING "VMA source URL")

FetchContent_Declare(
    VMA_src
    URL         ${VMA_URL}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(VMA_src)

# Determine source dir robustly
set(_VMA_SRC_DIR "${FETCHCONTENT_SOURCE_DIR_VMA_SRC}")
if(NOT _VMA_SRC_DIR)
    if(DEFINED VMA_src_SOURCE_DIR)
        set(_VMA_SRC_DIR "${VMA_src_SOURCE_DIR}")
    elseif(DEFINED VMA_SRC_SOURCE_DIR)
        set(_VMA_SRC_DIR "${VMA_SRC_SOURCE_DIR}")
    endif()
endif()

# Fallback guesses if still empty
if(NOT _VMA_SRC_DIR)
    set(_cand1 "${CMAKE_BINARY_DIR}/_deps/vma_src-src")
    set(_cand2 "${CMAKE_BINARY_DIR}/_deps/VulkanMemoryAllocator-${VMA_VERSION}")
    if(EXISTS "${_cand1}/include/vk_mem_alloc.h")
        set(_VMA_SRC_DIR "${_cand1}")
    elseif(EXISTS "${_cand2}/include/vk_mem_alloc.h")
        set(_VMA_SRC_DIR "${_cand2}")
    endif()
endif()

if(NOT _VMA_SRC_DIR)
    message(WARNING "Could not determine VMA source directory; falling back to single-header download")
endif()

# Compose include dir and validate; if missing, fallback to single-header download
set(VMA_INCLUDE_DIR "${_VMA_SRC_DIR}/include" CACHE PATH "Path to VMA headers" FORCE)
if(NOT _VMA_SRC_DIR OR NOT EXISTS "${VMA_INCLUDE_DIR}/vk_mem_alloc.h")
    set(_vma_single_dir "${CMAKE_BINARY_DIR}/_deps/vma_single/include")
    file(MAKE_DIRECTORY "${_vma_single_dir}")
    set(_vma_hdr_url "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v${VMA_VERSION}/include/vk_mem_alloc.h")
    message(STATUS "Downloading VMA single header from ${_vma_hdr_url}")
    file(DOWNLOAD "${_vma_hdr_url}" "${_vma_single_dir}/vk_mem_alloc.h" SHOW_PROGRESS STATUS _vma_dl_status)
    list(GET _vma_dl_status 0 _st)
    if(NOT _st EQUAL 0)
        list(GET _vma_dl_status 1 _msg)
        message(FATAL_ERROR "Failed to download VMA header: ${_msg}")
    endif()
    set(VMA_INCLUDE_DIR "${_vma_single_dir}" CACHE PATH "Path to VMA headers" FORCE)
endif()

if(NOT TARGET VMA::VMA)
    add_library(VMA::VMA INTERFACE IMPORTED)
    set_target_properties(VMA::VMA PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${VMA_INCLUDE_DIR}"
    )
endif()

function(use_vma TARGET_NAME)
    if(NOT TARGET ${TARGET_NAME})
        message(FATAL_ERROR "use_vma called with unknown target `${TARGET_NAME}`")
    endif()
    target_link_libraries(${TARGET_NAME} PUBLIC VMA::VMA)
    target_include_directories(${TARGET_NAME} PUBLIC "${VMA_INCLUDE_DIR}")
endfunction()
