# Examples CMakeLists
cmake_minimum_required(VERSION 3.26)

set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_BIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BIN_DIR})

find_program(GLSLC glslc HINTS ENV VULKAN_SDK PATH_SUFFIXES Bin bin)
if (NOT GLSLC)
    message(WARNING "glslc not found. Will still build examples but SPIR-V must exist in ${SHADER_BIN_DIR}.")
endif ()

set(SHADERS
        triangle.vert
        triangle.frag
        pc_triangle.vert
        pc_triangle.frag
        comp_noise.comp
)
set(SPV_FILES)
foreach (SH ${SHADERS})
    set(SRC ${SHADER_SRC_DIR}/${SH})
    set(SPV ${SHADER_BIN_DIR}/${SH}.spv)
    if (GLSLC)
        add_custom_command(OUTPUT ${SPV}
                COMMAND ${GLSLC} -O -c ${SRC} -o ${SPV}
                DEPENDS ${SRC}
                COMMENT "[glslc] ${SH} -> ${SPV}"
                VERBATIM)
        list(APPEND SPV_FILES ${SPV})
    endif ()
endforeach ()
if (SPV_FILES)
    add_custom_target(example_shaders DEPENDS ${SPV_FILES})
endif ()

set(EXAMPLE_SHADER_OUTPUT_DIR ${SHADER_BIN_DIR})
set(EXAMPLE_SHADER_SOURCE_DIR ${SHADER_SRC_DIR})

function(add_vv_example name src)
    add_executable(${name} ${src})
    if (TARGET example_shaders)
        add_dependencies(${name} example_shaders)
    endif ()
    target_compile_definitions(${name} PRIVATE SHADER_OUTPUT_DIR="${EXAMPLE_SHADER_OUTPUT_DIR}" SHADER_SOURCE_DIR="${EXAMPLE_SHADER_SOURCE_DIR}")
    target_link_libraries(${name} PRIVATE vulkan_visualizer)
    if (WIN32)
        add_custom_command(TARGET ${name} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${name}> $<TARGET_FILE_DIR:${name}>
                COMMAND_EXPAND_LISTS)
    endif ()
endfunction()

# ex00..ex08
add_vv_example(ex00_basic_window ex00_basic_window.cpp)
add_vv_example(ex01_imgui_minimal_ui ex01_imgui_minimal_ui.cpp)
add_vv_example(ex02_shader_hot_reload ex02_shader_hot_reload.cpp)
add_vv_example(ex03_hello_primitives ex03_hello_primitives.cpp)
add_vv_example(ex04_uniforms_push_constants ex04_uniforms_push_constants.cpp)
add_vv_example(ex05_compute_to_image ex05_compute_to_image.cpp)
add_vv_example(ex06_presentation_modes_showcase ex06_presentation_modes_showcase.cpp)
add_vv_example(ex07_direct_to_swapchain ex07_direct_to_swapchain.cpp)
add_vv_example(ex08_screenshot_workflow ex08_screenshot_workflow.cpp)
