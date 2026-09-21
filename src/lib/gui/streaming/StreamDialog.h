// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "SenderController.h"
#include <QDialog>
class QComboBox;
class QListWidget;
class QLabel;
class QPushButton;
class QTabBar;
class QCheckBox;
namespace deskflow::gui {
class StreamDialog : public QDialog {
  Q_OBJECT
public:
  explicit StreamDialog(SenderController *controller, QWidget *parent = nullptr);
  QJsonObject selection() const;
  void selectLocalFile(const QString &path);
private:
  void populate();
  void validate();
  SenderController *m_controller;
  QTabBar *m_kind;
  QListWidget *m_sources;
  QPushButton *m_fileButton, *m_start, *m_stop, *m_refresh;
  QComboBox *m_target, *m_audio, *m_device, *m_quality;
  QLabel *m_fileName, *m_preview, *m_reason, *m_status;
  QString m_path;
  QCheckBox *m_playbackPermission;
};
// Reusable actual main-window entry/status surface; closing the selector leaves Stop visible.
class StreamLauncher : public QWidget {
  Q_OBJECT
public:
  explicit StreamLauncher(SenderController *controller, QWidget *parent = nullptr, bool automaticAcceptance = false);
};
} // namespace deskflow::gui
