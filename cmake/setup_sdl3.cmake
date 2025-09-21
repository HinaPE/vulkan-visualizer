# ============================================================================
# setup_sdl3.cmake (modern)
# Fetch SDL3 via FetchContent and expose SDL3::SDL3 target.
# Exposes: use_sdl3(<target>)
# ============================================================================

if(DEFINED _SETUP_SDL3_INCLUDED)
    return()
endif()
set(_SETUP_SDL3_INCLUDED TRUE)

include(FetchContent)

set(SDL3_GIT_TAG "release-3.2.2" CACHE STRING "SDL3 git tag/branch to fetch")

# Common SDL options
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        ${SDL3_GIT_TAG}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(SDL3)

# SDL3 provides SDL3::SDL3 imported/alias target
function(use_sdl3 TARGET_NAME)
    if(NOT TARGET ${TARGET_NAME})
        message(FATAL_ERROR "use_sdl3 called with unknown target `${TARGET_NAME}`")
    endif()
    if(NOT TARGET SDL3::SDL3)
        message(FATAL_ERROR "SDL3::SDL3 target not found. setup_sdl3.cmake must be included before use.")
    endif()
    target_link_libraries(${TARGET_NAME} PUBLIC SDL3::SDL3)

    if(WIN32)
        # Auto-copy runtime DLLs for executable/shared targets
        get_target_property(_t ${TARGET_NAME} TYPE)
        if(_t STREQUAL "EXECUTABLE" OR _t STREQUAL "SHARED_LIBRARY" OR _t STREQUAL "MODULE_LIBRARY")
            get_property(_sdl3_copied TARGET ${TARGET_NAME} PROPERTY _SDL3_RUNTIME_COPIED SET)
            if(NOT _sdl3_copied)
                add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_RUNTIME_DLLS:${TARGET_NAME}>
                        $<TARGET_FILE_DIR:${TARGET_NAME}>
                    COMMAND_EXPAND_LISTS
                )
                set_property(TARGET ${TARGET_NAME} PROPERTY _SDL3_RUNTIME_COPIED TRUE)
            endif()
        endif()
    endif()
endfunction()
