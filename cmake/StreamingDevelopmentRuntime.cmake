# SPDX-License-Identifier: MIT
if(APPLE)
  set(STREAMING_GUI_TARGET Deskflow)
else()
  set(STREAMING_GUI_TARGET deskflow)
endif()
foreach(STREAMING_DEVELOPMENT_OUTPUT "$<TARGET_FILE_DIR:${STREAMING_GUI_TARGET}>"
    "${CMAKE_BINARY_DIR}/src/unittests/streaming"
    "${CMAKE_BINARY_DIR}/src/unittests/gui")
  file(GENERATE OUTPUT "${STREAMING_DEVELOPMENT_OUTPUT}/streaming-runtime.development"
    CONTENT "${CMAKE_BINARY_DIR}")
endforeach()
if(APPLE)
  file(GENERATE OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/streaming-runtime.development"
    CONTENT "${CMAKE_BINARY_DIR}")
endif()
