cmake_minimum_required(VERSION 3.25)

project(mikumikudesu VERSION 1.20.0 LANGUAGES CXX)

include(CTest)
include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(DAYO_BACKEND "VULKAN" CACHE STRING "Graphics backend (VULKAN or D3D12)")
set_property(CACHE DAYO_BACKEND PROPERTY STRINGS VULKAN D3D12)
option(DAYO_ENABLE_IMGUI "Build the Dear ImGui SDL3/Vulkan UI" ON)
option(DAYO_ENABLE_OIDN "Detect Open Image Denoise support" ON)
option(DAYO_ENABLE_MEDIA "Detect FFmpeg media support" ON)
option(DAYO_ENABLE_VALIDATION "Enable Vulkan validation when available" ON)
option(DAYO_FETCH_DEPENDENCIES "Fetch small missing source dependencies" ON)

if(NOT DAYO_BACKEND STREQUAL "VULKAN")
  message(FATAL_ERROR "The native target currently supports DAYO_BACKEND=VULKAN. The original D3D12 source remains under MikuMikuDayo/src.")
endif()

find_package(Vulkan 1.3 QUIET)
if(NOT Vulkan_FOUND)
  if(NOT DAYO_FETCH_DEPENDENCIES)
    message(FATAL_ERROR
      "Vulkan development headers were not found. Install vulkan-headers or enable DAYO_FETCH_DEPENDENCIES.")
  endif()
  message(STATUS "System Vulkan headers not found; fetching Khronos Vulkan-Headers v1.4.321")
  FetchContent_Declare(vulkan_headers
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.321.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(vulkan_headers)
  find_library(DAYO_VULKAN_LOADER NAMES vulkan REQUIRED)
  if(NOT TARGET Vulkan::Vulkan)
    add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
    set_target_properties(Vulkan::Vulkan PROPERTIES
      IMPORTED_LOCATION "${DAYO_VULKAN_LOADER}"
      INTERFACE_LINK_LIBRARIES Vulkan::Headers
    )
  endif()
endif()
find_package(SDL3 CONFIG REQUIRED)

add_library(dayo_core
  src/core/asset.cpp
  src/core/model_probe.cpp
)
target_include_directories(dayo_core PUBLIC src)

add_library(dayo_platform
  src/platform/sdl_platform.cpp
)
target_include_directories(dayo_platform PUBLIC src)
target_link_libraries(dayo_platform PUBLIC SDL3::SDL3)

add_library(dayo_graphics
  src/graphics/device.cpp
  src/graphics/vulkan/vulkan_device.cpp
)
target_include_directories(dayo_graphics PUBLIC src)
target_link_libraries(dayo_graphics PUBLIC Vulkan::Vulkan SDL3::SDL3)
target_compile_definitions(dayo_graphics PRIVATE
  DAYO_ENABLE_VALIDATION=$<BOOL:${DAYO_ENABLE_VALIDATION}>
)

include(cmake/CompileHlsl.cmake)
dayo_compile_hlsl(DAYO_PREVIEW_VERTEX_SPV
  SOURCE shaders/preview.hlsl ENTRY VS PROFILE vs_6_6 STAGE vert
)
dayo_compile_hlsl(DAYO_PREVIEW_FRAGMENT_SPV
  SOURCE shaders/preview.hlsl ENTRY PS PROFILE ps_6_6 STAGE frag
)
add_custom_target(dayo_shaders ALL
  DEPENDS ${DAYO_PREVIEW_VERTEX_SPV} ${DAYO_PREVIEW_FRAGMENT_SPV}
)
add_dependencies(dayo_graphics dayo_shaders)
target_compile_definitions(dayo_graphics PRIVATE
  DAYO_PREVIEW_VERTEX_SPV="${DAYO_PREVIEW_VERTEX_SPV}"
  DAYO_PREVIEW_FRAGMENT_SPV="${DAYO_PREVIEW_FRAGMENT_SPV}"
)

if(DAYO_ENABLE_IMGUI)
  if(NOT DAYO_FETCH_DEPENDENCIES)
    message(FATAL_ERROR "DAYO_ENABLE_IMGUI requires DAYO_FETCH_DEPENDENCIES in this source tree")
  endif()
  FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(imgui)
  add_library(dayo_imgui
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
  )
  target_include_directories(dayo_imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
  )
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

set(DAYO_HAS_MEDIA 0)
if(DAYO_ENABLE_MEDIA)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
      libavcodec libavformat libavutil libswresample libswscale
    )
    if(FFMPEG_FOUND)
      set(DAYO_HAS_MEDIA 1)
    endif()
  endif()
endif()

add_executable(mikumikudesu
  src/app/application.cpp
  src/app/main.cpp
)
target_include_directories(mikumikudesu PRIVATE src)
target_link_libraries(mikumikudesu PRIVATE dayo_core dayo_platform dayo_graphics)
target_compile_definitions(mikumikudesu PRIVATE
  DAYO_HAS_OIDN=${DAYO_HAS_OIDN}
  DAYO_HAS_MEDIA=${DAYO_HAS_MEDIA}
  DAYO_VERSION="${PROJECT_VERSION}"
)
if(DAYO_HAS_OIDN)
  target_link_libraries(mikumikudesu PRIVATE OpenImageDenoise)
endif()
if(DAYO_HAS_MEDIA)
  target_link_libraries(mikumikudesu PRIVATE PkgConfig::FFMPEG)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  foreach(target dayo_core dayo_platform dayo_graphics mikumikudesu)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  endforeach()
endif()

install(TARGETS mikumikudesu RUNTIME DESTINATION bin)
install(DIRECTORY MikuMikuDayo/hlsl MikuMikuDayo/renderer
  DESTINATION share/mikumikudesu
  FILES_MATCHING PATTERN "*.hlsl" PATTERN "*.hlsli" PATTERN "*.fxdayo"
)

if(BUILD_TESTING)
  add_executable(dayo_core_tests tests/core_tests.cpp)
  target_link_libraries(dayo_core_tests PRIVATE dayo_core)
  add_test(NAME core COMMAND dayo_core_tests)
  add_test(NAME vulkan_probe COMMAND mikumikudesu --probe --hidden)
  set_tests_properties(vulkan_probe PROPERTIES TIMEOUT 20)
endif()

message(STATUS "mikumikudesu backend: ${DAYO_BACKEND}")
message(STATUS "Dear ImGui: ${DAYO_ENABLE_IMGUI}")
message(STATUS "OIDN detected: ${DAYO_HAS_OIDN} (CPU always preferred; HIP is runtime-optional)")
message(STATUS "FFmpeg detected: ${DAYO_HAS_MEDIA}")
