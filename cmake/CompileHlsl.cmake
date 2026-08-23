function(dayo_compile_hlsl output_var)
  cmake_parse_arguments(ARG "" "SOURCE;ENTRY;PROFILE;STAGE" "INCLUDES;DEFINES" ${ARGN})
  if(NOT ARG_SOURCE OR NOT ARG_ENTRY OR NOT ARG_PROFILE OR NOT ARG_STAGE)
    message(FATAL_ERROR "dayo_compile_hlsl requires SOURCE, ENTRY, PROFILE and STAGE")
  endif()

  get_filename_component(source_abs "${ARG_SOURCE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
  get_filename_component(source_name "${ARG_SOURCE}" NAME_WE)
  set(output "${CMAKE_CURRENT_BINARY_DIR}/shaders/${source_name}.${ARG_ENTRY}.spv")

  find_program(DAYO_DXC NAMES dxc HINTS ENV VULKAN_SDK PATH_SUFFIXES bin)
  if(DAYO_DXC)
    set(command
      "${DAYO_DXC}" -spirv -fspv-target-env=vulkan1.3
      -fvk-use-dx-layout -enable-16bit-types
      -E "${ARG_ENTRY}" -T "${ARG_PROFILE}"
      "${source_abs}" -Fo "${output}"
    )
    set(compiler_name "DXC")
  else()
    find_program(DAYO_GLSLC NAMES glslc REQUIRED)
    set(command
      "${DAYO_GLSLC}" -x hlsl --target-env=vulkan1.3
      -fshader-stage=${ARG_STAGE} -fentry-point=${ARG_ENTRY}
      "${source_abs}" -o "${output}"
    )
    set(compiler_name "glslc fallback")
  endif()

  foreach(include_dir IN LISTS ARG_INCLUDES)
    list(APPEND command "-I${include_dir}")
  endforeach()
  foreach(define IN LISTS ARG_DEFINES)
    list(APPEND command "-D${define}")
  endforeach()

  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND ${command}
    DEPENDS "${source_abs}"
    COMMENT "Compiling HLSL ${ARG_SOURCE}:${ARG_ENTRY} to SPIR-V with ${compiler_name}"
    VERBATIM
  )
  set(${output_var} "${output}" PARENT_SCOPE)
endfunction()

