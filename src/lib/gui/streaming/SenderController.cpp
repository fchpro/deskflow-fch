// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "SenderController.h"
#include "SenderWorker.h"
#include <QJsonArray>

namespace deskflow::gui {
QString senderSelectionError(const QJsonObject &inventory, const QJsonObject &selection)
{
  if (!inventory["built"].toBool()) return QObject::tr("Streaming media is not included in this build.");
  if (!inventory["connected"].toBool() || inventory["id"].toString().isEmpty())
    return QObject::tr("No authenticated streaming connection. Use desktop mode and mutually trusted compatible peers.");
  if (inventory["busy"].toBool()) return QObject::tr("This computer already has a stream. Stop it first.");
  QJsonObject peer;
  for (const auto &entry : inventory["peers"].toArray())
    if (entry.toObject()["id"] == selection["to"]) peer = entry.toObject();
  if (peer.isEmpty() || peer["id"] == inventory["id"] || !peer["capabilities"].toObject()["receive"].toBool())
    return QObject::tr("Select a connected computer with a compatible receiving viewer.");
  const auto kind = selection["kind"].toString();
  QJsonObject source;
  if (kind == "file") {
    if (selection["path"].toString().isEmpty()) return QObject::tr("Choose a local video file.");
  } else {
    for (const auto &entry : inventory["sources"].toArray())
      if (entry.toObject()["id"] == selection["source"] && entry.toObject()["kind"] == kind)
        source = entry.toObject();
    if (source.isEmpty()) return QObject::tr("No source selected. Refresh available sources.");
    if (!source["available"].toBool()) return source["reason"].toString().isEmpty()
      ? QObject::tr("The selected source is unavailable.") : source["reason"].toString();
  }
  const auto audio = selection["audio"].toString();
  if (!peer["capabilities"].toObject()["audio"].toArray().contains(audio))
    return QObject::tr("The selected receiver does not support this audio scope.");
  if (audio != "off") {
    if (kind == "file" && audio != "file") return QObject::tr("Video files support only file audio or audio off.");
    if (kind != "file" && audio != "system" && audio != "application")
      return QObject::tr("Select system audio, application audio or audio off.");
    if (audio == "application" && !source["applicationAudio"].toBool())
      return QObject::tr("This source has no verified process association for application audio.");
    if (audio == "system" && !inventory["systemAudio"].toBool())
      return QObject::tr("System audio capture is unavailable.");
    if (audio == "system" && inventory["endpointRequired"].toBool()) {
      bool found = false;
      for (const auto &entry : inventory["endpoints"].toArray())
        found |= entry.toObject()["id"] == selection["device"];
      if (!found) return QObject::tr("Select an available audio output endpoint to capture.");
    }
  }
  if (!QStringList{"low", "balanced", "smooth"}.contains(selection["preset"].toString()))
    return QObject::tr("Select a quality preset.");
  if (selection["interactive"].toBool() && (kind=="file" || !inventory["control"].toBool() || !peer["capabilities"].toObject()["control"].toBool()))
    return QObject::tr("Interactive control is unavailable for the selected source or peer.");
  return {};
}

SenderController::SenderController(streaming::SessionClient *session, QObject *parent)
    : QObject(parent), m_sessionClient(session), m_worker(new SenderWorker)
{
  qRegisterMetaType<streaming::VideoFrame>();
  m_worker->moveToThread(&m_thread);
  connect(m_worker,&SenderWorker::presentedFrame,this,&SenderController::presentedFrame);
  connect(m_worker,&SenderWorker::controlChanged,this,&SenderController::controlChanged);
  connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
  connect(session, &streaming::SessionClient::received, m_worker, &SenderWorker::receive);
  connect(session, &streaming::SessionClient::connectedChanged, m_worker, &SenderWorker::connection);
  connect(m_worker, &SenderWorker::outgoing, session, [session](const auto &frame) { session->send(frame); });
  connect(m_worker, &SenderWorker::inventory, this, [this](const auto &value) {
    m_inventory = value;
    Q_EMIT inventoryChanged();
  });
  connect(m_worker, &SenderWorker::status, this, [this](const QString &text, bool active) {
    m_status = text; m_active = active;
    Q_EMIT statusChanged();
  });
  connect(m_worker, &SenderWorker::preview, this, [this](const QImage &image) {
    Q_EMIT preview(image);
    QMetaObject::invokeMethod(m_worker, &SenderWorker::acknowledgePreview, Qt::QueuedConnection);
  });
  connect(m_worker, &SenderWorker::incoming, this, &SenderController::incoming);
  connect(m_worker, &SenderWorker::playback, this, [this](const QJsonObject &value) {
    m_playback = value; Q_EMIT playbackChanged();
  });
  connect(m_worker, &SenderWorker::viewerFrame, this, [this](const QImage &image) {
    Q_EMIT viewerFrame(image);
    QMetaObject::invokeMethod(m_worker, &SenderWorker::acknowledgeViewer, Qt::QueuedConnection);
  });
  m_thread.start();
  QMetaObject::invokeMethod(m_worker, [this, connected = session->connected()] { m_worker->connection(connected); }, Qt::QueuedConnection);
}
SenderController::~SenderController()
{
  // The GUI cannot process queued Stop writes while joining the worker. Closing
  // this owner's private attachment revokes the core lease before native teardown.
  if (m_sessionClient) m_sessionClient->shutdown();
  QMetaObject::invokeMethod(m_worker, [this] { m_worker->stop({}, false); }, Qt::BlockingQueuedConnection);
  m_thread.quit(); m_thread.wait();
}
void SenderController::refresh() { QMetaObject::invokeMethod(m_worker, &SenderWorker::refresh, Qt::QueuedConnection); }
void SenderController::start(const QJsonObject &selection)
{
  if(!m_pendingStart.isEmpty())return;
  if(m_inventory["busy"].toBool()){
    QMetaObject::invokeMethod(m_worker,[this,selection]{m_worker->start(selection);},Qt::QueuedConnection);return;
  }
  const auto requested=streaming::randomId();m_pendingStart=requested;
  QMetaObject::invokeMethod(m_worker, [this, selection, requested] {
    m_worker->start(selection,requested);
    QMetaObject::invokeMethod(this,[this,requested]{if(m_pendingStart==requested)m_pendingStart.clear();},Qt::QueuedConnection);
  }, Qt::QueuedConnection);
}
void SenderController::queueSessionAction(std::function<void(SenderWorker &)> action) {
  const auto session=m_pendingStart.isEmpty()?m_inventory["session"].toString():m_pendingStart;
  QMetaObject::invokeMethod(m_worker,[this,session,action=std::move(action)]{
    if(m_worker->sessionMatches(session))action(*m_worker);
  },Qt::QueuedConnection);
}
void SenderController::stop() { queueSessionAction([](SenderWorker &worker){worker.stop();}); }
void SenderController::accept(const QString &endpoint) {
  queueSessionAction([endpoint](SenderWorker &worker){worker.accept(endpoint);});
}
void SenderController::playbackCommand(const QString &action, qint64 positionMs) {
  queueSessionAction([action,positionMs](SenderWorker &worker){worker.playbackCommand(action,positionMs);});
}
void SenderController::volume(double gain, bool muted) {
  queueSessionAction([gain,muted](SenderWorker &worker){worker.volume(gain,muted);});
}
void SenderController::grantControl(){queueSessionAction([](SenderWorker &worker){worker.grantControl();});}
void SenderController::revokeControl(){queueSessionAction([](SenderWorker &worker){worker.revokeControl();});}
void SenderController::viewerFocus(bool focused){queueSessionAction([focused](SenderWorker &worker){worker.viewerFocus(focused);});}
void SenderController::controlInput(const QJsonObject &event,const streaming::VideoFrame &presented){
  const auto lease=m_inventory["lease"].toString();
  queueSessionAction([event,presented,lease](SenderWorker &worker){if(worker.leaseMatches(lease))worker.controlInput(event,presented);});
}
} // namespace deskflow::gui
