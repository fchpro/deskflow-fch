// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "Capture.h"
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <libproc.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <AppKit/AppKit.h>
#include <mutex>

using namespace deskflow::streaming;
struct MacCaptureBuffer {
  QMutex mutex;
  std::optional<VideoFrame> latest;
  QString session, source, error;
  QRect geometry;
  double scale = 1;
  QRect presentedGeometry;
  quint64 sequence = 0, geometryGeneration = 0;
  QSize size;
  bool running = false;
};
@interface DeskflowCaptureOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
  std::shared_ptr<MacCaptureBuffer> state;
}
@property(nonatomic, copy) NSString *generation;
@end
@implementation DeskflowCaptureOutput
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
  QMutexLocker lock(&self->state->mutex);
  if (self->state->session != QString::fromUtf8(self.generation.UTF8String)) return;
  self->state->running = false;
  self->state->latest.reset();
  self->state->error = QString::fromUtf8(error.localizedDescription.UTF8String);
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type
{
  if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sample)) return;
  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
  if (!attachments || CFArrayGetCount(attachments) == 0) return;
  NSDictionary *metadata = (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments, 0);
  if ([metadata[SCStreamFrameInfoStatus] integerValue] != SCFrameStatusComplete) return;
  auto pixel = CMSampleBufferGetImageBuffer(sample);
  if (!pixel || CVPixelBufferGetPixelFormatType(pixel) != kCVPixelFormatType_32BGRA) return;
  const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
  if (!CMTIME_IS_NUMERIC(pts)) return;
  CVPixelBufferLockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
  VideoFrame frame;
  frame.pixels = QImage(static_cast<const uchar *>(CVPixelBufferGetBaseAddress(pixel)),
      int(CVPixelBufferGetWidth(pixel)), int(CVPixelBufferGetHeight(pixel)),
      qsizetype(CVPixelBufferGetBytesPerRow(pixel)), QImage::Format_ARGB32).copy();
  CVPixelBufferUnlockBaseAddress(pixel, kCVPixelBufferLock_ReadOnly);
  frame.captureTimeNs = CMTimeConvertScale(pts, 1000000000, kCMTimeRoundingMethod_Default).value;
  frame.mediaTimeNs = frame.captureTimeNs;
  CGRect screenRect=CGRectNull,contentRect=CGRectNull;
  if(@available(macOS 13.1,*)) {
    id rect=metadata[SCStreamFrameInfoScreenRect];
    if([rect isKindOfClass:NSDictionary.class])CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)rect,&screenRect);
  }
  id content=metadata[SCStreamFrameInfoContentRect];
  if([content isKindOfClass:NSDictionary.class])CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)content,&contentRect);
  const double scale=[metadata[SCStreamFrameInfoScaleFactor] doubleValue];
  // Only publish a control transform when this very sample describes all of its pixels.
  // Shadows, padding, missing metadata and ambiguous scaling remain view-only.
  if(!CGRectIsNull(screenRect) && scale>0 && scale<=8 &&
    CGRectEqualToRect(contentRect,CGRectMake(0,0,frame.pixels.width(),frame.pixels.height()))) {
    frame.physicalGeometry={qRound(screenRect.origin.x*scale),qRound(screenRect.origin.y*scale),qRound(screenRect.size.width*scale),qRound(screenRect.size.height*scale)};
    frame.scale=scale;frame.coordinateMappingValid=!frame.physicalGeometry.isEmpty();
  }
  QMutexLocker lock(&self->state->mutex);
  auto &state = *self->state;
  if (!state.running || state.session != QString::fromUtf8(self.generation.UTF8String)) return;
  if(!frame.coordinateMappingValid){frame.physicalGeometry=state.geometry;frame.scale=state.scale;}
  if (state.size != frame.pixels.size() || state.presentedGeometry!=frame.physicalGeometry) {
    state.size = frame.pixels.size();state.presentedGeometry=frame.physicalGeometry; ++state.geometryGeneration;
  }
  frame.sequence = ++state.sequence;
  frame.geometryGeneration = state.geometryGeneration;
  frame.session = state.session;
  frame.source = state.source;
  state.latest = std::move(frame);
}
@end

namespace deskflow::streaming {
class MacCapture final : public CaptureDevice {
public:
  MacCapture()
  {
    m_queue = dispatch_queue_create("org.deskflow.capture", DISPATCH_QUEUE_SERIAL);
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, [this] {
      QString failure;
      { QMutexLocker lock(&m_buffer.mutex); failure = m_buffer.error; }
      if (!failure.isEmpty()) { fail(CaptureState::ProtectedOrUnavailable, failure); return; }
      NSDictionary *session = CFBridgingRelease(CGSessionCopyCurrentDictionary());
      if (!CGPreflightScreenCaptureAccess()) { fail(CaptureState::Denied, "Screen Recording permission was revoked"); return; }
      if (!session || [session[@"CGSSessionScreenIsLocked"] boolValue]) {
        fail(CaptureState::Denied, "Desktop locked; start again after unlock"); return;
      }
      bool received = false;
      { QMutexLocker lock(&m_buffer.mutex); received = m_buffer.sequence > 0; }
      if (!received && m_lastFrame.elapsed() > 3000) fail(CaptureState::ProtectedOrUnavailable, "No first complete frame within three seconds");
    });
  }
  ~MacCapture() override { stop(); }
  QVector<CaptureSource> sources() override
  {
    if (!CGPreflightScreenCaptureAccess()) {
      m_status = {CaptureState::PermissionRequired, "Enable Screen Recording for Deskflow in System Settings"};
      Q_EMIT statusChanged();
      return {};
    }
    // Enumeration has a bounded wait; late native completion only writes shared local state.
    struct Enumeration { std::mutex mutex; SCShareableContent *content = nil; QString error; bool done = false; };
    auto result = std::make_shared<Enumeration>();
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:NO
      completionHandler:^(SCShareableContent *content, NSError *error) {
        std::lock_guard guard(result->mutex);
        result->content = content;
        result->error = error ? QString::fromUtf8(error.localizedDescription.UTF8String) : QString();
        result->done = true;
      }];
    QEventLoop loop;
    QTimer poll, deadline;
    poll.setInterval(10);
    deadline.setSingleShot(true);
    connect(&poll, &QTimer::timeout, &loop, [&] { std::lock_guard guard(result->mutex); if (result->done) loop.quit(); });
    connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start(); deadline.start(3000); loop.exec();
    std::lock_guard guard(result->mutex);
    if (!result->done || !result->content) {
      m_status = {CaptureState::ProtectedOrUnavailable, result->done ? result->error : "ScreenCaptureKit enumeration timed out"};
      Q_EMIT statusChanged(); return {};
    }
    m_content = result->content;
    QHash<QString, Entry> next;
    QVector<CaptureSource> output;
    for (SCDisplay *display in m_content.displays) {
      Entry entry;
      entry.display = display;
      entry.nativeId = display.displayID;
      entry.source = {{}, "screen", QString("Display %1").arg(display.displayID),
        QRect(int(display.frame.origin.x), int(display.frame.origin.y), int(display.width), int(display.height)),
        display.frame.size.width ? display.width / display.frame.size.width : 1, {CaptureState::Available, {}}};
      add(entry, next, output);
    }
    for (SCWindow *window in m_content.windows) {
      if (!window.owningApplication || window.windowLayer != 0 || !window.title.length) continue;
      Entry entry;
      entry.window = window;
      entry.nativeId = window.windowID;
      struct proc_bsdinfo info{};
      if (proc_pidinfo(window.owningApplication.processID, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info)) continue;
      entry.birth = quint64(info.pbi_start_tvsec) * 1000000 + info.pbi_start_tvusec;
      entry.pid = window.owningApplication.processID;
      entry.source = {{}, "window", QString::fromUtf8(window.title.UTF8String),
        QRect(int(window.frame.origin.x), int(window.frame.origin.y), int(window.frame.size.width), int(window.frame.size.height)),
        1, window.onScreen ? CaptureStatus{CaptureState::Available, {}} :
                            CaptureStatus{CaptureState::TemporarilyUnavailable, "Window is off screen or minimized"}};
      entry.source.audioProcessId = entry.pid;
      entry.source.audioProcessBirth = entry.birth;
      add(entry, next, output);
    }
    m_entries = std::move(next);
    return output;
  }
  bool start(const QString &source, const QString &session, int fps) override
  {
    stop();
    if (!validCaptureGeneration(source) || !validCaptureGeneration(session) || (fps != 30 && fps != 60))
      return fail(CaptureState::Denied, "Invalid accepted capture request");
    if (!CGPreflightScreenCaptureAccess()) return fail(CaptureState::PermissionRequired, "Enable Screen Recording in System Settings");
    if (!m_entries.contains(source)) return fail(CaptureState::SourceGone, "Select the source again");
    const auto entry = m_entries.value(source);
    if (entry.source.status.state != CaptureState::Available) return fail(entry.source.status.state, entry.source.status.reason);
    SCContentFilter *filter = entry.window
      ? [[SCContentFilter alloc] initWithDesktopIndependentWindow:entry.window]
      : [[SCContentFilter alloc] initWithDisplay:entry.display excludingWindows:@[]];
    SCStreamConfiguration *configuration = [SCStreamConfiguration new];
    configuration.pixelFormat = kCVPixelFormatType_32BGRA;
    configuration.colorSpaceName = kCGColorSpaceSRGB;
    configuration.showsCursor = YES;
    if(@available(macOS 14.2,*))configuration.ignoreShadowsSingleWindow=YES;
    configuration.capturesAudio = NO;
    configuration.queueDepth = 3;
    configuration.minimumFrameInterval = CMTimeMake(1, fps);
    configuration.width = entry.source.physicalGeometry.width();
    configuration.height = entry.source.physicalGeometry.height();
    m_selected=entry;
    m_output = [DeskflowCaptureOutput new];
    m_output->state = m_state;
    m_output.generation = [NSString stringWithUTF8String:session.toUtf8().constData()];
    m_stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:m_output];
    NSError *error = nil;
    if (![m_stream addStreamOutput:m_output type:SCStreamOutputTypeScreen sampleHandlerQueue:m_queue error:&error])
      return fail(CaptureState::ProtectedOrUnavailable, QString::fromUtf8(error.localizedDescription.UTF8String));
    { QMutexLocker lock(&m_buffer.mutex);
      m_buffer.session = session; m_buffer.source = source; m_buffer.geometry = entry.source.physicalGeometry;
      m_buffer.scale = entry.source.scale; m_buffer.running = true; m_buffer.error.clear();
      m_buffer.sequence = 0; m_buffer.geometryGeneration = 0; m_buffer.size = {};m_buffer.presentedGeometry={}; }
    auto output = m_output;
    [m_stream startCaptureWithCompletionHandler:^(NSError *failure) {
      if (failure) [output stream:nil didStopWithError:failure];
    }];
    m_lastFrame.start(); m_timer.start();
    m_status = {CaptureState::Starting, "Waiting for first captured frame"};
    Q_EMIT statusChanged(); return true;
  }
  void stop() override
  {
    m_timer.stop();
    { QMutexLocker lock(&m_buffer.mutex); m_buffer.running = false; m_buffer.latest.reset(); m_buffer.session.clear(); }
    if (m_stream) {
      [m_stream stopCaptureWithCompletionHandler:nil];
      [m_stream removeStreamOutput:m_output type:SCStreamOutputTypeScreen error:nil];
      dispatch_sync(m_queue, ^{}); // quiesce frame callbacks before buffer lifetime ends
      m_stream = nil;
    }
    m_status = {CaptureState::Stopped, {}};
    Q_EMIT statusChanged();
  }
  std::optional<VideoFrame> takeFrame() override
  {
    QMutexLocker lock(&m_buffer.mutex);
    auto result = std::exchange(m_buffer.latest, {});
    if (result) { m_lastFrame.restart(); m_status = {CaptureState::Available, {}};m_controlScale=result->coordinateMappingValid?result->scale:0; }
    return result;
  }
  CaptureStatus status() const override { return m_status; }
  QJsonObject controlTarget()const override {
    if(m_buffer.session.isEmpty() || m_controlScale<=0)return {};
    return {{"session",m_buffer.session},{"source",m_selected.source.id},{"kind",m_selected.source.kind},
      {"handle",qint64(m_selected.nativeId)},{"pid",qint64(m_selected.pid)},
      {"birth",QString::number(m_selected.birth,16)},{"scale",m_controlScale}};
  }
private:
  struct Entry { CaptureSource source; SCDisplay *display = nil; SCWindow *window = nil; quint64 nativeId = 0, birth = 0; pid_t pid = 0; };
  void add(Entry &entry, QHash<QString, Entry> &next, QVector<CaptureSource> &output)
  {
    for (const auto &previous : std::as_const(m_entries))
      if (previous.nativeId == entry.nativeId && previous.birth == entry.birth && previous.pid == entry.pid &&
          previous.source.kind == entry.source.kind) { entry.source.id = previous.source.id; break; }
    if (entry.source.id.isEmpty()) entry.source.id = QUuid::createUuid().toString(QUuid::Id128);
    next.insert(entry.source.id, entry); output.append(entry.source);
  }
  bool fail(CaptureState state, const QString &reason) { stop(); m_status = {state, reason}; Q_EMIT statusChanged(); return false; }
  QHash<QString, Entry> m_entries;
  Entry m_selected;double m_controlScale=0;
  SCShareableContent *m_content = nil;
  SCStream *m_stream = nil;
  DeskflowCaptureOutput *m_output;
  dispatch_queue_t m_queue;
  std::shared_ptr<MacCaptureBuffer> m_state = std::make_shared<MacCaptureBuffer>();
  MacCaptureBuffer &m_buffer = *m_state;
  QTimer m_timer;
  QElapsedTimer m_lastFrame;
  CaptureStatus m_status;
};
std::unique_ptr<CaptureDevice> createCaptureDevice() { return std::make_unique<MacCapture>(); }
} // namespace deskflow::streaming
