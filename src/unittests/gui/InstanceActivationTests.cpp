/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "gui/InstanceActivation.h"

#include <QTest>
#include <QUuid>

class InstanceActivationTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void repeatedLaunches_data()
  {
    QTest::addColumn<bool>("disconnectImmediately");
    QTest::newRow("connected-launcher") << false;
    QTest::newRow("launcher-already-exited") << true;
  }

  void repeatedLaunches()
  {
    QFETCH(bool, disconnectImmediately);
    QLocalServer server;
    const auto name = QStringLiteral("deskflow-activation-test-%1").arg(QUuid::createUuid().toString());
    QVERIFY(server.listen(name));
    int activations = 0;
    deskflow::gui::connectInstanceActivation(&server, this, [&] { ++activations; });
    const int launches = server.maxPendingConnections() * 3;
    for (int i = 0; i < launches; ++i) {
      QLocalSocket launcher;
      launcher.connectToServer(name, QIODevice::ReadOnly);
      QVERIFY(launcher.waitForConnected(1000));
      if (disconnectImmediately)
        launcher.abort();
      QTRY_COMPARE_WITH_TIMEOUT(activations, i + 1, 1000);
      QVERIFY(!server.hasPendingConnections());
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      QCOMPARE(server.findChildren<QLocalSocket *>().size(), 0);
    }
  }
};

QTEST_GUILESS_MAIN(InstanceActivationTests)
#include "InstanceActivationTests.moc"
