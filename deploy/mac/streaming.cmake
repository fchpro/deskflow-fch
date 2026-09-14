# SPDX-License-Identifier: MIT
# Native host execution is required; no Windows cross-build acceptance is implied.
pkg_get_variable(STREAMING_PLUGIN_DIR gstreamer-1.0 pluginsdir)
pkg_get_variable(STREAMING_SCANNER_DIR gstreamer-1.0 pluginscannerdir)
pkg_get_variable(STREAMING_PREFIX gstreamer-1.0 prefix)
if(NOT Qt6_VERSION VERSION_EQUAL "6.10.3")
  message(FATAL_ERROR "Refresh and verify streaming package notices before changing the pinned Qt 6.10.3 runtime")
endif()
if(NOT GSTREAMER_gstreamer-1.0_VERSION STREQUAL "1.28.7")
  message(FATAL_ERROR "The private media package requires GStreamer exactly 1.28.7")
endif()
set(STREAMING_MEDIA_LICENSE_DIR "${STREAMING_PREFIX}/share/licenses" CACHE PATH
  "Complete license notices for the selected GStreamer SDK and its bundled dependencies")
if(NOT IS_DIRECTORY "${STREAMING_MEDIA_LICENSE_DIR}")
  message(FATAL_ERROR "Set STREAMING_MEDIA_LICENSE_DIR to the selected SDK's complete dependency notices")
endif()
set(STREAMING_MAC_PLUGINS app coreelements audioconvert audioresample volume videoconvertscale
  playback typefindfunctions matroska isomp4 vpx libav videoparsersbad audioparsers
  opus opusparse vorbis ogg rtp rtpmanager rsrtp nice dtls srtp sctp webrtc osxaudio
  videotestsrc audiotestsrc)
set(STREAMING_PLUGIN_FILES)
foreach(plugin IN LISTS STREAMING_MAC_PLUGINS)
  set(plugin_file "${STREAMING_PLUGIN_DIR}/libgst${plugin}.dylib")
  if(NOT EXISTS "${plugin_file}")
    message(FATAL_ERROR "Required media plugin not found: ${plugin_file}")
  endif()
  list(APPEND STREAMING_PLUGIN_FILES "${plugin_file}")
endforeach()
if(NOT EXISTS "${STREAMING_SCANNER_DIR}/gst-plugin-scanner")
  message(FATAL_ERROR "The selected SDK's native plugin scanner is missing")
endif()
configure_file("${CMAKE_CURRENT_LIST_DIR}/streaming-install.cmake.in"
  "${CMAKE_BINARY_DIR}/streaming-install.cmake" @ONLY)
install(SCRIPT "${CMAKE_BINARY_DIR}/streaming-install.cmake")
