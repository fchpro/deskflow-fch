// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "GstRuntime.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace deskflow::streaming {
bool configureGstRuntime(QString &error)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
  const auto root = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
  constexpr auto pathSensitivity = Qt::CaseInsensitive;
#else
  constexpr auto pathSensitivity = Qt::CaseSensitive;
#endif
  // CMake explicitly marks development outputs. A missing package marker never
  // enables SDK discovery merely because an install is staged below the build tree.
  QFile development(root + "/streaming-runtime.development");
  if (!QFile::exists(root + "/streaming-runtime.version") &&
      development.open(QIODevice::ReadOnly) &&
      development.readAll() == QByteArray(DESKFLOW_STREAMING_BUILD_DIRECTORY) &&
      root.startsWith(QStringLiteral(DESKFLOW_STREAMING_BUILD_DIRECTORY) + "/", pathSensitivity))
    return true;
  QFile marker(root + "/streaming-runtime.version");
  if (!marker.open(QIODevice::ReadOnly) || marker.readAll() != "1.28.7\n") {
    error = "Required packaged streaming runtime 1.28.7 is missing or invalid. Reinstall this feature build.";
    return false;
  }
  const auto plugins = root + "/gstreamer-1.0";
#ifdef Q_OS_WIN
  const auto scanner = root + "/gst-plugin-scanner.exe";
#else
  const auto scanner = root + "/gst-plugin-scanner";
#endif
  if (!QDir(plugins).exists() || !QFile::exists(scanner)) {
    error = "Packaged streaming plugins or scanner are missing. Reinstall this feature build.";
    return false;
  }
  // A new private registry prevents stale development/user cache entries from
  // advertising plugins outside this package. No write access to the install is needed.
  static QTemporaryDir registry(QDir::tempPath() + "/deskflow-gst-XXXXXX");
  if (!registry.isValid()) {
    error = "Cannot create the private streaming plugin registry.";
    return false;
  }
  qputenv("GST_PLUGIN_PATH_1_0", QFile::encodeName(plugins));
  qputenv("GST_PLUGIN_PATH", QFile::encodeName(plugins));
  // An empty qputenv value removes the variable in the Windows CRT. A concrete
  // private path is essential here; otherwise GStreamer discovers SDK defaults.
  qputenv("GST_PLUGIN_SYSTEM_PATH_1_0", QFile::encodeName(plugins));
  qputenv("GST_PLUGIN_SYSTEM_PATH", QFile::encodeName(plugins));
  qputenv("GST_PLUGIN_SCANNER_1_0", QFile::encodeName(scanner));
  qputenv("GST_PLUGIN_SCANNER", QFile::encodeName(scanner));
  qputenv("GST_REGISTRY_1_0", QFile::encodeName(registry.filePath("registry.bin")));
  qputenv("GST_REGISTRY", QFile::encodeName(registry.filePath("registry.bin")));
  qunsetenv("GST_REGISTRY_UPDATE");
  qunsetenv("GST_REGISTRY_DISABLE");
  qunsetenv("GST_REGISTRY_FORK");
#else
  Q_UNUSED(error);
#endif
  return true;
}
}
