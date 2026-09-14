// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <gst/video/video.h>
namespace deskflow::streaming {
inline void requestVideoKeyframe(GstElement *encoder, GstClockTime runningTime = GST_CLOCK_TIME_NONE) {
  auto *pad = gst_element_get_static_pad(encoder, "src");
  gst_pad_send_event(pad, gst_video_event_new_upstream_force_key_unit(runningTime, TRUE, 0));
  gst_object_unref(pad);
}
}
