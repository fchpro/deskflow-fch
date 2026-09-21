// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "NativeAudio.h"
#include <QMutex>
#include <QMutexLocker>
#include <QEventLoop>
#include <QTimer>
#include <libproc.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreMedia/CoreMedia.h>
#import <AppKit/AppKit.h>
#include <vector>
using namespace deskflow::streaming;
namespace {
struct Samples {
  QMutex mutex;
  GstElement *source = nullptr; // borrowed; detached synchronously before the graph is released
  QString error;
};
QString uid(AudioDeviceID id)
{
  AudioObjectPropertyAddress address{kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  CFStringRef value = nullptr; UInt32 size = sizeof(value);
  if (AudioObjectGetPropertyData(id, &address, 0, nullptr, &size, &value) != noErr || !value) return {};
  const auto result = QString::fromNSString((__bridge NSString *)value); CFRelease(value); return result;
}
std::vector<AudioDeviceID> devices()
{
  AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) != noErr) return {};
  std::vector<AudioDeviceID> result(size / sizeof(AudioDeviceID));
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, result.data()) != noErr) return {};
  return result;
}
}
@interface DeskflowAudioOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public std::shared_ptr<Samples> state;
}
@end
@implementation DeskflowAudioOutput
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
  QMutexLocker lock(&state->mutex);
  state->error = QString::fromNSString(error.localizedDescription);
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type
{
  if (type != SCStreamOutputTypeAudio || !CMSampleBufferIsValid(sample)) return;
  QMutexLocker lock(&state->mutex);
  if (!state->source) return;
  const auto format = CMSampleBufferGetFormatDescription(sample);
  const auto *description = CMAudioFormatDescriptionGetStreamBasicDescription(format);
  const auto count = CMSampleBufferGetNumSamples(sample);
  const auto pts = CMSampleBufferGetPresentationTimeStamp(sample);
  if (!description || description->mFormatID != kAudioFormatLinearPCM ||
      !(description->mFormatFlags & kAudioFormatFlagIsFloat) || description->mBitsPerChannel != 32 ||
      description->mChannelsPerFrame != 2 || description->mSampleRate != 48000 ||
      count <= 0 || count > 4800 || !CMTIME_IS_NUMERIC(pts)) {
    state->error = "ScreenCaptureKit returned an unsupported audio format"; return;
  }
  size_t needed = 0;
  CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, &needed, nullptr, 0, nullptr, nullptr, 0, nullptr);
  std::vector<uint8_t> storage(needed);
  auto *list = reinterpret_cast<AudioBufferList *>(storage.data());
  CMBlockBufferRef retained = nullptr;
  if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sample, nullptr, list, needed,
      kCFAllocatorDefault, kCFAllocatorDefault, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &retained) != noErr) {
    state->error = "ScreenCaptureKit audio sample mapping failed"; return;
  }
  const bool planar = description->mFormatFlags & kAudioFormatFlagIsNonInterleaved;
  const bool valid = planar ? list->mNumberBuffers == 2 && list->mBuffers[0].mDataByteSize >= count * 4 &&
    list->mBuffers[1].mDataByteSize >= count * 4 : list->mNumberBuffers == 1 && list->mBuffers[0].mDataByteSize >= count * 8;
  if (!valid) { CFRelease(retained); state->error = "ScreenCaptureKit audio plane size changed"; return; }
  auto *buffer = gst_buffer_new_allocate(nullptr, count * 8, nullptr);
  GstMapInfo map; gst_buffer_map(buffer, &map, GST_MAP_WRITE);
  auto *pcm = reinterpret_cast<float *>(map.data);
  if (planar) {
    for (CMItemCount i = 0; i < count; ++i) for (int channel = 0; channel < 2; ++channel)
      pcm[2 * i + channel] = static_cast<const float *>(list->mBuffers[channel].mData)[i];
  } else memcpy(pcm, list->mBuffers[0].mData, count * 8);
  gst_buffer_unmap(buffer, &map); CFRelease(retained);
  const auto native = CMTimeConvertScale(pts, GST_SECOND, kCMTimeRoundingMethod_Default).value;
  const auto base = gst_element_get_base_time(state->source);
  if (!GST_CLOCK_TIME_IS_VALID(base) || native < base) { gst_buffer_unref(buffer); return; }
  GST_BUFFER_PTS(buffer) = native - base; GST_BUFFER_DURATION(buffer) = count * GST_SECOND / 48000;
  const auto flow = gst_app_src_push_buffer(GST_APP_SRC(state->source), buffer);
  if (flow != GST_FLOW_OK && flow != GST_FLOW_FLUSHING) state->error = "ScreenCaptureKit PCM graph rejected samples";
}
@end
namespace deskflow::streaming {
quint64 nativeAudioProcessBirth(quint32 pid)
{
  proc_bsdinfo info{};
  if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info)) return 0;
  return quint64(info.pbi_start_tvsec) * 1000000 + info.pbi_start_tvusec;
}
QVector<AudioEndpoint> nativeAudioEndpoints(QString &error)
{
  QVector<AudioEndpoint> result;
  AudioDeviceID defaultDevice = kAudioObjectUnknown;
  AudioObjectPropertyAddress defaultAddress{kAudioHardwarePropertyDefaultOutputDevice,
    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  UInt32 defaultBytes = sizeof(defaultDevice);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &defaultAddress, 0, nullptr,
        &defaultBytes, &defaultDevice) != noErr) defaultDevice = kAudioObjectUnknown;
  for (auto id : devices()) {
    AudioObjectPropertyAddress address{kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain};
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(id, &address, 0, nullptr, &bytes) != noErr || !bytes) continue;
    const auto identity = uid(id); if (identity.isEmpty()) continue;
    address = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef name = nullptr; bytes = sizeof(name);
    if (AudioObjectGetPropertyData(id, &address, 0, nullptr, &bytes, &name) != noErr || !name) continue;
    result.push_back({identity, QString::fromNSString((__bridge NSString *)name), id == defaultDevice}); CFRelease(name);
  }
  if (result.isEmpty()) error = "No accessible CoreAudio render device";
  return result;
}
GstElement *nativeAudioSink(const QString &identity, QString &error)
{
  for (auto id : devices()) if (uid(id) == identity) {
    auto *sink = gst_element_factory_make("osxaudiosink", nullptr);
    if (!sink) { error = "Required osxaudiosink plugin is missing"; return nullptr; }
    g_object_set(sink, "device", gint(id), "unique-id", identity.toUtf8().constData(), nullptr); return sink;
  }
  error = "Selected CoreAudio device disappeared"; return nullptr;
}
class MacAudioCapture final : public NativeAudioCapture {
public:
  std::shared_ptr<Samples> state = std::make_shared<Samples>();
  SCStream *stream = nil;
  DeskflowAudioOutput *output = nil;
  GstElement *owned = nullptr;
  AudioSelection selection;
  ~MacAudioCapture() override {
    { QMutexLocker lock(&state->mutex); state->source = nullptr; }
    if (stream) [stream stopCaptureWithCompletionHandler:^(NSError *) {}];
    if (owned) gst_object_unref(owned);
  }
  GstElement *takeSource() override { return std::exchange(owned, nullptr); }
  bool start(QString &) override {
    auto shared = state;
    [stream startCaptureWithCompletionHandler:^(NSError *failure) {
      if (failure) { QMutexLocker lock(&shared->mutex); shared->error = QString::fromNSString(failure.localizedDescription); }
    }];
    return true;
  }
  QString error() override {
    if (!CGPreflightScreenCaptureAccess()) return "Screen Recording audio permission was revoked";
    NSDictionary *session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
    if (!session || [session[@"CGSSessionScreenIsLocked"] boolValue]) return "Desktop locked; audio stopped";
    if (selection.scope == "application" && nativeAudioProcessBirth(selection.processId) != selection.processBirth)
      return "Selected audio process exited or changed identity";
    QMutexLocker lock(&state->mutex); return state->error;
  }
};
std::unique_ptr<NativeAudioCapture> createNativeAudioCapture(const AudioSelection &selection, QString &error)
{
  if (@available(macOS 13.0, *)) {} else { error = "ScreenCaptureKit audio requires macOS 13 or newer"; return {}; }
  if (!CGPreflightScreenCaptureAccess()) { error = "Enable Screen Recording for Deskflow before selecting audio"; return {}; }
  if (selection.scope != "system" && selection.scope != "application") { error = "Unsupported audio scope"; return {}; }
  if (!selection.deviceId.isEmpty()) { error = "ScreenCaptureKit does not support selecting one audio endpoint"; return {}; }
  if (selection.scope == "application" && selection.processId == quint32(getpid())) {
    error = "Deskflow playback is excluded from ScreenCaptureKit audio capture"; return {};
  }
  if (selection.scope == "application" && (!selection.processBirth || nativeAudioProcessBirth(selection.processId) != selection.processBirth)) {
    error = "Selected audio process identity is stale"; return {};
  }
  struct Content { QMutex mutex; SCShareableContent *value = nil; QString error; bool done = false; };
  auto content = std::make_shared<Content>();
  [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:NO
    completionHandler:^(SCShareableContent *value, NSError *failure) {
      QMutexLocker lock(&content->mutex); content->value = value;
      content->error = failure ? QString::fromNSString(failure.localizedDescription) : QString(); content->done = true;
    }];
  QEventLoop loop; QTimer poll, deadline; poll.setInterval(10); deadline.setSingleShot(true);
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] { QMutexLocker lock(&content->mutex); if (content->done) loop.quit(); });
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start(); deadline.start(3000); loop.exec();
  QMutexLocker lock(&content->mutex);
  if (!content->done || !content->value || content->value.displays.count == 0) {
    error = content->error.isEmpty() ? "ScreenCaptureKit audio enumeration timed out or has no display" : content->error; return {};
  }
  SCDisplay *display = content->value.displays.firstObject;
  SCContentFilter *filter = nil;
  if (selection.scope == "application") {
    SCRunningApplication *selected = nil;
    for (SCRunningApplication *app in content->value.applications) if (app.processID == selection.processId) selected = app;
    if (!selected || nativeAudioProcessBirth(selection.processId) != selection.processBirth) { error = "Selected application is unavailable"; return {}; }
    filter = [[SCContentFilter alloc] initWithDisplay:display includingApplications:@[selected] exceptingWindows:@[]];
  } else filter = [[SCContentFilter alloc] initWithDisplay:display excludingApplications:@[] exceptingWindows:@[]];
  auto result = std::make_unique<MacAudioCapture>(); result->selection = selection;
  result->owned = gst_element_factory_make("appsrc", nullptr);
  if (!result->owned) { error = "Required appsrc plugin is missing"; return {}; }
  auto *caps = gst_caps_from_string("audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved");
  g_object_set(result->owned, "caps", caps, "format", GST_FORMAT_TIME, "is-live", TRUE, "block", FALSE,
    "max-bytes", guint64(38400), "leaky-type", 2, nullptr); gst_caps_unref(caps);
  result->state->source = result->owned;
  auto *configuration = [SCStreamConfiguration new];
  configuration.capturesAudio = YES; configuration.excludesCurrentProcessAudio = YES;
  configuration.sampleRate = 48000; configuration.channelCount = 2;
  configuration.width = 2; configuration.height = 2; configuration.queueDepth = 3;
  result->output = [DeskflowAudioOutput new]; result->output->state = result->state;
  result->stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:result->output];
  NSError *failure = nil;
  if (![result->stream addStreamOutput:result->output type:SCStreamOutputTypeAudio
      sampleHandlerQueue:dispatch_queue_create("org.deskflow.audio", DISPATCH_QUEUE_SERIAL) error:&failure]) {
    error = QString::fromNSString(failure.localizedDescription); return {};
  }
  return result;
}
} // namespace deskflow::streaming
