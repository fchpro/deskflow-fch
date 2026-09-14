// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/FileSource.h"
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
using namespace deskflow::streaming;
class StreamingFileTimelineTests : public QObject {
  Q_OBJECT
  QTemporaryDir fixtures;
private Q_SLOTS:
  void initTestCase()
  {
    QProcess generator;
    generator.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-nostdin", "-f", "lavfi", "-i",
      "testsrc2=size=160x96:rate=30", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
      "-t", "2", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", fixtures.filePath("reordered.mp4")});
    if (!generator.waitForFinished(10000) || generator.exitCode()) qFatal("Owned fixture generation failed");
  }
  void mediaTimestamp_data()
  {
    QTest::addColumn<bool>("audio");
    QTest::addColumn<qint64>("position");
    QTest::newRow("video-initial") << false << qint64(0);
    QTest::newRow("audio-initial") << true << qint64(0);
    QTest::newRow("video-seek") << false << qint64(500000000);
    QTest::newRow("audio-seek") << true << qint64(500000000);
  }
  void mediaTimestamp()
  {
    QFETCH(bool, audio);
    QFETCH(qint64, position);
    FileSource file;
    const QString session(32, 'a'), source(32, 'b');
    if (!file.open(fixtures.filePath("reordered.mp4"), session, source, true) ||
        !QTest::qWaitFor([&] { return file.state() != FilePlaybackState::Opening; }, 3000) ||
        file.state() != FilePlaybackState::Paused) qFatal("File not ready: %s", qPrintable(file.error()));
    if (position && (!file.command(session, source, "seek", position) ||
        !QTest::qWaitFor([&] { return file.state() != FilePlaybackState::Seeking; }, 3000) ||
        file.state() != FilePlaybackState::Paused)) qFatal("Seek not ready: %s", qPrintable(file.error()));
    qint64 mediaTime;
    if (audio) {
      if (!file.command(session, source, "resume")) qFatal("Resume failed");
      std::optional<FileAudioBlock> block;
      if (!QTest::qWaitFor([&] { block = file.takeAudio(); return block.has_value(); }, 3000)) qFatal("No PCM");
      mediaTime = block->mediaTimeNs;
    } else {
      const auto frame = file.takePreroll();
      if (!frame) qFatal("No decoded preroll");
      mediaTime = frame->mediaTimeNs;
      qInfo() << "Decoded frame" << frame->pixels.size() << "top-left" << frame->pixels.pixelColor(0, 0);
    }
    qInfo() << "track" << (audio ? "audio" : "video") << "requested-media-ns" << position << "sample-media-ns" << mediaTime;
    QCOMPARE(mediaTime, position);
  }
};
QTEST_GUILESS_MAIN(StreamingFileTimelineTests)
#include "StreamingFileTimelineTests.moc"
