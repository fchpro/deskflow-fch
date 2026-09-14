// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/Capture.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTimer>
#include <QUuid>
#include <windows.h>
using namespace deskflow::streaming;
namespace {
constexpr wchar_t title[] = L"Deskflow controlled capture fixture";
QFile *logFile = nullptr;
void capturedLog(QtMsgType, const QMessageLogContext &, const QString &message)
{
  if (logFile) { logFile->write(message.toUtf8() + '\n'); logFile->flush(); }
}
LRESULT CALLBACK fixture(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
  if (message == WM_PAINT) {
    PAINTSTRUCT paint;
    auto dc = BeginPaint(window, &paint);
    RECT bounds; GetClientRect(window, &bounds);
    const auto ticks = GetTickCount64();
    auto brush = CreateSolidBrush((ticks / 500) % 2 ? RGB(20, 80, 200) : RGB(220, 70, 20));
    FillRect(dc, &bounds, brush); DeleteObject(brush);
    SetTextColor(dc, RGB(255, 255, 255)); SetBkMode(dc, TRANSPARENT);
    const auto text = QString("DESKFLOW CAPTURE FIXTURE\nMonotonic milliseconds: %1\nChanging blue / orange pixels").arg(ticks).toStdWString();
    DrawTextW(dc, text.c_str(), -1, &bounds, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    EndPaint(window, &paint); return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}
bool isolatedDesktop()
{
  auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!input) return false;
  wchar_t active[256]{}, current[256]{}; DWORD needed = 0;
  const bool read = GetUserObjectInformationW(input, UOI_NAME, active, sizeof(active), &needed) &&
    GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, current, sizeof(current), &needed);
  CloseDesktop(input);
  return read && wcscmp(active, current) != 0;
}
}
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  const auto args = app.arguments();
  QFile log;
  const auto logIndex = args.indexOf("--log");
  if (logIndex >= 0 && logIndex + 1 < args.size()) {
    log.setFileName(args.at(logIndex + 1));
    if (!log.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 2;
    logFile = &log;
    qInstallMessageHandler(capturedLog);
  }
  auto capture = createCaptureDevice();
  if (!args.contains("--fixture")) {
    for (const auto &source : capture->sources())
      qInfo().noquote() << source.kind << source.id << source.physicalGeometry << source.scale
                       << captureStateName(source.status.state) << source.status.reason;
    return 0;
  }
  // Never create a visible fixture on the user's desktop without an explicit headed invocation.
  if (!isolatedDesktop() && !args.contains("--headed-authorized")) {
    qCritical("Fixture requires an unselected desktop or explicitly authorized --headed-authorized"); return 2;
  }
  const auto outputIndex = args.indexOf("--output");
  if (outputIndex < 0 || outputIndex + 1 >= args.size()) { qCritical("--output directory is required"); return 2; }
  const QDir output(args.at(outputIndex + 1));
  if (!QDir().mkpath(output.absolutePath())) return 2;
  WNDCLASSW cls{}; cls.lpfnWndProc = fixture; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = title;
  RegisterClassW(&cls);
  auto window = CreateWindowExW(WS_EX_NOACTIVATE, title, title, WS_OVERLAPPEDWINDOW,
    40, 40, 640, 400, nullptr, nullptr, cls.hInstance, nullptr);
  if (!window) { qCritical("Could not create the controlled capture fixture"); return 2; }
  ShowWindow(window, SW_SHOWNOACTIVATE); UpdateWindow(window);
  QString selected;
  for (const auto &source : capture->sources())
    if (source.kind == "window" && source.title == QString::fromWCharArray(title)) selected = source.id;
  qInfo().noquote() << "Controlled fixture HWND" << quint64(window) << "source generation" << selected;
  const auto session = QUuid::createUuid().toString(QUuid::Id128);
  if (selected.isEmpty() || !capture->start(selected, session)) {
    qCritical().noquote() << "Capture refused:" << captureStateName(capture->status().state) << capture->status().reason;
    DestroyWindow(window); return 3;
  }
  QElapsedTimer elapsed; elapsed.start();
  QTimer poll; poll.setInterval(33);
  int frames = 0;
  QObject::connect(&poll, &QTimer::timeout, &app, [&] {
    InvalidateRect(window, nullptr, FALSE);
    if (auto frame = capture->takeFrame()) {
      ++frames;
      if (frames == 1 || frames == 30 || frames == 60) {
        const auto path = output.filePath(QString("frame-%1-%2ns.png").arg(frames).arg(frame->captureTimeNs));
        if (!frame->pixels.save(path)) { qCritical() << "Could not save decoded frame"; app.exit(4); return; }
        qInfo().noquote() << "Captured frame" << frames << "timestamp-ns" << frame->captureTimeNs
                         << "dimensions" << frame->pixels.size() << "path" << path;
      }
      if (frames == 60) app.exit(0);
    }
    if (elapsed.elapsed() > 3000 && frames == 0) {
      qCritical().noquote() << "First frame unavailable:" << captureStateName(capture->status().state) << capture->status().reason;
      app.exit(3);
    }
    if (elapsed.elapsed() > 10000) { qCritical() << "Changing fixture did not produce 60 frames"; app.exit(3); }
  });
  poll.start();
  const int result = app.exec();
  capture->stop(); DestroyWindow(window);
  qInfo() << "Captured frames" << frames << "elapsed-ms" << elapsed.elapsed();
  return result;
}
