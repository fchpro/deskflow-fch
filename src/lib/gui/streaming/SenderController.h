// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "streaming/PrivateIpc.h"
#include <QImage>
#include "streaming/Capture.h"
#include <QThread>
#include <functional>

namespace deskflow::gui {
// Shared validation for the visible Start action and worker-side admission.
QString senderSelectionError(const QJsonObject &inventory, const QJsonObject &selection);
class SenderWorker;
class SenderController : public QObject {
  Q_OBJECT
  friend class StreamingViewerTests;
  friend class StreamingRecoveryTests;
public:
  explicit SenderController(streaming::SessionClient *session, QObject *parent = nullptr);
  ~SenderController() override;
  QJsonObject inventory() const { return m_inventory; }
  QString status() const { return m_status; }
  bool active() const { return m_active; }
  void refresh();
  void start(const QJsonObject &selection);
  void stop();
  void accept(const QString &endpoint);
  void playbackCommand(const QString &action, qint64 positionMs = 0);
  void volume(double gain, bool muted);
  void grantControl();
  void revokeControl();
  void viewerFocus(bool focused);
  void controlInput(const QJsonObject &event, const streaming::VideoFrame &presented);
  QJsonObject playbackState() const { return m_playback; }
Q_SIGNALS:
  void inventoryChanged();
  void statusChanged();
  void preview(const QImage &image);
  void incoming(const QJsonObject &offer);
  void viewerFrame(const QImage &image);
  void presentedFrame(const streaming::VideoFrame &frame);
  void controlChanged(bool granted);
  void playbackChanged();
private:
  void queueSessionAction(std::function<void(SenderWorker &)> action);
  QString m_pendingStart;
  QPointer<streaming::SessionClient> m_sessionClient;
  QThread m_thread;
  SenderWorker *m_worker;
  QJsonObject m_inventory;
  QJsonObject m_playback;
  QString m_status = tr("Start Deskflow in desktop mode with mutually trusted peers.");
  bool m_active = false;
};
} // namespace deskflow::gui
