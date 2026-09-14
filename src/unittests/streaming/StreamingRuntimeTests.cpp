// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/GstCapturePipeline.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
using namespace deskflow::streaming;

class StreamingRuntimeTests : public QObject {
  Q_OBJECT
private Q_SLOTS:
  void relocatedRuntime_data()
  {
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<int>("expected");
    for (const auto *scenario : {"missing-manifest", "missing-manifest-under-build", "relocated-development",
                                  "wrong-version", "missing-directory", "missing-scanner"})
      QTest::newRow(scenario) << QString(scenario) << 1;
    for (const auto *scenario : {"initialize", "origin", "external", "scanner", "registry"})
      QTest::newRow(scenario) << QString(scenario) << 0;
  }
  void relocatedRuntime()
  {
    QFETCH(QString, scenario);
    QFETCH(int, expected);
    QTemporaryDir directory(scenario == "missing-manifest-under-build"
      ? QCoreApplication::applicationDirPath() + "/incomplete-package-XXXXXX"
      : QDir::tempPath() + "/deskflow-runtime-test-XXXXXX");
    const auto root = directory.path();
    const auto sdk = QStringLiteral(DESKFLOW_TEST_GSTREAMER_SDK);
    if (!directory.isValid() || !QFile::copy(QCoreApplication::applicationFilePath(), root + "/probe.exe"))
      qFatal("Cannot prepare relocated executable fixture");
    if (scenario == "relocated-development" && !QFile::copy(
          QCoreApplication::applicationDirPath() + "/streaming-runtime.development", root + "/streaming-runtime.development"))
      qFatal("Cannot prepare relocated development marker fixture");
    if (!scenario.startsWith("missing-manifest") && scenario != "relocated-development") {
      QFile marker(root + "/streaming-runtime.version");
      if (!marker.open(QIODevice::WriteOnly) || marker.write(scenario == "wrong-version" ? "0\n" : "1.28.7\n") < 0)
        qFatal("Cannot prepare runtime marker fixture");
    }
    if (scenario != "missing-directory") {
      if (!QDir().mkpath(root + "/gstreamer-1.0") ||
          !QFile::copy(sdk + "/lib/gstreamer-1.0/gstapp.dll", root + "/gstreamer-1.0/gstapp.dll"))
        qFatal("Cannot prepare private app plugin fixture");
    }
    if (scenario != "missing-scanner" &&
        !QFile::copy(sdk + "/libexec/gstreamer-1.0/gst-plugin-scanner.exe", root + "/gst-plugin-scanner.exe"))
      qFatal("Cannot prepare scanner fixture");
    QProcess child;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("GST_PLUGIN_PATH_1_0", sdk + "/lib/gstreamer-1.0");
    environment.insert("GST_PLUGIN_SYSTEM_PATH_1_0", sdk + "/lib/gstreamer-1.0");
    environment.insert("GST_REGISTRY_1_0", root + "/untrusted-registry.bin");
    environment.insert("GST_PLUGIN_SCANNER_1_0", root + "/untrusted-scanner.exe");
    child.setProcessEnvironment(environment);
    child.start(root + "/probe.exe", {"--runtime-probe", scenario});
    if (!child.waitForStarted(3000) || !child.waitForFinished(3000))
      qFatal("Relocated runtime process did not complete");
    qInfo().noquote() << child.readAllStandardError();
    QCOMPARE(child.exitCode(), expected);
  }
};

int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  if (app.arguments().contains("--runtime-probe")) {
    QString error;
    if (!GstCapturePipeline::initialize(error)) {
      qInfo().noquote() << error;
      return 1;
    }
    const auto root = app.applicationDirPath();
    const auto scenario = app.arguments().last();
    if (scenario == "origin") {
      auto *factory = gst_element_factory_find("appsrc");
      if (!factory) return 2;
      auto *plugin = gst_plugin_feature_get_plugin(GST_PLUGIN_FEATURE(factory));
      const auto file = QString::fromUtf8(gst_plugin_get_filename(plugin));
      qInfo().noquote() << "Loaded plugin" << file;
      gst_object_unref(plugin);
      gst_object_unref(factory);
      return QDir::fromNativeSeparators(file) == root + "/gstreamer-1.0/gstapp.dll" ? 0 : 3;
    }
    if (scenario == "external") {
      auto *factory = gst_element_factory_find("fakesink");
      if (factory) { gst_object_unref(factory); return 4; }
    }
    if (scenario == "scanner")
      return QDir::fromNativeSeparators(qEnvironmentVariable("GST_PLUGIN_SCANNER_1_0")) == root + "/gst-plugin-scanner.exe" ? 0 : 5;
    if (scenario == "registry")
      return qEnvironmentVariable("GST_REGISTRY_1_0") != root + "/untrusted-registry.bin" ? 0 : 6;
    return 0;
  }
  StreamingRuntimeTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "StreamingRuntimeTests.moc"
