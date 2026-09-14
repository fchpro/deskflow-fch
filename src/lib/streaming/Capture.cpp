// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "Capture.h"
namespace deskflow::streaming {
bool validCaptureGeneration(const QString &id)
{
  if (id.size() != 32)
    return false;
  for (const auto c : id) {
    if (!(c >= u'0' && c <= u'9') && !(c >= u'a' && c <= u'f'))
      return false;
  }
  return true;
}
QString captureStateName(CaptureState state)
{
  switch (state) {
  case CaptureState::Available: return "available";
  case CaptureState::Starting: return "starting";
  case CaptureState::PermissionRequired: return "permissionRequired";
  case CaptureState::Denied: return "denied";
  case CaptureState::Unsupported: return "unsupported";
  case CaptureState::SourceGone: return "sourceGone";
  case CaptureState::ProtectedOrUnavailable: return "protectedOrUnavailable";
  case CaptureState::TemporarilyUnavailable: return "temporarilyUnavailable";
  case CaptureState::BackendMissing: return "backendMissing";
  case CaptureState::Stopped: return "stopped";
  }
  return "unsupported";
}
} // namespace deskflow::streaming
