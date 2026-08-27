/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "gui/core/ProcessList.h"

#include <QDialog>
#include <QStringList>

class QLineEdit;
class QListWidget;
class QPushButton;
class QLabel;

//! Edits the excluded apps list (apps that pause input sharing while in the
//! foreground).  Left: current list.  Right: running processes with a
//! search box; double-click or "Add" adds the exe name.
class ExcludedAppsDialog : public QDialog
{
  Q_OBJECT

public:
  explicit ExcludedAppsDialog(QWidget *parent = nullptr);

  QStringList excludedApps() const;
  void setExcludedApps(const QStringList &apps);

  //! Replace the process source (tests inject a fixed list)
  void setProcesses(const QList<deskflow::gui::ProcessInfo> &processes);

  void refreshProcesses();
  void addSelectedProcess();
  void removeSelectedApp();
  void addExe(const QString &exe);

  QLineEdit *searchBox() const
  {
    return m_search;
  }
  QListWidget *excludedList() const
  {
    return m_excludedList;
  }
  QListWidget *processList() const
  {
    return m_processList;
  }

private:
  void applyFilter();
  void updateButtons();

  QLineEdit *m_search = nullptr;
  QListWidget *m_excludedList = nullptr;
  QListWidget *m_processList = nullptr;
  QPushButton *m_addButton = nullptr;
  QPushButton *m_removeButton = nullptr;
  QPushButton *m_refreshButton = nullptr;
  QLabel *m_hint = nullptr;
  QList<deskflow::gui::ProcessInfo> m_processes;
};
