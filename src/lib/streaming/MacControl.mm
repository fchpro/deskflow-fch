// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "Control.h"
#include "ControlKeys.h"
#include "common/StreamingInputGate.h"
#include <QTimer>
#include <libproc.h>
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#include <cmath>

namespace deskflow::streaming {
namespace {
quint64 processBirth(pid_t pid) {
  proc_bsdinfo info{};
  return proc_pidinfo(pid,PROC_PIDTBSDINFO,0,&info,sizeof(info))==sizeof(info)
    ? quint64(info.pbi_start_tvsec)*1000000+info.pbi_start_tvusec : 0;
}
bool unlocked() {
  NSDictionary *session=CFBridgingRelease(CGSessionCopyCurrentDictionary());
  return session && ![session[@"CGSSessionScreenIsLocked"] boolValue] && AXIsProcessTrusted();
}
QRect physical(CGRect rect,double scale) {
  return {qRound(rect.origin.x*scale),qRound(rect.origin.y*scale),qRound(rect.size.width*scale),qRound(rect.size.height*scale)};
}
std::optional<CGKeyCode> keyCode(int code) {
  switch(code) {
  case 8:return kVK_Delete;case 9:return kVK_Tab;case 13:return kVK_Return;case 27:return kVK_Escape;
  case 33:return kVK_PageUp;case 34:return kVK_PageDown;case 35:return kVK_End;case 36:return kVK_Home;
  case 37:return kVK_LeftArrow;case 38:return kVK_UpArrow;case 39:return kVK_RightArrow;case 40:return kVK_DownArrow;
  case 45:return kVK_Help;case 46:return kVK_ForwardDelete;case 91:return kVK_Command;case 92:return kVK_RightCommand;
  case 160:return kVK_Shift;case 161:return kVK_RightShift;case 162:return kVK_Control;case 163:return kVK_RightControl;
  case 164:return kVK_Option;case 165:return kVK_RightOption;case 20:return kVK_CapsLock;
  }
  const CGKeyCode functions[]={kVK_F1,kVK_F2,kVK_F3,kVK_F4,kVK_F5,kVK_F6,kVK_F7,kVK_F8,kVK_F9,kVK_F10,kVK_F11,kVK_F12,kVK_F13,kVK_F14,kVK_F15,kVK_F16,kVK_F17,kVK_F18,kVK_F19,kVK_F20};
  if(code>=112 && code<132)return functions[code-112];
  // Resolve printable virtual-key symbols against the current source keyboard layout.
  const UniChar wanted=printableControlSymbol(code);
  if(!wanted)return {};
  auto input=TISCopyCurrentKeyboardLayoutInputSource();if(!input)return {};
  auto data=static_cast<CFDataRef>(TISGetInputSourceProperty(input,kTISPropertyUnicodeKeyLayoutData));
  std::optional<CGKeyCode> result;
  if(data)for(int key=0;key<128;++key){UInt32 dead=0;UniChar characters[4]{};UniCharCount count=0;
    if(UCKeyTranslate(reinterpret_cast<const UCKeyboardLayout *>(CFDataGetBytePtr(data)),key,kUCKeyActionDown,0,LMGetKbdType(),kUCKeyTranslateNoDeadKeysBit,&dead,4,&count,characters)==noErr && count==1 && characters[0]==wanted){result=CGKeyCode(key);break;}}
  CFRelease(input);return result;
}
class MacControl final:public NativeControl {
public:
  MacControl() {
    if(!AXIsProcessTrusted())return;
    const CGEventMask mask=CGEventMaskBit(kCGEventKeyDown)|CGEventMaskBit(kCGEventKeyUp)|CGEventMaskBit(kCGEventFlagsChanged)|
      CGEventMaskBit(kCGEventLeftMouseDown)|CGEventMaskBit(kCGEventLeftMouseUp)|CGEventMaskBit(kCGEventRightMouseDown)|CGEventMaskBit(kCGEventRightMouseUp)|
      CGEventMaskBit(kCGEventOtherMouseDown)|CGEventMaskBit(kCGEventOtherMouseUp)|CGEventMaskBit(kCGEventMouseMoved)|CGEventMaskBit(kCGEventLeftMouseDragged)|CGEventMaskBit(kCGEventRightMouseDragged)|CGEventMaskBit(kCGEventOtherMouseDragged)|CGEventMaskBit(kCGEventScrollWheel);
    m_tap=CGEventTapCreate(kCGHIDEventTap,kCGHeadInsertEventTap,kCGEventTapOptionDefault,mask,observed,this);
    if(!m_tap)return;
    m_runloop=CFRunLoopGetCurrent();CFRetain(m_runloop);
    m_source=CFMachPortCreateRunLoopSource(kCFAllocatorDefault,m_tap,0);
    if(!m_source)return;
    CFRunLoopAddSource(m_runloop,m_source,kCFRunLoopDefaultMode);
    QObject::connect(&m_dispatch,&QTimer::timeout,[this]{CFRunLoopRunInMode(kCFRunLoopDefaultMode,0,true);});m_dispatch.start(1);
  }
  ~MacControl()override {
    m_dispatch.stop();if(m_source){CFRunLoopRemoveSource(m_runloop,m_source,kCFRunLoopDefaultMode);CFRelease(m_source);}
    if(m_tap){CFMachPortInvalidate(m_tap);CFRelease(m_tap);}if(m_runloop)CFRelease(m_runloop);
  }
  bool available()const override{return m_tap && m_source && CGEventTapIsEnabled(m_tap) && unlocked();}
  bool bind(const QJsonObject &target)override {
    if(!fields(target,{"session","source","kind","handle","pid","birth","scale"}) ||
      !controlInteger(target["handle"],1,UINT32_MAX) || !controlInteger(target["pid"],0,INT32_MAX) ||
      !target["scale"].isDouble() || target["scale"].toDouble()<=0 || target["scale"].toDouble()>8)return false;
    bool ok=false;const auto birth=target["birth"].toString().toULongLong(&ok,16);
    if(!ok || (target["kind"]!="window" && target["kind"]!="screen"))return false;
    m_target=target;m_id=target["handle"].toInteger();m_pid=target["pid"].toInt();m_birth=birth;m_scale=target["scale"].toDouble();m_keys.clear();return true;
  }
  bool verify(const QRect &geometry,const std::optional<QPoint> &point,bool keyboard)override {
    if(!available())return false;
    CGRect rect=CGRectNull;
    const pid_t foreground=NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
    uint32_t focusedWindow=0;CGRect focusedBounds=CGRectNull;
    if(keyboard){NSArray *windows=CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly|kCGWindowListExcludeDesktopElements,kCGNullWindow));
      for(NSDictionary *window in windows)if([window[(id)kCGWindowOwnerPID] intValue]==foreground && [window[(id)kCGWindowLayer] intValue]==0){
        if(CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)window[(id)kCGWindowBounds],&focusedBounds))focusedWindow=[window[(id)kCGWindowNumber] unsignedIntValue];break;}}
    if(m_target["kind"]=="window") {
      if(!m_birth || processBirth(m_pid)!=m_birth)return false;
      NSArray *windows=CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly|kCGWindowListExcludeDesktopElements,kCGNullWindow));
      bool hit=!point;
      const CGPoint position=point?CGPointMake(point->x()/m_scale,point->y()/m_scale):CGPointZero;
      for(NSDictionary *window in windows){CGRect bounds{};if(!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)window[(id)kCGWindowBounds],&bounds))continue;
        const auto windowId=[window[(id)kCGWindowNumber] unsignedIntValue];
        if(point && !hit && CGRectContainsPoint(bounds,position) && [window[(id)kCGWindowAlpha] doubleValue]>0){if(windowId!=m_id)return false;hit=true;}
        if(windowId==m_id){if([window[(id)kCGWindowOwnerPID] intValue]!=m_pid)return false;rect=bounds;}
      }
      if(!hit || (keyboard && (foreground!=m_pid || focusedWindow!=m_id)))return false;
    }else {
      if(!CGDisplayIsActive(m_id))return false;rect=CGDisplayBounds(m_id);
      if(keyboard && (!focusedWindow || !CGRectContainsPoint(rect,CGPointMake(CGRectGetMidX(focusedBounds),CGRectGetMidY(focusedBounds)))))return false;
    }
    return !CGRectIsNull(rect) && physical(rect,m_scale)==geometry;
  }
  bool idle()const override {for(int key=0;key<128;++key)if(CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState,key))return false;for(int button=0;button<3;++button)if(CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState,CGMouseButton(button)))return false;return true;}
  bool physicalInput()override{return std::exchange(m_physical,false);}
  bool inject(const QJsonObject &event)override {
    if(!unlocked())return false;
    CGEventRef native=nullptr;const auto kind=event["kind"].toString();const int code=event["code"].toInt();const bool down=event["down"].toBool();
    if(kind=="key" || kind=="repeat") {
      const auto key=m_keys.contains(code)?std::optional<CGKeyCode>(m_keys.value(code)):keyCode(code);if(!key)return false;
      native=CGEventCreateKeyboardEvent(nullptr,*key,down);if(!native)return false;
      if(kind=="repeat")CGEventSetIntegerValueField(native,kCGKeyboardEventAutorepeat,1);
      if(down)m_keys.insert(code,*key);else m_keys.remove(code);
      CGEventFlags flags=0;for(auto it=m_keys.cbegin();it!=m_keys.cend();++it){const int v=it.key();if(v==160 || v==161)flags|=kCGEventFlagMaskShift;if(v==162 || v==163)flags|=kCGEventFlagMaskControl;if(v==164 || v==165)flags|=kCGEventFlagMaskAlternate;if(v==91 || v==92)flags|=kCGEventFlagMaskCommand;}
      CGEventSetFlags(native,flags);
    }else {
      CGPoint point=CGPointMake(event["x"].toDouble()/m_scale,event["y"].toDouble()/m_scale);
      if(event["release"].toBool()){auto current=CGEventCreate(nullptr);if(!current)return false;point=CGEventGetLocation(current);CFRelease(current);}
      if(kind=="wheel")native=CGEventCreateScrollWheelEvent(nullptr,kCGScrollEventUnitPixel,2,event["dy"].toInt(),-event["dx"].toInt());
      else {const CGMouseButton button=code==2?kCGMouseButtonRight:code==3?kCGMouseButtonCenter:kCGMouseButtonLeft;
        CGEventType type=kCGEventMouseMoved;
        if(kind=="button"){if(code<1 || code>3)return false;type=code==1?(down?kCGEventLeftMouseDown:kCGEventLeftMouseUp):code==2?(down?kCGEventRightMouseDown:kCGEventRightMouseUp):(down?kCGEventOtherMouseDown:kCGEventOtherMouseUp);if(down)m_buttons.insert(code);else m_buttons.remove(code);}
        else if(m_buttons.contains(1))type=kCGEventLeftMouseDragged;else if(m_buttons.contains(2))type=kCGEventRightMouseDragged;else if(m_buttons.contains(3))type=kCGEventOtherMouseDragged;
        native=CGEventCreateMouseEvent(nullptr,type,point,button);
      }
      if(native)CGEventSetLocation(native,point);
    }
    if(!native)return false;CGEventSetIntegerValueField(native,kCGEventSourceUserData,controlInputMarker);CGEventPost(kCGHIDEventTap,native);CFRelease(native);return true;
  }
private:
  static CGEventRef observed(CGEventTapProxy,CGEventType type,CGEventRef event,void *data) {
    auto self=static_cast<MacControl *>(data);
    if(type==kCGEventTapDisabledByTimeout || type==kCGEventTapDisabledByUserInput){self->m_physical=true;if(self->physicalPriority)self->physicalPriority();return event;}
    if(CGEventGetIntegerValueField(event,kCGEventSourceUserData)!=controlInputMarker){self->m_physical=true;if(self->physicalPriority)self->physicalPriority();}
    return event;
  }
  CFMachPortRef m_tap=nullptr;CFRunLoopSourceRef m_source=nullptr;CFRunLoopRef m_runloop=nullptr;QTimer m_dispatch;
  QJsonObject m_target;uint32_t m_id=0;pid_t m_pid=0;quint64 m_birth=0;double m_scale=0;bool m_physical=false;
  QHash<int,CGKeyCode> m_keys;QSet<int> m_buttons;
};
}
std::unique_ptr<NativeControl> createNativeControl(){return std::make_unique<MacControl>();}
}
