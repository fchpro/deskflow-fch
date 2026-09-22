/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/Settings.h"
#include "server/LeftModifierSwap.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

// Each child boots through production Settings discovery with its own portable
// directory. Neither the test parent nor any child opens the user's settings.
class LeftModifierSwapPersistenceTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void init()
  {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    QVERIFY(QDir(m_dir->path()).mkdir("settings"));
    m_exe = m_dir->filePath("modifier-test.exe");
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), m_exe));
    QSettings settings(config(), QSettings::IniFormat);
    settings.setValue("core/computerName", "isolated-test");
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
  }

  void migratesLegacyTargetAndSurvivesStockCleanup()
  {
    {
      QSettings settings(config(), QSettings::IniFormat);
      settings.setValue("server/leftCtrlSuperSwapScreen", QString("mac"));
    }
    probe("read", "mac");
    {
      QSettings settings(config(), QSettings::IniFormat);
      QVERIFY(!settings.contains("server/leftCtrlSuperSwapScreen"));
      // Reproduce the observed loss of custom keys in the stock config.
      settings.clear();
      settings.setValue("core/computerName", "after-stock-save");
    }
    probe("read", "mac");
  }

  void missingSettingKeepsSwapDisabled()
  {
    probe("read", "");
  }

  void forkTargetWinsOverStaleLegacyTarget()
  {
    probe("write", "mac");
    {
      QSettings settings(config(), QSettings::IniFormat);
      settings.setValue("server/leftCtrlSuperSwapScreen", "old-mac");
    }
    probe("read", "mac");
  }

  void savedSelectionSurvivesFreshCoreProcess()
  {
    probe("write", "mac");
    probe("read", "mac");
    QSettings fork(m_dir->filePath("settings/Deskflow-fch.conf"), QSettings::IniFormat);
    QCOMPARE(fork.value("server/leftCtrlSuperSwapScreen").toString(), QString("mac"));
  }

  void explicitDisableSurvivesRestartAndStaleLegacyConfig()
  {
    probe("write", "mac");
    probe("write", "");
    {
      QSettings settings(config(), QSettings::IniFormat);
      settings.setValue("server/leftCtrlSuperSwapScreen", QString("mac"));
    }
    probe("read", "");
    probe("read", "");
  }

  void switchingSettingsFilesUsesMatchingTarget()
  {
    probe("write", "mac");
    probe("switch", "mac");
  }

private:
  QString config() const { return m_dir->filePath("settings/Deskflow.conf"); }
  void probe(const QString &action, const QString &expected)
  {
    QProcess child;
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(m_exe, {"--probe", action, expected});
    QVERIFY(child.waitForFinished(3000));
    const auto output = child.readAll();
    QCOMPARE(child.exitStatus(), QProcess::NormalExit);
    QVERIFY2(child.exitCode() == 0, output.constData());
  }
  std::unique_ptr<QTemporaryDir> m_dir;
  QString m_exe;
};

int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  const auto args = app.arguments();
  if (args.size() == 4 && args[1] == "--probe") {
    const auto expected = args[3];
    const auto key = Settings::Server::LeftCtrlSuperSwapScreen;
    if (args[2] == "write") {
      Settings::setValue(key, expected);
      Settings::save(false);
    } else if (args[2] == "switch") {
      const auto original = Settings::settingsFile();
      QTemporaryDir other;
      Settings::setSettingsFile(other.filePath("Deskflow.conf"));
      if (!Settings::value(key).toString().isEmpty())
        return 2;
      Settings::setValue(key, QString("other"));
      Settings::setSettingsFile(original);
    }
    if (Settings::value(key).toString() != expected)
      return 3;
    const LeftModifierSwap swap(Settings::value(key).toString().toStdString());
    const auto mapped = swap.map("mac", false, kKeyControl_L, KeyModifierControl, {KeyModifierControl, 0});
    if (mapped.first != (expected == "mac" ? kKeySuper_L : kKeyControl_L) ||
        mapped.second != (expected == "mac" ? KeyModifierSuper : KeyModifierControl))
      return 4;
    const auto windows = swap.map("mac", false, kKeySuper_L, KeyModifierSuper, {KeyModifierSuper, 0});
    if (windows.first != (expected == "mac" ? kKeyControl_L : kKeySuper_L))
      return 5;
    if (swap.map("mac", true, kKeyControl_L, 0, {}).first != kKeyControl_L ||
        swap.map("other", false, kKeyControl_L, 0, {}).first != kKeyControl_L ||
        swap.map("mac", false, kKeyControl_R, 0, {}).first != kKeyControl_R)
      return 6;
    return 0;
  }
  LeftModifierSwapPersistenceTests tests;
  return QTest::qExec(&tests, argc, argv);
}

#include "LeftModifierSwapPersistenceTests.moc"
