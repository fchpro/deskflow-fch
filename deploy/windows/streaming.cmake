# SPDX-License-Identifier: MIT
find_package(Python3 REQUIRED COMPONENTS Interpreter)
if(NOT Qt6_VERSION VERSION_EQUAL "6.10.3")
  message(FATAL_ERROR "Refresh and verify streaming package notices before changing the pinned Qt 6.10.3 runtime")
endif()
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
include(InstallRequiredSystemLibraries)
list(GET CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS 0 STREAMING_CRT_FILE)
get_filename_component(STREAMING_CRT_DIR "${STREAMING_CRT_FILE}" DIRECTORY)
get_filename_component(STREAMING_QT_BIN "${DEPLOYQT}" DIRECTORY)
get_filename_component(STREAMING_QT_ROOT "${STREAMING_QT_BIN}" DIRECTORY)
cmake_path(SET STREAMING_SDK_ROOT NORMALIZE "${GSTREAMER_gstreamer-1.0_PREFIX}")
if(BUILD_TESTS)
  file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/streaming-package-inputs.json" CONTENT
    "{\"source\":\"${CMAKE_SOURCE_DIR}\",\"sdk\":\"${STREAMING_SDK_ROOT}\",\"qt\":\"${STREAMING_QT_ROOT}\",\"crt\":\"${STREAMING_CRT_DIR}\",\"minimum_crt\":\"${REQUIRED_MSVC_RUNTIME_MAJOR}.${REQUIRED_MSVC_RUNTIME_MINOR}\",\"application\":\"$<TARGET_FILE:deskflow>\",\"pipeline_test\":\"$<TARGET_FILE:StreamingCapturePipelineTests>\"}")
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/streaming-install.cmake.in"
  "${CMAKE_BINARY_DIR}/streaming-install.cmake" @ONLY)
install(SCRIPT "${CMAKE_BINARY_DIR}/streaming-install.cmake")
