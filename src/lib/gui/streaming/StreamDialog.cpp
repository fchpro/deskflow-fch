// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "StreamDialog.h"
#include "StreamViewer.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonArray>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QTabBar>
#include <QVBoxLayout>

namespace deskflow::gui {
namespace {
QLabel *plain(const QString &text, QWidget *parent) {
  auto *label = new QLabel(text, parent); label->setTextFormat(Qt::PlainText); label->setWordWrap(true); return label;
}
void disableOption(QComboBox *box, int row, const QString &reason) {
  box->setItemData(row, reason, Qt::ToolTipRole);
  box->setItemData(row, 0, Qt::UserRole - 1);
}
}
StreamDialog::StreamDialog(SenderController *controller, QWidget *parent)
    : QDialog(parent), m_controller(controller)
{
  setWindowTitle(tr("Stream to another computer")); resize(700, 640);
  setObjectName("streamDialog");
  auto *outer = new QVBoxLayout(this);
  auto *scroll = new QScrollArea(this); scroll->setWidgetResizable(true);
  auto *content = new QWidget(scroll); auto *layout = new QVBoxLayout(content);
  scroll->setWidget(content); outer->addWidget(scroll, 1);
  layout->addWidget(plain(tr("Choose what to share"), this));
  m_kind = new QTabBar(this); m_kind->setObjectName("sourceTabs");
  m_kind->addTab(tr("Screen")); m_kind->addTab(tr("App / browser window")); m_kind->addTab(tr("Video file"));
  layout->addWidget(m_kind);
  m_sources = new QListWidget(this); m_sources->setObjectName("streamSources"); m_sources->setAccessibleName(tr("Available sources"));
  m_sources->setMaximumHeight(120); layout->addWidget(m_sources);
  m_fileButton = new QPushButton(tr("Choose &video file…"), this); m_fileButton->setObjectName("chooseVideoFile");
  m_fileName = plain(tr("No file selected"), this); m_fileName->setObjectName("streamFileName");
  layout->addWidget(m_fileButton); layout->addWidget(m_fileName);
  m_refresh = new QPushButton(tr("&Refresh sources and audio endpoints"), this); layout->addWidget(m_refresh);
  auto *form = new QFormLayout;
  m_target = new QComboBox(this); m_target->setObjectName("streamDestination"); form->addRow(tr("&Destination"), m_target);
  m_audio = new QComboBox(this); m_audio->setObjectName("streamAudio"); form->addRow(tr("&Audio"), m_audio);
  m_device = new QComboBox(this); m_device->setObjectName("streamAudioEndpoint"); form->addRow(tr("Audio &endpoint"), m_device);
  m_quality = new QComboBox(this); m_quality->setObjectName("streamQuality");
  m_quality->addItem(tr("Low — up to 720p / 30 FPS"), "low");
  m_quality->addItem(tr("Balanced — up to 1080p / 30 FPS"), "balanced");
  m_quality->addItem(tr("Smooth — up to 1080p / 60 FPS"), "smooth"); m_quality->setCurrentIndex(1);
  form->addRow(tr("&Quality"), m_quality); layout->addLayout(form);
  m_playbackPermission = new QCheckBox(tr("Allow receiver to play, pause and seek this video file"), this);
  m_playbackPermission->setObjectName("allowFilePlayback"); layout->addWidget(m_playbackPermission);
  layout->addWidget(plain(tr("Configured limits; 1080p performance has not yet been validated across computers."), this));
  auto *viewOnly = new QCheckBox(tr("Viewing only — remote keyboard and mouse disabled"), this);
  viewOnly->setObjectName("viewingOnly"); viewOnly->setChecked(true); viewOnly->setEnabled(false); layout->addWidget(viewOnly);
  auto *interactive = new QCheckBox(tr("Allow a separate desktop control grant after acceptance"), this);
  interactive->setObjectName("interactiveControl"); interactive->setEnabled(false); layout->addWidget(interactive);
  m_preview = plain(tr("Source information only. Live pixels appear after the receiver accepts."), this);
  m_preview->setObjectName("streamPreview"); m_preview->setMinimumSize(320, 150); m_preview->setAlignment(Qt::AlignCenter);
  m_preview->setFrameShape(QFrame::StyledPanel); layout->addWidget(m_preview);
  auto *playback = new PlaybackPanel(controller, this); layout->addWidget(playback);
  connect(m_kind, &QTabBar::currentChanged, playback, [this, playback] { playback->setVisible(m_kind->currentIndex() == 2); });
  playback->hide();
  m_reason = plain({}, this); m_reason->setObjectName("streamValidation"); outer->addWidget(m_reason);
  m_status = plain(controller->status(), this); m_status->setObjectName("streamStatus"); outer->addWidget(m_status);
  auto *buttons = new QHBoxLayout;
  m_start = new QPushButton(tr("&Start stream"), this); m_start->setObjectName("startStream");
  m_stop = new QPushButton(tr("S&top stream"), this); m_stop->setObjectName("stopStream");
  auto *close = new QPushButton(tr("&Close"), this);
  buttons->addWidget(m_start); buttons->addWidget(m_stop); buttons->addStretch(); buttons->addWidget(close); outer->addLayout(buttons);
  connect(close, &QPushButton::clicked, this, &QDialog::hide);
  connect(m_start, &QPushButton::clicked, this, [this] { m_controller->start(selection()); });
  connect(m_stop, &QPushButton::clicked, controller, &SenderController::stop);
  connect(m_refresh, &QPushButton::clicked, controller, &SenderController::refresh);
  connect(m_kind, &QTabBar::currentChanged, this, &StreamDialog::populate);
  connect(m_sources, &QListWidget::currentRowChanged, this, &StreamDialog::validate);
  for (auto *box : {m_target, m_audio, m_device, m_quality})
    connect(box, &QComboBox::currentIndexChanged, this, &StreamDialog::validate);
  connect(m_fileButton, &QPushButton::clicked, this, [this] {
    const auto file = QFileDialog::getOpenFileName(this, tr("Choose a local video"), {},
      tr("Video files (*.webm *.mp4 *.mov *.mkv);;All files (*)"));
    if (file.isEmpty()) return;
    selectLocalFile(file);
  });
  connect(controller, &SenderController::inventoryChanged, this, &StreamDialog::populate);
  connect(controller, &SenderController::statusChanged, this, [this] { m_status->setText(m_controller->status()); validate(); });
  connect(controller, &SenderController::preview, this, [this](const QImage &image) {
    if (image.isNull()) { m_preview->clear(); m_preview->setText(tr("Capture stopped. No live preview.")); }
    else m_preview->setPixmap(QPixmap::fromImage(image).scaled(m_preview->contentsRect().size(), Qt::KeepAspectRatio, Qt::FastTransformation));
  });
  populate(); controller->refresh();
}
void StreamDialog::selectLocalFile(const QString &path)
{
  m_path = path; m_fileName->setText(QFileInfo(path).fileName()); validate();
}
QJsonObject StreamDialog::selection() const
{
  return {{"kind", QStringList{"screen", "window", "file"}[m_kind->currentIndex()]},
    {"source", m_sources->currentItem() ? m_sources->currentItem()->data(Qt::UserRole).toString() : QString{}},
    {"path", m_path}, {"to", m_target->currentData().toString()}, {"audio", m_audio->currentData().toString()},
    {"device", m_device->currentData().toString()}, {"preset", m_quality->currentData().toString()}, {"interactive", findChild<QCheckBox *>("interactiveControl")->isChecked()},
    {"playback", m_kind->currentIndex() == 2 && m_playbackPermission->isChecked()}};
}
void StreamDialog::populate()
{
  const auto old = selection(), data = m_controller->inventory();
  const QSignalBlocker sources(m_sources), target(m_target), audio(m_audio), device(m_device);
  const bool file = m_kind->currentIndex() == 2;
  m_sources->setVisible(!file); m_fileButton->setVisible(file); m_fileName->setVisible(file);
  m_playbackPermission->setVisible(file);
  m_sources->clear();
  for (const auto &entry : data["sources"].toArray()) {
    const auto value = entry.toObject();
    if (value["kind"] != old["kind"]) continue;
    auto *item = new QListWidgetItem(value["title"].toString(), m_sources);
    item->setData(Qt::UserRole, value["id"].toVariant()); item->setToolTip(value["reason"].toString());
    if (value["id"] == old["source"]) m_sources->setCurrentItem(item);
  }
  m_target->clear(); m_target->addItem(tr("Select a connected computer…"), QString{});
  for (const auto &entry : data["peers"].toArray()) {
    const auto value = entry.toObject(); if (value["id"] == data["id"]) continue;
    const bool compatible = value["capabilities"].toObject()["receive"].toBool();
    m_target->addItem(value["name"].toString() + (compatible ? QString{} : tr(" — receiver unavailable")), value["id"].toVariant());
    if (!compatible) disableOption(m_target, m_target->count() - 1, tr("This peer has no compatible receiving viewer."));
  }
  m_target->setCurrentIndex(qMax(0, m_target->findData(old["to"].toVariant())));
  m_audio->clear(); m_audio->addItem(tr("Off — no audio capture or transmission"), "off");
  if (file) m_audio->addItem(tr("Video file audio"), "file");
  else {
    m_audio->addItem(tr("Whole-system audio"), "system");
    m_audio->addItem(tr("Selected app process tree (not one tab / window)"), "application");
    if (!data["systemAudio"].toBool()) disableOption(m_audio, 1, tr("System audio capture is unavailable."));
  }
  m_audio->setCurrentIndex(qMax(0, m_audio->findData(old["audio"].toVariant())));
  m_device->clear(); m_device->addItem(tr("Select the exact output endpoint…"), QString{});
  for (const auto &entry : data["endpoints"].toArray())
    m_device->addItem(entry.toObject()["name"].toString(), entry.toObject()["id"].toVariant());
  m_device->setCurrentIndex(qMax(0, m_device->findData(old["device"].toVariant())));
  validate();
}
void StreamDialog::validate()
{
  const auto data = m_controller->inventory(); const auto choice = selection();
  const auto error = senderSelectionError(data, choice);
  m_reason->setText(error.isEmpty() ? tr("Start opens the viewer automatically on the connected computer.") : error);
  m_start->setEnabled(error.isEmpty()); m_stop->setEnabled(m_controller->active());
  const bool idle = !data["busy"].toBool();
  m_playbackPermission->setEnabled(idle);
  auto *interactive=findChild<QCheckBox *>("interactiveControl");
  interactive->setEnabled(idle && choice["kind"]!="file" && data["control"].toBool());
  if(!interactive->isEnabled())interactive->setChecked(false);
  for (QWidget *widget : QList<QWidget *>{static_cast<QWidget *>(m_kind), m_sources, m_fileButton, m_refresh, m_target, m_audio, m_quality}) widget->setEnabled(idle);
  m_device->setEnabled(idle && choice["audio"] == "system" && data["endpointRequired"].toBool());
  if (!m_controller->active() && m_kind->currentIndex() != 2) {
    QString info = tr("No source selected. Refresh to discover screens and windows.");
    for (const auto &entry : data["sources"].toArray()) {
      const auto value = entry.toObject();
      if (value["id"] == choice["source"]) info = tr("%1\n%2 × %3 physical pixels\n%4\nLive preview starts when the receiver is ready.")
        .arg(value["title"].toString()).arg(value["width"].toInt()).arg(value["height"].toInt()).arg(value["reason"].toString());
    }
    if (data["sources"].toArray().isEmpty() && !data["discoveryError"].toString().isEmpty()) info += "\n" + data["discoveryError"].toString();
    m_preview->setText(info);
  } else if (!m_controller->active()) {
    m_preview->setText(m_path.isEmpty() ? tr("Choose a local video file. Decoding starts when the receiver is ready.")
      : tr("%1\nFile details and live preview become available when the receiver is ready.").arg(QFileInfo(m_path).fileName()));
  }
}
StreamLauncher::StreamLauncher(SenderController *controller, QWidget *parent, bool automaticAcceptance) : QWidget(parent)
{
  setObjectName("streamLauncher");
  auto *layout = new QHBoxLayout(this); layout->setContentsMargins(3, 3, 3, 3);
  auto *launch = new QPushButton(tr("&Stream…"), this); launch->setObjectName("streamButton");
  auto *status = plain(controller->status(), this); status->setObjectName("senderStatus");
  auto *stop = new QPushButton(tr("Stop stream"), this); stop->setObjectName("senderStop"); stop->setEnabled(false);
  layout->addWidget(launch); layout->addWidget(status, 1); layout->addWidget(stop);
  connect(stop, &QPushButton::clicked, controller, &SenderController::stop);
  connect(controller, &SenderController::statusChanged, this, [controller, status, stop] {
    status->setText(controller->status()); stop->setEnabled(controller->active());
  });
  auto *grant=new QPushButton(tr("Grant control"),this);grant->setObjectName("grantDesktopControl");layout->addWidget(grant);
  auto *revoke=new QPushButton(tr("Revoke control"),this);revoke->setObjectName("revokeDesktopControl");layout->addWidget(revoke);
  connect(grant,&QPushButton::clicked,controller,&SenderController::grantControl);
  connect(revoke,&QPushButton::clicked,controller,&SenderController::revokeControl);
  auto controlState=[controller,grant,revoke]{const auto info=controller->inventory();
    const bool allowed=info["interactive"].toBool() && !info["receiving"].toBool() && info["state"]=="streaming";
    grant->setEnabled(allowed);revoke->setEnabled(allowed);
  };
  connect(controller,&SenderController::inventoryChanged,this,controlState);controlState();
  auto dialog = std::make_shared<QPointer<StreamDialog>>();
  connect(controller, &SenderController::incoming, this, [this, controller, automaticAcceptance](const QJsonObject &offer) {
    auto *viewer = new StreamViewer(controller, offer, this, automaticAcceptance); viewer->setAttribute(Qt::WA_DeleteOnClose); viewer->show();
  });
  connect(launch, &QPushButton::clicked, this, [this, controller, dialog] {
    if (!*dialog) *dialog = new StreamDialog(controller, this);
    (*dialog)->show(); (*dialog)->raise(); (*dialog)->activateWindow();
  });
}
} // namespace deskflow::gui
