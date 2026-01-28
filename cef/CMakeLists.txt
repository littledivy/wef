# Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

cmake_minimum_required(VERSION 3.19)
project(wef VERSION 1.0.0 LANGUAGES CXX OBJCXX)

set(WEF_SRCS_COMMON
  src/app.cc
  src/app.h
  src/runtime_loader.cc
  src/runtime_loader.h
)

set(WEF_SRCS_MAC
  src/main_mac.mm
)

set(WEF_RENDERER_SRCS
  src/render_process_handler.cc
  src/render_process_handler.h
  src/renderer_app.cc
  src/renderer_app.h
)

set(WEF_HELPER_SRCS
  src/helper.cc
  ${WEF_RENDERER_SRCS}
)

set(WEF_RESOURCES_SRCS
  macos/Info.plist.in
)

if(NOT DEFINED CEF_ROOT)
  message(FATAL_ERROR "CEF_ROOT must be defined")
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CEF_ROOT}/cmake")

find_package(CEF REQUIRED)

include(cef_macros)
include(cef_variables)

set(CEF_WRAPPER_LIB "${CEF_ROOT}/libcef_dll_wrapper/libcef_dll_wrapper.a")
if(EXISTS "${CEF_WRAPPER_LIB}" AND NOT TARGET libcef_dll_wrapper)
  add_library(libcef_dll_wrapper STATIC IMPORTED GLOBAL)
  set_target_properties(libcef_dll_wrapper PROPERTIES
    IMPORTED_LOCATION "${CEF_WRAPPER_LIB}"
  )
  target_include_directories(libcef_dll_wrapper INTERFACE
    "${CEF_ROOT}"
    "${CEF_ROOT}/include"
  )
endif()

set(CEF_TARGET "wef")
set(CEF_HELPER_TARGET "wef_Helper")
set(CEF_HELPER_OUTPUT_NAME "wef Helper")

SET_CEF_TARGET_OUT_DIR()

if(OS_MAC)
  option(OPTION_USE_ARC "Build with ARC (automatic Reference Counting) on macOS." ON)
  if(OPTION_USE_ARC)
    list(APPEND CEF_COMPILER_FLAGS
      -fobjc-arc
    )
    set_target_properties(${target} PROPERTIES
      CLANG_ENABLE_OBJC_ARC "YES"
    )
  endif()

  set(CEF_APP "${CEF_TARGET_OUT_DIR}/${CEF_TARGET}.app")

  set(EXECUTABLE_NAME "${CEF_TARGET}")
  set(PRODUCT_NAME "${CEF_TARGET}")

  add_executable(${CEF_TARGET} MACOSX_BUNDLE ${WEF_RESOURCES_SRCS} ${WEF_SRCS_COMMON} ${WEF_SRCS_MAC})
  SET_EXECUTABLE_TARGET_PROPERTIES(${CEF_TARGET})
  target_link_libraries(${CEF_TARGET} libcef_dll_wrapper ${CEF_STANDARD_LIBS})
  target_include_directories(${CEF_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  set_target_properties(${CEF_TARGET} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in
  )

  COPY_MAC_FRAMEWORK("${CEF_TARGET}" "${CEF_BINARY_DIR}" "${CEF_APP}")

  foreach(_suffix_list ${CEF_HELPER_APP_SUFFIXES})
    string(REPLACE ":" ";" _suffix_list ${_suffix_list})
    list(GET _suffix_list 0 _name_suffix)
    list(GET _suffix_list 1 _target_suffix)
    list(GET _suffix_list 2 _plist_suffix)

    set(_helper_target "${CEF_HELPER_TARGET}${_target_suffix}")
    set(_helper_output_name "${CEF_HELPER_OUTPUT_NAME}${_name_suffix}")

    set(_helper_info_plist "${CMAKE_CURRENT_BINARY_DIR}/helper-Info${_target_suffix}.plist")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/macos/helper-Info.plist.in" _plist_contents)
    string(REPLACE "\${EXECUTABLE_NAME}" "${_helper_output_name}" _plist_contents ${_plist_contents})
    string(REPLACE "\${PRODUCT_NAME}" "${_helper_output_name}" _plist_contents ${_plist_contents})
    string(REPLACE "\${BUNDLE_ID_SUFFIX}" "${_plist_suffix}" _plist_contents ${_plist_contents})
    file(WRITE ${_helper_info_plist} ${_plist_contents})

    add_executable(${_helper_target} MACOSX_BUNDLE ${WEF_HELPER_SRCS})
    SET_EXECUTABLE_TARGET_PROPERTIES(${_helper_target})
    target_link_libraries(${_helper_target} libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_include_directories(${_helper_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    set_target_properties(${_helper_target} PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST ${_helper_info_plist}
      OUTPUT_NAME ${_helper_output_name}
    )

    add_dependencies(${CEF_TARGET} "${_helper_target}")

    add_custom_command(
      TARGET ${CEF_TARGET}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CEF_TARGET_OUT_DIR}/${_helper_output_name}.app"
              "${CEF_APP}/Contents/Frameworks/${_helper_output_name}.app"
      VERBATIM
    )
  endforeach()

  set(PREFIXES "macos/")
  COPY_MAC_RESOURCES("${WEF_RESOURCES_SRCS}" "${PREFIXES}" "${CEF_TARGET}" "${CMAKE_CURRENT_SOURCE_DIR}" "${CEF_APP}")
endif()
