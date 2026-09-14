// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// Read-only package diagnostic. Uses only a caller-provided owned media fixture;
// never opens MainWindow, settings, native capture, audio devices or input hooks.
#include "streaming/FileSource.h"
#include "streaming/GstCapturePipeline.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUuid>
#include <windows.h>
#include <tlhelp32.h>
using namespace deskflow::streaming;
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  if (argc != 4) return 64; // owned input, decoded PNG, JSON log
  FileSource source;
  const auto session = QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex());
  const auto sourceId = QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex());
  QJsonObject result;
  const auto recordError = [&](const QString &error) {
    result["error"] = error;
    QFile output(QString::fromLocal8Bit(argv[3]));
    if (output.open(QIODevice::WriteOnly)) output.write(QJsonDocument(result).toJson());
  };
  QString initializationError;
  if (!GstCapturePipeline::initialize(initializationError)) { recordError(initializationError); return 1; }
  QJsonArray capabilities;
  bool factoriesAvailable = true;
  for (const auto *name : {"webrtcbin", "rtpgccbwe", "dtlssrtpenc", "nicesrc", "avdec_h264", "avdec_aac", "wasapi2src",
                           "wasapi2sink", "vp8enc", "vp8dec", "opusenc", "opusdec"}) {
    auto *factory = gst_element_factory_find(name);
    auto *loaded = factory ? gst_plugin_feature_load(GST_PLUGIN_FEATURE(factory)) : nullptr;
    QJsonObject capability{{"factory", name}, {"available", loaded != nullptr}};
    if (loaded) {
      auto *plugin = gst_plugin_feature_get_plugin(loaded);
      capability["plugin"] = QString::fromUtf8(gst_plugin_get_filename(plugin));
      capability["version"] = QString::fromUtf8(gst_plugin_get_version(plugin));
      capability["license"] = QString::fromUtf8(gst_plugin_get_license(plugin));
      gst_object_unref(plugin);
      gst_object_unref(loaded);
    } else factoriesAvailable = false;
    if (factory) gst_object_unref(factory);
    capabilities.append(capability);
  }
  result["capabilities"] = capabilities;
  int frames = 0, audioBlocks = 0;
  qint64 samples = 0;
  bool saved = false, playing = false;
  QTimer poll;
  QElapsedTimer time;
  time.start();
  QObject::connect(&poll, &QTimer::timeout, &app, [&] {
    if (!playing && source.state() == FilePlaybackState::Paused) {
      playing = source.command(session, sourceId, "resume");
    }
    if (auto frame = source.takeFrame()) {
      ++frames;
      if (!saved) {
        saved = frame->pixels.save(QString::fromLocal8Bit(argv[2]));
        result["decodedPngMediaTimeNs"] = frame->mediaTimeNs;
      }
    }
    if (auto audio = source.takeAudio()) {
      ++audioBlocks;
      samples += audio->samples.size() / (2 * sizeof(float));
    }
    if ((frames >= 6 && audioBlocks >= 6 && saved) || time.elapsed() >= 3000 || source.state() == FilePlaybackState::Error) {
      result["frames"] = frames;
      result["pcmBlocks"] = audioBlocks;
      result["pcmStereoFrames"] = samples;
      result["decodedPngSaved"] = saved;
      result["error"] = source.error();
      result["elapsedMs"] = time.elapsed();
      QJsonArray modules;
      const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
      MODULEENTRY32W entry{sizeof(entry)};
      if (snapshot == INVALID_HANDLE_VALUE || !Module32FirstW(snapshot, &entry)) {
        result["moduleError"] = static_cast<int>(GetLastError());
      } else {
        do { modules.append(QString::fromWCharArray(entry.szExePath)); } while (Module32NextW(snapshot, &entry));
        if (GetLastError() != ERROR_NO_MORE_FILES) result["moduleError"] = static_cast<int>(GetLastError());
      }
      if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
      result["modules"] = modules;
      source.stop();
      QFile output(QString::fromLocal8Bit(argv[3]));
      if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(result).toJson()) < 0) app.exit(3);
      else app.exit(frames >= 6 && audioBlocks >= 6 && saved && factoriesAvailable && !result.contains("moduleError") ? 0 : 2);
    }
  });
  if (!source.open(QString::fromLocal8Bit(argv[1]), session, sourceId, true)) {
    recordError(source.error());
    return 1;
  }
  poll.start(5);
  return app.exec();
}
