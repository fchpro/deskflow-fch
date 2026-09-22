/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include <QLocalServer>
#include <QLocalSocket>
#include <functional>

namespace deskflow::gui {

inline void connectInstanceActivation(QLocalServer *server, QObject *receiver, std::function<void()> activate)
{
  QObject::connect(server, &QLocalServer::newConnection, receiver, [server, activate] {
    bool requested = false;
    // Each duplicate launch is a connection, even if the launcher has already
    // exited. Drain it or the bounded pending queue eventually stops accepting.
    while (auto *socket = server->nextPendingConnection()) {
      socket->close();
      socket->deleteLater();
      requested = true;
    }
    if (requested)
      activate();
  });
}

} // namespace deskflow::gui
