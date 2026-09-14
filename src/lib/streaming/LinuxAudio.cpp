// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "NativeAudio.h"
#include <QFile>
#include <QHash>
#include <QSet>
#include <pipewire/pipewire.h>
#include <map>
#include <mutex>
#include <limits>
#include <unistd.h>
namespace deskflow::streaming {
quint64 nativeAudioProcessBirth(quint32 pid)
{
  QFile file(QString("/proc/%1/stat").arg(pid));
  if (!pid || !file.open(QIODevice::ReadOnly)) return 0;
  const auto raw = file.readAll();
  const auto fields = raw.mid(raw.lastIndexOf(')') + 2).split(' ');
  return fields.size() > 19 ? fields[19].toULongLong() : 0;
}
namespace {
bool processTreeMember(quint32 pid, quint32 root, quint64 birth)
{
  QSet<quint32> visited;
  quint64 youngerThan = (std::numeric_limits<quint64>::max)();
  for (int depth = 0; pid && depth < 64 && !visited.contains(pid); ++depth) {
    visited.insert(pid);
    QFile file(QString("/proc/%1/stat").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto raw = file.readAll(); const auto fields = raw.mid(raw.lastIndexOf(')') + 2).split(' ');
    if (fields.size() <= 19) return false;
    const auto created = fields[19].toULongLong();
    if (!created || created > youngerThan) return false;
    if (pid == root) return created == birth;
    youngerThan = created; pid = fields[1].toUInt();
  }
  return false;
}
struct Node { QString serial, name, kind; uint32_t client = PW_ID_ANY; };
class Graph {
public:
  pw_thread_loop *loop = nullptr;
  pw_context *context = nullptr;
  pw_core *core = nullptr;
  pw_registry *registry = nullptr;
  spa_hook coreHook{}, registryHook{};
  pw_core_events coreEvents{};
  pw_registry_events registryEvents{};
  QString failure;
  int sequence = -1, completed = -1;
  bool started = false;
  std::map<uint32_t, Node> nodes;
  struct Client {
    Graph *owner = nullptr; pw_client *proxy = nullptr; spa_hook hook{}; pw_client_events events{};
    quint32 pid = 0, uid = quint32(-1);
    quint64 birth = 0;
    ~Client() { if (proxy) { spa_hook_remove(&hook); pw_proxy_destroy(reinterpret_cast<pw_proxy *>(proxy)); } }
  };
  std::map<uint32_t, std::unique_ptr<Client>> clients;
  Graph() {
    static std::once_flag initialized; std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
    loop = pw_thread_loop_new("deskflow-audio-registry", nullptr);
    if (!loop) { failure = "Could not create PipeWire registry loop"; return; }
    context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
    if (!context) { failure = "Could not create PipeWire context"; return; }
    core = pw_context_connect(context, nullptr, 0);
    if (!core) { failure = "Cannot connect to this user's PipeWire daemon"; return; }
    coreEvents.version = PW_VERSION_CORE_EVENTS;
    coreEvents.done = [](void *data, uint32_t, int sequence) {
      auto *self = static_cast<Graph *>(data); self->completed = sequence; pw_thread_loop_signal(self->loop, false);
    };
    coreEvents.error = [](void *data, uint32_t, int, int, const char *message) {
      auto *self = static_cast<Graph *>(data); self->failure = QString::fromUtf8(message); pw_thread_loop_signal(self->loop, false);
    };
    pw_core_add_listener(core, &coreHook, &coreEvents, this);
    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (!registry) { failure = "Could not create PipeWire registry proxy"; return; }
    registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
    registryEvents.global = [](void *data, uint32_t id, uint32_t, const char *type, uint32_t version, const spa_dict *properties) {
      auto *self = static_cast<Graph *>(data);
      if (self->nodes.size() + self->clients.size() >= 4096) { self->failure = "PipeWire registry exceeds the bounded audio inventory"; return; }
      if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0 && properties) {
        const auto value = [&](const char *key) { return QString::fromUtf8(spa_dict_lookup(properties, key)); };
        self->nodes[id] = {value("object.serial"), value("node.description"), value("media.class"), value("client.id").toUInt()};
      } else if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
        auto client = std::make_unique<Client>(); client->owner = self;
        client->proxy = static_cast<pw_client *>(pw_registry_bind(self->registry, id, type,
          std::min(version, uint32_t(PW_VERSION_CLIENT)), 0));
        if (!client->proxy) return;
        client->events.version = PW_VERSION_CLIENT_EVENTS;
        client->events.info = [](void *data, const pw_client_info *info) {
          auto *client = static_cast<Client *>(data);
          // Daemon-established socket credentials; never use application.process.id as identity.
          const auto *pid = info->props ? spa_dict_lookup(info->props, "pipewire.sec.pid") : nullptr;
          const auto *uid = info->props ? spa_dict_lookup(info->props, "pipewire.sec.uid") : nullptr;
          client->pid = pid ? QString::fromUtf8(pid).toUInt() : 0;
          client->uid = uid ? QString::fromUtf8(uid).toUInt() : quint32(-1);
          if (!client->birth && client->pid) client->birth = nativeAudioProcessBirth(client->pid);
        };
        pw_client_add_listener(client->proxy, &client->hook, &client->events, client.get());
        self->clients[id] = std::move(client);
      }
    };
    registryEvents.global_remove = [](void *data, uint32_t id) {
      auto *self = static_cast<Graph *>(data); self->nodes.erase(id); self->clients.erase(id);
    };
    pw_registry_add_listener(registry, &registryHook, &registryEvents, this);
    if (pw_thread_loop_start(loop) < 0) { failure = "Could not start PipeWire registry loop"; return; }
    started = true;
    pw_thread_loop_lock(loop);
    // Two daemon round trips: globals, then bound client security properties.
    for (int round = 0; round < 2 && failure.isEmpty(); ++round) {
      sequence = pw_core_sync(core, PW_ID_CORE, sequence);
      timespec deadline{}; pw_thread_loop_get_time(loop, &deadline, SPA_NSEC_PER_SEC);
      while (completed != sequence && failure.isEmpty())
        if (pw_thread_loop_timed_wait_full(loop, &deadline) < 0) { failure = "PipeWire audio registry timed out"; break; }
    }
    pw_thread_loop_unlock(loop);
  }
  ~Graph() {
    if (started) pw_thread_loop_stop(loop);
    clients.clear();
    if (registry) { spa_hook_remove(&registryHook); pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry)); }
    if (core) { spa_hook_remove(&coreHook); pw_core_disconnect(core); }
    if (context) pw_context_destroy(context);
    if (loop) pw_thread_loop_destroy(loop);
  }
  QString error() {
    if (!started) return failure;
    pw_thread_loop_lock(loop); const auto result = failure; pw_thread_loop_unlock(loop); return result;
  }
  QSet<QString> application(quint32 pid, quint64 birth) const {
    QSet<QString> result;
    for (const auto &[id, node] : nodes) {
      const auto client = clients.find(node.client);
      if (node.kind == "Stream/Output/Audio" && client != clients.end() && client->second->uid == geteuid() &&
          client->second->birth && nativeAudioProcessBirth(client->second->pid) == client->second->birth &&
          processTreeMember(client->second->pid, pid, birth) && !node.serial.isEmpty()) result.insert(node.serial);
    }
    return result;
  }
};
GstElement *pipewireElement(const char *factory, const QString &serial, bool monitor, QString &error)
{
  auto *element = gst_element_factory_make(factory, nullptr);
  if (!element || !g_object_class_find_property(G_OBJECT_GET_CLASS(element), "target-object") ||
      !g_object_class_find_property(G_OBJECT_GET_CLASS(element), "stream-properties")) {
    if (element) gst_object_unref(element);
    error = "Required PipeWire plugin target-object/stream-properties API is missing"; return nullptr;
  }
  auto *properties = gst_structure_new("properties", "node.dont-reconnect", G_TYPE_BOOLEAN, TRUE,
    "node.dont-fallback", G_TYPE_BOOLEAN, TRUE, "stream.capture.sink", G_TYPE_BOOLEAN, monitor,
    "node.dont-move", G_TYPE_BOOLEAN, TRUE, "node.passive", G_TYPE_BOOLEAN, strcmp(factory, "pipewiresrc") == 0, nullptr);
  g_object_set(element, "target-object", serial.toUtf8().constData(), "stream-properties", properties, nullptr);
  gst_structure_free(properties);
  return element;
}
class LinuxAudioCapture final : public NativeAudioCapture {
public:
  std::unique_ptr<Graph> graph;
  AudioSelection selection;
  QSet<QString> selected;
  GstElement *owned = nullptr;
  ~LinuxAudioCapture() override { if (owned) gst_object_unref(owned); }
  GstElement *takeSource() override { return std::exchange(owned, nullptr); }
  QString error() override {
    pw_thread_loop_lock(graph->loop);
    QString error = graph->failure;
    QSet<QString> current;
    if (selection.scope == "application") current = graph->application(selection.processId, selection.processBirth);
    else for (const auto &[id, node] : graph->nodes) if (node.serial == selection.deviceId) current.insert(node.serial);
    if (error.isEmpty() && current != selected) error = "Selected PipeWire audio nodes changed or disappeared; select again";
    pw_thread_loop_unlock(graph->loop);
    if (selection.scope == "application" && nativeAudioProcessBirth(selection.processId) != selection.processBirth)
      error = "Selected audio process exited or changed identity";
    return error;
  }
};
}
QVector<AudioEndpoint> nativeAudioEndpoints(QString &error)
{
  Graph graph; QVector<AudioEndpoint> result;
  if (const auto failure = graph.error(); !failure.isEmpty()) { error = failure; return result; }
  pw_thread_loop_lock(graph.loop);
  for (const auto &[id, node] : graph.nodes)
    if (node.kind == "Audio/Sink" && !node.serial.isEmpty()) result.push_back({node.serial, node.name});
  pw_thread_loop_unlock(graph.loop);
  if (result.isEmpty()) error = "No accessible PipeWire render node";
  return result;
}
GstElement *nativeAudioSink(const QString &id, QString &error)
{
  const auto available = nativeAudioEndpoints(error);
  if (!std::any_of(available.begin(), available.end(), [&](const auto &entry) { return entry.id == id; })) {
    error = "Selected PipeWire render node disappeared"; return nullptr;
  }
  return pipewireElement("pipewiresink", id, false, error);
}
std::unique_ptr<NativeAudioCapture> createNativeAudioCapture(const AudioSelection &selection, QString &error)
{
  if (selection.scope != "system" && selection.scope != "application") { error = "Unsupported audio scope"; return {}; }
  if (selection.scope == "application" && (!selection.processBirth || nativeAudioProcessBirth(selection.processId) != selection.processBirth)) {
    error = "Selected audio process identity is stale"; return {};
  }
  auto result = std::make_unique<LinuxAudioCapture>(); result->selection = selection;
  result->graph = std::make_unique<Graph>();
  if (const auto failure = result->graph->error(); !failure.isEmpty()) { error = failure; return {}; }
  auto &graph = *result->graph;
  pw_thread_loop_lock(graph.loop);
  if (selection.scope == "application") result->selected = graph.application(selection.processId, selection.processBirth);
  else for (const auto &[id, node] : graph.nodes)
    if (node.kind == "Audio/Sink" && node.serial == selection.deviceId) result->selected.insert(node.serial);
  pw_thread_loop_unlock(graph.loop);
  if (result->selected.isEmpty() || result->selected.size() > 32) {
    error = "No bounded set of accessible audio nodes with verified process credentials; no system-audio substitution"; return {};
  }
  result->owned = gst_bin_new(nullptr);
  auto *mixer = gst_element_factory_make("audiomixer", nullptr);
  if (!mixer) { error = "Required audiomixer plugin is missing"; return {}; }
  gst_bin_add(GST_BIN(result->owned), mixer);
  for (const auto &serial : result->selected) {
    auto *input = pipewireElement("pipewiresrc", serial, selection.scope == "system", error);
    auto *convert = gst_element_factory_make("audioconvert", nullptr);
    auto *resample = gst_element_factory_make("audioresample", nullptr);
    if (!input || !convert || !resample) {
      for (auto *element : {input, convert, resample}) if (element) gst_object_unref(element);
      if (error.isEmpty()) error = "PCM conversion plugin is missing"; return {};
    }
    gst_bin_add_many(GST_BIN(result->owned), input, convert, resample, nullptr);
    if (!gst_element_link_many(input, convert, resample, mixer, nullptr)) { error = "PipeWire audio graph cannot link selected nodes"; return {}; }
  }
  auto *pad = gst_element_get_static_pad(mixer, "src");
  gst_element_add_pad(result->owned, gst_ghost_pad_new("src", pad)); gst_object_unref(pad);
  return result;
}
} // namespace deskflow::streaming
