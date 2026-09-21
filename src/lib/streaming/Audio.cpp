// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "Audio.h"
#include "GstCapturePipeline.h"
#include "NativeAudio.h"
#include <gst/audio/audio.h>
#include <atomic>
#include <cmath>
#include <limits>
#ifdef Q_OS_WIN
#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#endif
namespace deskflow::streaming {
namespace {
// Single media role per process; guards independent workers as well as one worker's objects.
std::atomic_int role{0}; // 0 idle, 1 source, 2 receiver
constexpr quint64 maxQueueBytes = 48000 * 2 * sizeof(float) / 10;
QString busError(GstElement *pipeline)
{
  auto *bus = gst_element_get_bus(pipeline);
  auto *message = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
  gst_object_unref(bus);
  if (!message) return {};
  QString result = "Audio stream ended";
  if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
    GError *failure = nullptr; gchar *debug = nullptr;
    gst_message_parse_error(message, &failure, &debug);
    // Backend diagnostics can contain private device IDs, paths and server names.
    result = "Audio backend failed. Check the selected device and installed audio components.";
    g_clear_error(&failure); g_free(debug);
  }
  gst_message_unref(message);
  return result;
}
GstCaps *pcmCaps() { return gst_caps_from_string("audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved"); }
void systemClock(GstElement *pipeline)
{
  auto *clock = gst_system_clock_obtain();
  gst_pipeline_use_clock(GST_PIPELINE(pipeline), clock);
  gst_object_unref(clock);
}
}
AudioBlock fileAudioBlock(const FileAudioBlock &block)
{
  return {block.samples, block.session, block.source, block.timelineEpoch, block.mediaTimeNs, block.durationNs, 0};
}
qint64 audioHostTimeNs()
{
  auto *clock = gst_system_clock_obtain();
  const auto now = gst_clock_get_time(clock);
  gst_object_unref(clock);
  return qint64(now);
}
bool validAudioBlock(const AudioBlock &block)
{
  return validCaptureGeneration(block.session) && validCaptureGeneration(block.source) &&
    !block.samples.isEmpty() && block.samples.size() <= maxQueueBytes && block.samples.size() % 8 == 0 &&
    block.mediaTimeNs >= 0 && block.mediaTimeNs <= (std::numeric_limits<qint64>::max)() - 100 * GST_MSECOND &&
    block.durationNs == qint64(block.samples.size() / 8) * GST_SECOND / 48000;
}
quint64 audioProcessBirth(quint32 pid)
{
#ifdef Q_OS_WIN
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return 0;
  FILETIME created{}, exited{}, kernel{}, user{};
  const bool ok = GetProcessTimes(process, &created, &exited, &kernel, &user);
  CloseHandle(process);
  return ok ? (quint64(created.dwHighDateTime) << 32) | created.dwLowDateTime : 0;
#else
  return nativeAudioProcessBirth(pid);
#endif
}
QVector<AudioEndpoint> audioOutputEndpoints(QString &error)
{
  QVector<AudioEndpoint> result;
#ifdef Q_OS_WIN
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  using Microsoft::WRL::ComPtr;
  ComPtr<IMMDeviceEnumerator> enumerator;
  ComPtr<IMMDeviceCollection> collection;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (SUCCEEDED(hr)) hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
  UINT count = 0;
  if (SUCCEEDED(hr)) hr = collection->GetCount(&count);
  QString defaultId;
  ComPtr<IMMDevice> defaultDevice;
  if (enumerator && SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &defaultDevice))) {
    LPWSTR id = nullptr;
    if (SUCCEEDED(defaultDevice->GetId(&id))) {
      defaultId = QString::fromWCharArray(id);
      CoTaskMemFree(id);
    }
  }
  for (UINT i = 0; SUCCEEDED(hr) && i < count; ++i) {
    ComPtr<IMMDevice> device; LPWSTR id = nullptr;
    if (FAILED(collection->Item(i, &device)) || FAILED(device->GetId(&id))) continue;
    AudioEndpoint entry{QString::fromWCharArray(id), {}};
    entry.isDefault = entry.id == defaultId;
    CoTaskMemFree(id);
    ComPtr<IPropertyStore> properties;
    PROPVARIANT name; PropVariantInit(&name);
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)) &&
        SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR)
      entry.name = QString::fromWCharArray(name.pwszVal);
    PropVariantClear(&name);
    result.push_back(entry);
  }
  if (FAILED(hr)) error = "Could not enumerate active Windows render endpoints";
  defaultDevice.Reset(); collection.Reset(); enumerator.Reset();
  if (SUCCEEDED(com)) CoUninitialize();
#else
  return nativeAudioEndpoints(error);
#endif
  return result;
}
AudioInput::AudioInput() = default;
AudioInput::~AudioInput() { stop(); }
void AudioInput::stop()
{
  m_native.reset();
  if (m_pipeline) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL); gst_object_unref(m_pipeline);
    m_pipeline = nullptr; m_sink = nullptr; role.store(0);
  }
#ifdef Q_OS_WIN
  if (m_process) CloseHandle(m_process);
#endif
  m_process = nullptr;
}
void AudioInput::fail(const QString &reason) { stop(); m_error = reason; }
bool AudioInput::start(const AudioSelection &selection, const QString &session, const QString &source, quint64 epoch)
{
  stop(); m_error.clear();
  if (selection.scope == "off") return true;
  if (!GstCapturePipeline::initialize(m_error)) return false;
#ifdef Q_OS_WIN
  if (selection.scope != "system" && selection.scope != "application") {
    m_error = "Unsupported audio scope"; return false;
  }
  auto *element = gst_element_factory_make("wasapi2src", nullptr);
  if (!element) { m_error = "Required wasapi2src plugin is missing"; return false; }
  HANDLE process = nullptr;
  if (selection.scope == "application") {
    if (!selection.processId || !selection.processBirth ||
        !g_object_class_find_property(G_OBJECT_GET_CLASS(element), "loopback-target-pid") ||
        audioProcessBirth(selection.processId) != selection.processBirth) {
      gst_object_unref(element); m_error = "Selected process identity or process-loopback API is unavailable"; return false;
    }
    process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, selection.processId);
    if (!process || WaitForSingleObject(process, 0) != WAIT_TIMEOUT ||
        audioProcessBirth(selection.processId) != selection.processBirth) {
      if (process) CloseHandle(process);
      gst_object_unref(element); m_error = "Selected audio process exited or changed identity"; return false;
    }
    g_object_set(element, "loopback-mode", 1, "loopback-target-pid", selection.processId, nullptr);
  } else {
    const auto endpoints = audioOutputEndpoints(m_error);
    if (selection.deviceId.isEmpty() || !std::any_of(endpoints.begin(), endpoints.end(),
        [&](const auto &entry) { return entry.id == selection.deviceId; })) {
      gst_object_unref(element); m_error = "Select an active render endpoint for system audio"; return false;
    }
    g_object_set(element, "device", selection.deviceId.toUtf8().constData(), "loopback", TRUE, nullptr);
  }
  g_object_set(element, "continue-on-error", FALSE, "low-latency", TRUE, "provide-clock", FALSE,
    "slave-method", 0, "buffer-time", gint64(40000), "latency-time", gint64(10000), nullptr);
  if (!startSource(element, session, source, epoch)) { if (process) CloseHandle(process); return false; }
  m_process = process;
  return true;
#else
  auto native = createNativeAudioCapture(selection, m_error);
  if (!native) return false;
  if (!startSource(native->takeSource(), session, source, epoch)) return false;
  m_native = std::move(native);
  if (!m_native->start(m_error)) { const auto failure = m_error; fail(failure); return false; }
  return true;
#endif
}
bool AudioInput::startSource(GstElement *source, const QString &session, const QString &identity, quint64 epoch)
{
  stop(); m_error.clear();
  int idle = 0;
  if (!source || !validCaptureGeneration(session) || !validCaptureGeneration(identity) ||
      !role.compare_exchange_strong(idle, 1)) {
    if (source) gst_object_unref(source);
    m_error = "Invalid audio identity or another local media role is active"; return false;
  }
  m_pipeline = gst_pipeline_new(nullptr);
  auto *convert = gst_element_factory_make("audioconvert", nullptr);
  auto *resample = gst_element_factory_make("audioresample", nullptr);
  auto *sink = gst_element_factory_make("appsink", nullptr);
  if (!convert || !resample || !sink) {
    for (auto *item : {source, convert, resample, sink}) if (item) gst_object_unref(item);
    fail("Required PCM conversion plugin is missing"); return false;
  }
  auto *caps = pcmCaps();
  g_object_set(sink, "caps", caps, "max-buffers", 4U, "max-bytes", maxQueueBytes,
    "max-time", guint64(100 * GST_MSECOND), "leaky-type", 2, "sync", FALSE, "wait-on-eos", FALSE,
    "enable-last-sample", FALSE, nullptr);
  gst_caps_unref(caps);
  gst_bin_add_many(GST_BIN(m_pipeline), source, convert, resample, sink, nullptr);
  if (!gst_element_link_many(source, convert, resample, sink, nullptr)) {
    fail("Audio source cannot negotiate 48 kHz stereo PCM"); return false;
  }
  m_sink = GST_APP_SINK(sink); m_session = session; m_source = identity; m_epoch = epoch;
  systemClock(m_pipeline);
  if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    fail("Audio capture backend refused to start"); return false;
  }
  return true;
}
std::optional<AudioBlock> AudioInput::takeAudio()
{
  if (!m_pipeline) return {};
  if (m_native) if (const auto failure = m_native->error(); !failure.isEmpty()) { fail(failure); return {}; }
#ifdef Q_OS_WIN
  if (m_process && WaitForSingleObject(m_process, 0) != WAIT_TIMEOUT) {
    fail("Selected audio process exited"); return {};
  }
#endif
  if (const auto failure = busError(m_pipeline); !failure.isEmpty()) { fail(failure); return {}; }
  auto *sample = gst_app_sink_try_pull_sample(m_sink, 0);
  if (!sample) return {};
  auto *buffer = gst_sample_get_buffer(sample);
  GstMapInfo map; GstAudioInfo info;
  std::optional<AudioBlock> result;
  if (GST_BUFFER_PTS_IS_VALID(buffer) && gst_audio_info_from_caps(&info, gst_sample_get_caps(sample)) &&
      GST_AUDIO_INFO_FORMAT(&info) == GST_AUDIO_FORMAT_F32LE && GST_AUDIO_INFO_RATE(&info) == 48000 &&
      GST_AUDIO_INFO_CHANNELS(&info) == 2 && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    AudioBlock block{QByteArray(reinterpret_cast<const char *>(map.data), qsizetype(map.size)), m_session, m_source,
      m_epoch, qint64(GST_BUFFER_PTS(buffer)), qint64(map.size / 8) * GST_SECOND / 48000,
      qint64(gst_element_get_base_time(m_pipeline) + GST_BUFFER_PTS(buffer))};
    gst_buffer_unmap(buffer, &map);
    if (validAudioBlock(block)) result = std::move(block);
  }
  gst_sample_unref(sample);
  if (!result) fail("Invalid audio PCM format, buffer size or timestamp");
  return result;
}
AudioOutput::~AudioOutput() { stop(); }
void AudioOutput::stop()
{
  if (m_pipeline) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL); gst_object_unref(m_pipeline);
    m_pipeline = nullptr; m_source = nullptr; m_volume = nullptr; role.store(0);
  }
}
void AudioOutput::fail(const QString &reason) { stop(); m_error = reason; }
bool AudioOutput::start(const QString &deviceId, const QString &session, const QString &source, quint64 epoch, qint64 origin, qint64 margin)
{
  stop(); m_error.clear();
  if (!GstCapturePipeline::initialize(m_error)) return false;
  const auto endpoints = audioOutputEndpoints(m_error);
  if (deviceId.isEmpty() || !std::any_of(endpoints.begin(), endpoints.end(), [&](const auto &e) { return e.id == deviceId; })) {
    m_error = "Select an active audio output endpoint"; return false;
  }
#ifdef Q_OS_WIN
  auto *sink = gst_element_factory_make("wasapi2sink", nullptr);
  if (!sink) { m_error = "Required wasapi2sink plugin is missing"; return false; }
  g_object_set(sink, "device", deviceId.toUtf8().constData(), "continue-on-error", FALSE, "low-latency", TRUE,
    "provide-clock", FALSE, "slave-method", 0, "buffer-time", gint64(40000), "latency-time", gint64(10000), nullptr);
  return startSink(sink, session, source, epoch, origin, margin);
#else
  auto *sink = nativeAudioSink(deviceId, m_error);
  if (!sink) return false;
  return startSink(sink, session, source, epoch, origin, margin);
#endif
}
bool AudioOutput::startSink(GstElement *sink, const QString &session, const QString &source, quint64 epoch, qint64 origin, qint64 margin)
{
  stop(); m_error.clear();
  int idle = 0;
  if (!sink || !validCaptureGeneration(session) || !validCaptureGeneration(source) || origin < 0 || margin < 0 || margin > 100 * GST_MSECOND ||
      !role.compare_exchange_strong(idle, 2)) {
    if (sink) gst_object_unref(sink);
    m_error = "Invalid audio identity or another local media role is active"; return false;
  }
  m_pipeline = gst_pipeline_new(nullptr);
  auto *input = gst_element_factory_make("appsrc", nullptr);
  auto *convert = gst_element_factory_make("audioconvert", nullptr);
  auto *resample = gst_element_factory_make("audioresample", nullptr);
  auto *volume = gst_element_factory_make("volume", nullptr);
  if (!input || !convert || !resample || !volume) {
    for (auto *item : {sink, input, convert, resample, volume}) if (item) gst_object_unref(item);
    fail("Required PCM output plugin is missing"); return false;
  }
  auto *caps = pcmCaps();
  // Receiver PCM is already timestamped/buffered. Permit bounded preroll when paused;
  // a GstBaseSrc live unlock races immediate pause/resume before the first buffer.
  g_object_set(input, "caps", caps, "format", GST_FORMAT_TIME, "is-live", FALSE, "block", FALSE,
    "max-bytes", maxQueueBytes, "max-time", guint64(100 * GST_MSECOND), "leaky-type", 2, nullptr);
  gst_caps_unref(caps);
  g_object_set(sink, "sync", TRUE, "async", FALSE, nullptr);
  gst_bin_add_many(GST_BIN(m_pipeline), input, volume, convert, resample, sink, nullptr);
  if (!gst_element_link_many(input, volume, convert, resample, sink, nullptr)) {
    fail("Audio output cannot negotiate PCM"); return false;
  }
  m_source = GST_APP_SRC(input); m_volume = volume; m_session = session; m_identity = source;
  m_epoch = epoch; m_origin = origin; m_lastEnd = -1; m_paused = false; m_pausedTime = 0; m_playoutMargin = margin;
  systemClock(m_pipeline);
  if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    fail("Audio output backend refused to start"); return false;
  }
  return true;
}
QString AudioOutput::error()
{
  if (m_pipeline) if (const auto failure = busError(m_pipeline); !failure.isEmpty()) fail(failure);
  return m_error;
}
bool AudioOutput::push(const AudioBlock &block)
{
  if (!error().isEmpty() || !m_pipeline || m_paused || !validAudioBlock(block) || block.session != m_session ||
      block.source != m_identity || block.timelineEpoch != m_epoch || block.mediaTimeNs < m_origin ||
      block.mediaTimeNs < m_lastEnd) return false;
  if (queuedBytes() + quint64(block.samples.size()) > maxQueueBytes) return false; // explicit backpressure
  auto *buffer = gst_buffer_new_allocate(nullptr, block.samples.size(), nullptr);
  gst_buffer_fill(buffer, 0, block.samples.constData(), block.samples.size());
  GST_BUFFER_PTS(buffer) = block.mediaTimeNs - m_origin + m_playoutMargin;
  GST_BUFFER_DURATION(buffer) = block.durationNs;
  const auto flow = gst_app_src_push_buffer(m_source, buffer);
  if (flow == GST_FLOW_FLUSHING) return false; // state transition has not finished; caller retains/retries this block
  if (flow != GST_FLOW_OK) { fail("Audio output rejected PCM"); return false; }
  m_lastEnd = block.mediaTimeNs + block.durationNs;
  return true;
}
bool AudioOutput::reset(quint64 epoch, qint64 origin)
{
  if (!m_pipeline || epoch <= m_epoch || origin < 0) return false;
  if (gst_element_set_state(m_pipeline, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE) {
    fail("Audio output flush failed"); return false;
  }
  m_epoch = epoch; m_origin = origin; m_lastEnd = -1; m_pausedTime = 0;
  if (gst_element_set_state(m_pipeline, m_paused ? GST_STATE_PAUSED : GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    fail("Audio output restart failed"); return false;
  }
  return true;
}
bool AudioOutput::pause(bool paused)
{
  if (!m_pipeline) return false;
  const auto pausedTime = runningTimeNs();
  if (gst_element_set_state(m_pipeline, paused ? GST_STATE_PAUSED : GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    fail("Audio output pause/resume failed"); return false;
  }
  m_paused = paused;
  if (paused) m_pausedTime = pausedTime;
  return true;
}
bool AudioOutput::setVolume(double volume, bool muted)
{
  if (!m_pipeline || !std::isfinite(volume) || volume < 0 || volume > 1) return false;
  g_object_set(m_volume, "volume", volume, "mute", muted, nullptr);
  return true;
}
qint64 AudioOutput::runningTimeNs() const
{
  if (!m_pipeline) return 0;
  if (m_paused) return m_pausedTime;
  auto *clock = gst_element_get_clock(m_pipeline);
  if (!clock) return 0;
  const auto now = gst_clock_get_time(clock), base = gst_element_get_base_time(m_pipeline);
  gst_object_unref(clock);
  return now >= base ? qint64(now - base) : 0;
}
quint64 AudioOutput::queuedBytes() const { return m_source ? gst_app_src_get_current_level_bytes(m_source) : 0; }
qint64 AudioOutput::mediaOriginNs() const { return m_origin; }
} // namespace deskflow::streaming
