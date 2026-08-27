/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ExcludedAppsDialog.h"

#include "common/Settings.h"

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

using deskflow::gui::ProcessInfo;
using deskflow::gui::ProcessList;

ExcludedAppsDialog::ExcludedAppsDialog(QWidget *parent) : QDialog(parent)
{
  setWindowTitle(tr("Excluded Apps"));
  setObjectName(QStringLiteral("ExcludedAppsDialog"));
  resize(760, 480);

  // left: excluded list
  auto *excludedGroup = new QGroupBox(tr("Excluded apps (input sharing pauses while they are in front)"), this);
  m_excludedList = new QListWidget(excludedGroup);
  m_excludedList->setObjectName(QStringLiteral("excludedList"));
  m_removeButton = new QPushButton(tr("&Remove"), excludedGroup);
  m_removeButton->setObjectName(QStringLiteral("btnRemove"));
  auto *excludedLayout = new QVBoxLayout(excludedGroup);
  excludedLayout->addWidget(m_excludedList);
  excludedLayout->addWidget(m_removeButton, 0, Qt::AlignLeft);

  // right: running processes with search
  auto *processGroup = new QGroupBox(tr("Running processes"), this);
  m_search = new QLineEdit(processGroup);
  m_search->setObjectName(QStringLiteral("searchBox"));
  m_search->setPlaceholderText(tr("Search by exe name or window title"));
  m_search->setClearButtonEnabled(true);
  m_processList = new QListWidget(processGroup);
  m_processList->setObjectName(QStringLiteral("processList"));
  m_addButton = new QPushButton(tr("&Add"), processGroup);
  m_addButton->setObjectName(QStringLiteral("btnAdd"));
  m_refreshButton = new QPushButton(tr("Re&fresh"), processGroup);
  m_refreshButton->setObjectName(QStringLiteral("btnRefresh"));
  m_hint = new QLabel(tr("Processes with a visible window are listed first. Double-click to add."), processGroup);
  m_hint->setWordWrap(true);
  auto *processButtons = new QHBoxLayout();
  processButtons->addWidget(m_addButton);
  processButtons->addWidget(m_refreshButton);
  processButtons->addStretch();
  auto *processLayout = new QVBoxLayout(processGroup);
  processLayout->addWidget(m_search);
  processLayout->addWidget(m_processList);
  processLayout->addLayout(processButtons);
  processLayout->addWidget(m_hint);

  auto *columns = new QHBoxLayout();
  columns->addWidget(excludedGroup, 2);
  columns->addWidget(processGroup, 3);

  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  buttons->setObjectName(QStringLiteral("buttonBox"));
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto *note = new QLabel(
      tr("Changes apply after the server restarts (done automatically when it is running)."), this
  );
  note->setWordWrap(true);

  auto *layout = new QVBoxLayout(this);
  layout->addLayout(columns, 1);
  layout->addWidget(note);
  layout->addWidget(buttons);

  connect(m_search, &QLineEdit::textChanged, this, &ExcludedAppsDialog::applyFilter);
  connect(m_addButton, &QPushButton::clicked, this, &ExcludedAppsDialog::addSelectedProcess);
  connect(m_processList, &QListWidget::itemDoubleClicked, this, &ExcludedAppsDialog::addSelectedProcess);
  connect(m_removeButton, &QPushButton::clicked, this, &ExcludedAppsDialog::removeSelectedApp);
  connect(m_refreshButton, &QPushButton::clicked, this, &ExcludedAppsDialog::refreshProcesses);
  connect(m_processList, &QListWidget::itemSelectionChanged, this, &ExcludedAppsDialog::updateButtons);
  connect(m_excludedList, &QListWidget::itemSelectionChanged, this, &ExcludedAppsDialog::updateButtons);

  setExcludedApps(Settings::value(Settings::Server::ExcludedApps).toStringList());
  refreshProcesses();
  m_search->setFocus();
}

QStringList ExcludedAppsDialog::excludedApps() const
{
  QStringList apps;
  for (int i = 0; i < m_excludedList->count(); ++i) {
    apps.append(m_excludedList->item(i)->text());
  }
  return apps;
}

void ExcludedAppsDialog::setExcludedApps(const QStringList &apps)
{
  m_excludedList->clear();
  for (const auto &app : apps) {
    const auto name = app.trimmed();
    if (!name.isEmpty()) {
      m_excludedList->addItem(name);
    }
  }
  updateButtons();
}

void ExcludedAppsDialog::setProcesses(const QList<ProcessInfo> &processes)
{
  m_processes = ProcessList::sorted(ProcessList::dedupeByExe(processes));
  applyFilter();
}

void ExcludedAppsDialog::refreshProcesses()
{
  setProcesses(ProcessList::running());
}

void ExcludedAppsDialog::applyFilter()
{
  m_processList->clear();
  for (const auto &info : ProcessList::filter(m_processes, m_search->text())) {
    auto *item = new QListWidgetItem(ProcessList::displayText(info), m_processList);
    item->setData(Qt::UserRole, info.exe);
    item->setToolTip(tr("pid %1").arg(info.pid));
  }
  updateButtons();
}

void ExcludedAppsDialog::addSelectedProcess()
{
  auto *item = m_processList->currentItem();
  if (item == nullptr) {
    return;
  }
  addExe(item->data(Qt::UserRole).toString());
}

void ExcludedAppsDialog::addExe(const QString &exe)
{
  const auto name = exe.trimmed();
  if (name.isEmpty()) {
    return;
  }
  for (int i = 0; i < m_excludedList->count(); ++i) {
    if (m_excludedList->item(i)->text().compare(name, Qt::CaseInsensitive) == 0) {
      m_excludedList->setCurrentRow(i);
      return;
    }
  }
  m_excludedList->addItem(name);
  m_excludedList->setCurrentRow(m_excludedList->count() - 1);
  updateButtons();
}

void ExcludedAppsDialog::removeSelectedApp()
{
  delete m_excludedList->takeItem(m_excludedList->currentRow());
  updateButtons();
}

void ExcludedAppsDialog::updateButtons()
{
  m_addButton->setEnabled(m_processList->currentItem() != nullptr);
  m_removeButton->setEnabled(m_excludedList->currentItem() != nullptr);
}
