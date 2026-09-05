if(NOT DEFINED LLVM_PROFDATA OR NOT EXISTS "${LLVM_PROFDATA}")
  message(FATAL_ERROR "LLVM_PROFDATA must point to llvm-profdata")
endif()
if(NOT DEFINED PGO_PROFILE_DIR)
  message(FATAL_ERROR "PGO_PROFILE_DIR is required")
endif()
if(NOT DEFINED PGO_OUTPUT)
  set(PGO_OUTPUT "${PGO_PROFILE_DIR}/default.profdata")
endif()

file(GLOB profile_files CONFIGURE_DEPENDS "${PGO_PROFILE_DIR}/*.profraw")
if(NOT profile_files)
  message(FATAL_ERROR "No Clang .profraw files found in ${PGO_PROFILE_DIR}")
endif()

execute_process(
  COMMAND "${LLVM_PROFDATA}" merge -output="${PGO_OUTPUT}" ${profile_files}
  RESULT_VARIABLE merge_result
  OUTPUT_VARIABLE merge_output
  ERROR_VARIABLE merge_error
)
if(NOT merge_result EQUAL 0)
  message(FATAL_ERROR "llvm-profdata merge failed: ${merge_output}${merge_error}")
endif()
message(STATUS "Merged ${profile_files} into ${PGO_OUTPUT}")
