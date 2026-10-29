# GLSL -> SPIR-V at build time, using glslc (Vulkan SDK) or glslang (vcpkg).
#
#   gdl_add_shaders(<target> OUTPUT_DIR <dir> SOURCES <file.vert> <file.frag> ...
#                   INCLUDES <file.glsl> ...)
#
# Produces <dir>/<file>.spv for every source and makes <target> depend on them. The sources
# and includes are copied beside the binaries as well, for the OpenGL device, which has the
# driver compile the same text at run time.

set(_gdl_shader_hints "$ENV{VULKAN_SDK}/Bin")
foreach(_dir IN ITEMS
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/glslang"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/tools/glslang")
    list(APPEND _gdl_shader_hints "${_dir}")
endforeach()

find_program(GDL_GLSLC_EXECUTABLE NAMES glslc HINTS ${_gdl_shader_hints})
find_program(GDL_GLSLANG_EXECUTABLE NAMES glslang glslangValidator HINTS ${_gdl_shader_hints})

if(GDL_GLSLC_EXECUTABLE)
    set(GDL_GLSL_COMPILER "${GDL_GLSLC_EXECUTABLE}")
    set(GDL_GLSL_COMPILER_KIND "glslc")
elseif(GDL_GLSLANG_EXECUTABLE)
    set(GDL_GLSL_COMPILER "${GDL_GLSLANG_EXECUTABLE}")
    set(GDL_GLSL_COMPILER_KIND "glslang")
else()
    message(FATAL_ERROR
        "No GLSL compiler found. Install the LunarG Vulkan SDK (glslc) or let vcpkg build "
        "glslang[tools] from vcpkg.json. Searched: ${_gdl_shader_hints}")
endif()
message(STATUS "GLSL compiler: ${GDL_GLSL_COMPILER} (${GDL_GLSL_COMPILER_KIND})")

function(gdl_add_shaders target)
    cmake_parse_arguments(ARG "" "OUTPUT_DIR" "SOURCES;INCLUDES" ${ARGN})
    if(NOT ARG_OUTPUT_DIR)
        message(FATAL_ERROR "gdl_add_shaders: OUTPUT_DIR is required")
    endif()

    set(_outputs)
    foreach(_src IN LISTS ARG_SOURCES)
        get_filename_component(_name "${_src}" NAME)
        set(_out "${ARG_OUTPUT_DIR}/${_name}.spv")
        if(GDL_GLSL_COMPILER_KIND STREQUAL "glslc")
            set(_cmd "${GDL_GLSL_COMPILER}" --target-env=vulkan1.3 -o "${_out}" "${_src}")
        else()
            set(_cmd "${GDL_GLSL_COMPILER}" -V --target-env vulkan1.3 -o "${_out}" "${_src}")
        endif()
        add_custom_command(
            OUTPUT "${_out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${ARG_OUTPUT_DIR}"
            COMMAND ${_cmd}
            DEPENDS "${_src}" ${ARG_INCLUDES}
            COMMENT "Compiling shader ${_name}"
            VERBATIM)
        list(APPEND _outputs "${_out}")
    endforeach()
    foreach(_src IN LISTS ARG_SOURCES ARG_INCLUDES)
        get_filename_component(_name "${_src}" NAME)
        set(_copy "${ARG_OUTPUT_DIR}/${_name}")
        add_custom_command(
            OUTPUT "${_copy}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${ARG_OUTPUT_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "${_copy}"
            DEPENDS "${_src}"
            COMMENT "Copying shader source ${_name}"
            VERBATIM)
        list(APPEND _outputs "${_copy}")
    endforeach()

    add_custom_target(${target}_shaders DEPENDS ${_outputs} SOURCES ${ARG_SOURCES})
    set_target_properties(${target}_shaders PROPERTIES FOLDER "shaders")
    add_dependencies(${target} ${target}_shaders)
endfunction()
