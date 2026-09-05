# Shared with scripts/fetch-mikumikudayo.py; missing assets fail at install time.
set(DAYO_RUNTIME_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/deps/mikumikudayo-runtime.manifest")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DAYO_RUNTIME_MANIFEST}")
file(STRINGS "${DAYO_RUNTIME_MANIFEST}" DAYO_RUNTIME_PATHS)
foreach(path IN LISTS DAYO_RUNTIME_PATHS)
  string(STRIP "${path}" path)
  if(path STREQUAL "" OR path MATCHES "^#")
    continue()
  endif()
  if(path MATCHES "^/|\\\\|:|(^|/)\\.\\.?(/|$)|//")
    message(FATAL_ERROR "Unsafe runtime manifest path: ${path}")
  endif()
  if(path MATCHES "/$")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/MikuMikuDayo/${path}"
      DESTINATION "share/mikumikudesu/${path}")
  else()
    get_filename_component(parent "${path}" DIRECTORY)
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/MikuMikuDayo/${path}"
      DESTINATION "share/mikumikudesu/${parent}")
  endif()
endforeach()
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/deps/mikumikudayo.lock" "${DAYO_RUNTIME_MANIFEST}"
  DESTINATION share/mikumikudesu)
