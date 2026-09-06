if(NOT TARGET dayo_core)
  return()
endif()

add_executable(fxedit fxedit/main.cpp)
target_link_libraries(fxedit PRIVATE dayo_core)

add_executable(sequence_movie sequence_movie/main.cpp)
target_link_libraries(sequence_movie PRIVATE dayo_core)

add_executable(vdb2dds vdb2dds/main.cpp)
target_link_libraries(vdb2dds PRIVATE dayo_core)

add_executable(capability_suite capability_suite/main.cpp)
target_link_libraries(capability_suite PRIVATE dayo_core)

foreach(tool fxedit sequence_movie vdb2dds capability_suite)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      ${tool}
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
  endif()
endforeach()
