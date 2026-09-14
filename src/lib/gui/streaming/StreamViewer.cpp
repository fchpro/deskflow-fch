// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "StreamViewer.h"
#include "streaming/Control.h"
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QFocusEvent>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

namespace deskflow::gui {
namespace {
QLabel *label(const QString &text, const QString &name, QWidget *parent) {
  auto *result = new QLabel(text, parent); result->setObjectName(name);
  result->setTextFormat(Qt::PlainText); result->setWordWrap(true); return result;
}
QString timeText(qint64 ms) { return QString("%1:%2").arg(ms / 60000).arg(ms / 1000 % 60, 2, 10, QLatin1Char('0')); }
}
VideoSurface::VideoSurface(QWidget *parent) : QWidget(parent) {
  setObjectName("receivedVideo"); setMinimumSize(240, 135);
  setFocusPolicy(Qt::StrongFocus);setMouseTracking(true);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); setAccessibleName(tr("Received stream video"));
}
void VideoSurface::setFrame(const QImage &frame) { m_frame = frame; if(frame.isNull() || frame.cacheKey()!=m_presented.pixels.cacheKey())m_presented={}; update(); }
void VideoSurface::setPresentedFrame(const streaming::VideoFrame &frame){m_presented=frame;m_frame=frame.pixels;update();}
void VideoSurface::setControlEnabled(bool enabled){m_control=enabled;}
void VideoSurface::pointerEvent(const QString &kind,const QPointF &position,int code,bool down,int dx,int dy){
  if(!m_control || !m_presented.coordinateMappingValid)return;
  const auto point=streaming::mapControlPoint(videoRect(),m_presented.physicalGeometry,position);
  if(!point){if(kind=="button" && !down)Q_EMIT revokeRequested();return;}
  QJsonObject event{{"kind",kind},{"x",point->x()},{"y",point->y()}};
  if(kind=="button"){event["code"]=code;event["down"]=down;}
  if(kind=="wheel"){event["dx"]=dx;event["dy"]=dy;}
  Q_EMIT inputRequested(event,m_presented);
}
namespace { int controlButton(Qt::MouseButton button){return button==Qt::LeftButton?1:button==Qt::RightButton?2:button==Qt::MiddleButton?3:0;} }
void VideoSurface::mousePressEvent(QMouseEvent *e){setFocus();pointerEvent("button",e->position(),controlButton(e->button()),true);e->accept();}
void VideoSurface::mouseReleaseEvent(QMouseEvent *e){pointerEvent("button",e->position(),controlButton(e->button()),false);e->accept();}
void VideoSurface::mouseMoveEvent(QMouseEvent *e){pointerEvent("move",e->position());e->accept();}
void VideoSurface::wheelEvent(QWheelEvent *e){pointerEvent("wheel",e->position(),0,false,e->angleDelta().x(),e->angleDelta().y());e->accept();}
void VideoSurface::keyEvent(QKeyEvent *e,bool down){
  if(e->key()==Qt::Key_Escape || e->key()==Qt::Key_F11 || (e->key()==Qt::Key_Space && e->modifiers().testFlag(Qt::ControlModifier))){Q_EMIT revokeRequested();e->ignore();return;}
  if(!m_control || !m_presented.coordinateMappingValid || (e->isAutoRepeat() && !down)){e->accept();return;}
  int code=0;
#ifdef Q_OS_WIN
  code=int(e->nativeVirtualKey());
  if(code==17)code=(e->nativeScanCode()&0x100)?163:162;
  if(code==18)code=(e->nativeScanCode()&0x100)?165:164;
  if(code==16)code=(e->nativeScanCode()&0xff)==0x36?161:160;
#elif defined(Q_OS_MACOS)
  // Qt swaps Command/Control semantics on macOS; preserve physical modifier sides.
  switch(e->nativeVirtualKey()){case 59:code=162;break;case 62:code=163;break;case 55:code=91;break;case 54:code=92;break;
    case 56:code=160;break;case 60:code=161;break;case 58:code=164;break;case 61:code=165;break;default:break;}
#else
  // X11/Wayland native virtual keys are XKB keysyms rather than Windows virtual keys.
  switch(e->nativeVirtualKey()){case 0xffe1:code=160;break;case 0xffe2:code=161;break;case 0xffe3:code=162;break;case 0xffe4:code=163;break;
    case 0xffe9:code=164;break;case 0xffea:code=165;break;case 0xffeb:code=91;break;case 0xffec:code=92;break;default:break;}
#endif
  if(!code){const int key=e->key();
    if(key==Qt::Key_Space || (key>=Qt::Key_0 && key<=Qt::Key_9) || (key>=Qt::Key_A && key<=Qt::Key_Z))code=key;
    else if(key>=Qt::Key_F1 && key<=Qt::Key_F24)code=112+key-Qt::Key_F1;
    else switch(key){case Qt::Key_Shift:code=160;break;case Qt::Key_Control:code=162;break;case Qt::Key_Alt:code=164;break;case Qt::Key_Meta:code=91;break;
      case Qt::Key_Return:case Qt::Key_Enter:code=13;break;case Qt::Key_Backspace:code=8;break;case Qt::Key_Tab:code=9;break;
      case Qt::Key_Left:code=37;break;case Qt::Key_Up:code=38;break;case Qt::Key_Right:code=39;break;case Qt::Key_Down:code=40;break;
      case Qt::Key_Delete:code=46;break;case Qt::Key_Home:code=36;break;case Qt::Key_End:code=35;break;case Qt::Key_PageUp:code=33;break;case Qt::Key_PageDown:code=34;break;case Qt::Key_Insert:code=45;break;
      case Qt::Key_Semicolon:case Qt::Key_Colon:code=186;break;case Qt::Key_Equal:case Qt::Key_Plus:code=187;break;case Qt::Key_Comma:case Qt::Key_Less:code=188;break;
      case Qt::Key_Minus:case Qt::Key_Underscore:code=189;break;case Qt::Key_Period:case Qt::Key_Greater:code=190;break;case Qt::Key_Slash:case Qt::Key_Question:code=191;break;
      case Qt::Key_QuoteLeft:case Qt::Key_AsciiTilde:code=192;break;case Qt::Key_BracketLeft:case Qt::Key_BraceLeft:code=219;break;case Qt::Key_Backslash:case Qt::Key_Bar:code=220;break;
      case Qt::Key_BracketRight:case Qt::Key_BraceRight:code=221;break;case Qt::Key_Apostrophe:case Qt::Key_QuoteDbl:code=222;break;default:break;}
  }
  if(code>0 && code<256)Q_EMIT inputRequested({{"kind",e->isAutoRepeat()?"repeat":"key"},{"code",code},{"down",down}},m_presented);
  e->accept();
}
void VideoSurface::keyPressEvent(QKeyEvent *e){keyEvent(e,true);}
void VideoSurface::keyReleaseEvent(QKeyEvent *e){keyEvent(e,false);}
void VideoSurface::focusInEvent(QFocusEvent *e){Q_EMIT focusChanged(true);QWidget::focusInEvent(e);}
void VideoSurface::focusOutEvent(QFocusEvent *e){m_control=false;Q_EMIT focusChanged(false);Q_EMIT revokeRequested();QWidget::focusOutEvent(e);}
QRect VideoSurface::videoRect() const {
  if (m_frame.isNull()) return {};
  QRect area(QPoint{}, m_frame.size().scaled(size(), Qt::KeepAspectRatio)); area.moveCenter(rect().center()); return area;
}
void VideoSurface::paintEvent(QPaintEvent *) {
  QPainter painter(this); painter.fillRect(rect(), Qt::black);
  if (!m_frame.isNull()) painter.drawImage(videoRect(), m_frame);
  else { painter.setPen(Qt::white); painter.drawText(rect(), Qt::AlignCenter, tr("No received video")); }
}
PlaybackPanel::PlaybackPanel(SenderController *controller, QWidget *parent) : QWidget(parent), m_controller(controller) {
  setObjectName("playbackPanel"); auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0,0,0,0);
  m_permission = label({}, "playbackPermission", this); layout->addWidget(m_permission);
  auto *row = new QHBoxLayout; layout->addLayout(row);
  m_play = new QPushButton(tr("&Pause"), this); m_play->setObjectName("filePlayPause"); row->addWidget(m_play);
  m_seek = new QSlider(Qt::Horizontal, this); m_seek->setObjectName("fileSeek");
  m_seek->setAccessibleName(tr("Video file position")); m_seek->setTracking(false); row->addWidget(m_seek, 1);
  m_time = label({}, "fileTime", this); row->addWidget(m_time);
  connect(m_play, &QPushButton::clicked, this, &PlaybackPanel::toggle);
  connect(m_seek, &QSlider::valueChanged, this, [this](int position) { m_controller->playbackCommand("seek", position); });
  connect(controller, &SenderController::playbackChanged, this, &PlaybackPanel::updateState); updateState();
}
void PlaybackPanel::toggle() {
  if (m_play->isEnabled()) m_controller->playbackCommand(m_controller->playbackState()["paused"].toBool() ? "resume" : "pause");
}
void PlaybackPanel::updateState() {
  const auto state = m_controller->playbackState(); const bool allowed = state["allowed"].toBool();
  m_permission->setText(state.isEmpty() ? tr("Waiting for file timeline…") : allowed
    ? tr("File playback controls authorized") : tr("Playback controlled by sender"));
  m_play->setEnabled(allowed); m_play->setText(state["paused"].toBool() ? tr("&Play") : tr("&Pause"));
  m_seek->setEnabled(allowed && state["seekable"].toBool());
  const QSignalBlocker blocker(m_seek);
  m_seek->setRange(0, qMax(0, state["durationMs"].toInt() - 1));
  if (!m_seek->isSliderDown()) m_seek->setValue(state["positionMs"].toInt());
  m_time->setText(timeText(state["positionMs"].toInteger()) + " / " + timeText(state["durationMs"].toInteger()));
}
StreamViewer::StreamViewer(SenderController *controller, const QJsonObject &offer, QWidget *parent)
    : QDialog(parent), m_controller(controller) {
  setObjectName("streamViewer"); setWindowTitle(tr("Incoming stream — Deskflow"));
  resize(900, 640); setMinimumSize(420, 360); setModal(false);
  auto *layout = new QVBoxLayout(this);
  layout->addWidget(label(tr("From: %1\nSource: %2 (%3)\nAudio: %4")
    .arg(offer["senderName"].toString(), offer["title"].toString().isEmpty() ? offer["source"].toString() : offer["title"].toString(),
      offer["kind"].toString(), offer["audio"].toString()), "receiverIdentity", this));
  layout->addWidget(label(tr("Viewing only — desktop input disabled"), "receiverMode", this));
  auto *status = label(controller->status(), "receiverStatus", this); layout->addWidget(status);
  connect(controller, &SenderController::statusChanged, this, [this, controller, status] {
    if (m_live) status->setText(controller->status());
  });
  auto *consent = new QWidget(this); consent->setObjectName("receiverConsent"); auto *choices = new QHBoxLayout(consent);
  auto *endpoint = new QComboBox(consent); endpoint->setObjectName("receiverEndpoint"); endpoint->setAccessibleName(tr("Playback audio output endpoint"));
  endpoint->addItem(tr("Select audio output…"), QString{});
  for (const auto &entry : offer["endpoints"].toArray()) endpoint->addItem(entry.toObject()["name"].toString(), entry.toObject()["id"].toString());
  endpoint->setVisible(offer["audio"] != "off"); choices->addWidget(endpoint, 1);
  auto *accept = new QPushButton(tr("&Accept stream"), consent); accept->setObjectName("acceptStream"); choices->addWidget(accept);
  auto *decline = new QPushButton(tr("&Decline"), consent); decline->setObjectName("declineStream"); choices->addWidget(decline);
  auto valid = [offer, endpoint] { return offer["audio"] == "off" || !endpoint->currentData().toString().isEmpty(); };
  accept->setEnabled(valid()); connect(endpoint, &QComboBox::currentIndexChanged, this, [accept, valid] { accept->setEnabled(valid()); });
  connect(accept, &QPushButton::clicked, this, [this, controller, endpoint] {
    if (m_live) controller->accept(endpoint->currentData().toString());
  });
  connect(controller, &SenderController::inventoryChanged, this, [controller, consent, offer] {
    const auto data = controller->inventory();
    if (data["session"] == offer["session"] && data["state"] != "awaitingConsent") consent->hide();
  });
  connect(decline, &QPushButton::clicked, this, &StreamViewer::close); layout->addWidget(consent);
  m_surface = new VideoSurface(this); layout->addWidget(m_surface, 1);
  connect(controller, &SenderController::viewerFrame, this, [this](const QImage &image) { if (m_live) m_surface->setFrame(image); });
  connect(controller,&SenderController::presentedFrame,this,[this,offer](const streaming::VideoFrame &frame){
    if(m_live && frame.session==offer["session"] && frame.source==offer["source"])m_surface->setPresentedFrame(frame);
  });
  connect(m_surface,&VideoSurface::inputRequested,controller,&SenderController::controlInput);
  connect(m_surface,&VideoSurface::focusChanged,controller,[controller](bool focused){if(focused)controller->viewerFocus(true);});
  connect(m_surface,&VideoSurface::revokeRequested,controller,&SenderController::revokeControl);
  connect(controller,&SenderController::controlChanged,this,[this](bool granted){
    if(!m_live)return;if(granted)m_surface->setFocus();m_surface->setControlEnabled(granted);
    findChild<QLabel *>("receiverMode")->setText(granted?tr("Interactive control granted - Escape or focus loss releases control"):tr("Viewing only - desktop input disabled"));
  });
  m_playback = new PlaybackPanel(controller, this); m_playback->setVisible(offer["kind"] == "file"); layout->addWidget(m_playback);
  auto *row = new QHBoxLayout; layout->addLayout(row);
  auto *volume = new QSlider(Qt::Horizontal, this); volume->setObjectName("receiverVolume"); volume->setRange(0, 100); volume->setValue(100);
  volume->setAccessibleName(tr("Receiver volume")); row->addWidget(label(tr("Volume"), "volumeLabel", this)); row->addWidget(volume, 1);
  auto *mute = new QCheckBox(tr("&Mute"), this); mute->setObjectName("receiverMute"); row->addWidget(mute);
  volume->setEnabled(offer["audio"] != "off"); mute->setEnabled(offer["audio"] != "off");
  auto gain = [this, controller, volume, mute] { if (m_live) controller->volume(volume->value() / 100.0, mute->isChecked()); };
  connect(volume, &QSlider::valueChanged, this, gain); connect(mute, &QCheckBox::toggled, this, gain);
  m_fullscreen = new QPushButton(tr("&Fullscreen (F11)"), this); m_fullscreen->setObjectName("receiverFullscreen"); row->addWidget(m_fullscreen);
  connect(m_fullscreen, &QPushButton::clicked, this, &StreamViewer::toggleFullscreen);
  auto *stop = new QPushButton(tr("&Stop stream"), this); stop->setObjectName("receiverStop"); row->addWidget(stop);
  connect(stop, &QPushButton::clicked, this, [this, controller] { if (m_live) controller->stop(); });
  auto retire = [this, consent, volume, mute, stop] {
    m_live = false; consent->hide(); m_surface->setFrame({}); m_playback->setEnabled(false);
    volume->setEnabled(false); mute->setEnabled(false); stop->setEnabled(false);
  };
  connect(controller, &SenderController::incoming, this, [offer, retire](const QJsonObject &next) {
    if (next["session"] != offer["session"] || next["source"] != offer["source"]) retire();
  });
  connect(controller, &SenderController::statusChanged, this, [controller, retire] {
    if (!controller->active()) retire();
  });
  auto *fullscreenKey = new QShortcut(QKeySequence(Qt::Key_F11), this); fullscreenKey->setAutoRepeat(false);
  connect(fullscreenKey, &QShortcut::activated, this, &StreamViewer::toggleFullscreen);
  auto *escapeKey = new QShortcut(QKeySequence(Qt::Key_Escape), this); escapeKey->setAutoRepeat(false);
  connect(escapeKey, &QShortcut::activated, this, &StreamViewer::reject);
  auto *playKey = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Space), this); playKey->setAutoRepeat(false);
  connect(playKey, &QShortcut::activated, this, [this]{if(m_live)m_controller->revokeControl();m_playback->toggle();});
}
void StreamViewer::toggleFullscreen() {
  if(m_live)m_controller->revokeControl();
  if (isFullScreen()) showNormal(); else showFullScreen();
  m_fullscreen->setText(isFullScreen() ? tr("Exit fullscreen") : tr("&Fullscreen (F11)"));
  m_fullscreen->setToolTip(tr("F11 toggles fullscreen; Escape exits fullscreen."));
}
void StreamViewer::reject() { if(m_live)m_controller->revokeControl(); if (isFullScreen()) toggleFullscreen(); else close(); }
bool StreamViewer::event(QEvent *event){
  if(m_live && event->type()==QEvent::WindowActivate)m_controller->viewerFocus(true);
  if(m_live && event->type()==QEvent::WindowDeactivate){m_controller->viewerFocus(false);m_controller->revokeControl();}
  return QDialog::event(event);
}
void StreamViewer::closeEvent(QCloseEvent *event) { if (m_live) m_controller->stop(); event->accept(); }
} // namespace deskflow::gui
