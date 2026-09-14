// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "Control.h"
#include "common/StreamingInputGate.h"
#include <cmath>
namespace deskflow::streaming {
bool controlInteger(const QJsonValue &v, qint64 lo, qint64 hi) {
  return v.isDouble() && v.toDouble() >= double(lo) && v.toDouble() <= double(hi) &&
    v.toDouble() == double(v.toInteger(lo - 1));
}
bool validControlGeometry(const QJsonObject &g) {
  return fields(g,{"session","source","frame","epoch","geometry","x","y","width","height","valid"}) &&
    identifier(g["session"].toString()) && identifier(g["source"].toString()) &&
    controlInteger(g["frame"],1,9007199254740991LL) && controlInteger(g["epoch"],0,9007199254740991LL) &&
    controlInteger(g["geometry"],1,9007199254740991LL) && controlInteger(g["x"],-131072,131072) &&
    controlInteger(g["y"],-131072,131072) && controlInteger(g["width"],1,32768) &&
    controlInteger(g["height"],1,32768) && g["valid"].isBool();
}
bool validControlInput(const QJsonObject &e) {
  auto common=e; for (const auto *key:{"kind","code","down","x","y","dx","dy"}) common.remove(key);
  if (!fields(common,{"session","source","lease","sequence","frame","epoch","geometry"}) ||
      !identifier(e["session"].toString()) || !identifier(e["source"].toString()) || !identifier(e["lease"].toString()) ||
      !controlInteger(e["sequence"],1,9007199254740991LL) || !controlInteger(e["frame"],1,9007199254740991LL) ||
      !controlInteger(e["epoch"],0,9007199254740991LL) || !controlInteger(e["geometry"],1,9007199254740991LL)) return false;
  const auto kind=e["kind"].toString();
  if (kind=="key" || kind=="repeat") return e.size()==10 && controlInteger(e["code"],1,255) && e["down"].isBool() && (kind!="repeat" || e["down"].toBool());
  if (!controlInteger(e["x"],-131072,163840) || !controlInteger(e["y"],-131072,163840)) return false;
  if (kind=="move") return e.size()==10;
  if (kind=="button") return e.size()==12 && controlInteger(e["code"],1,3) && e["down"].isBool();
  if (kind=="wheel") return e.size()==12 && controlInteger(e["dx"],-1200,1200) && controlInteger(e["dy"],-1200,1200);
  return false;
}
std::optional<QPoint> mapControlPoint(const QRect &video,const QRect &physical,const QPointF &p) {
  if (video.isEmpty() || physical.isEmpty() || !std::isfinite(p.x()) || !std::isfinite(p.y()) ||
      p.x()<video.x() || p.y()<video.y() || p.x()>=video.x()+video.width() || p.y()>=video.y()+video.height()) return {};
  return QPoint(physical.x()+int((p.x()-video.x())*physical.width()/video.width()),
                physical.y()+int((p.y()-video.y())*physical.height()/video.height()));
}
ControlOwner::ControlOwner(std::unique_ptr<NativeControl> native):m_native(std::move(native)) {
  if(m_native)m_native->physicalPriority=[this]{revoke();};
}
ControlOwner::~ControlOwner(){revoke();}
bool ControlOwner::available() const {return m_native && m_native->available();}
bool ControlOwner::target(const QJsonObject &target) {
  revoke();
  if(m_releasing)return false;
  if(!m_target.isEmpty() && target["session"]==m_target["session"])return false;
  m_geometry={}; m_target={};
  if (!available() || !identifier(target["session"].toString()) || !identifier(target["source"].toString()) || !m_native->bind(target)) return false;
  m_target=target; return true;
}
bool ControlOwner::geometry(const QJsonObject &g) {
  if (!validControlGeometry(g) || g["session"]!=m_target["session"] || g["source"]!=m_target["source"] ||
      (!m_geometry.isEmpty() && g["frame"].toInteger()<=m_geometry["frame"].toInteger())) return false;
  if (!m_geometry.isEmpty() && (g["geometry"]!=m_geometry["geometry"] || g["epoch"]!=m_geometry["epoch"])) revoke();
  m_geometry=g;
  if (!g["valid"].toBool()) revoke();
  return true;
}
bool ControlOwner::grant(const QString &lease,qint64 now) {
  if (active() || m_releasing || !available() || !ordinaryInputLocal || viewerOwnsInput || !identifier(lease) ||
      !m_geometry["valid"].toBool() || !m_native->idle() ||
      !m_native->verify(QRect(m_geometry["x"].toInt(),m_geometry["y"].toInt(),m_geometry["width"].toInt(),m_geometry["height"].toInt()),{},false)) return false;
  bool expected=false; if (!controlOwnsInput.compare_exchange_strong(expected,true)) return false;
  if(!ordinaryInputLocal){controlOwnsInput=false;return false;}
  m_native->physicalInput(); m_lease=lease; m_deadline=now+1500; m_sequence=0; m_motionAt=now-m_motionInterval; return true;
}
void ControlOwner::heartbeat(qint64 now){if(active())m_deadline=now+1500;}
bool ControlOwner::send(const QJsonObject &e) {
  const QRect bounds(m_geometry["x"].toInt(),m_geometry["y"].toInt(),m_geometry["width"].toInt(),m_geometry["height"].toInt());
  const bool key=e["kind"]=="key" || e["kind"]=="repeat";
  const std::optional<QPoint> point=key?std::optional<QPoint>{}:QPoint(e["x"].toInt(),e["y"].toInt());
  if ((point && !bounds.contains(*point)) || !m_native->verify(bounds,point,key) || !active() || !m_native->inject(e)) { revoke(); return false; }
  if (key || e["kind"]=="button") {
    auto &held=key?m_keys:m_buttons;
    if(e["down"].toBool())held.insert(e["code"].toInt());else held.remove(e["code"].toInt());
  }
  // A native hook can revoke reentrantly while injection is in progress. Release
  // any successfully delivered down event again after that native call returns.
  if(!active()){m_releasing=true;revoke();return false;}
  return true;
}
bool ControlOwner::input(const QJsonObject &e,qint64 now) {
  if (!poll(now) || !validControlInput(e) || e["session"]!=m_geometry["session"] || e["source"]!=m_geometry["source"] ||
      e["lease"]!=m_lease || e["epoch"]!=m_geometry["epoch"] || e["geometry"]!=m_geometry["geometry"] ||
      e["frame"].toInteger()>m_geometry["frame"].toInteger() || e["sequence"].toInteger()<=m_sequence) return false;
  const auto kind=e["kind"].toString(); const int code=e["code"].toInt();
  if(kind=="repeat" && !m_keys.contains(code))return false;
  if ((kind=="key" || kind=="button") && (e["down"].toBool()==(kind=="key"?m_keys:m_buttons).contains(code))) return false;
  m_sequence=e["sequence"].toInteger();
  if(kind=="move" && now-m_motionAt<m_motionInterval){m_motion=e;return true;}
  if(!m_motion.isEmpty()){const auto motion=std::exchange(m_motion,{});if(!send(motion))return false;}
  if(kind=="move")m_motionAt=now;
  return send(e);
}
bool ControlOwner::poll(qint64 now) {
  if(m_releasing){revoke();return false;}
  if(!active())return false;
  if(now>=m_deadline || m_native->physicalInput() || !ordinaryInputLocal){revoke();return false;}
  if(!m_motion.isEmpty() && now-m_motionAt>=m_motionInterval){auto e=std::exchange(m_motion,{});m_motionAt=now;return send(e);}
  const QRect bounds(m_geometry["x"].toInt(),m_geometry["y"].toInt(),m_geometry["width"].toInt(),m_geometry["height"].toInt());
  if(!m_native->verify(bounds,{},false)){revoke();return false;} return true;
}
void ControlOwner::revoke() {
  if(!active() && !m_releasing)return;
  m_lease.clear();m_motion={};
  for(int code:QSet<int>(m_keys))if(m_native->inject({{"kind","key"},{"code",code},{"down",false}}))m_keys.remove(code);
  for(int code:QSet<int>(m_buttons))if(m_native->inject({{"kind","button"},{"code",code},{"down",false},{"release",true}}))m_buttons.remove(code);
  m_releasing=!m_keys.isEmpty() || !m_buttons.isEmpty();controlOwnsInput=m_releasing;
}
}
