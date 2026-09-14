// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/Service.h"
#include <QTest>
using namespace deskflow::streaming;
class StreamingContextTests:public QObject {
 Q_OBJECT
private Q_SLOTS:
 void listenerIdentity(){QCOMPARE(publishedMediaAddress(QHostAddress("192.0.2.1"),QHostAddress("198.51.100.1")),QHostAddress("192.0.2.1"));}
 void clientIdentity(){QCOMPARE(publishedMediaAddress({},QHostAddress("198.51.100.1")),QHostAddress("198.51.100.1"));}
};
QTEST_APPLESS_MAIN(StreamingContextTests)
#include "StreamingContextTests.moc"
