# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

add_subdirectory(config)
add_subdirectory(core)

create_test(
  NAME LoggerTests
  DEPENDS gui
  SOURCE LoggerTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME KeySequenceTests
  DEPENDS gui
  SOURCE KeySequenceTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME ExcludedAppsDialogTests
  DEPENDS gui
  SOURCE ExcludedAppsDialogTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

if(BUILD_STREAMING)
  create_test(NAME StreamingCaptureLifetimeTests DEPENDS gui SOURCE StreamingCaptureLifetimeTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  create_test(NAME StreamingAutoAcceptTests DEPENDS gui SOURCE StreamingAutoAcceptTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  create_test(NAME StreamingRecoveryTests DEPENDS gui SOURCE StreamingRecoveryTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  if(WIN32)
    create_test(NAME StreamingRecoveryResourceTests DEPENDS gui LIBS psapi iphlpapi SOURCE StreamingRecoveryResourceTests.cpp
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(StreamingRecoveryResourceTests PROPERTIES LABELS streaming-extended)
    add_dependencies(StreamingRecoveryResourceTests StreamingSenderTests)
  endif()
  create_test(NAME StreamingControlBoundaryTests DEPENDS gui SOURCE StreamingControlBoundaryTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  create_test(NAME StreamingControlLifecycleTests DEPENDS gui SOURCE StreamingControlLifecycleTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  create_test(NAME StreamingControlTests DEPENDS gui SOURCE StreamingControlTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  create_test(NAME StreamingViewerTests DEPENDS gui SOURCE StreamingViewerTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  set_tests_properties(StreamingViewerTests PROPERTIES LABELS streaming-extended)
  add_test(NAME StreamingViewerQuickTests COMMAND StreamingViewerTests brokerPermission brokerTimeline surfaceAspect presentation clipping packetDecode packetOwnership metadataBinding epochAudioReadiness permanentAudioRefusal
    staleViewerPixels staleViewerClose unavailableOutputRetry fullscreenLabelFits chosenMuteSurvivesAccept chosenGainSurvivesAccept
    silentFileSeek outputGain receiverGuards realReceiverWorkflow playout timelineEndExcluded receiverPlayoutClock queuedEpochKeyframe queuedPlayback)
  set_tests_properties(StreamingViewerQuickTests PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  create_test(NAME StreamingSenderTests DEPENDS gui SOURCE StreamingSenderTests.cpp
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
