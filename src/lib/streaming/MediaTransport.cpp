// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#define NOMINMAX
#include "MediaTransport.h"
#include "MediaPacer.h"
#include "DecodeProgress.h"
#include "AudioClipping.h"
#include "PacketMetadata.h"
#include "VideoKeyframe.h"
#include "GstCapturePipeline.h"
#include "Protocol.h"
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QThread>
#include <QRegularExpression>
#include <QtEndian>
#include <gst/webrtc/webrtc.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtphdrext.h>
#include <gst/video/video.h>
#include <gst/video/video-event.h>
#include <gst/audio/gstaudiometa.h>
#include <nice/agent.h>
#include <atomic>
#include <map>
#include <deque>
#include <condition_variable>
#include <mutex>
#include <cmath>

namespace deskflow::streaming {
namespace {
std::atomic_bool occupied{false};
constexpr char twcc[] = "http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01";
// Five RFC8285 one-byte extension slots; all 80 bytes authenticated by SRTP.
// IDs 2..6 are reserved by Deskflow media v1; TWCC uses ID 1.
QByteArray metadata(const VideoFrame &f) {
  QByteArray b(80, '\0');
  const quint64 values[] = {f.sequence, quint64(f.captureTimeNs), quint64(f.mediaTimeNs), f.timelineEpoch,
    f.geometryGeneration, quint64(qint64(f.physicalGeometry.x())), quint64(qint64(f.physicalGeometry.y())),
    (quint64(quint32(f.physicalGeometry.width())) << 32) | quint32(f.physicalGeometry.height()),
    quint64(f.scale * 1000000), f.coordinateMappingValid ? 1ULL : 0ULL};
  for (int i=0;i<10;++i) qToBigEndian(values[i], b.data()+8*i);
  return b;
}
VideoFrame metadata(const QByteArray &b) {
  VideoFrame f; quint64 v[10];
  for (int i=0;i<10;++i) v[i]=qFromBigEndian<quint64>(b.constData()+8*i);
  f.sequence=v[0]; f.captureTimeNs=qint64(v[1]); f.mediaTimeNs=qint64(v[2]); f.timelineEpoch=v[3];
  f.geometryGeneration=v[4]; f.physicalGeometry=QRect(int(qint64(v[5])),int(qint64(v[6])),int(v[7]>>32),int(quint32(v[7])));
  f.scale=double(v[8])/1000000; f.coordinateMappingValid=v[9]==1;
  return f;
}
QString fingerprint(const QString &sdp) {
  for (const auto &line:sdp.split("\r\n")) if(line.startsWith("a=fingerprint:sha-256 ")) return line.mid(22);
  return {};
}
bool candidate(const QString &value,const QHostAddress &address) {
  const auto t=value.split(' '); bool ok=false;
  const uint port=t.size()>5?t[5].toUInt(&ok):0;
  return t.size()>=8 && t.size()<=18 && t[0].startsWith("candidate:") && t[1]=="1" && t[2].toUpper()=="UDP" &&
    ok && port>=24802 && port<=24831 && t[6]=="typ" && t[7]=="host" && QHostAddress(t[4])==address;
}
}
std::optional<MediaPreset> mediaPreset(const QString &name) {
  if(name=="low") return MediaPreset{1280,720,30,2000000,3000000};
  if(name=="balanced") return MediaPreset{1920,1080,30,5000000,8000000};
  if(name=="smooth") return MediaPreset{1920,1080,60,8000000,12000000};
  return {};
}
struct MediaTransport::Impl {
  MediaTransport *owner;
  MediaConfiguration config;
  MediaPreset preset{};
  GstElement *pipeline=nullptr,*rtc=nullptr,*encoder=nullptr,*gcc=nullptr;
  GstAppSrc *video=nullptr,*audio=nullptr;
  GstAppSink *videoSink=nullptr,*audioSink=nullptr;
  mutable std::mutex mutex;
  std::map<quint64,VideoFrame> sentAudio;
  PacketMetadata packetMetadata;
  std::map<quint64,AudioClipping> encodedClipping;
  MediaPacer pacer;
  std::atomic_bool pacerFailed{false},stopping{false};
  std::condition_variable pacerSpace;
  QString failure;
  MediaStatistics stats;
  QElapsedTimer elapsed;
  qint64 connectedAt=-1,lowSince=-1,lastHeartbeat=0,lastLog=0,lastKeyRequest=0;
  quint64 token=0,receiveEpoch=0;
  std::atomic<quint64> epoch{0};
  qint64 lastVideo=-1,lastVideoPts=-1,nextVideoDue=0,anchorMedia=0,anchorRunning=0;
  bool owned=false,offered=false,remoteSet=false,anchored=false;
  std::optional<VideoFrame> latestVideo;
  std::optional<quint64> submittedAudioEpoch;
  std::optional<std::pair<quint64,quint64>> deliveredVideo;
  // Only authenticated video identities can establish progress past the last decoded frame.
  // Repeated static-frame identities and a paused source cannot arm this deadline.
  DecodeProgress decodeProgress;
  qint64 lastInputAt=0;
  std::atomic_bool repeatRequested{false};
  std::vector<std::pair<uint,QString>> candidates;
  explicit Impl(MediaTransport *o):owner(o) {}
  void fail(const QString &why) {
    if(!failure.isEmpty()) return;
    failure=why;
    qWarning().noquote()<<"Streaming media:"<<why;
    const auto generation=token;
    QMetaObject::invokeMethod(owner,[this,generation]{if(token==generation)owner->stop();},Qt::QueuedConnection);
    if(pipeline)Q_EMIT owner->signaling(envelope("Stop"));
    Q_EMIT owner->failed(why);
  }
  void postFailure(const QString &why) {
    const auto generation=token;
    QMetaObject::invokeMethod(owner,[this,generation,why]{if(token==generation&&pipeline) fail(why);},Qt::QueuedConnection);
  }
  GstClockTime running() const {
    auto *clock=gst_element_get_clock(pipeline); if(!clock) return 0;
    const auto time=gst_clock_get_time(clock)-gst_element_get_base_time(pipeline); gst_object_unref(clock); return time;
  }
  QJsonObject envelope(const QString &type,QJsonObject data={}) const {
    data.insert("session",config.session); data.insert("source",config.source); return message(type,data);
  }
  struct DescriptionContext { QPointer<MediaTransport> owner; quint64 token; bool sender; };
  GstPromise *descriptionPromise() {
    return gst_promise_new_with_change_func(description,new DescriptionContext{owner,token,config.sender},
      +[](gpointer data){delete static_cast<DescriptionContext *>(data);});
  }
  static void description(GstPromise *promise,gpointer data) {
    const auto context=*static_cast<DescriptionContext *>(data);
    GstWebRTCSessionDescription *sdp=nullptr;
    const auto *reply=gst_promise_get_reply(promise);
    if(reply)gst_structure_get(reply,context.sender?"offer":"answer",GST_TYPE_WEBRTC_SESSION_DESCRIPTION,&sdp,nullptr);
    QString text;
    if(sdp){auto *raw=gst_sdp_message_as_text(sdp->sdp);text=QString::fromUtf8(raw);g_free(raw);gst_webrtc_session_description_free(sdp);}
    gst_promise_unref(promise);
    // Promise owns no Impl pointer. All graph/owner access is dispatched to its
    // Qt thread and generation checked there; stop may precede this callback.
    if(!context.owner)return;
    QMetaObject::invokeMethod(context.owner,[context,text]{
      if(!context.owner)return;
      auto *self=context.owner->d.get();
      if(self->token!=context.token||!self->pipeline)return;
      if(text.isEmpty()){self->fail("WebRTC could not create session description");return;}
      GstSDPMessage *parsed=nullptr;gst_sdp_message_new(&parsed);const auto bytes=text.toUtf8();
      gst_sdp_message_parse_buffer(reinterpret_cast<const guint8 *>(bytes.constData()),bytes.size(),parsed);
      auto *description=gst_webrtc_session_description_new(context.sender?GST_WEBRTC_SDP_TYPE_OFFER:GST_WEBRTC_SDP_TYPE_ANSWER,parsed);
      auto *local=gst_promise_new();g_signal_emit_by_name(self->rtc,"set-local-description",description,local);
      gst_promise_interrupt(local);gst_promise_unref(local);gst_webrtc_session_description_free(description);
      Q_EMIT self->owner->signaling(self->envelope(context.sender?"SdpOffer":"SdpAnswer",{{"sdp",text},{"fingerprint",fingerprint(text)}}));
    },Qt::QueuedConnection);
  }
  static void ice(GstElement *,guint index,gchar *value,gpointer data) {
    auto *self=static_cast<Impl *>(data); const QString text=QString::fromUtf8(value); if(text.isEmpty())return; // end-of-candidates is not a target
    const auto generation=self->token;
    QMetaObject::invokeMethod(self->owner,[self,generation,index,text]{
      if(self->token!=generation||!self->pipeline)return;
      if(!candidate(text,self->config.localAddress)) {self->fail("ICE produced a candidate outside the authenticated interface: " + text.split(' ').mid(0,8).join(' '));return;}
      Q_EMIT self->owner->signaling(self->envelope("IceCandidate",{{"candidate",text},{"mline",int(index)}}));
    },Qt::QueuedConnection);
  }
  static GstPadProbeReturn paceInput(GstPad *,GstPadProbeInfo *info,gpointer data) {
    auto *self=static_cast<Impl *>(data);auto *buffer=GST_PAD_PROBE_INFO_BUFFER(info);if(!buffer)return GST_PAD_PROBE_OK;
    const auto size=gst_buffer_get_size(buffer);std::unique_lock lock(self->mutex);
    if(!self->stats.connected)return GST_PAD_PROBE_DROP; // no encoded backlog before DTLS readiness
    // GCC exposes no queue ceiling. Count admission/departure at its own pads
    // and fail before exceeding this finite byte/packet bound.
    const bool room=self->pacerSpace.wait_for(lock,std::chrono::milliseconds(100),[&]{
      return self->stopping||self->pacerFailed||self->pacer.allows(size,self->stats.estimatedBitrate);
    });
    if(self->stopping||self->pacerFailed)return GST_PAD_PROBE_DROP;
    if(!room){if(!self->pacerFailed.exchange(true))self->postFailure("Media pacer exceeded bounded backpressure deadline");return GST_PAD_PROBE_DROP;}
    if(!self->pacer.admit(size,self->stats.estimatedBitrate,audioHostTimeNs())){self->pacerFailed=true;self->postFailure("Media pacer admission limit exceeded");return GST_PAD_PROBE_DROP;}
    self->stats.pacerBytes=self->pacer.queuedBytes();return GST_PAD_PROBE_OK;
  }
  static GstPadProbeReturn paceOutput(GstPad *,GstPadProbeInfo *info,gpointer data) {
    auto *self=static_cast<Impl *>(data);std::lock_guard lock(self->mutex);
    self->pacer.depart();self->stats.pacerBytes=self->pacer.queuedBytes();self->pacerSpace.notify_all();
    auto *buffer=GST_PAD_PROBE_INFO_BUFFER(info);if(buffer){++self->stats.sentPackets;self->stats.sentBytes+=gst_buffer_get_size(buffer);
      GstRTPBuffer rtp=GST_RTP_BUFFER_INIT;if(gst_rtp_buffer_map(buffer,GST_MAP_READ,&rtp)){if(gst_rtp_buffer_get_payload_type(&rtp)==97)++self->stats.audioPackets;gst_rtp_buffer_unmap(&rtp);}}
    return GST_PAD_PROBE_OK;
  }
  static GstElement *aux(GstElement *,GstWebRTCDTLSTransport *,gpointer data) {
    auto *self=static_cast<Impl *>(data);
    auto *gcc=gst_element_factory_make("rtpgccbwe",nullptr);
    if(!gcc) {self->postFailure("Required rtpgccbwe plugin missing");return nullptr;}
    g_object_set(gcc,"min-bitrate",100000U,"max-bitrate",guint(self->preset.maximum),"estimated-bitrate",guint(self->preset.bitrate),nullptr);
    auto *input=gst_element_get_static_pad(gcc,"sink"),*output=gst_element_get_static_pad(gcc,"src");
    gst_pad_add_probe(input,GST_PAD_PROBE_TYPE_BUFFER,paceInput,self,nullptr);gst_pad_add_probe(output,GST_PAD_PROBE_TYPE_BUFFER,paceOutput,self,nullptr);
    gst_object_unref(input);gst_object_unref(output);
    {std::lock_guard lock(self->mutex);self->gcc=gcc;self->stats.congestionControl=true;}
    g_signal_connect(gcc,"notify::estimated-bitrate",G_CALLBACK(+[](GObject *object,GParamSpec *,gpointer data){
      auto *self=static_cast<Impl *>(data); guint estimate=0;g_object_get(object,"estimated-bitrate",&estimate,nullptr);
      const int video=std::max(10000,int(estimate)-(self->config.audio?96000:0));
      if(self->encoder)g_object_set(self->encoder,"target-bitrate",video,nullptr);
      std::lock_guard lock(self->mutex); self->stats.estimatedBitrate=estimate;
    }),self);
    return gcc;
  }
  static GstPadProbeReturn annotate(GstBuffer *&buffer,Impl *self) {
    buffer=gst_buffer_make_writable(buffer);
    GstRTPBuffer rtp=GST_RTP_BUFFER_INIT;if(!gst_rtp_buffer_map(buffer,GST_MAP_READWRITE,&rtp))return GST_PAD_PROBE_DROP;
    std::lock_guard lock(self->mutex);
    const bool audio=gst_rtp_buffer_get_payload_type(&rtp)==97;
    VideoFrame frame;
    if(audio) {
      auto it=self->sentAudio.upper_bound(GST_BUFFER_PTS(buffer));
      if(it==self->sentAudio.begin()){gst_rtp_buffer_unmap(&rtp);return GST_PAD_PROBE_DROP;}
      --it;frame=it->second;frame.mediaTimeNs+=qint64(GST_BUFFER_PTS(buffer)-it->first);
    } else {
      const auto bytes=self->packetMetadata.read(buffer);
      if(bytes.size()!=80){gst_rtp_buffer_unmap(&rtp);return GST_PAD_PROBE_DROP;}
      frame=metadata(bytes);
    }
    if(frame.timelineEpoch!=self->epoch){gst_rtp_buffer_unmap(&rtp);return GST_PAD_PROBE_DROP;}
    const auto bytes=metadata(frame);
    bool added=true;
    for(int i=0;i<5;++i)added = (gst_rtp_buffer_add_extension_onebyte_header(&rtp,2+i,bytes.constData()+16*i,16)!=FALSE) && added;
    if(audio) {
      const auto timing=self->encodedClipping.find(GST_BUFFER_PTS(buffer));
      if(timing==self->encodedClipping.end()) added=false;
      else { const auto clip=encodeAudioClipping(timing->second);
        added=(gst_rtp_buffer_add_extension_onebyte_header(&rtp,7,clip.constData(),16)!=FALSE) && added; }
    }
    gst_rtp_buffer_unmap(&rtp);
    if(!added){self->postFailure("RTP source metadata extension failed");return GST_PAD_PROBE_DROP;}
    return GST_PAD_PROBE_OK;
  }
  static GstPadProbeReturn rememberClipping(GstPad *,GstPadProbeInfo *info,gpointer data) {
    auto *self=static_cast<Impl *>(data); auto *buffer=GST_PAD_PROBE_INFO_BUFFER(info); if(!buffer)return GST_PAD_PROBE_OK;
    const auto *meta=gst_buffer_get_audio_clipping_meta(buffer); AudioClipping clip;
    if(meta) {
      if(meta->format!=GST_FORMAT_DEFAULT || meta->start>960 || meta->end>960) {
        self->postFailure("Unsupported Opus clipping metadata");return GST_PAD_PROBE_DROP;
      }
      clip={quint32(meta->start),quint32(meta->end)};
    }
    std::lock_guard lock(self->mutex); self->encodedClipping[GST_BUFFER_PTS(buffer)]=clip;
    while(self->encodedClipping.size()>128)self->encodedClipping.erase(self->encodedClipping.begin());
    return GST_PAD_PROBE_OK;
  }
  static GstPadProbeReturn sendPacket(GstPad *,GstPadProbeInfo *info,gpointer data) {
    auto *self=static_cast<Impl *>(data);
    if(GST_PAD_PROBE_INFO_TYPE(info)&GST_PAD_PROBE_TYPE_BUFFER_LIST){
      auto *list=gst_buffer_list_make_writable(GST_PAD_PROBE_INFO_BUFFER_LIST(info));GST_PAD_PROBE_INFO_DATA(info)=list;
      gst_buffer_list_foreach(list,+[](GstBuffer **buffer,guint,gpointer data)->gboolean {
        if(annotate(*buffer,static_cast<Impl *>(data))==GST_PAD_PROBE_DROP){gst_buffer_unref(*buffer);*buffer=nullptr;}
        return TRUE;
      },self);
      return gst_buffer_list_length(list)?GST_PAD_PROBE_OK:GST_PAD_PROBE_DROP;
    }
    auto *buffer=GST_PAD_PROBE_INFO_BUFFER(info);if(!buffer)return GST_PAD_PROBE_OK;
    const auto result=annotate(buffer,self);GST_PAD_PROBE_INFO_DATA(info)=buffer;return result;
  }
  static GstPadProbeReturn receivePacket(GstPad *,GstPadProbeInfo *info,gpointer data) {
    auto *self=static_cast<Impl *>(data);auto *buffer=GST_PAD_PROBE_INFO_BUFFER(info);if(!buffer)return GST_PAD_PROBE_OK;
    GstRTPBuffer rtp=GST_RTP_BUFFER_INIT;if(!gst_rtp_buffer_map(buffer,GST_MAP_READ,&rtp))return GST_PAD_PROBE_DROP;
    QByteArray bytes;bool valid=true;const bool audio=gst_rtp_buffer_get_payload_type(&rtp)==97;
    for(int i=0;i<5;++i){gpointer part=nullptr;guint size=0;
      if(!gst_rtp_buffer_get_extension_onebyte_header(&rtp,2+i,0,&part,&size)||size!=16){valid=false;break;}
      bytes.append(static_cast<const char *>(part),16);
    }
    std::optional<AudioClipping> clip;
    if(audio) {
      gpointer part=nullptr;guint size=0;
      if(gst_rtp_buffer_get_extension_onebyte_header(&rtp,7,0,&part,&size) && size==16)
        clip=decodeAudioClipping(QByteArray(static_cast<const char *>(part),16));
      if(!clip) { gst_rtp_buffer_unmap(&rtp);self->postFailure("Missing or invalid authenticated Opus clipping metadata");return GST_PAD_PROBE_DROP; }
    }
    gst_rtp_buffer_unmap(&rtp);if(!valid)return GST_PAD_PROBE_DROP;
    auto frame=metadata(bytes);
    if(frame.mediaTimeNs<0 || frame.captureTimeNs<0 || !std::isfinite(frame.scale) || frame.scale<=0)return GST_PAD_PROBE_DROP;
    std::lock_guard lock(self->mutex);
    if(frame.timelineEpoch<self->receiveEpoch)return GST_PAD_PROBE_DROP;
    self->receiveEpoch=frame.timelineEpoch;
    // The RTP depayloaders, codecs and converters copy untagged GstMeta with
    // each buffer. Fragment loss/PLC cannot consume another packet's identity.
    if(audio) bytes+=encodeAudioClipping(*clip);
    const bool attached=self->packetMetadata.attach(buffer,bytes);
    GST_PAD_PROBE_INFO_DATA(info)=buffer;
    if(!attached) {
      self->postFailure("Authenticated packet metadata could not be attached"); return GST_PAD_PROBE_DROP;
    }
    if(!audio)self->decodeProgress.received(frame.timelineEpoch,frame.sequence,audioHostTimeNs());
    ++self->stats.receivedPackets;self->stats.receivedBytes+=gst_buffer_get_size(buffer);
    return GST_PAD_PROBE_OK;
  }
  bool submitVideo(const VideoFrame &frame,qint64 pts) {
  QImage pixels=frame.pixels;if(pixels.width()>preset.width||pixels.height()>preset.height)pixels=pixels.scaled(preset.width,preset.height,Qt::KeepAspectRatio,Qt::FastTransformation);
  // VP8 I420 requires even dimensions; preserve native geometry independently.
  const QSize even(std::max(2,pixels.width()&~1),std::max(2,pixels.height()&~1));if(pixels.size()!=even)pixels=pixels.scaled(even);
  pixels=pixels.convertToFormat(QImage::Format_ARGB32);
  auto *caps=gst_caps_new_simple("video/x-raw","format",G_TYPE_STRING,"BGRA","width",G_TYPE_INT,pixels.width(),"height",G_TYPE_INT,pixels.height(),"framerate",GST_TYPE_FRACTION,preset.fps,1,nullptr);
  gst_app_src_set_caps(video,caps);gst_caps_unref(caps);
  auto *buffer=gst_buffer_new_allocate(nullptr,pixels.sizeInBytes(),nullptr);gst_buffer_fill(buffer,0,pixels.constBits(),pixels.sizeInBytes());
  GST_BUFFER_PTS(buffer)=pts;GST_BUFFER_DURATION(buffer)=GST_SECOND/preset.fps;
  if(!packetMetadata.attach(buffer,metadata(frame))) {gst_buffer_unref(buffer);fail("Video source metadata could not be attached");return false;}
    lastVideoPts=pts;return gst_app_src_push_buffer(video,buffer)==GST_FLOW_OK;
  }
  static GstPadProbeReturn codedFrame(GstPad *,GstPadProbeInfo *info,gpointer data) {
    auto *self=static_cast<Impl *>(data);auto *buffer=GST_PAD_PROBE_INFO_BUFFER(info);if(!buffer)return GST_PAD_PROBE_OK;
    guint8 header[10];if(gst_buffer_extract(buffer,0,header,sizeof(header))!=sizeof(header))return GST_PAD_PROBE_DROP;
    if(!(header[0]&1)){
      const int width=(header[6]|(header[7]<<8))&0x3fff,height=(header[8]|(header[9]<<8))&0x3fff;
      if(header[3]!=0x9d||header[4]!=0x01||header[5]!=0x2a||width<2||height<2||width>self->preset.width||height>self->preset.height){
        self->postFailure("Encoded VP8 dimensions exceed negotiated preset");return GST_PAD_PROBE_DROP;
      }
    }
    return GST_PAD_PROBE_OK;
  }
  static void pad(GstElement *,GstPad *pad,gpointer data) {
    auto *self=static_cast<Impl *>(data);if(GST_PAD_DIRECTION(pad)!=GST_PAD_SRC)return;
    auto *caps=gst_pad_get_current_caps(pad);if(!caps)caps=gst_pad_query_caps(pad,nullptr);
    const auto *s=gst_caps_get_structure(caps,0);const QString encoding=QString::fromUtf8(gst_structure_get_string(s,"encoding-name"));gst_caps_unref(caps);
    const bool audio=encoding=="OPUS";
    if(self->config.sender || (encoding!="VP8"&&!audio) ||(audio&&!self->config.audio)){self->postFailure("Unexpected incoming media track");return;}
    GError *error=nullptr;
    const auto graph=audio?
      "rtpopusdepay ! opusdec plc=true use-inband-fec=true ! audioconvert ! audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! appsink name=decoded max-buffers=3 max-time=60000000 drop=true sync=false enable-last-sample=false wait-on-eos=false":
      "rtpvp8depay name=depay request-keyframe=true wait-for-keyframe=true ! vp8dec ! videoconvert ! video/x-raw,format=BGRA ! appsink name=decoded max-buffers=1 drop=true sync=false enable-last-sample=false wait-on-eos=false";
    auto *bin=gst_parse_bin_from_description(graph,TRUE,&error);
    if(!bin){self->postFailure("Media decoder could not be created. Check the installed codec components.");g_clear_error(&error);return;}
    if(!audio){auto *depay=gst_bin_get_by_name(GST_BIN(bin),"depay");auto *coded=gst_element_get_static_pad(depay,"src");gst_pad_add_probe(coded,GST_PAD_PROBE_TYPE_BUFFER,codedFrame,self,nullptr);gst_object_unref(coded);gst_object_unref(depay);}
    auto *sink=gst_bin_get_by_name(GST_BIN(bin),"decoded");
    gst_bin_add(GST_BIN(self->pipeline),bin);
    auto *input=gst_element_get_static_pad(bin,"sink");
    gst_pad_add_probe(pad,GST_PAD_PROBE_TYPE_BUFFER,receivePacket,self,nullptr);
    const auto result=gst_pad_link(pad,input);gst_object_unref(input);
    if(result!=GST_PAD_LINK_OK){self->postFailure("Incoming RTP could not link decoder");gst_object_unref(sink);return;}
    {std::lock_guard lock(self->mutex);(audio?self->audioSink:self->videoSink)=GST_APP_SINK(sink);}
    gst_element_sync_state_with_parent(bin);
  }
};
MediaTransport::MediaTransport(QObject *parent):QObject(parent),d(std::make_unique<Impl>(this)) {
  auto *timer=new QTimer(this);connect(timer,&QTimer::timeout,this,&MediaTransport::poll);timer->start(10);
}
MediaTransport::~MediaTransport(){stop();}
bool MediaTransport::start(const MediaConfiguration &config) {
  stop();d->failure.clear();d->config=config;
  auto preset=mediaPreset(config.preset);
  if(!preset || !identifier(config.session)||!identifier(config.source)||config.localAddress.isNull()||config.peerAddress.isNull()) {d->fail("Invalid authenticated media configuration");return false;}
  if(occupied.exchange(true)){d->fail("Another media role is already active in this process");return false;}d->owned=true;d->preset=*preset;
  if(!GstCapturePipeline::initialize(d->failure)){stop();return false;}
  for(const auto *factory:{"webrtcbin","rtpgccbwe","nicesrc","nicesink","dtlssrtpenc","dtlssrtpdec","vp8enc","vp8dec","rtpvp8pay","rtpvp8depay","opusenc","opusdec","rtpopuspay","rtpopusdepay","rtphdrexttwcc"}){
    auto *f=gst_element_factory_find(factory);if(!f){d->fail(QString("Required media plugin missing: %1").arg(factory));stop();return false;}gst_object_unref(f);
  }
  d->pipeline=gst_pipeline_new(nullptr);d->rtc=gst_element_factory_make("webrtcbin","transport");
  g_object_set(d->rtc,"bundle-policy",GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,"latency",40U,nullptr);
  GstWebRTCICE *ice=nullptr;g_object_get(d->rtc,"ice-agent",&ice,nullptr);
  g_object_set(ice,"min-rtp-port",24802U,"max-rtp-port",24831U,nullptr);
  NiceAgent *agent=nullptr;g_object_get(ice,"agent",&agent,nullptr);
  if(!agent){gst_object_unref(ice);d->fail("Required libnice agent unavailable");stop();return false;}
  g_object_set(agent,"ice-tcp",FALSE,"ice-udp",TRUE,"upnp",FALSE,nullptr);
  NiceAddress address;nice_address_init(&address);
  if(config.localAddress.protocol()==QAbstractSocket::IPv4Protocol)nice_address_set_ipv4(&address,config.localAddress.toIPv4Address());
  else {const auto bytes=config.localAddress.toIPv6Address();nice_address_set_ipv6(&address,bytes.c);}
  const bool pinned=nice_agent_add_local_address(agent,&address);
  g_object_unref(agent);gst_object_unref(ice);
  if(!pinned){d->fail("Authenticated local media interface unavailable");stop();return false;}
  gst_bin_add(GST_BIN(d->pipeline),d->rtc);
  g_signal_connect(d->rtc,"deep-element-added",G_CALLBACK(+[](GstBin *,GstBin *,GstElement *element,gpointer data){
    auto *factory=gst_element_get_factory(element);
    if(factory&&QString::fromUtf8(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)))=="rtpjitterbuffer"){
      g_object_set(element,"latency",40U,"drop-on-latency",TRUE,"max-dropout-time",100U,"max-misorder-time",100U,"do-lost",TRUE,nullptr);
      gboolean drop=FALSE;guint latency=0;g_object_get(element,"drop-on-latency",&drop,"latency",&latency,nullptr);
      auto *self=static_cast<Impl *>(data);if(!drop||latency>100){self->postFailure("Receiver jitter bounds unavailable");return;}
      std::lock_guard lock(self->mutex);++self->stats.boundedJitterBuffers;
    }
  }),d.get());
  g_signal_connect(d->rtc,"on-ice-candidate",G_CALLBACK(Impl::ice),d.get());
  g_signal_connect(d->rtc,"pad-added",G_CALLBACK(Impl::pad),d.get());
  if(config.sender) {
    g_signal_connect(d->rtc,"request-aux-sender",G_CALLBACK(Impl::aux),d.get());
    for(int track=0;track<(config.audio?2:1);++track) {
      const bool audio=track==1;GError *error=nullptr;
      const QString graph=audio?
        "appsrc name=input is-live=true format=time block=false max-time=60000000 max-bytes=23040 ! audioconvert ! audioresample ! opusenc hard-resync=true bitrate=96000 frame-size=20 inband-fec=true packet-loss-percentage=1 ! rtpopuspay name=pay pt=97 mtu=1100 ! application/x-rtp,media=audio,encoding-name=OPUS,payload=97,clock-rate=48000,encoding-params=(string)2,rtcp-fb-transport-cc=(boolean)true,extmap-1=(string)http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01 ! identity":
        QString("appsrc name=input is-live=true format=time block=false max-buffers=2 max-bytes=0 leaky-type=downstream ! videoconvert ! video/x-raw,format=I420,colorimetry=bt709 ! vp8enc name=encoder deadline=1 lag-in-frames=0 cpu-used=8 threads=4 end-usage=cbr buffer-size=100 buffer-initial-size=50 buffer-optimal-size=50 max-intra-bitrate=250 target-bitrate=%1 keyframe-max-dist=%2 ! rtpvp8pay name=pay pt=96 mtu=1100 picture-id-mode=15-bit ! application/x-rtp,media=video,encoding-name=VP8,payload=96,clock-rate=90000,rtcp-fb-nack-pli=(boolean)true,rtcp-fb-ccm-fir=(boolean)true,rtcp-fb-transport-cc=(boolean)true,extmap-1=(string)http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01 ! identity").arg(preset->bitrate).arg(preset->fps*2);
      auto *bin=gst_parse_bin_from_description(graph.toUtf8().constData(),TRUE,&error);
      if(!bin||error){d->fail("Media encoder could not be created. Check the installed codec components.");g_clear_error(&error);if(bin)gst_object_unref(bin);stop();return false;}
      auto *input=gst_bin_get_by_name(GST_BIN(bin),"input");
      auto *caps=audio?gst_caps_from_string("audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved"):
        gst_caps_new_simple("video/x-raw","format",G_TYPE_STRING,"BGRA","width",G_TYPE_INT,preset->width,"height",G_TYPE_INT,preset->height,"framerate",GST_TYPE_FRACTION,preset->fps,1,nullptr);
      gst_app_src_set_caps(GST_APP_SRC(input),caps);gst_caps_unref(caps);
      (audio?d->audio:d->video)=GST_APP_SRC(input);
      if(!audio)d->encoder=gst_bin_get_by_name(GST_BIN(bin),"encoder");
      if(!audio){auto *pad=gst_element_get_static_pad(d->encoder,"src");
        gst_pad_add_probe(pad,GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,+[](GstPad *,GstPadProbeInfo *info,gpointer data){
          if(gst_video_event_is_force_key_unit(GST_PAD_PROBE_INFO_EVENT(info))){auto *self=static_cast<Impl *>(data);std::lock_guard lock(self->mutex);++self->stats.keyframeRequests;self->repeatRequested=true;}
          return GST_PAD_PROBE_OK;
        },d.get(),nullptr);gst_object_unref(pad);}

      auto *pay=gst_bin_get_by_name(GST_BIN(bin),"pay");
      if(audio) { auto *encoded=gst_element_get_static_pad(pay,"sink");
        gst_pad_add_probe(encoded,GST_PAD_PROBE_TYPE_BUFFER,Impl::rememberClipping,d.get(),nullptr);gst_object_unref(encoded); }
      auto *extension=gst_rtp_header_extension_create_from_uri(twcc);gst_rtp_header_extension_set_id(extension,1);
      g_signal_emit_by_name(pay,"add-extension",extension);gst_object_unref(extension);
      auto *src=gst_element_get_static_pad(pay,"src");gst_pad_add_probe(src,GstPadProbeType(GST_PAD_PROBE_TYPE_BUFFER|GST_PAD_PROBE_TYPE_BUFFER_LIST),Impl::sendPacket,d.get(),nullptr);gst_object_unref(src);gst_object_unref(pay);
      gst_bin_add(GST_BIN(d->pipeline),bin);
      auto *out=gst_element_get_static_pad(bin,"src");auto *sink=gst_element_request_pad_simple(d->rtc,"sink_%u");
      GstWebRTCRTPTransceiver *transceiver=nullptr;g_object_get(sink,"transceiver",&transceiver,nullptr);
      g_object_set(transceiver,"direction",GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,nullptr);gst_object_unref(transceiver);
      const auto linked=gst_pad_link(out,sink);gst_object_unref(out);gst_object_unref(sink);
      if(linked!=GST_PAD_LINK_OK){d->fail("RTP sender could not link WebRTC");stop();return false;}
    }
  }
  if(!config.sender){
    for(int i=0;i<(config.audio?2:1);++i){
      auto *caps=gst_caps_new_simple("application/x-rtp","media",G_TYPE_STRING,i==0?"video":"audio",
        "encoding-name",G_TYPE_STRING,i==0?"VP8":"OPUS","payload",G_TYPE_INT,i==0?96:97,
        "clock-rate",G_TYPE_INT,i==0?90000:48000,"rtcp-fb-transport-cc",G_TYPE_BOOLEAN,TRUE,"extmap-1",G_TYPE_STRING,twcc,nullptr);
      if(i==0)gst_caps_set_simple(caps,"rtcp-fb-nack-pli",G_TYPE_BOOLEAN,TRUE,"rtcp-fb-ccm-fir",G_TYPE_BOOLEAN,TRUE,nullptr);
      else gst_caps_set_simple(caps,"encoding-params",G_TYPE_STRING,"2",nullptr);
      GstWebRTCRTPTransceiver *transceiver=nullptr;
      g_signal_emit_by_name(d->rtc,"add-transceiver",GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY,caps,&transceiver);
      gst_caps_unref(caps);if(transceiver)gst_object_unref(transceiver);
    }
  }
  d->elapsed.start();d->stats.estimatedBitrate=preset->bitrate;
  if(gst_element_set_state(d->pipeline,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE){d->fail("Media graph refused PLAYING");stop();return false;}
  qInfo()<<"Media started"<<(config.sender?"sender":"receiver")<<config.preset<<"VP8 software / Opus"<<config.audio;
  return true;
}
void MediaTransport::stop() {
  ++d->token;d->stopping=true;d->pacerSpace.notify_all();
  if(d->pipeline){gst_element_set_state(d->pipeline,GST_STATE_NULL);gst_object_unref(d->pipeline);}
  for(auto *object:{GST_OBJECT(d->video),GST_OBJECT(d->audio),GST_OBJECT(d->encoder),GST_OBJECT(d->videoSink),GST_OBJECT(d->audioSink)})if(object)gst_object_unref(object);
  d->pipeline=nullptr;d->rtc=nullptr;d->video=nullptr;d->audio=nullptr;d->encoder=nullptr;d->gcc=nullptr;d->videoSink=nullptr;d->audioSink=nullptr;
  {std::lock_guard lock(d->mutex);d->sentAudio.clear();d->encodedClipping.clear();d->pacer.clear();d->pacerFailed=false;d->stopping=false;d->stats={};d->decodeProgress.clear();}
  d->latestVideo.reset();d->deliveredVideo.reset();d->submittedAudioEpoch.reset();
  d->offered=false;d->remoteSet=false;d->anchored=false;d->candidates.clear();d->epoch=0;d->receiveEpoch=0;d->lastVideo=-1;d->lastVideoPts=-1;
  d->connectedAt=-1;d->lowSince=-1;d->lastHeartbeat=0;d->lastLog=0;d->lastKeyRequest=0;
  if(d->owned){occupied=false;d->owned=false;}
}
bool MediaTransport::receive(const QJsonObject &frame) {
  if(!d->pipeline)return false;const auto data=frame["data"].toObject(); const auto type=frame["type"].toString();
  if(data["session"]!=d->config.session||data["source"]!=d->config.source)return false;
  if(type=="Stopped"){stop();return true;}
  if(type=="IceCandidate") {
    const int index=data["mline"].toInt(-1);const auto value=data["candidate"].toString();
    if(index<0||index>=(d->config.audio?2:1)||!candidate(value,d->config.peerAddress))return false;
    if(!d->remoteSet){if(d->candidates.size()>=32)return false;d->candidates.emplace_back(index,value);}
    else g_signal_emit_by_name(d->rtc,"add-ice-candidate",guint(index),value.toUtf8().constData());
    return true;
  }
  if(type!=(d->config.sender?"SdpAnswer":"SdpOffer")||d->remoteSet)return false;
  const auto text=data["sdp"].toString();
  if(text.size()>200000||text.contains("a=candidate:")||fingerprint(text)!=data["fingerprint"].toString()||fingerprint(text).isEmpty())return false;
  GstSDPMessage *sdp=nullptr;gst_sdp_message_new(&sdp);
  const auto bytes=text.toUtf8();
  if(gst_sdp_message_parse_buffer(reinterpret_cast<const guint8 *>(bytes.constData()),bytes.size(),sdp)!=GST_SDP_OK){gst_sdp_message_free(sdp);return false;}
  if(gst_sdp_message_medias_len(sdp)!=(d->config.audio?2U:1U)){gst_sdp_message_free(sdp);return false;}
  const auto expectedFingerprint=data["fingerprint"].toString();
  bool valid=QRegularExpression("^[0-9A-F]{2}(:[0-9A-F]{2}){31}$").match(expectedFingerprint).hasMatch();
  QStringList mids;
  for(guint i=0;i<gst_sdp_message_medias_len(sdp);++i){const auto *m=gst_sdp_message_get_media(sdp,i);
    const auto value=[&](const char *key){return QString::fromUtf8(gst_sdp_media_get_attribute_val(m,key));};
    const auto expectedPayload=i==0?QString("96"):QString("97");
    valid &= QString::fromUtf8(gst_sdp_media_get_media(m))==(i==0?"video":"audio") &&
      QString::fromUtf8(gst_sdp_media_get_proto(m))=="UDP/TLS/RTP/SAVPF" &&
      value("fingerprint")=="sha-256 "+expectedFingerprint &&
      gst_sdp_media_formats_len(m)==1 && QString::fromUtf8(gst_sdp_media_get_format(m,0))==expectedPayload &&
      value("rtpmap")==expectedPayload+(i==0?" VP8/90000":" OPUS/48000/2") &&
      gst_sdp_media_get_attribute_val(m,"rtcp-mux")!=nullptr &&
      gst_sdp_media_get_attribute_val(m,d->config.sender?"recvonly":"sendonly")!=nullptr;
    bool feedback=false,extension=false,pli=false;
    for(guint j=0;j<gst_sdp_media_attributes_len(m);++j){const auto *a=gst_sdp_media_get_attribute(m,j);
      if(QString::fromUtf8(a->key)=="rtcp-fb"&&QString::fromUtf8(a->value)==expectedPayload+" transport-cc")feedback=true;
      if(QString::fromUtf8(a->key)=="rtcp-fb"&&QString::fromUtf8(a->value)==expectedPayload+" nack pli")pli=true;
      if(QString::fromUtf8(a->key)=="extmap"&&QString::fromUtf8(a->value)=="1 "+QString::fromLatin1(twcc))extension=true;
    }
    valid &= (i!=0||pli)&&feedback&&extension&&!value("mid").isEmpty()&&!mids.contains(value("mid"));mids.append(value("mid"));
  }
  valid &= QString::fromUtf8(gst_sdp_message_get_attribute_val(sdp,"group"))=="BUNDLE "+mids.join(' ');
  if(!valid){gst_sdp_message_free(sdp);return false;}
  qInfo()<<"Media negotiated VP8 PLI and TWCC";
  auto *description=gst_webrtc_session_description_new(d->config.sender?GST_WEBRTC_SDP_TYPE_ANSWER:GST_WEBRTC_SDP_TYPE_OFFER,sdp);
  auto *promise=gst_promise_new();g_signal_emit_by_name(d->rtc,"set-remote-description",description,promise);
  const auto result=gst_promise_wait(promise);const auto *reply=gst_promise_get_reply(promise);
  const bool ok=result==GST_PROMISE_RESULT_REPLIED && (!reply||!gst_structure_has_field(reply,"error"));
  gst_promise_unref(promise);gst_webrtc_session_description_free(description);
  if(!ok){d->fail("WebRTC rejected remote description");return false;}d->remoteSet=true;
  for(const auto &[index,value]:d->candidates)g_signal_emit_by_name(d->rtc,"add-ice-candidate",index,value.toUtf8().constData());d->candidates.clear();
  if(!d->config.sender)g_signal_emit_by_name(d->rtc,"create-answer",nullptr,d->descriptionPromise());
  return true;
}
bool MediaTransport::pushVideo(const VideoFrame &frame) {
  if(!d->failure.isEmpty()||!d->video||frame.session!=d->config.session||frame.source!=d->config.source||frame.timelineEpoch<d->epoch||frame.pixels.isNull()||frame.mediaTimeNs<0)return false;
  const bool epoch=!d->anchored||frame.timelineEpoch>d->epoch;
  if(!epoch&&d->lastVideo>=0&&frame.mediaTimeNs+GST_MSECOND<d->nextVideoDue){std::lock_guard lock(d->mutex);++d->stats.rawDropped;return false;}
  if(epoch){d->nextVideoDue=frame.mediaTimeNs;d->anchored=true;d->epoch=frame.timelineEpoch;d->anchorMedia=frame.mediaTimeNs;d->anchorRunning=(std::max)(qint64(d->running()),d->lastVideoPts+GST_SECOND/d->preset.fps);
    {std::lock_guard lock(d->mutex);d->sentAudio.clear();}
    requestVideoKeyframe(d->encoder,d->anchorRunning);}
  const qint64 pts=d->anchorRunning+frame.mediaTimeNs-d->anchorMedia;if(pts<0)return false;
  d->latestVideo=frame;d->lastInputAt=d->elapsed.elapsed();d->repeatRequested=false;
  {std::lock_guard lock(d->mutex);++d->stats.videoSubmitted;}
  d->nextVideoDue+=GST_SECOND/d->preset.fps;
  if(d->nextVideoDue<frame.mediaTimeNs)d->nextVideoDue=frame.mediaTimeNs+GST_SECOND/d->preset.fps;
  d->lastVideo=frame.mediaTimeNs;return d->submitVideo(frame,pts);
}
bool MediaTransport::pushAudio(const AudioBlock &block) {
  if(!d->failure.isEmpty()||!d->audio||!d->anchored||!validAudioBlock(block)||block.session!=d->config.session||block.source!=d->config.source||block.timelineEpoch!=d->epoch)return false;
  const qint64 pts=d->anchorRunning+block.mediaTimeNs-d->anchorMedia;if(pts<0)return false;
  if(gst_app_src_get_current_level_bytes(d->audio)+quint64(block.samples.size())>23040)return false;
  {std::lock_guard lock(d->mutex);VideoFrame meta;meta.timelineEpoch=block.timelineEpoch;meta.mediaTimeNs=block.mediaTimeNs;meta.captureTimeNs=block.captureTimeNs;
    d->sentAudio[pts]=meta;while(d->sentAudio.size()>128)d->sentAudio.erase(d->sentAudio.begin());}
  auto *buffer=gst_buffer_new_allocate(nullptr,block.samples.size(),nullptr);gst_buffer_fill(buffer,0,block.samples.constData(),block.samples.size());
  GST_BUFFER_PTS(buffer)=pts;GST_BUFFER_DURATION(buffer)=block.durationNs;
  if(!d->submittedAudioEpoch || *d->submittedAudioEpoch!=block.timelineEpoch) GST_BUFFER_FLAG_SET(buffer,GST_BUFFER_FLAG_DISCONT);
  const bool submitted=gst_app_src_push_buffer(d->audio,buffer)==GST_FLOW_OK;
  if(submitted)d->submittedAudioEpoch=block.timelineEpoch;
  return submitted;
}
std::optional<VideoFrame> MediaTransport::takeVideo() {
  GstAppSink *sink=nullptr;{std::lock_guard lock(d->mutex);sink=d->videoSink;if(sink)gst_object_ref(sink);}if(!sink)return {};
  auto *sample=gst_app_sink_try_pull_sample(sink,0);gst_object_unref(sink);if(!sample)return {};
  auto *buffer=gst_sample_get_buffer(sample);VideoFrame frame;bool found=false;
  {std::lock_guard lock(d->mutex);const auto bytes=d->packetMetadata.read(buffer);if(bytes.size()==80){frame=metadata(bytes);found=frame.timelineEpoch==d->receiveEpoch;}}
  GstVideoInfo info;GstVideoFrame mapped;
  if(found&&gst_video_info_from_caps(&info,gst_sample_get_caps(sample))&&gst_video_frame_map(&mapped,&info,buffer,GST_MAP_READ)){
    frame.pixels=QImage(static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&mapped,0)),info.width,info.height,GST_VIDEO_FRAME_PLANE_STRIDE(&mapped,0),QImage::Format_ARGB32).copy();gst_video_frame_unmap(&mapped);
    frame.session=d->config.session;frame.source=d->config.source;
  }else found=false;
  gst_sample_unref(sample);if(found){const auto id=std::pair(frame.timelineEpoch,frame.sequence);if(d->deliveredVideo==id)return {};d->deliveredVideo=id;{std::lock_guard lock(d->mutex);++d->stats.videoDecoded;
    d->decodeProgress.decoded(frame.timelineEpoch,frame.sequence,audioHostTimeNs());
  }return frame;}return {};
}
std::optional<AudioBlock> MediaTransport::takeAudio() {
  GstAppSink *sink=nullptr;{std::lock_guard lock(d->mutex);sink=d->audioSink;if(sink)gst_object_ref(sink);}if(!sink)return {};
  auto *sample=gst_app_sink_try_pull_sample(sink,0);gst_object_unref(sink);if(!sample)return {};
  auto *buffer=gst_sample_get_buffer(sample);VideoFrame meta;AudioClipping clip;bool found=false;
  {std::lock_guard lock(d->mutex);const auto bytes=d->packetMetadata.read(buffer);
    if(bytes.size()==96) {meta=metadata(bytes.left(80));const auto clipping=decodeAudioClipping(bytes.mid(80));
      if(clipping && meta.timelineEpoch==d->receiveEpoch){clip=*clipping;found=true;++d->stats.audioDecoded;}}}
  AudioBlock block;GstMapInfo map;
  if(found&&gst_buffer_map(buffer,&map,GST_MAP_READ)){
    block={QByteArray(reinterpret_cast<const char *>(map.data),qsizetype(map.size)),d->config.session,d->config.source,meta.timelineEpoch,meta.mediaTimeNs,qint64(map.size/8)*GST_SECOND/48000,meta.captureTimeNs};gst_buffer_unmap(buffer,&map);
    if(!clipDecodedAudio(block,clip)) { gst_sample_unref(sample);d->fail("Decoded Opus clipping exceeds PCM packet");return {}; }
  }else found=false;
  gst_sample_unref(sample);if(found&&validAudioBlock(block))return block;return {};
}
MediaStatistics MediaTransport::statistics() const {
  std::lock_guard lock(d->mutex);auto stats=d->stats;
  if(d->video)stats.queuedVideo=gst_app_src_get_current_level_buffers(d->video);
  if(d->audio)stats.queuedAudioBytes=gst_app_src_get_current_level_bytes(d->audio);return stats;
}
QString MediaTransport::error() const{return d->failure;}
bool MediaTransport::active() const{return d->pipeline!=nullptr;}
bool MediaTransport::consumeBusError(GstBus *bus) {
  auto *event=gst_bus_pop_filtered(bus,GST_MESSAGE_ERROR);
  if(!event)return false;
  GError *error=nullptr;gst_message_parse_error(event,&error,nullptr);
  d->fail("Media backend failed. Check the selected source, connection and installed codec components.");
  g_clear_error(&error);gst_message_unref(event);return true;
}
void MediaTransport::poll() {
  if(!d->pipeline||!d->failure.isEmpty())return;
  auto *bus=gst_element_get_bus(d->pipeline);const bool failed=consumeBusError(bus);gst_object_unref(bus);
  if(failed)return;
  if(d->config.sender&&!d->offered&&statistics().videoSubmitted){d->offered=true;g_signal_emit_by_name(d->rtc,"create-offer",nullptr,d->descriptionPromise());}
  GstWebRTCPeerConnectionState state;g_object_get(d->rtc,"connection-state",&state,nullptr);
  const auto now=d->elapsed.elapsed();
  if(state==GST_WEBRTC_PEER_CONNECTION_STATE_FAILED){d->fail("Direct LAN ICE/DTLS connection failed");return;}
  if(state==GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED&&d->connectedAt<0){d->connectedAt=now;{std::lock_guard lock(d->mutex);d->stats.connected=true;}
    if(d->encoder){auto *pad=gst_element_get_static_pad(d->encoder,"src");gst_pad_send_event(pad,gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE,TRUE,0));gst_object_unref(pad);}
    Q_EMIT connectedChanged(true);}
  // Repeat startup keyframes while the peer may still lack a complete initial frame.
  // RTCP PLI remains enabled for later loss recovery; retries never increase queue bounds.
  if(d->encoder&&d->connectedAt>=0&&now-d->connectedAt<3000&&now-d->lastKeyRequest>=500){
    {std::lock_guard lock(d->mutex);++d->stats.startupRetries;}
    d->lastKeyRequest=now;auto *pad=gst_element_get_static_pad(d->encoder,"src");
    gst_pad_send_event(pad,gst_video_event_new_upstream_force_key_unit(GST_CLOCK_TIME_NONE,TRUE,0));gst_object_unref(pad);
    qInfo()<<"Startup keyframe recovery requested"<<now;
  }
  if(d->encoder&&d->latestVideo&&d->repeatRequested&&now-d->lastInputAt>=200){
      d->repeatRequested=false;
      const auto pts=(std::max)(qint64(d->running()),d->lastVideoPts+GST_SECOND/d->preset.fps);
      if(d->submitVideo(*d->latestVideo,pts)){std::lock_guard lock(d->mutex);++d->stats.staticRepeats;}qInfo()<<"Retained static frame recovery"<<d->latestVideo->sequence;
  }
  if(d->connectedAt<0&&now>10000){d->fail("Direct LAN negotiation timed out");return;}
  auto stats=statistics();
  bool pacerLate=false;{std::lock_guard lock(d->mutex);pacerLate=stats.connected&&d->pacer.expired(audioHostTimeNs());}
  if(pacerLate){d->fail("Media pacer exceeded 100 ms latency bound");return;}
  bool decodeStalled=false;{
    std::lock_guard lock(d->mutex);
    decodeStalled=d->decodeProgress.stalled(audioHostTimeNs());
  }
  if(!d->config.sender&&decodeStalled){qInfo()<<"Authenticated source advanced without decoded progress for3s";d->fail("Video stopped updating. Start a new stream to try again.");return;}
  if(!d->config.sender&&d->connectedAt>=0&&!stats.videoDecoded&&now-d->connectedAt>3000){d->fail("First decoded video frame timed out");return;}
  if(d->config.sender&&stats.connected&&stats.estimatedBitrate<500000){if(d->lowSince<0)d->lowSince=now;if(now-d->lowSince>=3000){d->fail("Insufficient bandwidth below 500 kbit/s");return;}}else d->lowSince=-1;
  if(now-d->lastHeartbeat>=1000){d->lastHeartbeat=now;Q_EMIT signaling(d->envelope("Heartbeat"));}
  if(now-d->lastLog>=1000){d->lastLog=now;qInfo()<<"Media telemetry ms"<<now<<"packets tx/rx"<<stats.sentPackets<<stats.receivedPackets<<"bytes tx/rx"<<stats.sentBytes<<stats.receivedBytes<<"frames submitted/decoded/dropped"<<stats.videoSubmitted<<stats.videoDecoded<<stats.rawDropped<<"GCC bps"<<stats.estimatedBitrate<<"queues video/audioBytes"<<stats.queuedVideo<<stats.queuedAudioBytes<<"pacerBytes"<<stats.pacerBytes<<"boundedJitter"<<stats.boundedJitterBuffers<<"staticRepeats"<<stats.staticRepeats;}
}
} // namespace deskflow::streaming
