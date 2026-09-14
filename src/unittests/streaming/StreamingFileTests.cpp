// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/FileSource.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace deskflow::streaming;
namespace {
const QString session(32, 'a'), source(32, 'b');
QTemporaryDir fixtures;
QString fixture(const QString &name) { return fixtures.filePath(name); }
void generate(const QString &name, const QStringList &options)
{
  QProcess process;
  QStringList args{"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
                   "color=c=red:s=160x96:r=30:d=1", "-f", "lavfi", "-i", "color=c=blue:s=160x96:r=30:d=1"};
  args << options << fixture(name);
  process.start("ffmpeg", args);
  if (!process.waitForFinished(10000) || process.exitCode()) qFatal("Generated fixture failed: %s", process.readAll().constData());
}
void ready(FileSource &file, const QString &name = "av.webm", bool audio = true)
{
  if (!file.open(fixture(name), session, source, audio) ||
      !QTest::qWaitFor([&] { return file.state() != FilePlaybackState::Opening; }, 3000) ||
      file.state() != FilePlaybackState::Paused)
    qFatal("File readiness failed: %s", qPrintable(file.error()));
}
VideoFrame frame(FileSource &file)
{
  std::optional<VideoFrame> value;
  if (!QTest::qWaitFor([&] { value = file.takeFrame(); return value.has_value(); }, 3000))
    qFatal("No decoded frame: %s", qPrintable(file.error()));
  return std::move(*value);
}
}
class StreamingFileTests : public QObject {
  Q_OBJECT
private Q_SLOTS:
  void initTestCase()
  {
    const QStringList video{"-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0[v]", "-map", "[v]"};
    generate("av.webm", QStringList{"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=2"} +
             video + QStringList{"-map", "2:a", "-c:v", "libvpx", "-deadline", "realtime", "-c:a", "libopus"});
    generate("silent.webm", video + QStringList{"-c:v", "libvpx", "-deadline", "realtime"});
    generate("vp9-vorbis.webm", QStringList{"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=2"} +
             video + QStringList{"-map", "2:a", "-c:v", "libvpx-vp9", "-deadline", "realtime", "-cpu-used", "8", "-c:a", "libvorbis"});
    generate("av.mp4", QStringList{"-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=2"} +
             video + QStringList{"-map", "2:a", "-c:v", "libx264", "-preset", "ultrafast", "-c:a", "aac", "-movflags", "+faststart"});
    for (const auto &name : {QString("av.mov"), QString("av.mkv")}) {
      QProcess remux;
      remux.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-i", fixture("av.mp4"), "-c", "copy", fixture(name)});
      if (!remux.waitForFinished(10000) || remux.exitCode()) qFatal("Remux fixture failed");
    }
    generate("unsupported-codec.mkv", video + QStringList{"-c:v", "ffv1"});
    generate("vfr.webm", {"-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0,select='if(lt(t,1),not(mod(n,3)),1)'[v]",
                          "-map", "[v]", "-fps_mode", "vfr", "-c:v", "libvpx", "-deadline", "realtime"});
    QFile invalid(fixture("invalid.webm"));
    if (!invalid.open(QIODevice::WriteOnly)) qFatal("Cannot create invalid fixture");
    invalid.write("#EXTM3U\nhttp://127.0.0.1:9/forbidden.ts\n");
    QFile truncated(fixture("truncated.mp4"));
    if (!truncated.open(QIODevice::WriteOnly)) qFatal("Cannot create truncated fixture");
    truncated.write(QByteArray::fromHex("000000186674797069736f6d00000200"));
    invalid.close();
    truncated.close();
    const auto proof = qEnvironmentVariable("DESKFLOW_FILE_PROOF");
    if (!proof.isEmpty()) {
      QDir dir(proof);
      if (!dir.mkpath("fixtures")) qFatal("Could not create fixture proof directory");
      for (const auto &name : QDir(fixtures.path()).entryList(QDir::Files)) {
        const auto destination = dir.filePath("fixtures/" + name);
        if (!QFileInfo::exists(destination) && !QFile::copy(fixture(name), destination)) qFatal("Could not preserve fixture");
      }
    }
    qInfo() << "Generated owned red/blue and 440-Hz fixtures with FFmpeg; no user media. Duration 2s; transition 1s.";
  }
  void metadata()
  {
    FileSource file; ready(file);
    QVERIFY(file.metadata().durationNs >= 2000000000LL && file.metadata().durationNs < 2100000000LL &&
            file.metadata().seekable && file.metadata().hasAudio && file.metadata().name == "av.webm");
  }
  void formats_data()
  {
    QTest::addColumn<QString>("name");
    QTest::newRow("vp8-opus") << "av.webm";
    QTest::newRow("h264-aac") << "av.mp4";
    QTest::newRow("vp9-vorbis") << "vp9-vorbis.webm";
    QTest::newRow("mov-h264-aac") << "av.mov";
    QTest::newRow("mkv-h264-aac") << "av.mkv";
    QTest::newRow("silent") << "silent.webm";
    QTest::newRow("vfr") << "vfr.webm";
  }
  void formats()
  {
    QFETCH(QString, name);
    FileSource file; ready(file, name); file.command(session, source, "resume");
    const auto decoded = frame(file);
    std::optional<FileAudioBlock> audio;
    if (file.metadata().hasAudio && !QTest::qWaitFor([&] { audio = file.takeAudio(); return audio.has_value(); }, 3000))
      qFatal("No decoded audio track in %s", qPrintable(name));
    QVERIFY(decoded.pixels.pixelColor(50, 50).red() > 240 && decoded.mediaTimeNs >= 0 &&
            decoded.session == session && decoded.source == source && (!audio || !audio->samples.isEmpty()));
    const auto proof = qEnvironmentVariable("DESKFLOW_FILE_PROOF");
    if (!proof.isEmpty()) {
      if (!decoded.pixels.save(QDir(proof).filePath(name + "-first.png"))) qFatal("Could not save decoded proof");
      qInfo() << "Decoded" << name << "PTS-ns" << decoded.mediaTimeNs << "epoch" << decoded.timelineEpoch;
    }
  }
  void pauseResume()
  {
    FileSource file; ready(file); file.command(session, source, "resume"); frame(file);
    file.command(session, source, "pause");
    const bool paused = file.state() == FilePlaybackState::Paused && !file.takeAudio() && !file.takeFrame();
    const bool accepted = file.command(session, source, "resume");
    std::optional<VideoFrame> resumed;
    if (accepted) QTest::qWaitFor([&] { resumed = file.takeFrame(); return resumed.has_value(); }, 3000);
    QVERIFY(paused && accepted && file.state() == FilePlaybackState::Playing && resumed && resumed->mediaTimeNs >= 0);
  }
  void seekFlush()
  {
    FileSource file; ready(file); file.command(session, source, "resume"); const auto before = frame(file);
    QSignalSpy resets(&file, &FileSource::timelineReset);
    const bool requested = file.command(session, source, "seek", 1250000000LL);
    const bool hidden = !file.takeFrame() && !file.takeAudio();
    if (!QTest::qWaitFor([&] { return file.state() != FilePlaybackState::Seeking; }, 3000)) qFatal("Seek timed out");
    const auto after = frame(file);
    QVERIFY(requested && hidden && resets.size() == 1 && after.timelineEpoch == before.timelineEpoch + 1 &&
            after.mediaTimeNs >= 1250000000LL && after.pixels.pixelColor(50, 50).blue() > 240);
    const auto proof = qEnvironmentVariable("DESKFLOW_FILE_PROOF");
    if (!proof.isEmpty()) {
      if (!before.pixels.save(QDir(proof).filePath("seek-before.png")) ||
          !after.pixels.save(QDir(proof).filePath("seek-after.png"))) qFatal("Could not save seek proof");
      qInfo() << "Seek actual decoded frames PTS-ns:" << before.mediaTimeNs << after.mediaTimeNs
              << "epochs:" << before.timelineEpoch << after.timelineEpoch;
    }
  }
  void audioClock()
  {
    FileSource file; ready(file); file.command(session, source, "resume");
    std::optional<FileAudioBlock> audio;
    if (!QTest::qWaitFor([&] { audio = file.takeAudio(); return audio.has_value(); }, 3000)) qFatal("No audio");
    const auto video = frame(file);
    QVERIFY(audio->samples.size() > 0 && audio->samples.size() % 8 == 0 && audio->durationNs > 0 &&
            qAbs(audio->mediaTimeNs - video.mediaTimeNs) < 100000000LL && audio->timelineEpoch == video.timelineEpoch);
    qInfo() << "Decoded PCM bytes" << audio->samples.size() << "audio PTS" << audio->mediaTimeNs
            << "video PTS" << video.mediaTimeNs << "duration" << audio->durationNs;
  }
  void audioOffAndSilent()
  {
    FileSource file; ready(file, "av.webm", false); file.command(session, source, "resume"); frame(file);
    const bool off = !file.takeAudio();
    ready(file, "silent.webm", true); file.command(session, source, "resume"); frame(file);
    QVERIFY(off && !file.metadata().hasAudio && !file.takeAudio());
  }
  void variablePresentationTimes()
  {
    FileSource file; ready(file, "vfr.webm", false); file.command(session, source, "resume");
    const auto first = frame(file);
    const auto second = frame(file);
    QVERIFY(second.mediaTimeNs - first.mediaTimeNs == 100000000LL);
    qInfo() << "VFR retained file PTS gap" << second.mediaTimeNs - first.mediaTimeNs;
  }
  void pausedSeekAudioEpoch()
  {
    FileSource file; ready(file); file.command(session, source, "seek", 1250000000LL);
    if (!QTest::qWaitFor([&] { return file.state() == FilePlaybackState::Paused; }, 3000)) qFatal("Paused seek failed");
    const bool stayedPaused = !file.takeFrame() && !file.takeAudio();
    file.command(session, source, "resume");
    std::optional<FileAudioBlock> audio;
    if (!QTest::qWaitFor([&] { audio = file.takeAudio(); return audio.has_value(); }, 3000)) qFatal("No seek audio");
    QVERIFY(stayedPaused && audio->timelineEpoch == 1 && audio->mediaTimeNs >= 1250000000LL);
  }
  void ownershipAndStop()
  {
    FileSource file; ready(file); file.command(session, source, "resume"); frame(file);
    const bool rejected = !file.command(QString(32, 'c'), source, "stop");
    file.command(session, source, "stop");
    QVERIFY(rejected && file.state() == FilePlaybackState::Stopped && !file.takeFrame() && !file.takeAudio() &&
            file.positionNs() == 0 && !file.command(session, source, "resume"));
  }
  void endOfFile()
  {
    FileSource file; ready(file); file.command(session, source, "seek", 1800000000LL);
    if (!QTest::qWaitFor([&] { return file.state() == FilePlaybackState::Paused; }, 3000)) qFatal("Seek setup failed");
    file.command(session, source, "resume");
    const bool ended = QTest::qWaitFor([&] { file.takeFrame(); file.takeAudio(); return file.state() == FilePlaybackState::Ended; }, 3000);
    QVERIFY(ended && file.positionNs() == file.metadata().durationNs && !file.takeFrame() &&
            !file.command(session, source, "resume"));
  }
  void invalidFiles_data()
  {
    QTest::addColumn<QString>("name");
    QTest::newRow("missing") << "missing.webm";
    QTest::newRow("unsupported") << "invalid.webm";
    QTest::newRow("truncated") << "truncated.mp4";
    QTest::newRow("unsupported-codec") << "unsupported-codec.mkv";
  }
#ifdef Q_OS_WIN
  void cancelReleasesFile()
  {
    FileSource file;
    bool released = true;
    for (int iteration = 0; iteration < 10; ++iteration) {
      file.open(fixture("av.webm"), session, source, true);
      file.stop();
      const auto path = fixture("av.webm");
      const auto lock = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
      if (lock == INVALID_HANDLE_VALUE) released = false;
      else CloseHandle(lock);
    }
    QVERIFY(released && file.state() == FilePlaybackState::Stopped);
  }
  void unreadableFile()
  {
    const auto path = fixture("av.webm");
    const auto locked = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (locked == INVALID_HANDLE_VALUE) qFatal("Could not lock fixture exclusively");
    FileSource file; const bool opened = file.open(path, session, source, true);
    CloseHandle(locked);
    QVERIFY(!opened && file.state() == FilePlaybackState::Error && file.error().contains("unreadable"));
  }
#endif
  void invalidFiles()
  {
    QFETCH(QString, name);
    FileSource file; file.open(fixture(name), session, source, true);
    const bool failed = QTest::qWaitFor([&] { return file.state() == FilePlaybackState::Error; }, 3000);
    QVERIFY(failed && !file.error().isEmpty() && !file.takeFrame() && !file.takeAudio());
    qInfo() << name << file.error();
  }
};
QTEST_GUILESS_MAIN(StreamingFileTests)
#include "StreamingFileTests.moc"
