// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QRect>
#include <QString>
#include <QVector>
#include <memory>
#include <optional>

namespace deskflow::streaming {
enum class CaptureState { Available, Starting, PermissionRequired, Denied, Unsupported, SourceGone,
                          ProtectedOrUnavailable, TemporarilyUnavailable, BackendMissing, Stopped };
struct CaptureStatus {
  CaptureState state = CaptureState::Stopped;
  QString reason;
};
struct CaptureSource {
  QString id; // opaque enumeration generation, never a title or a reusable native handle
  QString kind; // screen or window
  QString title;
  QRect physicalGeometry;
  double scale = 1.0;
  CaptureStatus status;
  quint32 audioProcessId = 0;
  quint64 audioProcessBirth = 0; // native process creation identity; zero means unavailable
};
struct VideoFrame {
  QImage pixels; // owned BGRA/RGBA SDR pixels; never points into a released native buffer
  QString session;
  QString source;
  quint64 sequence = 0;
  qint64 captureTimeNs = 0; // monotonic host clock; not a wall clock or cross-host timestamp
  qint64 mediaTimeNs = 0;
  quint64 timelineEpoch = 0; // file seek flush generation; independent of geometry/source identity
  quint64 geometryGeneration = 0;
  QRect physicalGeometry;
  double scale = 1.0;
  bool coordinateMappingValid = false; // control must remain disabled until frame/native geometry is verified
};

// Create and use on the GUI media worker (with a Qt event loop), never on the input thread.
// Caller MUST bind start to the broker's accepted session; enumeration does not grant capture.
// Poll takeFrame(): a single latest frame is retained. Stop discards it synchronously.
class CaptureDevice : public QObject {
  Q_OBJECT
public:
  using QObject::QObject;
  virtual QVector<CaptureSource> sources() = 0;
  virtual bool start(const QString &source, const QString &session, int fps = 30) = 0;
  virtual void stop() = 0;
  virtual std::optional<VideoFrame> takeFrame() = 0;
  virtual CaptureStatus status() const = 0;
  virtual QJsonObject controlTarget() const { return {}; }
Q_SIGNALS:
  void statusChanged(); // consume status(); terminal states require broker Stop before a fresh start
};
std::unique_ptr<CaptureDevice> createCaptureDevice();
bool validCaptureGeneration(const QString &id);
QString captureStateName(CaptureState state);
} // namespace deskflow::streaming

Q_DECLARE_METATYPE(deskflow::streaming::VideoFrame)
