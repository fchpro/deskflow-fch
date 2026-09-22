/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "common/Settings.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

// Each child boots through production Settings discovery with its own portable
// directory. Neither the test parent nor any child opens the user's settings.
class ExcludedAppsPersistenceTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void init()
  {
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    QVERIFY(QDir(m_dir->path()).mkdir("settings"));
    m_exe = m_dir->filePath("exclusion-test.exe");
    QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), m_exe));
    QSettings settings(config(), QSettings::IniFormat);
    settings.setValue("core/computerName", "isolated-test");
    settings.sync();
    QCOMPARE(settings.status(), QSettings::NoError);
  }

  void migratesLegacyListAndSurvivesStockCleanup()
  {
    {
      QSettings settings(config(), QSettings::IniFormat);
      settings.setValue("server/excludedApps", QStringList{"bf6.exe", "another.exe"});
    }
    probe("read", "bf6.exe,another.exe");
    {
      QSettings settings(config(), QSettings::IniFormat);
      QVERIFY(!settings.contains("server/excludedApps"));
      // Reproduce the observed loss of custom keys in the stock config.
      settings.clear();
      settings.setValue("core/computerName", "after-stock-save");
    }
    probe("read", "bf6.exe,another.exe");
  }

  void savedSelectionSurvivesFreshCoreProcess()
  {
    probe("write", "bf6.exe");
    probe("read", "bf6.exe");
    QSettings fork(m_dir->filePath("settings/Deskflow-fch.conf"), QSettings::IniFormat);
    QCOMPARE(fork.value("server/excludedApps").toStringList(), QStringList{"bf6.exe"});
  }

  void explicitEmptyListSurvivesRestartAndStaleLegacyConfig()
  {
    probe("write", "bf6.exe");
    probe("write", "");
    {
      QSettings settings(config(), QSettings::IniFormat);
      settings.setValue("server/excludedApps", QStringList{"bf6.exe"});
    }
    probe("read", "");
    probe("read", "");
  }

  void switchingSettingsFilesUsesMatchingExclusions()
  {
    probe("write", "bf6.exe");
    probe("switch", "bf6.exe");
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
    const auto expected = args[3].split(',', Qt::SkipEmptyParts);
    const auto key = Settings::Server::ExcludedApps;
    if (args[2] == "write") {
      Settings::setValue(key, expected);
      Settings::save(false);
    } else if (args[2] == "switch") {
      const auto original = Settings::settingsFile();
      QTemporaryDir other;
      Settings::setSettingsFile(other.filePath("Deskflow.conf"));
      if (!Settings::value(key).toStringList().isEmpty())
        return 2;
      Settings::setValue(key, QStringList{"other.exe"});
      Settings::setSettingsFile(original);
    }
    if (Settings::value(key).toStringList() != expected)
      return 3;
    return 0;
  }
  ExcludedAppsPersistenceTests tests;
  return QTest::qExec(&tests, argc, argv);
}

#include "ExcludedAppsPersistenceTests.moc"
