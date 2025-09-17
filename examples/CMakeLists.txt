# Examples CMakeLists
cmake_minimum_required(VERSION 3.26)

set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_BIN_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BIN_DIR})

find_program(GLSLC glslc HINTS ENV VULKAN_SDK PATH_SUFFIXES Bin bin)
if (NOT GLSLC)
    message(WARNING "glslc not found. Will still build example but SPIR-V shaders must exist manually in ${SHADER_BIN_DIR}.")
endif ()

set(SHADERS triangle.vert triangle.frag)
set(SPV_FILES)
foreach (SH ${SHADERS})
    set(SRC ${SHADER_SRC_DIR}/${SH})
    set(SPV ${SHADER_BIN_DIR}/${SH}.spv)
    if (GLSLC)
        add_custom_command(
                OUTPUT ${SPV}
                COMMAND ${GLSLC} -O -c ${SRC} -o ${SPV}
                DEPENDS ${SRC}
                COMMENT "[glslc] Compiling ${SH} -> ${SPV}"
                VERBATIM
        )
        list(APPEND SPV_FILES ${SPV})
    endif ()
endforeach ()

if (SPV_FILES)
    add_custom_target(example_shaders DEPENDS ${SPV_FILES})
endif ()

add_executable(basic_window basic_window.cpp)

if (TARGET example_shaders)
    add_dependencies(basic_window example_shaders)
endif ()

# Provide shader directory to code
set(EXAMPLE_SHADER_OUTPUT_DIR ${SHADER_BIN_DIR})
set(EXAMPLE_SHADER_SOURCE_DIR ${SHADER_SRC_DIR})

target_compile_definitions(basic_window PRIVATE
        SHADER_OUTPUT_DIR="${EXAMPLE_SHADER_OUTPUT_DIR}"
        SHADER_SOURCE_DIR="${EXAMPLE_SHADER_SOURCE_DIR}"
)

# Link against engine library
target_link_libraries(basic_window PRIVATE vulkan_visualizer)

# Copy SPIR-V to runtime folder after build for convenience
add_custom_command(TARGET basic_window POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:basic_window>/shaders
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${SHADER_BIN_DIR} $<TARGET_FILE_DIR:basic_window>/shaders
        COMMENT "Copying compiled shaders to runtime directory"
)

