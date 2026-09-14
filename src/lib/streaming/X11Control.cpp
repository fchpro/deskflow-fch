// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "Control.h"
#include "ControlKeys.h"
#include <QDBusInterface>
#include <QDBusReply>
#include <QFile>
#include <QSocketNotifier>
#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <xcb/xtest.h>
#include <xcb/randr.h>
#include <xcb/xcb_keysyms.h>
#include <X11/keysym.h>
#include <unistd.h>
#include <cstdlib>

namespace deskflow::streaming {
namespace {
quint64 birth(qint64 pid) {
  QFile file(QString("/proc/%1/stat").arg(pid));if(!file.open(QIODevice::ReadOnly))return 0;
  const auto line=file.readAll();const auto fields=line.mid(line.lastIndexOf(')')+2).split(' ');
  return fields.size()>19?fields[19].toULongLong():0;
}
xcb_keysym_t symbol(int code) {
  if(const auto printable=printableControlSymbol(code))return printable;
  if(code>=112 && code<=135)return XK_F1+code-112;
  switch(code){case 8:return XK_BackSpace;case 9:return XK_Tab;case 13:return XK_Return;case 20:return XK_Caps_Lock;case 27:return XK_Escape;
    case 33:return XK_Page_Up;case 34:return XK_Page_Down;case 35:return XK_End;case 36:return XK_Home;case 37:return XK_Left;case 38:return XK_Up;case 39:return XK_Right;case 40:return XK_Down;case 45:return XK_Insert;case 46:return XK_Delete;
    case 91:return XK_Super_L;case 92:return XK_Super_R;case 160:return XK_Shift_L;case 161:return XK_Shift_R;case 162:return XK_Control_L;case 163:return XK_Control_R;case 164:return XK_Alt_L;case 165:return XK_Alt_R;
    case 186:return XK_semicolon;case 187:return XK_equal;case 188:return XK_comma;case 189:return XK_minus;case 190:return XK_period;case 191:return XK_slash;case 192:return XK_grave;case 219:return XK_bracketleft;case 220:return XK_backslash;case 221:return XK_bracketright;case 222:return XK_apostrophe;default:return XCB_NO_SYMBOL;}
}
class X11Control final:public NativeControl {
public:
  X11Control() {
    int screen=0;m_connection=xcb_connect(nullptr,&screen);if(xcb_connection_has_error(m_connection))return;
    auto it=xcb_setup_roots_iterator(xcb_get_setup(m_connection));while(screen-- && it.rem)xcb_screen_next(&it);if(!it.rem)return;m_root=it.data->root;
    const auto *xi=xcb_get_extension_data(m_connection,&xcb_input_id);const auto *xt=xcb_get_extension_data(m_connection,&xcb_test_id);
    if(!xi || !xi->present || !xt || !xt->present)return;m_xi=xi->major_opcode;
    auto version=xcb_input_xi_query_version_reply(m_connection,xcb_input_xi_query_version(m_connection,2,0),nullptr);if(!version)return;free(version);
    auto devices=xcb_input_xi_query_device_reply(m_connection,xcb_input_xi_query_device(m_connection,XCB_INPUT_DEVICE_ALL),nullptr);
    if(!devices)return;
    for(auto d=xcb_input_xi_query_device_infos_iterator(devices);d.rem;xcb_input_xi_device_info_next(&d)) {
      const auto name=QByteArray(xcb_input_xi_device_info_name(d.data),xcb_input_xi_device_info_name_length(d.data));
      if(name=="Virtual core XTEST pointer" || name=="Virtual core XTEST keyboard")m_synthetic.insert(d.data->deviceid);
    }free(devices);if(m_synthetic.size()!=2)return;
    struct Mask {xcb_input_event_mask_t header;uint32_t mask;};
    const Mask selection[]={{{XCB_INPUT_DEVICE_ALL_MASTER,1},
      XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS|XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE|XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_PRESS|XCB_INPUT_XI_EVENT_MASK_RAW_BUTTON_RELEASE|XCB_INPUT_XI_EVENT_MASK_RAW_MOTION},
      {{XCB_INPUT_DEVICE_ALL,1},XCB_INPUT_XI_EVENT_MASK_HIERARCHY}};
    if(!checked(xcb_input_xi_select_events_checked(m_connection,m_root,2,&selection[0].header)))return;
    QDBusInterface manager("org.freedesktop.login1","/org/freedesktop/login1","org.freedesktop.login1.Manager",QDBusConnection::systemBus());manager.setTimeout(1000);
    QDBusReply<QDBusObjectPath> login=manager.call("GetSessionByPID",quint32(getpid()));if(login.isValid())m_login=login.value().path();
    m_notifier=std::make_unique<QSocketNotifier>(xcb_get_file_descriptor(m_connection),QSocketNotifier::Read);
    QObject::connect(m_notifier.get(),&QSocketNotifier::activated,[this]{drain();});m_ready=true;
  }
  ~X11Control()override {m_notifier.reset();if(m_connection)xcb_disconnect(m_connection);}
  bool available()const override{return m_ready && !xcb_connection_has_error(m_connection) && unlocked();}
  bool bind(const QJsonObject &target)override {
    drain();if(!fields(target,{"session","source","kind","handle","monitor","pid","birth"}) ||
      !controlInteger(target["handle"],1,UINT32_MAX) || !controlInteger(target["monitor"],0,UINT32_MAX) || !controlInteger(target["pid"],0,INT32_MAX))return false;
    bool ok=false;const auto stamp=target["birth"].toString().toULongLong(&ok,16);if(!ok || (target["kind"]!="screen" && target["kind"]!="window"))return false;
    m_target=target;m_window=target["handle"].toInteger();m_pid=target["pid"].toInteger();m_birth=stamp;m_dead=false;
    const uint32_t mask=XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    return checked(xcb_change_window_attributes_checked(m_connection,m_window,XCB_CW_EVENT_MASK,&mask));
  }
  bool verify(const QRect &geometry,const std::optional<QPoint> &point,bool keyboard)override {
    drain();if(!available() || m_dead)return false;
    QRect actual;
    if(m_target["kind"]=="window") {
      if(!m_birth || birth(m_pid)!=m_birth || property(m_window,"_NET_WM_PID")!=quint32(m_pid))return false;
      auto attributes=xcb_get_window_attributes_reply(m_connection,xcb_get_window_attributes(m_connection,m_window),nullptr);
      const bool shown=attributes && attributes->map_state==XCB_MAP_STATE_VIEWABLE;free(attributes);if(!shown)return false;
      auto shape=xcb_get_geometry_reply(m_connection,xcb_get_geometry(m_connection,m_window),nullptr);
      auto origin=xcb_translate_coordinates_reply(m_connection,xcb_translate_coordinates(m_connection,m_window,m_root,0,0),nullptr);
      if(shape && origin)actual={origin->dst_x,origin->dst_y,shape->width,shape->height};free(shape);free(origin);
      if(keyboard){auto focus=xcb_get_input_focus_reply(m_connection,xcb_get_input_focus(m_connection),nullptr);const bool match=focus && descendant(focus->focus,m_window);free(focus);if(!match)return false;}
      if(point){auto location=xcb_translate_coordinates_reply(m_connection,xcb_translate_coordinates(m_connection,m_root,m_root,point->x(),point->y()),nullptr);
        const auto top=location?location->child:XCB_NONE;free(location);if(top==XCB_NONE || !(descendant(top,m_window) || descendant(m_window,top)))return false;}
    }else {
      auto monitors=xcb_randr_get_monitors_reply(m_connection,xcb_randr_get_monitors(m_connection,m_root,true),nullptr);
      if(monitors){for(auto m=xcb_randr_get_monitors_monitors_iterator(monitors);m.rem;xcb_randr_monitor_info_next(&m))if(m.data->name==m_target["monitor"].toInteger()){actual={m.data->x,m.data->y,m.data->width,m.data->height};break;}free(monitors);}
      if(keyboard){auto focus=xcb_get_input_focus_reply(m_connection,xcb_get_input_focus(m_connection),nullptr);if(!focus)return false;
        auto origin=xcb_translate_coordinates_reply(m_connection,xcb_translate_coordinates(m_connection,focus->focus,m_root,0,0),nullptr);free(focus);
        const bool inside=origin && actual.contains(origin->dst_x,origin->dst_y);free(origin);if(!inside)return false;}
    }
    return !actual.isEmpty() && actual==geometry;
  }
  bool idle()const override {
    auto keys=xcb_query_keymap_reply(m_connection,xcb_query_keymap(m_connection),nullptr);if(!keys)return false;
    bool clear=true;for(auto key:keys->keys)clear&=key==0;free(keys);
    auto pointer=xcb_query_pointer_reply(m_connection,xcb_query_pointer(m_connection,m_root),nullptr);
    clear=clear && pointer && !(pointer->mask&(XCB_BUTTON_MASK_1|XCB_BUTTON_MASK_2|XCB_BUTTON_MASK_3|XCB_BUTTON_MASK_4|XCB_BUTTON_MASK_5));free(pointer);return clear;
  }
  bool physicalInput()override{drain();return std::exchange(m_physical,false);}
  bool inject(const QJsonObject &event)override {
    if(!available())return false;const auto kind=event["kind"].toString();const int code=event["code"].toInt();
    if(kind=="key" || kind=="repeat") {
      auto native=m_keys.value(code,0);
      if(!native){auto symbols=xcb_key_symbols_alloc(m_connection);if(!symbols)return false;auto codes=xcb_key_symbols_get_keycode(symbols,symbol(code));if(codes)native=codes[0];free(codes);xcb_key_symbols_free(symbols);}if(!native)return false;
      if(!fake(event["down"].toBool()?XCB_KEY_PRESS:XCB_KEY_RELEASE,native,0,0))return false;
      if(event["down"].toBool())m_keys.insert(code,native);else m_keys.remove(code);return true;
    }
    if(!event["release"].toBool() && !fake(XCB_MOTION_NOTIFY,0,event["x"].toInt(),event["y"].toInt()))return false;
    if(kind=="button"){const int native=code==1?1:code==2?3:code==3?2:0;return native && fake(event["down"].toBool()?XCB_BUTTON_PRESS:XCB_BUTTON_RELEASE,native,0,0);}
    if(kind=="wheel")for(const auto *axis:{"dx","dy"}){
      auto &remainder=QString(axis)=="dx"?m_horizontal:m_vertical;remainder+=event[axis].toInt();
      while(std::abs(remainder)>=120){const bool positive=remainder>0;const int button=QString(axis)=="dx"?(positive?7:6):(positive?4:5);
        if(!fake(XCB_BUTTON_PRESS,button,0,0) || !fake(XCB_BUTTON_RELEASE,button,0,0))return false;remainder+=positive?-120:120;}
    }
    return true;
  }
private:
  bool checked(xcb_void_cookie_t cookie)const {auto error=xcb_request_check(m_connection,cookie);const bool ok=!error;free(error);return ok;}
  bool fake(uint8_t type,uint8_t detail,int x,int y) {return checked(xcb_test_fake_input_checked(m_connection,type,detail,XCB_CURRENT_TIME,m_root,x,y,0));}
  bool unlocked()const {if(m_login.isEmpty())return false;QDBusInterface session("org.freedesktop.login1",m_login,"org.freedesktop.login1.Session",QDBusConnection::systemBus());session.setTimeout(100);const auto active=session.property("Active"),locked=session.property("LockedHint");return active.isValid() && locked.isValid() && active.toBool() && !locked.toBool();}
  uint32_t property(xcb_window_t window,const char *name) {
    auto atom=xcb_intern_atom_reply(m_connection,xcb_intern_atom(m_connection,true,strlen(name),name),nullptr);if(!atom)return 0;
    auto value=xcb_get_property_reply(m_connection,xcb_get_property(m_connection,false,window,atom->atom,XCB_ATOM_CARDINAL,0,1),nullptr);free(atom);
    const uint32_t result=value && xcb_get_property_value_length(value)==4?*static_cast<uint32_t *>(xcb_get_property_value(value)):0;free(value);return result;
  }
  bool descendant(xcb_window_t window,xcb_window_t ancestor) {
    for(int depth=0;window && depth<64;++depth){if(window==ancestor)return true;auto tree=xcb_query_tree_reply(m_connection,xcb_query_tree(m_connection,window),nullptr);if(!tree)return false;const auto parent=tree->parent;free(tree);if(parent==window)return false;window=parent;}return false;
  }
  void drain() {
    if(!m_connection)return;
    for(int count=0;count<256;++count){auto event=xcb_poll_for_event(m_connection);if(!event)break;const auto type=event->response_type&0x7f;
      bool physical=false;
      if(type==XCB_DESTROY_NOTIFY && reinterpret_cast<xcb_destroy_notify_event_t *>(event)->window==m_window)m_dead=true;
      if(type==XCB_GE_GENERIC){auto generic=reinterpret_cast<xcb_ge_generic_event_t *>(event);
        if(generic->extension==m_xi){if(generic->event_type==XCB_INPUT_HIERARCHY){m_ready=false;physical=true;}
          else if(generic->event_type>=XCB_INPUT_RAW_KEY_PRESS && generic->event_type<=XCB_INPUT_RAW_MOTION){const auto raw=reinterpret_cast<xcb_input_raw_key_press_event_t *>(event);physical=!m_synthetic.contains(raw->sourceid);}}
      }
      free(event);if(physical || m_dead){m_physical=true;if(physicalPriority)physicalPriority();}
    }
  }
  xcb_connection_t *m_connection=nullptr;xcb_window_t m_root=0,m_window=0;uint8_t m_xi=0;bool m_ready=false,m_dead=false,m_physical=false;
  QJsonObject m_target;QString m_login;qint64 m_pid=0;quint64 m_birth=0;QSet<int> m_synthetic;QHash<int,xcb_keycode_t> m_keys;
  std::unique_ptr<QSocketNotifier> m_notifier;int m_horizontal=0,m_vertical=0;
};
}
std::unique_ptr<NativeControl> createNativeControl(){if(qEnvironmentVariable("XDG_SESSION_TYPE")=="wayland" || !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))return {};return std::make_unique<X11Control>();}
}
