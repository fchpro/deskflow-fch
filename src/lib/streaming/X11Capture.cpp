// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "Capture.h"
#include <QDateTime>
#include <QFile>
#include <QHash>
#include <QTimer>
#include <QUuid>
#include <QDBusInterface>
#include <QDBusReply>
#include <QPainter>
#include <xcb/xcb.h>
#include <xcb/composite.h>
#include <xcb/randr.h>
#include <xcb/xfixes.h>
#include <chrono>
#include <cstdlib>
#include <unistd.h>

namespace deskflow::streaming {
class X11Capture final : public CaptureDevice {
public:
  X11Capture()
  {
    int screen = 0;
    m_connection = xcb_connect(nullptr, &screen);
    if (xcb_connection_has_error(m_connection)) return;
    auto roots = xcb_setup_roots_iterator(xcb_get_setup(m_connection));
    while (screen-- > 0) xcb_screen_next(&roots);
    m_root = roots.data;
    m_timer.setInterval(33);
    connect(&m_timer, &QTimer::timeout, this, [this] { poll(); });
    QDBusInterface manager("org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", QDBusConnection::systemBus());
    manager.setTimeout(1000);
    QDBusReply<QDBusObjectPath> path = manager.call("GetSessionByPID", quint32(getpid()));
    if (path.isValid()) m_loginPath = path.value().path();
  }
  ~X11Capture() override { stop(); xcb_disconnect(m_connection); }
  QVector<CaptureSource> sources() override
  {
    if (!m_root) { m_status = {CaptureState::BackendMissing, "Cannot open X11 display"}; return {}; }
    drain();
    QVector<CaptureSource> result;
    QHash<QString, Entry> next;
    auto *monitors = xcb_randr_get_monitors_reply(m_connection, xcb_randr_get_monitors(m_connection, m_root->root, true), nullptr);
    if (monitors) {
      for (auto it = xcb_randr_get_monitors_monitors_iterator(monitors); it.rem; xcb_randr_monitor_info_next(&it)) {
        Entry entry;
        entry.window = m_root->root; entry.monitor = it.data->name;
        entry.source = {{}, "screen", QString("X11 monitor %1").arg(it.data->name),
          QRect(it.data->x, it.data->y, it.data->width, it.data->height), 1, {CaptureState::Available, {}}};
        add(entry, next, result);
      }
      free(monitors);
    }
    auto *list = property(m_root->root, atom("_NET_CLIENT_LIST"), XCB_ATOM_WINDOW);
    if (list) {
      const auto *windows = static_cast<xcb_window_t *>(xcb_get_property_value(list));
      for (int i = 0; i < xcb_get_property_value_length(list) / 4; ++i) {
        auto *geometry = xcb_get_geometry_reply(m_connection, xcb_get_geometry(m_connection, windows[i]), nullptr);
        auto *title = property(windows[i], atom("_NET_WM_NAME"), atom("UTF8_STRING"));
        if (geometry && title) {
          Entry entry;
          entry.window = windows[i];
          auto *pid=property(windows[i],atom("_NET_WM_PID"),XCB_ATOM_CARDINAL);
          if(pid && xcb_get_property_value_length(pid)==4)entry.pid=*static_cast<uint32_t *>(xcb_get_property_value(pid));
          free(pid);
          if(entry.pid){QFile process(QString("/proc/%1/stat").arg(entry.pid));if(process.open(QIODevice::ReadOnly)){
            const auto stat=process.readAll();const auto fields=stat.mid(stat.lastIndexOf(')')+2).split(' ');if(fields.size()>19)entry.birth=fields[19].toULongLong();}}
          entry.source = {{}, "window", QString::fromUtf8(static_cast<const char *>(xcb_get_property_value(title)), xcb_get_property_value_length(title)),
            QRect(geometry->x, geometry->y, geometry->width, geometry->height), 1, {CaptureState::Available, {}}};
          const uint32_t events = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
          auto *error = xcb_request_check(m_connection, xcb_change_window_attributes_checked(m_connection, windows[i], XCB_CW_EVENT_MASK, &events));
          if (!error) add(entry, next, result);
          free(error);
        }
        free(geometry); free(title);
      }
      free(list);
    }
    m_entries = std::move(next); return result;
  }
  bool start(const QString &source, const QString &session, int fps) override
  {
    stop(); drain();
    if (!validCaptureGeneration(source) || !validCaptureGeneration(session) || (fps != 30 && fps != 60))
      return fail(CaptureState::Denied, "Invalid accepted capture request");
    if (!m_entries.contains(source)) return fail(CaptureState::SourceGone, "Selected X11 source no longer exists");
    if (!unlocked()) return fail(CaptureState::Denied, "Cannot verify an unlocked login session");
    m_selected = m_entries.value(source);
    if (m_selected.source.kind == "window") {
      const auto *extension = xcb_get_extension_data(m_connection, &xcb_composite_id);
      if (!extension || !extension->present) return fail(CaptureState::Unsupported, "XComposite is required for isolated window capture");
      auto *owner = xcb_get_selection_owner_reply(m_connection, xcb_get_selection_owner(m_connection, atom("_NET_WM_CM_S0")), nullptr);
      const bool compositor = owner && owner->owner != XCB_NONE;
      free(owner);
      if (!compositor) return fail(CaptureState::Unsupported, "A compositing window manager is required");
    }
    const auto *cursor = xcb_get_extension_data(m_connection, &xcb_xfixes_id);
    if (!cursor || !cursor->present) return fail(CaptureState::Unsupported, "XFixes cursor capture is required");
    if (xcb_get_setup(m_connection)->image_byte_order != XCB_IMAGE_ORDER_LSB_FIRST)
      return fail(CaptureState::Unsupported, "This X11 pixel byte order has not been implemented");
    m_session = session; m_sequence = 0; m_geometry = 0; m_size = {};
    m_timer.start(fps == 60 ? 16 : 33); poll(); return !m_session.isEmpty();
  }
  void stop() override
  {
    m_timer.stop(); m_latest.reset(); m_session.clear();
    m_status = {CaptureState::Stopped, {}}; Q_EMIT statusChanged();
  }
  std::optional<VideoFrame> takeFrame() override { return std::exchange(m_latest, {}); }
  CaptureStatus status() const override { return m_status; }
  QJsonObject controlTarget()const override {
    if(m_session.isEmpty() || (m_selected.source.kind=="window" && !m_selected.birth))return {};
    return {{"session",m_session},{"source",m_selected.source.id},{"kind",m_selected.source.kind},
      {"handle",qint64(m_selected.window)},{"monitor",qint64(m_selected.monitor)},
      {"pid",qint64(m_selected.pid)},{"birth",QString::number(m_selected.birth,16)}};
  }
private:
  struct Entry { CaptureSource source; xcb_window_t window = 0; xcb_atom_t monitor = 0; uint32_t pid=0;quint64 birth=0; };
  bool unlocked() const
  {
    if (m_loginPath.isEmpty()) return false;
    QDBusInterface session("org.freedesktop.login1", m_loginPath, "org.freedesktop.login1.Session", QDBusConnection::systemBus());
    session.setTimeout(100);
    const auto locked = session.property("LockedHint");
    const auto active = session.property("Active");
    return locked.isValid() && active.isValid() && !locked.toBool() && active.toBool();
  }
  xcb_atom_t atom(const char *name)
  {
    auto *reply = xcb_intern_atom_reply(m_connection, xcb_intern_atom(m_connection, false, uint16_t(strlen(name)), name), nullptr);
    const auto result = reply ? reply->atom : XCB_ATOM_NONE;
    free(reply); return result;
  }
  xcb_get_property_reply_t *property(xcb_window_t window, xcb_atom_t key, xcb_atom_t type)
  { return xcb_get_property_reply(m_connection, xcb_get_property(m_connection, false, window, key, type, 0, 16384), nullptr); }
  void add(Entry &entry, QHash<QString, Entry> &next, QVector<CaptureSource> &result)
  {
    for (const auto &old : std::as_const(m_entries))
      if (entry.window == old.window && entry.monitor == old.monitor && entry.pid==old.pid && entry.birth==old.birth) { entry.source.id = old.source.id; break; }
    if (entry.source.id.isEmpty()) entry.source.id = QUuid::createUuid().toString(QUuid::Id128);
    next.insert(entry.source.id, entry); result.append(entry.source);
  }
  void drain()
  {
    if (!m_root) return;
    while (auto *event = xcb_poll_for_event(m_connection)) {
      if ((event->response_type & 0x7f) == XCB_DESTROY_NOTIFY) {
        auto window = reinterpret_cast<xcb_destroy_notify_event_t *>(event)->window;
        for (auto it = m_entries.begin(); it != m_entries.end();) {
          if (it->window == window) it = m_entries.erase(it); else ++it;
        }
        if (window == m_selected.window && !m_session.isEmpty()) fail(CaptureState::SourceGone, "Selected X11 window was destroyed");
      }
      free(event);
    }
  }
  void poll()
  {
    drain();
    if (m_session.isEmpty()) return;
    if (xcb_connection_has_error(m_connection)) { fail(CaptureState::SourceGone, "X11 connection closed"); return; }
    if (!unlocked()) { fail(CaptureState::Denied, "Login session locked or inactive"); return; }
    auto *attributes = xcb_get_window_attributes_reply(m_connection, xcb_get_window_attributes(m_connection, m_selected.window), nullptr);
    const bool mapped = attributes && attributes->map_state == XCB_MAP_STATE_VIEWABLE;
    free(attributes);
    if (!mapped) { fail(CaptureState::TemporarilyUnavailable, "Selected window was hidden or minimized"); return; }
    QRect bounds = m_selected.source.physicalGeometry;
    if (m_selected.source.kind == "screen") {
      auto *monitors = xcb_randr_get_monitors_reply(m_connection, xcb_randr_get_monitors(m_connection, m_root->root, true), nullptr);
      bool found = false;
      if (monitors) {
        for (auto it = xcb_randr_get_monitors_monitors_iterator(monitors); it.rem; xcb_randr_monitor_info_next(&it)) {
          if (it.data->name == m_selected.monitor) {
            bounds = QRect(it.data->x, it.data->y, it.data->width, it.data->height); found = true; break;
          }
        }
        free(monitors);
      }
      if (!found) { fail(CaptureState::SourceGone, "Selected X11 monitor disconnected"); return; }
    }
    xcb_drawable_t drawable = m_selected.window;
    xcb_pixmap_t pixmap = 0;
    if (m_selected.source.kind == "window") {
      auto *geometry = xcb_get_geometry_reply(m_connection, xcb_get_geometry(m_connection, drawable), nullptr);
      if (!geometry) { fail(CaptureState::SourceGone, "Window geometry is unavailable"); return; }
      bounds = QRect(0, 0, geometry->width, geometry->height); free(geometry);
      pixmap = xcb_generate_id(m_connection);
      auto *error = xcb_request_check(m_connection, xcb_composite_name_window_pixmap_checked(m_connection, drawable, pixmap));
      if (error) { free(error); fail(CaptureState::ProtectedOrUnavailable, "Compositor did not expose the selected window pixmap"); return; }
      drawable = pixmap;
    }
    auto *image = xcb_get_image_reply(m_connection, xcb_get_image(m_connection, XCB_IMAGE_FORMAT_Z_PIXMAP, drawable,
      bounds.x(), bounds.y(), bounds.width(), bounds.height(), ~0U), nullptr);
    if (pixmap) xcb_free_pixmap(m_connection, pixmap);
    if (!image || (image->depth != 24 && image->depth != 32) ||
        xcb_get_image_data_length(image) != bounds.width() * bounds.height() * 4) {
      free(image); fail(CaptureState::Unsupported, "Selected X11 source must provide 32-bit packed SDR RGB pixels"); return;
    }
    VideoFrame frame;
    frame.pixels = QImage(xcb_get_image_data(image), bounds.width(), bounds.height(), bounds.width() * 4, QImage::Format_RGB32).copy();
    free(image);
    auto *cursor = xcb_xfixes_get_cursor_image_reply(m_connection, xcb_xfixes_get_cursor_image(m_connection), nullptr);
    if (!cursor) { fail(CaptureState::ProtectedOrUnavailable, "Could not capture the cursor"); return; }
    auto *origin = xcb_translate_coordinates_reply(m_connection,
      xcb_translate_coordinates(m_connection, m_selected.window, m_root->root, bounds.x(), bounds.y()), nullptr);
    if (!origin) { free(cursor); fail(CaptureState::SourceGone, "Source coordinate transform is unavailable"); return; }
    QImage pointer(reinterpret_cast<const uchar *>(xcb_xfixes_get_cursor_image_cursor_image(cursor)),
      cursor->width, cursor->height, cursor->width * 4, QImage::Format_ARGB32_Premultiplied);
    { QPainter painter(&frame.pixels); painter.drawImage(cursor->x - cursor->xhot - origin->dst_x,
        cursor->y - cursor->yhot - origin->dst_y, pointer); }
    frame.physicalGeometry = QRect(origin->dst_x, origin->dst_y, bounds.width(), bounds.height());
    free(origin); free(cursor);
    if (m_size != frame.pixels.size() || m_bounds!=frame.physicalGeometry) { m_size = frame.pixels.size();m_bounds=frame.physicalGeometry; ++m_geometry; }
    frame.session = m_session; frame.source = m_selected.source.id; frame.sequence = ++m_sequence;
    frame.geometryGeneration = m_geometry;
    frame.coordinateMappingValid = true;
    frame.captureTimeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    frame.mediaTimeNs = frame.captureTimeNs;
    m_latest = std::move(frame);
    if (m_status.state != CaptureState::Available) { m_status = {CaptureState::Available, {}}; Q_EMIT statusChanged(); }
  }
  bool fail(CaptureState state, const QString &reason) { stop(); m_status = {state, reason}; Q_EMIT statusChanged(); return false; }
  xcb_connection_t *m_connection = nullptr;
  xcb_screen_t *m_root = nullptr;
  QHash<QString, Entry> m_entries;
  Entry m_selected;
  QString m_session, m_loginPath;
  QTimer m_timer;
  CaptureStatus m_status;
  std::optional<VideoFrame> m_latest;
  quint64 m_sequence = 0, m_geometry = 0;
  QSize m_size;
  QRect m_bounds;
};
std::unique_ptr<CaptureDevice> createX11CaptureDevice() { return std::make_unique<X11Capture>(); }
} // namespace deskflow::streaming
