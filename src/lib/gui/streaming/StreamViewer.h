// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "SenderController.h"
#include <QDialog>
class QLabel;
class QPushButton;
class QSlider;
namespace deskflow::gui {
class VideoSurface : public QWidget {
  Q_OBJECT
public:
  explicit VideoSurface(QWidget *parent = nullptr);
  void setFrame(const QImage &frame);
  void setPresentedFrame(const streaming::VideoFrame &frame);
  void setControlEnabled(bool enabled);
  QRect videoRect() const;
Q_SIGNALS:
  void inputRequested(const QJsonObject &event,const streaming::VideoFrame &presented);
  void focusChanged(bool focused);
  void revokeRequested();
protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void wheelEvent(QWheelEvent *) override;
  void keyPressEvent(QKeyEvent *) override;
  void keyReleaseEvent(QKeyEvent *) override;
  void focusInEvent(QFocusEvent *) override;
  void focusOutEvent(QFocusEvent *) override;
private:
  void pointerEvent(const QString &kind,const QPointF &position,int code=0,bool down=false,int dx=0,int dy=0);
  void keyEvent(QKeyEvent *,bool down);
  QImage m_frame;
  streaming::VideoFrame m_presented;
  bool m_control=false;
};
class PlaybackPanel : public QWidget {
  Q_OBJECT
public:
  explicit PlaybackPanel(SenderController *, QWidget *parent = nullptr);
  void toggle();
private:
  void updateState();
  SenderController *m_controller;
  QPushButton *m_play;
  QSlider *m_seek;
  QLabel *m_time, *m_permission;
};
class StreamViewer : public QDialog {
  Q_OBJECT
public:
  explicit StreamViewer(SenderController *, const QJsonObject &offer, QWidget *parent = nullptr);
  void toggleFullscreen();
protected:
  bool event(QEvent *) override;
  void closeEvent(QCloseEvent *) override;
  void reject() override;
private:
  SenderController *m_controller;
  VideoSurface *m_surface;
  PlaybackPanel *m_playback;
  QPushButton *m_fullscreen;
  bool m_live = true;
};
} // namespace deskflow::gui
