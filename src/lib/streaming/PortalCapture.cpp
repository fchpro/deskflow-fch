// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "Capture.h"
#include "GstCapturePipeline.h"
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <libportal/portal.h>
#include <unistd.h>

namespace deskflow::streaming {
// The portal, not a guessed window ID or title, owns Wayland source selection and its lifetime.
class PortalCapture final : public CaptureDevice {
public:
  PortalCapture()
  {
    m_context = g_main_context_new();
    g_main_context_push_thread_default(m_context);
    m_portal = xdp_portal_new();
    g_main_context_pop_thread_default(m_context);
    m_screen = QUuid::createUuid().toString(QUuid::Id128);
    m_window = QUuid::createUuid().toString(QUuid::Id128);
    m_timer.setInterval(10);
    connect(&m_timer, &QTimer::timeout, this, [this] { poll(); });
    m_timer.start();
  }
  ~PortalCapture() override
  {
    stop();
    // Cancellation callbacks own QPointers, so a late completion cannot access destroyed state.
    while (g_main_context_pending(m_context)) g_main_context_iteration(m_context, FALSE);
    g_object_unref(m_portal);
    g_main_context_unref(m_context);
  }
  QVector<CaptureSource> sources() override
  {
    GError *error = nullptr;
    auto *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!bus) { g_clear_error(&error); m_status = {CaptureState::BackendMissing, "Session D-Bus is unavailable"}; return {}; }
    auto *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", "org.freedesktop.portal.ScreenCast", "AvailableSourceTypes"),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, &error);
    g_object_unref(bus);
    if (!reply) { g_clear_error(&error); m_status = {CaptureState::BackendMissing, "ScreenCast portal is unavailable"}; return {}; }
    GVariant *wrapped = nullptr;
    g_variant_get(reply, "(v)", &wrapped);
    const auto types = g_variant_get_uint32(wrapped);
    g_variant_unref(wrapped); g_variant_unref(reply);
    m_types = types;
    QVector<CaptureSource> result;
    for (const auto &kind : {QString("screen"), QString("window")}) {
      const bool supported = types & (kind == "screen" ? 1 : 2);
      result.append({kind == "screen" ? m_screen : m_window, kind,
        kind == "screen" ? "Choose display in system picker" : "Choose window in system picker", {}, 1,
        supported ? CaptureStatus{CaptureState::PermissionRequired, "The compositor will request capture permission"} :
                    CaptureStatus{CaptureState::Unsupported, "The compositor does not advertise this source type"}});
    }
    return result;
  }
  bool start(const QString &source, const QString &session, int fps) override
  {
    stop();
    if (!validCaptureGeneration(source) || !validCaptureGeneration(session) || (fps != 30 && fps != 60))
      return fail(CaptureState::Denied, "Invalid accepted capture request");
    m_selectedType = source == m_screen ? 1U : source == m_window ? 2U : 0U;
    if (!m_selectedType) return fail(CaptureState::SourceGone, "Unknown portal selection generation");
    if (!(m_types & m_selectedType)) return fail(CaptureState::Unsupported, "Portal does not support the selected source type");
    QString error;
    if (!GstCapturePipeline::initialize(error)) return fail(CaptureState::BackendMissing, error);
    m_source = source; m_session = session; m_fps = fps;
    m_cancel = g_cancellable_new();
    m_status = {CaptureState::PermissionRequired, "Choose the source and approve it in the system picker"};
    g_main_context_push_thread_default(m_context);
    xdp_portal_create_screencast_session(m_portal, XdpOutputType(m_selectedType), XDP_SCREENCAST_FLAG_NONE,
      XDP_CURSOR_MODE_EMBEDDED, XDP_PERSIST_MODE_NONE, nullptr, m_cancel, created, new Pending{this, m_session});
    g_main_context_pop_thread_default(m_context);
    Q_EMIT statusChanged(); return true;
  }
  void stop() override
  {
    m_session.clear();
    if (m_cancel) { g_cancellable_cancel(m_cancel); g_object_unref(m_cancel); m_cancel = nullptr; }
    m_pipeline.stop();
    if (m_native) {
      g_signal_handlers_disconnect_by_data(m_native, this);
      xdp_session_close(m_native);
      g_object_unref(m_native); m_native = nullptr;
    }
    if (m_fd >= 0) { close(m_fd); m_fd = -1; }
    m_latest.reset(); m_running = false; m_sequence = 0;
    m_status = {CaptureState::Stopped, {}}; Q_EMIT statusChanged();
  }
  std::optional<VideoFrame> takeFrame() override { return std::exchange(m_latest, {}); }
  CaptureStatus status() const override { return m_status; }
private:
  struct Pending { QPointer<PortalCapture> owner; QString session; };
  static void created(GObject *object, GAsyncResult *result, gpointer data)
  {
    std::unique_ptr<Pending> pending(static_cast<Pending *>(data));
    GError *error = nullptr;
    auto *session = xdp_portal_create_screencast_session_finish(XDP_PORTAL(object), result, &error);
    auto self = pending->owner;
    if (!self || self->m_session != pending->session) {
      if (session) { xdp_session_close(session); g_object_unref(session); }
      g_clear_error(&error); return;
    }
    if (!session) { self->fail(CaptureState::Denied, QString::fromUtf8(error ? error->message : "Portal selection denied")); g_clear_error(&error); return; }
    self->m_native = session;
    g_signal_connect(session, "closed", G_CALLBACK(closed), self.data());
    g_main_context_push_thread_default(self->m_context);
    xdp_session_start(session, nullptr, self->m_cancel, started, new Pending{self, self->m_session});
    g_main_context_pop_thread_default(self->m_context);
  }
  static void started(GObject *object, GAsyncResult *result, gpointer data)
  {
    std::unique_ptr<Pending> pending(static_cast<Pending *>(data));
    GError *error = nullptr;
    const bool ok = xdp_session_start_finish(XDP_SESSION(object), result, &error);
    auto self = pending->owner;
    if (!self || self->m_session != pending->session) { g_clear_error(&error); return; }
    if (!ok) { self->fail(CaptureState::Denied, QString::fromUtf8(error ? error->message : "Portal permission denied")); g_clear_error(&error); return; }
    auto *streams = xdp_session_get_streams(self->m_native); // borrowed a(ua{sv})
    if (!streams || g_variant_n_children(streams) != 1) { self->fail(CaptureState::Denied, "Portal must grant exactly one source"); return; }
    GVariant *properties = nullptr;
    guint32 node = 0, type = 0;
    g_variant_get_child(streams, 0, "(u@a{sv})", &node, &properties);
    g_variant_lookup(properties, "source_type", "u", &type);
    g_variant_unref(properties);
    if (type != self->m_selectedType || !node) { self->fail(CaptureState::Denied, "Portal returned a different or unidentified source type"); return; }
    self->m_fd = xdp_session_open_pipewire_remote(self->m_native);
    if (self->m_fd < 0) { self->fail(CaptureState::Denied, "Could not open the portal-scoped PipeWire connection"); return; }
    auto *native = gst_element_factory_make("pipewiresrc", nullptr);
    if (!native) { self->fail(CaptureState::BackendMissing, "GStreamer PipeWire source is missing"); return; }
    const auto path = QByteArray::number(node);
    g_object_set(native, "fd", self->m_fd, "path", path.constData(), "do-timestamp", TRUE, nullptr);
    QString failure;
    if (!self->m_pipeline.start(native, self->m_fps, failure)) { self->fail(CaptureState::ProtectedOrUnavailable, failure); return; }
    self->m_running = true; self->m_lastFrame.start();
    self->m_status = {CaptureState::Starting, "Waiting for first portal frame"}; Q_EMIT self->statusChanged();
  }
  static void closed(XdpSession *, gpointer data)
  { static_cast<PortalCapture *>(data)->fail(CaptureState::Denied, "Compositor closed or revoked the capture session"); }
  void poll()
  {
    // Bound dispatch work so an active GLib producer cannot starve Stop on the Qt worker.
    for (int i = 0; i < 16 && g_main_context_pending(m_context); ++i) g_main_context_iteration(m_context, FALSE);
    if (!m_running) return;
    QString error;
    auto frame = m_pipeline.pull(error);
    if (!error.isEmpty()) { fail(CaptureState::ProtectedOrUnavailable, error); return; }
    if (!frame) {
      if (m_sequence == 0 && m_lastFrame.elapsed() > 3000) fail(CaptureState::ProtectedOrUnavailable, "No first portal frame within three seconds");
      return;
    }
    if (frame->pixels.size() != m_size) { m_size = frame->pixels.size(); ++m_geometry; }
    frame->session = m_session; frame->source = m_source; frame->sequence = ++m_sequence;
    frame->geometryGeneration = m_geometry;
    frame->physicalGeometry = QRect(QPoint(), m_size);
    m_latest = std::move(frame); m_lastFrame.restart();
    if (m_status.state != CaptureState::Available) { m_status = {CaptureState::Available, {}}; Q_EMIT statusChanged(); }
  }
  bool fail(CaptureState state, const QString &reason) { stop(); m_status = {state, reason}; Q_EMIT statusChanged(); return false; }
  GMainContext *m_context = nullptr;
  XdpPortal *m_portal = nullptr;
  XdpSession *m_native = nullptr;
  GCancellable *m_cancel = nullptr;
  int m_fd = -1, m_fps = 30;
  guint32 m_types = 0, m_selectedType = 0;
  QString m_screen, m_window, m_session, m_source;
  QTimer m_timer;
  QElapsedTimer m_lastFrame;
  CaptureStatus m_status;
  GstCapturePipeline m_pipeline;
  std::optional<VideoFrame> m_latest;
  bool m_running = false;
  QSize m_size;
  quint64 m_geometry = 0, m_sequence = 0;
};
std::unique_ptr<CaptureDevice> createPortalCaptureDevice() { return std::make_unique<PortalCapture>(); }
} // namespace deskflow::streaming
