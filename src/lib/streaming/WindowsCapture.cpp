// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "Capture.h"
#include "CaptureDeadline.h"
#include "WindowsWgcSource.h"
#include <QElapsedTimer>
#include <QHash>
#include <QTimer>
#include <QUuid>
#include <windows.h>
#include <dwmapi.h>
#include <wtsapi32.h>
#include <shellscalingapi.h>
#include <roapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>

namespace deskflow::streaming {
namespace {
QString token() { return QUuid::createUuid().toString(QUuid::Id128); }
struct NativeSource {
  CaptureSource source;
  quint64 handle = 0, processBirth = 0;
  DWORD pid = 0;
  QString device;
};
quint64 processBirth(DWORD pid)
{
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return 0;
  FILETIME created{}, exited{}, kernel{}, user{};
  const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user);
  CloseHandle(process);
  return ok ? (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime : 0;
}
QRect rectangle(const RECT &r) { return {r.left, r.top, r.right - r.left, r.bottom - r.top}; }
CaptureStatus displayColor(const QString &device)
{
  UINT32 pathCount = 0, modeCount = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
    return {CaptureState::Unsupported, "Could not verify display SDR color mode"};
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
    return {CaptureState::TemporarilyUnavailable, "Display configuration changed; select again"};
  for (UINT32 i = 0; i < pathCount; ++i) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
    name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(name), paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id};
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS || QString::fromWCharArray(name.viewGdiDeviceName) != device) continue;
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
    color.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO, sizeof(color), paths[i].targetInfo.adapterId, paths[i].targetInfo.id};
    if (DisplayConfigGetDeviceInfo(&color.header) != ERROR_SUCCESS)
      return {CaptureState::Unsupported, "Could not verify display SDR color mode"};
    if (color.advancedColorEnabled) return {CaptureState::Unsupported, "HDR/advanced-color capture requires an implemented SDR conversion"};
    return {CaptureState::Available, {}};
  }
  return {CaptureState::SourceGone, "Display identity is not active"};
}
bool unlocked()
{
  LPWSTR raw = nullptr;
  DWORD size = 0;
  if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx, &raw, &size))
    return false;
  auto *info = reinterpret_cast<WTSINFOEXW *>(raw);
  const bool result = size >= sizeof(WTSINFOEXW) && info->Level == 1 &&
      info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_UNLOCK;
  WTSFreeMemory(raw);
  if (!result) return false;
  HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
  if (!input) return false;
  wchar_t inputName[256]{}, currentName[256]{};
  DWORD needed = 0;
  const bool same = GetUserObjectInformationW(input, UOI_NAME, inputName, sizeof(inputName), &needed) &&
      GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, currentName, sizeof(currentName), &needed) &&
      wcscmp(inputName, currentName) == 0;
  CloseDesktop(input);
  return same;
}
}
class WindowsCapture final : public CaptureDevice {
public:
  WindowsCapture()
  {
    m_com = RoInitialize(RO_INIT_MULTITHREADED);
    m_dpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // Out-of-context WinEvent hooks are delivered on this worker's Qt message loop.
    m_hook = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY, nullptr, destroyed, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (m_hook) hooks().insert(m_hook, this);
    m_timer.setInterval(10);
    connect(&m_timer, &QTimer::timeout, this, [this] { poll(); });
  }
  ~WindowsCapture() override
  {
    stop();
    hooks().remove(m_hook);
    if (m_hook) UnhookWinEvent(m_hook);
    if (m_dpi) SetThreadDpiAwarenessContext(m_dpi);
    if (SUCCEEDED(m_com)) {
      // Static C++/WinRT factories must release their COM references before
      // this worker's apartment unloads their implementing DLLs.
      winrt::clear_factory_cache();
      RoUninitialize();
    }
  }
  QVector<CaptureSource> sources() override
  {
    QVector<NativeSource> found;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
      MONITORINFOEXW info{};
      info.cbSize = sizeof(info);
      if (GetMonitorInfoW(monitor, &info)) {
        NativeSource native;
        native.handle = quint64(monitor);
        native.device = QString::fromWCharArray(info.szDevice);
        native.source = {{}, "screen", native.device, rectangle(info.rcMonitor), 1.0, {CaptureState::Available, {}}};
        UINT x = 0, y = 0;
        if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y))) native.source.scale = x / 96.0;
        reinterpret_cast<QVector<NativeSource> *>(data)->append(native);
      }
      return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    EnumWindows([](HWND window, LPARAM data) -> BOOL {
      if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) || !GetWindowTextLengthW(window)) return TRUE;
      DWORD cloaked = 0;
      if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
      wchar_t title[1024]{};
      GetWindowTextW(window, title, 1024);
      DWORD pid = 0;
      GetWindowThreadProcessId(window, &pid);
      const auto birth = processBirth(pid);
      if (!birth) return TRUE;
      RECT bounds{};
      if (!GetWindowRect(window, &bounds)) return TRUE;
      NativeSource native;
      native.handle = quint64(window);
      native.pid = pid;
      native.processBirth = birth;
      native.source = {{}, "window", QString::fromWCharArray(title), rectangle(bounds),
                       GetDpiForWindow(window) / 96.0, {CaptureState::Available, {}}, pid, birth};
      reinterpret_cast<QVector<NativeSource> *>(data)->append(native);
      return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    QHash<QString, NativeSource> next;
    QVector<CaptureSource> result;
    for (auto &native : found) {
      for (const auto &old : std::as_const(m_sources)) {
        if (old.handle == native.handle && old.pid == native.pid && old.processBirth == native.processBirth &&
            old.device == native.device && old.source.kind == native.source.kind) {
          native.source.id = old.source.id;
          break;
        }
      }
      if (native.source.id.isEmpty()) native.source.id = token();
      native.source.status = health(native);

      result.append(native.source);
      next.insert(native.source.id, native);
    }
    m_sources = std::move(next);
    return result;
  }
  bool start(const QString &source, const QString &session, int fps) override
  {
    stop();
    if (!validCaptureGeneration(source) || !validCaptureGeneration(session) || (fps != 30 && fps != 60))
      return fail(CaptureState::Denied, "Invalid accepted session/source generation or frame rate");
    if (!m_sources.contains(source)) return fail(CaptureState::SourceGone, "Selected source is no longer enumerated");
    m_selected = m_sources.value(source);
    const auto check = health(m_selected);
    if (check.state != CaptureState::Available) return fail(check.state, check.reason);
    QString error;
    if (!m_pipeline.start(m_selected.source.kind == "window" ? m_selected.handle : 0,
                          m_selected.source.kind == "screen" ? m_selected.handle : 0, fps, error))
      return fail(CaptureState::ProtectedOrUnavailable, error);
    m_session = session;
    m_sequence = 0;
    m_geometryGeneration = 0;
    m_lastSize = {};
    m_lastFrame.start();
    m_deadline.start(0);
    m_status = {CaptureState::Starting, "Waiting for first captured frame"};
    m_timer.start();
    Q_EMIT statusChanged();
    return true;

  }
  void stop() override
  {
    m_timer.stop();
    m_pipeline.stop();
    m_latest.reset();
    m_session.clear();
    m_status = {CaptureState::Stopped, {}};
    Q_EMIT statusChanged();
  }
  std::optional<VideoFrame> takeFrame() override { return std::exchange(m_latest, {}); }
  CaptureStatus status() const override { return m_status; }
  QJsonObject controlTarget() const override {
    if(m_session.isEmpty())return {};
    return {{"session",m_session},{"source",m_selected.source.id},{"kind",m_selected.source.kind},
      {"handle",QString::number(m_selected.handle,16)},{"pid",qint64(m_selected.pid)},
      {"birth",QString::number(m_selected.processBirth,16)},{"device",m_selected.device}};
  }
private:
  static QHash<HWINEVENTHOOK, WindowsCapture *> &hooks() { static thread_local QHash<HWINEVENTHOOK, WindowsCapture *> value; return value; }
  static void CALLBACK destroyed(HWINEVENTHOOK hook, DWORD, HWND window, LONG object, LONG child, DWORD, DWORD)
  {
    auto *self = hooks().value(hook);
    if (!self || object != OBJID_WINDOW || child != CHILDID_SELF) return;
    for (auto it = self->m_sources.begin(); it != self->m_sources.end();) {
      if (it->source.kind == "window" && it->handle == quint64(window)) {
        if (self->m_selected.source.id == it.key() && !self->m_session.isEmpty())
          self->fail(CaptureState::SourceGone, "Selected window was destroyed");
        it = self->m_sources.erase(it);
      } else ++it;
    }
  }
  CaptureStatus health(const NativeSource &native) const
  {
    if (!m_hook) return {CaptureState::Unsupported, "Window lifecycle event subscription failed"};
    if (!m_dpi) return {CaptureState::Unsupported, "Physical-pixel DPI awareness could not be established"};
    if (!unlocked()) return {CaptureState::Denied, "Capture requires the unlocked interactive desktop"};
    try {
      if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported())
        return {CaptureState::Unsupported, "Windows Graphics Capture is unavailable"};
    } catch (const winrt::hresult_error &) { return {CaptureState::Unsupported, "Windows Graphics Capture initialization failed"}; }
    if (native.source.kind == "window") {
      const auto window = reinterpret_cast<HWND>(native.handle);
      DWORD pid = 0;
      GetWindowThreadProcessId(window, &pid);
      if (!IsWindow(window) || pid != native.pid || processBirth(pid) != native.processBirth)
        return {CaptureState::SourceGone, "Selected window identity no longer exists"};
      if (IsIconic(window) || !IsWindowVisible(window))
        return {CaptureState::TemporarilyUnavailable, "Selected window is minimized or hidden; restore it and start again"};
      DWORD affinity = 0;
      if (GetWindowDisplayAffinity(window, &affinity) && affinity != WDA_NONE)
        return {CaptureState::ProtectedOrUnavailable, "Selected window excludes capture"};
      MONITORINFOEXW monitor{};
      monitor.cbSize = sizeof(monitor);
      if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONULL), &monitor))
        return {CaptureState::TemporarilyUnavailable, "Window is not on an active display"};
      return displayColor(QString::fromWCharArray(monitor.szDevice));
    } else {
      MONITORINFOEXW info{};
      info.cbSize = sizeof(info);
      if (!GetMonitorInfoW(reinterpret_cast<HMONITOR>(native.handle), &info) ||
          QString::fromWCharArray(info.szDevice) != native.device)
        return {CaptureState::SourceGone, "Selected display disconnected or changed identity"};
      return displayColor(native.device);
    }
    return {CaptureState::Available, {}};
  }
  bool fail(CaptureState state, const QString &reason)
  {
    stop();
    m_status = {state, reason};
    Q_EMIT statusChanged();
    return false;
  }
  void poll()
  {
    const auto check = health(m_selected);
    if (check.state != CaptureState::Available) { fail(check.state, check.reason); return; }
    QString error;
    auto frame = m_pipeline.pull(error);
    if (!error.isEmpty()) { fail(CaptureState::ProtectedOrUnavailable, error); return; }
    if (!frame) {
      if (m_deadline.expired(m_lastFrame.elapsed())) fail(CaptureState::ProtectedOrUnavailable, "No first captured frame within three seconds");
      return;
    }
    RECT bounds{};
    if (m_selected.source.kind == "window") {
      auto window = reinterpret_cast<HWND>(m_selected.handle);
      if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
        m_selected.source.physicalGeometry = rectangle(bounds);
      else m_selected.source.physicalGeometry = {};
      m_selected.source.scale = GetDpiForWindow(window) / 96.0;
    } else {
      MONITORINFOEXW info{};
      info.cbSize = sizeof(info);
      if (GetMonitorInfoW(reinterpret_cast<HMONITOR>(m_selected.handle), &info))
        m_selected.source.physicalGeometry = rectangle(info.rcMonitor);
    }
    if (frame->pixels.size() != m_lastSize || m_lastGeometry != m_selected.source.physicalGeometry) {
      ++m_geometryGeneration;
      m_lastSize = frame->pixels.size();
      m_lastGeometry = m_selected.source.physicalGeometry;
    }
    frame->session = m_session;
    frame->source = m_selected.source.id;
    frame->sequence = ++m_sequence;
    frame->geometryGeneration = m_geometryGeneration;
    frame->physicalGeometry = m_selected.source.physicalGeometry;
    frame->scale = m_selected.source.scale;
    frame->coordinateMappingValid = frame->physicalGeometry.size() == frame->pixels.size();
    m_latest = std::move(frame);
    m_deadline.frame(m_lastFrame.elapsed());
    if (m_status.state != CaptureState::Available) { m_status = {CaptureState::Available, {}}; Q_EMIT statusChanged(); }
  }
  HWINEVENTHOOK m_hook = nullptr;
  HRESULT m_com = E_FAIL;
  DPI_AWARENESS_CONTEXT m_dpi = nullptr;
  QHash<QString, NativeSource> m_sources;
  NativeSource m_selected;
  QTimer m_timer;
  QElapsedTimer m_lastFrame;
  CaptureDeadline m_deadline;
  CaptureStatus m_status;
  QString m_session;
  quint64 m_sequence = 0, m_geometryGeneration = 0;
  QSize m_lastSize;
  QRect m_lastGeometry;
  std::optional<VideoFrame> m_latest;
  WindowsWgcSource m_pipeline;
};
std::unique_ptr<CaptureDevice> createCaptureDevice() { return std::make_unique<WindowsCapture>(); }
} // namespace deskflow::streaming
