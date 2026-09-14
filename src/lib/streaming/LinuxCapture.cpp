// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "Capture.h"
namespace deskflow::streaming {
std::unique_ptr<CaptureDevice> createPortalCaptureDevice();
std::unique_ptr<CaptureDevice> createX11CaptureDevice();
#ifndef DESKFLOW_CAPTURE_GSTREAMER
class DisabledCapture final : public CaptureDevice {
public:
  QVector<CaptureSource> sources() override { return {}; }
  bool start(const QString &, const QString &, int) override { return false; }
  void stop() override {}
  std::optional<VideoFrame> takeFrame() override { return {}; }
  CaptureStatus status() const override { return {CaptureState::BackendMissing, "Build with BUILD_STREAMING=ON"}; }
};
#endif
std::unique_ptr<CaptureDevice> createCaptureDevice()
{
#ifdef DESKFLOW_CAPTURE_GSTREAMER
  if (qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland" || !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
    return createPortalCaptureDevice();
  return createX11CaptureDevice();
#else
  return std::make_unique<DisabledCapture>();
#endif
}
}
