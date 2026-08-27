/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class ExcludedAppsDialogTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  // Test are run in order top to bottom
  void structureHasListsSearchAndButtons();
  void searchFiltersProcessList();
  void addSelectedProcessAppendsExe();
  void addIgnoresDuplicatesCaseInsensitive();
  void removeSelectedAppDeletesEntry();
  void foregroundMonitorMatchesExcludedApps();
  // renders the dialog to PNG files when DESKFLOW_PROOF_DIR is set (proof of work), skipped otherwise
  void renderProofScreenshots();
};
