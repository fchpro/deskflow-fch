// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "Audio.h"
namespace deskflow::streaming {
class NativeAudioCapture {
public:
  virtual ~NativeAudioCapture() = default;
  virtual GstElement *takeSource() = 0; // transfer the initial reference once
  virtual bool start(QString &) { return true; } // called only after source role and PCM graph are active
  virtual QString error() = 0;
};
std::unique_ptr<NativeAudioCapture> createNativeAudioCapture(const AudioSelection &, QString &error);
QVector<AudioEndpoint> nativeAudioEndpoints(QString &error);
GstElement *nativeAudioSink(const QString &id, QString &error);
quint64 nativeAudioProcessBirth(quint32 pid);
} // namespace deskflow::streaming
