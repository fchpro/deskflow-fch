// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#define NOMINMAX
#include <Windows.h>
#include <QByteArray>
#include <QLocalSocket>
#include <QWinEventNotifier>
#include <functional>
#include <memory>

namespace deskflow::streaming {
struct WindowsLogin {
  DWORD session = 0;
  QByteArray user, logon;
  bool system = false;
  QByteArray key() const;
};
WindowsLogin windowsProcessLogin(DWORD pid);
WindowsLogin windowsInteractiveLogin();
bool windowsSameLogin(QLocalSocket *socket, bool serverEnd);
QString windowsPrivatePolicy(const WindowsLogin &login, bool privileged);
HANDLE windowsOpenPrivatePipe(const QString &endpoint);

// Qt's UserAccessOption grants the process user, not a selected logon SID.
// Elevated service-launched cores need an exact interactive-logon DACL instead.
class WindowsPrivateListener : public QObject {
public:
  explicit WindowsPrivateListener(QObject *parent = nullptr);
  ~WindowsPrivateListener() override;
  bool listen(const QString &endpoint);
  void close();
  bool rearm();
  std::function<void(HANDLE)> accepted;
  WindowsLogin login() const { return m_login; }
private:
  void complete();
  QString m_endpoint;
  WindowsLogin m_login;
  HANDLE m_pipe = INVALID_HANDLE_VALUE, m_event = nullptr;
  OVERLAPPED m_overlap{};
  std::unique_ptr<QWinEventNotifier> m_notifier;
};
}
