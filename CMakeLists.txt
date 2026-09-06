cmake_minimum_required(VERSION 3.25)

project(mikumikudesu VERSION 0.1.0 LANGUAGES CXX)

# Native application version is independent of the upstream compatibility baseline.
set(DAYO_UPSTREAM_VERSION "1.30")

include(CTest)
include(FetchContent)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum policy version for FetchContent dependencies")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(DAYO_BACKEND "VULKAN" CACHE STRING "Graphics backend (VULKAN or D3D12)")
set_property(CACHE DAYO_BACKEND PROPERTY STRINGS VULKAN D3D12)
option(DAYO_ENABLE_IMGUI "Build the Dear ImGui SDL3/Vulkan UI" ON)
option(DAYO_ENABLE_OIDN "Detect Open Image Denoise support" ON)
option(DAYO_ENABLE_MEDIA "Detect FFmpeg media support" ON)
option(DAYO_ENABLE_PHYSICS "Build Bullet PMX physics" ON)
option(DAYO_ENABLE_VALIDATION "Enable Vulkan validation when available" ON)
option(DAYO_FETCH_DEPENDENCIES "Fetch small missing source dependencies" ON)
option(DAYO_ENABLE_VMA "Use AMD/Vulkan Memory Allocator for generic resources" ON)
option(DAYO_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(DAYO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(DAYO_ENABLE_NATIVE_TUNING "Enable -march=native/-mtune=native for local Linux builds" OFF)
option(DAYO_ENABLE_BOLT "Run LLVM BOLT on the Linux executable after linking" OFF)
set(DAYO_BOLT_PROFILE "" CACHE FILEPATH "perf2bolt/fdata profile used by LLVM BOLT")
set(DAYO_BOLT_EXECUTABLE "" CACHE FILEPATH "LLVM BOLT executable (auto-detected when empty)")
option(DAYO_ENABLE_THIN_LTO "Enable Clang ThinLTO for native targets" OFF)
option(DAYO_ENABLE_PGO_GENERATE "Instrument native targets for PGO profile generation" OFF)
option(DAYO_ENABLE_PGO_USE "Use an existing PGO profile for native targets" OFF)
set(DAYO_PGO_PROFILE_DIR "${CMAKE_BINARY_DIR}/pgo" CACHE PATH "Directory containing or receiving PGO data")

if(DAYO_ENABLE_PGO_GENERATE AND DAYO_ENABLE_PGO_USE)
  message(FATAL_ERROR "DAYO_ENABLE_PGO_GENERATE and DAYO_ENABLE_PGO_USE are mutually exclusive")
endif()
if(DAYO_ENABLE_THIN_LTO AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "DAYO_ENABLE_THIN_LTO currently requires Clang")
endif()
if((DAYO_ENABLE_PGO_GENERATE OR DAYO_ENABLE_PGO_USE) AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  message(FATAL_ERROR "DAYO_ENABLE_PGO_GENERATE and DAYO_ENABLE_PGO_USE require Clang or GNU")
endif()
if(DAYO_ENABLE_PGO_USE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(DAYO_CLANG_PGO_PROFILE "${DAYO_PGO_PROFILE_DIR}/default.profdata")
  if(NOT EXISTS "${DAYO_CLANG_PGO_PROFILE}")
    message(FATAL_ERROR "Clang PGO profile not found: ${DAYO_CLANG_PGO_PROFILE}")
  endif()
endif()
if(DAYO_ENABLE_PGO_USE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(NOT IS_DIRECTORY "${DAYO_PGO_PROFILE_DIR}")
    message(FATAL_ERROR "GCC PGO profile directory not found: ${DAYO_PGO_PROFILE_DIR}")
  endif()
  file(GLOB_RECURSE DAYO_GCC_PGO_PROFILES CONFIGURE_DEPENDS "${DAYO_PGO_PROFILE_DIR}/*.gcda")
  if(NOT DAYO_GCC_PGO_PROFILES)
    message(FATAL_ERROR "GCC PGO profile data not found in: ${DAYO_PGO_PROFILE_DIR}")
  endif()
endif()
if(DAYO_ENABLE_PGO_GENERATE)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    find_program(DAYO_LLVM_PROFDATA_EXECUTABLE NAMES llvm-profdata REQUIRED)
  endif()
  file(MAKE_DIRECTORY "${DAYO_PGO_PROFILE_DIR}")
endif()

if(NOT DAYO_BACKEND STREQUAL "VULKAN")
  message(
    FATAL_ERROR
    "The native target currently supports DAYO_BACKEND=VULKAN. The original D3D12 source remains under MikuMikuDayo/src."
  )
endif()

if(DAYO_ENABLE_NATIVE_TUNING AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "DAYO_ENABLE_NATIVE_TUNING is only supported on Linux")
endif()

if(DAYO_ENABLE_BOLT AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "DAYO_ENABLE_BOLT is only supported on Linux")
endif()

find_package(Vulkan 1.3 QUIET)
if(NOT Vulkan_FOUND)
  if(NOT DAYO_FETCH_DEPENDENCIES)
    message(
      FATAL_ERROR
      "Vulkan development headers were not found. Install vulkan-headers or enable DAYO_FETCH_DEPENDENCIES."
    )
  endif()
  message(STATUS "System Vulkan headers not found; fetching Khronos Vulkan-Headers v1.4.321")
  FetchContent_Declare(
    vulkan_headers
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.321.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(vulkan_headers)
  find_library(DAYO_VULKAN_LOADER NAMES vulkan REQUIRED)
  if(NOT TARGET Vulkan::Vulkan)
    add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
    set_target_properties(
      Vulkan::Vulkan
      PROPERTIES IMPORTED_LOCATION "${DAYO_VULKAN_LOADER}" INTERFACE_LINK_LIBRARIES Vulkan::Headers
    )
  endif()
endif()
find_package(SDL3 CONFIG QUIET)
if(NOT TARGET SDL3::SDL3)
  if(NOT DAYO_FETCH_DEPENDENCIES)
    message(FATAL_ERROR "SDL3 was not found. Install SDL3 or enable DAYO_FETCH_DEPENDENCIES")
  endif()
  message(STATUS "System SDL3 was not found; fetching SDL3 release-3.2.8")
  set(SDL_SHARED ON CACHE BOOL "" FORCE)
  set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    SDL3
    URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.2.8.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(SDL3)
endif()

find_path(DAYO_STB_INCLUDE_DIR stb_image.h PATH_SUFFIXES stb)
if(NOT DAYO_STB_INCLUDE_DIR)
  if(NOT DAYO_FETCH_DEPENDENCIES)
    message(FATAL_ERROR "stb_image.h was not found; install stb or enable DAYO_FETCH_DEPENDENCIES")
  endif()
  FetchContent_Declare(
    stb
    URL https://github.com/nothings/stb/archive/f0569113c93ad095470c54bf34a17b36646bbbb5.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(stb)
  set(DAYO_STB_INCLUDE_DIR "${stb_SOURCE_DIR}")
endif()

add_library(
  dayo_core
  src/core/animation.cpp
  src/core/asset.cpp
  src/core/denoiser.cpp
  src/core/effect.cpp
  src/core/ffmpeg_utils.cpp
  src/core/image.cpp
  src/core/audio_export.cpp
  src/core/video_export.cpp
  src/core/media.cpp
  src/core/mapped_file.cpp
  src/core/model_probe.cpp
  src/core/motion.cpp
  src/core/editor.cpp
  src/core/output.cpp
  src/core/physics.cpp
  src/core/profiling.cpp
  src/core/project.cpp
  src/core/scene.cpp
  src/core/upload_ring.cpp
  src/core/task_scheduler.cpp
  src/core/subayai_hair.cpp
  src/core/subayai_material.cpp
  src/core/vmdayo.cpp
)
target_include_directories(dayo_core PUBLIC src)
target_include_directories(dayo_core PRIVATE "${DAYO_STB_INCLUDE_DIR}")
target_link_libraries(dayo_core PUBLIC SDL3::SDL3)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  set_source_files_properties(
    src/core/image.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-conversion;-Wno-unused-but-set-variable"
  )
  set_source_files_properties(src/core/physics.cpp PROPERTIES COMPILE_OPTIONS "-Wno-conversion;-Wno-float-conversion")
  set_source_files_properties(src/core/media.cpp PROPERTIES COMPILE_OPTIONS "-Wno-conversion;-Wno-old-style-cast")
  set_source_files_properties(
    src/core/output.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-conversion;-Wno-missing-field-initializers"
  )
  set_source_files_properties(src/core/vmdayo.cpp PROPERTIES COMPILE_OPTIONS "-Wno-cpp;-Wno-deprecated-declarations")
  set_source_files_properties(
    src/core/effect.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-conversion;-Wno-shadow;-Wno-cpp;-Wno-deprecated-declarations"
  )
  set_source_files_properties(
    src/core/audio_export.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-conversion;-Wno-shadow;-Wno-deprecated-declarations"
  )
  set_source_files_properties(
    src/core/video_export.cpp
    PROPERTIES COMPILE_OPTIONS "-Wno-conversion;-Wno-shadow;-Wno-deprecated-declarations"
  )
  set_source_files_properties(src/core/ffmpeg_utils.cpp PROPERTIES COMPILE_OPTIONS "-Wno-deprecated-declarations")
  set_source_files_properties(src/core/project.cpp PROPERTIES COMPILE_OPTIONS "-Wno-cpp;-Wno-deprecated-declarations")
endif()

add_library(dayo_platform src/platform/sdl_platform.cpp)
target_include_directories(dayo_platform PUBLIC src)
target_link_libraries(dayo_platform PUBLIC SDL3::SDL3)

if(DAYO_ENABLE_VMA)
  find_path(DAYO_VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES include)
  if(NOT DAYO_VMA_INCLUDE_DIR)
    if(NOT DAYO_FETCH_DEPENDENCIES)
      message(FATAL_ERROR "Vulkan Memory Allocator was not found; install VMA or enable DAYO_FETCH_DEPENDENCIES")
    endif()
    FetchContent_Declare(
      vulkan_memory_allocator
      URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(vulkan_memory_allocator)
    set(DAYO_VMA_INCLUDE_DIR "${vulkan_memory_allocator_SOURCE_DIR}/include")
  endif()
endif()

add_library(
  dayo_graphics
  src/graphics/bdpt_accumulation.cpp
  src/graphics/device.cpp
  src/graphics/sbt.cpp
  src/graphics/subayai_acceleration_structure.cpp
  src/graphics/subayai_environment.cpp
  src/graphics/subayai_light_sampling.cpp
  src/graphics/vulkan/vulkan_device.cpp
  src/graphics/vulkan/vulkan_upload_context.cpp
)
target_include_directories(dayo_graphics PUBLIC src)
target_link_libraries(dayo_graphics PUBLIC dayo_core Vulkan::Vulkan SDL3::SDL3)
target_compile_definitions(dayo_graphics PRIVATE DAYO_ENABLE_VALIDATION=$<BOOL:${DAYO_ENABLE_VALIDATION}>)
if(DAYO_ENABLE_VMA)
  target_sources(dayo_graphics PRIVATE src/graphics/vulkan/vma_impl.cpp)
  target_include_directories(dayo_graphics SYSTEM PRIVATE "${DAYO_VMA_INCLUDE_DIR}")
  target_compile_definitions(dayo_graphics PUBLIC DAYO_ENABLE_VMA=1)
  set_source_files_properties(src/graphics/vulkan/vma_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
else()
  target_compile_definitions(dayo_graphics PUBLIC DAYO_ENABLE_VMA=0)
endif()

include(cmake/CompileHlsl.cmake)
dayo_compile_hlsl(DAYO_PREVIEW_VERTEX_SPV SOURCE shaders/preview.hlsl ENTRY VS PROFILE vs_6_6 STAGE vert)
dayo_compile_hlsl(DAYO_PREVIEW_EDGE_VERTEX_SPV SOURCE shaders/preview.hlsl ENTRY EdgeVS PROFILE vs_6_6 STAGE vert)
dayo_compile_hlsl(DAYO_PREVIEW_FRAGMENT_SPV SOURCE shaders/preview.hlsl ENTRY PS PROFILE ps_6_6 STAGE frag)
add_custom_target(
  dayo_shaders
  ALL
  DEPENDS ${DAYO_PREVIEW_VERTEX_SPV} ${DAYO_PREVIEW_EDGE_VERTEX_SPV} ${DAYO_PREVIEW_FRAGMENT_SPV}
)
add_dependencies(dayo_graphics dayo_shaders)
target_compile_definitions(
  dayo_graphics
  PRIVATE
    DAYO_PREVIEW_VERTEX_SPV="${DAYO_PREVIEW_VERTEX_SPV}"
    DAYO_PREVIEW_EDGE_VERTEX_SPV="${DAYO_PREVIEW_EDGE_VERTEX_SPV}"
    DAYO_PREVIEW_FRAGMENT_SPV="${DAYO_PREVIEW_FRAGMENT_SPV}"
)

if(DAYO_ENABLE_IMGUI)
  if(NOT DAYO_FETCH_DEPENDENCIES)
    message(FATAL_ERROR "DAYO_ENABLE_IMGUI requires DAYO_FETCH_DEPENDENCIES in this source tree")
  endif()
  FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(imgui)
  add_library(
    dayo_imgui
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
  )
  target_include_directories(dayo_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(dayo_imgui PUBLIC SDL3::SDL3 Vulkan::Vulkan)
  target_link_libraries(dayo_graphics PUBLIC dayo_imgui)
  target_link_libraries(dayo_platform PUBLIC dayo_imgui)
  target_compile_definitions(dayo_graphics PUBLIC DAYO_HAS_IMGUI=1)
  target_compile_definitions(dayo_platform PUBLIC DAYO_HAS_IMGUI=1)
else()
  target_compile_definitions(dayo_graphics PUBLIC DAYO_HAS_IMGUI=0)
  target_compile_definitions(dayo_platform PUBLIC DAYO_HAS_IMGUI=0)
endif()

set(DAYO_HAS_OIDN 0)
if(DAYO_ENABLE_OIDN)
  find_package(OpenImageDenoise CONFIG QUIET)
  if(OpenImageDenoise_FOUND)
    set(DAYO_HAS_OIDN 1)
  endif()
endif()
target_compile_definitions(dayo_core PUBLIC DAYO_HAS_OIDN=${DAYO_HAS_OIDN})
if(DAYO_HAS_OIDN)
  target_link_libraries(dayo_core PUBLIC OpenImageDenoise)
endif()

set(DAYO_HAS_BULLET 0)
if(DAYO_ENABLE_PHYSICS)
  find_package(Bullet QUIET)
  if(NOT Bullet_FOUND AND DAYO_FETCH_DEPENDENCIES)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
    set(BUILD_BULLET2_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_BULLET3 OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
    set(BUILD_CPU_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_OPENGL3_DEMOS OFF CACHE BOOL "" FORCE)
    set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(USE_GRAPHICAL_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(INSTALL_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
      bullet
      URL https://github.com/bulletphysics/bullet3/archive/refs/tags/3.25.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(bullet)
    set(BULLET_INCLUDE_DIRS "${bullet_SOURCE_DIR}/src")
    set(BULLET_LIBRARIES BulletDynamics BulletCollision LinearMath)
    set(Bullet_FOUND TRUE)
  endif()
  if(Bullet_FOUND)
    set(DAYO_HAS_BULLET 1)
    target_include_directories(dayo_core PRIVATE ${BULLET_INCLUDE_DIRS})
    target_link_libraries(dayo_core PUBLIC ${BULLET_LIBRARIES})
  endif()
endif()
target_compile_definitions(dayo_core PUBLIC DAYO_HAS_BULLET=${DAYO_HAS_BULLET})
find_package(cereal CONFIG QUIET)
find_package(directxtex CONFIG QUIET)
set(DAYO_HAS_JSONNET 0)
find_package(jsonnet CONFIG QUIET)
if(NOT TARGET jsonnet::libjsonnet)
  find_path(DAYO_JSONNET_INCLUDE_DIR libjsonnet.h)
  find_library(DAYO_JSONNET_LIBRARY NAMES jsonnet)
  if(DAYO_JSONNET_INCLUDE_DIR AND DAYO_JSONNET_LIBRARY)
    add_library(jsonnet::libjsonnet UNKNOWN IMPORTED)
    set_target_properties(
      jsonnet::libjsonnet
      PROPERTIES IMPORTED_LOCATION "${DAYO_JSONNET_LIBRARY}" INTERFACE_INCLUDE_DIRECTORIES "${DAYO_JSONNET_INCLUDE_DIR}"
    )
  endif()
endif()
if(TARGET jsonnet::libjsonnet)
  set(DAYO_JSONNET_TARGET jsonnet::libjsonnet)
  set(DAYO_HAS_JSONNET 1)
elseif(DAYO_FETCH_DEPENDENCIES)
  set(BUILD_JSONNET ON CACHE BOOL "" FORCE)
  set(BUILD_JSONNETFMT OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
  set(USE_SYSTEM_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    jsonnet_source
    URL https://github.com/google/jsonnet/archive/refs/tags/v0.21.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(jsonnet_source)
  if(BUILD_TESTING)
    set_property(TEST regression_test DIRECTORY "${jsonnet_source_SOURCE_DIR}/test_suite" PROPERTY DISABLED TRUE)
  endif()
  set(DAYO_JSONNET_TARGET libjsonnet_static)
  set(DAYO_HAS_JSONNET 1)
endif()
target_compile_definitions(dayo_core PUBLIC DAYO_HAS_JSONNET=${DAYO_HAS_JSONNET})
if(DAYO_HAS_JSONNET)
  target_link_libraries(dayo_core PUBLIC ${DAYO_JSONNET_TARGET})
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
  find_package(nlohmann_json 3.6.1 QUIET)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json AND DAYO_FETCH_DEPENDENCIES)
  FetchContent_Declare(
    nlohmann_json_source
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(nlohmann_json_source)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
  message(FATAL_ERROR "nlohmann_json is required for .dayo project support")
endif()
target_link_libraries(dayo_core PUBLIC nlohmann_json::nlohmann_json)

set(DAYO_HAS_MEDIA 0)
if(DAYO_ENABLE_MEDIA)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(
      FFMPEG
      QUIET
      IMPORTED_TARGET
      libavcodec
      libavformat
      libavutil
      libswresample
      libswscale
    )
    if(FFMPEG_FOUND)
      set(DAYO_HAS_MEDIA 1)
    endif()
  endif()
endif()
target_compile_definitions(dayo_core PUBLIC DAYO_HAS_MEDIA=${DAYO_HAS_MEDIA})
if(DAYO_HAS_MEDIA)
  target_link_libraries(dayo_core PUBLIC PkgConfig::FFMPEG)
endif()

add_executable(
  mikumikudesu
  src/app/application.cpp
  src/app/audio_export_command.cpp
  src/app/audio_export_job.cpp
  src/app/video_export_job.cpp
  src/app/main.cpp
)
target_include_directories(mikumikudesu PRIVATE src)
target_link_libraries(mikumikudesu PRIVATE dayo_core dayo_platform dayo_graphics)
target_compile_definitions(
  mikumikudesu
  PRIVATE DAYO_HAS_OIDN=${DAYO_HAS_OIDN} DAYO_HAS_MEDIA=${DAYO_HAS_MEDIA} DAYO_VERSION="${PROJECT_VERSION}"
)
if(DAYO_HAS_MEDIA)
  target_link_libraries(mikumikudesu PRIVATE PkgConfig::FFMPEG)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  foreach(target dayo_core dayo_platform dayo_graphics mikumikudesu)
    target_compile_options(
      ${target}
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    if(DAYO_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
      if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${target} PRIVATE -Wno-error=deprecated-literal-operator)
      endif()
    endif()
    if(DAYO_ENABLE_NATIVE_TUNING)
      target_compile_options(${target} PRIVATE -march=native -mtune=native)
    endif()
    if(DAYO_ENABLE_THIN_LTO)
      target_compile_options(${target} PRIVATE -flto=thin)
      target_link_options(${target} PRIVATE -flto=thin)
    endif()
    if(DAYO_ENABLE_PGO_GENERATE)
      if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${target} PRIVATE -fprofile-instr-generate)
        target_link_options(${target} PRIVATE "-fprofile-instr-generate=${DAYO_PGO_PROFILE_DIR}/%p.profraw")
      else()
        target_compile_options(${target} PRIVATE "-fprofile-generate=${DAYO_PGO_PROFILE_DIR}")
        target_link_options(${target} PRIVATE "-fprofile-generate=${DAYO_PGO_PROFILE_DIR}")
      endif()
    elseif(DAYO_ENABLE_PGO_USE)
      if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${target} PRIVATE "-fprofile-instr-use=${DAYO_CLANG_PGO_PROFILE}")
      else()
        target_compile_options(${target} PRIVATE "-fprofile-use=${DAYO_PGO_PROFILE_DIR}" -fprofile-correction)
        target_link_options(${target} PRIVATE "-fprofile-use=${DAYO_PGO_PROFILE_DIR}")
      endif()
    endif()
  endforeach()
  target_compile_options(dayo_graphics PRIVATE -Wno-missing-field-initializers)
endif()

if(DAYO_ENABLE_BOLT)
  if(NOT DAYO_BOLT_PROFILE)
    message(FATAL_ERROR "DAYO_ENABLE_BOLT requires DAYO_BOLT_PROFILE=/path/to/profile.fdata")
  endif()
  if(NOT DAYO_BOLT_EXECUTABLE)
    find_program(DAYO_BOLT_EXECUTABLE NAMES llvm-bolt bolt REQUIRED)
  endif()
  target_link_options(mikumikudesu PRIVATE LINKER:--emit-relocs)
  add_custom_command(
    TARGET mikumikudesu
    POST_BUILD
    COMMAND
      "${DAYO_BOLT_EXECUTABLE}" "$<TARGET_FILE:mikumikudesu>" -o "$<TARGET_FILE_DIR:mikumikudesu>/mikumikudesu.bolt"
      -data "${DAYO_BOLT_PROFILE}" -reorder-blocks=ext-tsp -reorder-functions=hfsort+ -split-functions -split-all-cold
    VERBATIM
    COMMENT "Optimizing mikumikudesu with LLVM BOLT"
  )
endif()

if(DAYO_ENABLE_BOLT)
  install(PROGRAMS "$<TARGET_FILE_DIR:mikumikudesu>/mikumikudesu.bolt" DESTINATION bin RENAME mikumikudesu)
else()
  install(TARGETS mikumikudesu RUNTIME DESTINATION bin)
endif()
include(cmake/InstallMikuMikuDayo.cmake)
if(DAYO_ENABLE_PGO_GENERATE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_custom_target(
    dayo_pgo_merge
    COMMAND
      "${CMAKE_COMMAND}" -DLLVM_PROFDATA="${DAYO_LLVM_PROFDATA_EXECUTABLE}" -DPGO_PROFILE_DIR="${DAYO_PGO_PROFILE_DIR}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MergePgoProfiles.cmake"
    DEPENDS mikumikudesu
    USES_TERMINAL
    COMMENT "Merging Clang PGO profiles"
  )
endif()

if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME upstream_fetch COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/fetch_mikumikudayo_tests.py")
  add_executable(dayo_core_tests tests/core_tests.cpp)
  target_link_libraries(dayo_core_tests PRIVATE dayo_core)
  add_executable(dayo_upload_ring_tests tests/upload_ring_tests.cpp)
  target_link_libraries(dayo_upload_ring_tests PRIVATE dayo_core)
  add_executable(dayo_preview_tests tests/preview_render_tests.cpp)
  target_include_directories(dayo_preview_tests PRIVATE src)
  target_link_libraries(dayo_preview_tests PRIVATE dayo_platform dayo_graphics)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      dayo_preview_tests
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    if(DAYO_WARNINGS_AS_ERRORS)
      target_compile_options(dayo_preview_tests PRIVATE -Werror)
      if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(dayo_preview_tests PRIVATE -Wno-error=deprecated-literal-operator)
      endif()
    endif()
  endif()
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      dayo_core_tests
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    if(DAYO_WARNINGS_AS_ERRORS)
      target_compile_options(dayo_core_tests PRIVATE -Werror)
      if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(dayo_core_tests PRIVATE -Wno-error=deprecated-literal-operator)
      endif()
    endif()
  endif()

  add_executable(dayo_upstream_compat_tests tests/upstream_compat_tests.cpp)
  target_link_libraries(dayo_upstream_compat_tests PRIVATE dayo_core)
  target_compile_definitions(dayo_upstream_compat_tests PRIVATE DAYO_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      dayo_upstream_compat_tests
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    if(DAYO_WARNINGS_AS_ERRORS)
      target_compile_options(dayo_upstream_compat_tests PRIVATE -Werror)
      if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(dayo_upstream_compat_tests PRIVATE -Wno-error=deprecated-literal-operator)
      endif()
    endif()
  endif()

  add_executable(dayo_subayai_bdpt_tests tests/subayai_bdpt_tests.cpp)
  target_include_directories(dayo_subayai_bdpt_tests PRIVATE src)
  target_link_libraries(dayo_subayai_bdpt_tests PRIVATE dayo_core dayo_graphics)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(
      dayo_subayai_bdpt_tests
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    if(DAYO_WARNINGS_AS_ERRORS)
      target_compile_options(dayo_subayai_bdpt_tests PRIVATE -Werror)
      if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(dayo_subayai_bdpt_tests PRIVATE -Wno-error=deprecated-literal-operator)
      endif()
    endif()
  endif()

  add_test(NAME core COMMAND dayo_core_tests)
  add_test(NAME upload_ring COMMAND dayo_upload_ring_tests)
  add_test(NAME subayai_bdpt COMMAND dayo_subayai_bdpt_tests)
  add_test(NAME upstream_compat COMMAND dayo_upstream_compat_tests)
  set_tests_properties(upstream_compat PROPERTIES LABELS "upstream" TIMEOUT 60)
  add_test(NAME vulkan_probe COMMAND mikumikudesu --probe --hidden)
  add_test(NAME preview_render COMMAND dayo_preview_tests)
  set_tests_properties(vulkan_probe PROPERTIES TIMEOUT 60)
  set_tests_properties(preview_render PROPERTIES TIMEOUT 20)
  if(DAYO_HAS_MEDIA)
    add_test(
      NAME audio_export_cli
      COMMAND bash "${CMAKE_SOURCE_DIR}/tests/audio_export_cli.sh" "$<TARGET_FILE:mikumikudesu>"
    )
    set_tests_properties(audio_export_cli PROPERTIES TIMEOUT 20)
    add_test(
      NAME video_export_cli
      COMMAND bash "${CMAKE_SOURCE_DIR}/tests/video_export_cli.sh" "$<TARGET_FILE:mikumikudesu>"
    )
    set_tests_properties(video_export_cli PROPERTIES TIMEOUT 60)
  endif()
endif()

if(DAYO_ENABLE_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "DAYO_ENABLE_SANITIZERS requires GCC or Clang")
  endif()
  set(DAYO_SANITIZER_COMPILE_OPTIONS -fsanitize=address,undefined -fno-omit-frame-pointer)
  set(DAYO_SANITIZER_LINK_OPTIONS -fsanitize=address,undefined)
  foreach(
    target
    dayo_core
    dayo_platform
    dayo_graphics
    mikumikudesu
    dayo_core_tests
    dayo_upload_ring_tests
    dayo_preview_tests
    dayo_subayai_bdpt_tests
    dayo_upstream_compat_tests
  )
    if(TARGET ${target})
      target_compile_options(${target} PRIVATE ${DAYO_SANITIZER_COMPILE_OPTIONS})
    endif()
  endforeach()
  target_link_options(mikumikudesu PRIVATE ${DAYO_SANITIZER_LINK_OPTIONS})
  if(TARGET dayo_core_tests)
    target_link_options(dayo_core_tests PRIVATE ${DAYO_SANITIZER_LINK_OPTIONS})
  endif()
  if(TARGET dayo_upload_ring_tests)
    target_link_options(dayo_upload_ring_tests PRIVATE ${DAYO_SANITIZER_LINK_OPTIONS})
  endif()
  if(TARGET dayo_preview_tests)
    target_link_options(dayo_preview_tests PRIVATE ${DAYO_SANITIZER_LINK_OPTIONS})
  endif()
  if(TARGET dayo_upstream_compat_tests)
    target_link_options(dayo_upstream_compat_tests PRIVATE ${DAYO_SANITIZER_LINK_OPTIONS})
  endif()
  if(TARGET dayo_subayai_bdpt_tests)
    target_link_options(dayo_subayai_bdpt_tests PRIVATE ${DAYO_SANITIZER_LINK_OPTIONS})
  endif()
endif()

message(STATUS "MikuMikuDayo upstream baseline: ${DAYO_UPSTREAM_VERSION}")
message(STATUS "mikumikudesu backend: ${DAYO_BACKEND}")
message(STATUS "Dear ImGui: ${DAYO_ENABLE_IMGUI}")
message(STATUS "OIDN detected: ${DAYO_HAS_OIDN} (CPU always preferred; HIP is runtime-optional)")
message(STATUS "FFmpeg detected: ${DAYO_HAS_MEDIA}")
message(STATUS "Bullet physics: ${DAYO_HAS_BULLET}")
message(STATUS "cereal detected: ${cereal_FOUND}")
message(STATUS "DirectXTex detected: ${directxtex_FOUND}")
message(STATUS "Jsonnet effects: ${DAYO_HAS_JSONNET}")
