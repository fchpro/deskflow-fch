# SPDX-License-Identifier: MIT
create_test(NAME StreamingProtocolTests DEPENDS streaming SOURCE StreamingProtocolTests.cpp
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
create_test(NAME StreamingTransportTests DEPENDS streaming SOURCE StreamingTransportTests.cpp
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
if(WIN32)
  create_test(NAME StreamingEnvironmentTests DEPENDS streaming-capture SOURCE StreamingEnvironmentTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  create_test(NAME StreamingWindowsIpcTests DEPENDS streaming SOURCE StreamingWindowsIpcTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  create_test(NAME StreamingCaptureTests DEPENDS streaming-capture SOURCE StreamingCaptureTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
if(BUILD_STREAMING)
  if(WIN32)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME StreamingBundleTests COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/test_streaming_bundle.py"
      --config "${CMAKE_BINARY_DIR}/streaming-package-inputs.json" -v)
    set_tests_properties(StreamingBundleTests PROPERTIES LABELS streaming-extended)
    create_test(NAME StreamingRuntimeTests DEPENDS streaming-capture SOURCE StreamingRuntimeTests.cpp
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(StreamingRuntimeTests PRIVATE
      DESKFLOW_TEST_GSTREAMER_SDK="${GSTREAMER_gstreamer-1.0_PREFIX}")
    set_tests_properties(StreamingRuntimeTests PROPERTIES LABELS streaming-extended)
    add_test(NAME StreamingRuntimeQuickTests COMMAND $<TARGET_FILE:StreamingRuntimeTests>
      relocatedRuntime:missing-manifest relocatedRuntime:initialize
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  create_test(NAME StreamingPacerTests DEPENDS streaming SOURCE StreamingPacerTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  create_test(NAME StreamingMediaTests DEPENDS streaming-capture SOURCE StreamingMediaTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(StreamingMediaTests PROPERTIES LABELS streaming-extended)
  add_test(NAME StreamingMediaQuickTests COMMAND $<TARGET_FILE:StreamingMediaTests>
    epochZeroAudioOutput invalidNegotiation quantizedThirtyFps transport:video-only transport:audio-video
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  if(WIN32)
    target_link_libraries(StreamingMediaTests psapi)
  endif()
  create_test(NAME StreamingDecodeProgressPolicyTests DEPENDS streaming SOURCE StreamingDecodeProgressPolicyTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  create_test(NAME StreamingDecodeProgressTests DEPENDS streaming-capture SOURCE StreamingDecodeProgressTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(StreamingDecodeProgressTests PROPERTIES LABELS streaming-extended)
  create_test(NAME StreamingAudioTests DEPENDS streaming-capture SOURCE StreamingAudioTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  create_test(NAME StreamingFileTimelineTests DEPENDS streaming-capture SOURCE StreamingFileTimelineTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  create_test(NAME StreamingFileTests DEPENDS streaming-capture SOURCE StreamingFileTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  create_test(NAME StreamingCapturePipelineTests DEPENDS streaming-capture SOURCE StreamingCapturePipelineTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()

create_test(NAME StreamingContextTests DEPENDS streaming SOURCE StreamingContextTests.cpp
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
create_test(NAME StreamingControlTransportTests DEPENDS streaming SOURCE StreamingControlTransportTests.cpp
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set_tests_properties(StreamingControlTransportTests PROPERTIES LABELS streaming-extended)
