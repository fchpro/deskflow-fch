// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "Protocol.h"
#include <QPointF>
#include <QRect>
#include <QSet>
#include <memory>
#include <optional>
namespace deskflow::streaming {
bool controlInteger(const QJsonValue &, qint64 minimum, qint64 maximum);
bool validControlGeometry(const QJsonObject &);
bool validControlInput(const QJsonObject &);
std::optional<QPoint> mapControlPoint(const QRect &video, const QRect &physical, const QPointF &point);
class NativeControl {
public:
  virtual ~NativeControl() = default;
  virtual bool available() const = 0;
  virtual bool bind(const QJsonObject &target) = 0;
  virtual bool verify(const QRect &geometry, const std::optional<QPoint> &point, bool keyboard) = 0;
  virtual bool idle() const = 0;
  virtual bool inject(const QJsonObject &event) = 0;
  virtual bool physicalInput() = 0;
  std::function<void()> physicalPriority;
  virtual void exclusions(const QStringList &) {}
};
std::unique_ptr<NativeControl> createNativeControl();
// Core signaling worker owns this lease. Native calls never occur in GUI/media.
class ControlOwner {
public:
  explicit ControlOwner(std::unique_ptr<NativeControl> native = createNativeControl());
  ~ControlOwner();
  bool available() const;
  void exclusions(const QStringList &apps){if(m_native)m_native->exclusions(apps);}
  bool target(const QJsonObject &target);
  bool geometry(const QJsonObject &geometry);
  bool grant(const QString &lease, qint64 now);
  bool input(const QJsonObject &input, qint64 now);
  bool poll(qint64 now);
  void heartbeat(qint64 now);
  void motionRate(int hz){m_motionInterval=hz>0?1000.0/hz:0;}
  void revoke();
  bool active() const { return !m_lease.isEmpty(); }
  bool releasePending() const { return m_releasing; }
  QJsonObject frame() const { return m_geometry; }
private:
  bool send(const QJsonObject &event);
  std::unique_ptr<NativeControl> m_native;
  QJsonObject m_geometry, m_target;
  QString m_lease;
  QSet<int> m_keys, m_buttons;
  qint64 m_deadline = 0, m_sequence = 0;
  double m_motionAt = -4, m_motionInterval = 4;
  QJsonObject m_motion;
  bool m_releasing = false;
};
}
