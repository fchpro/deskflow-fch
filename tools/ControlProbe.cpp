// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/Control.h"
#include "../src/unittests/streaming/ControlTlsFixture.h"
#include "base/Log.h"
#include <QApplication>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QTest>
#include <QTimer>
#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>
using namespace deskflow::streaming;
namespace {
// Fixture-only physical observer: abort even if local activity precedes the lease.
struct ObservedNative final:NativeControl {
  std::unique_ptr<NativeControl> native;bool &aborted;
  ObservedNative(std::unique_ptr<NativeControl> value,bool &flag):native(std::move(value)),aborted(flag){native->physicalPriority=[this]{aborted=true;if(physicalPriority)physicalPriority();};}
  bool available()const override{return native->available();}bool bind(const QJsonObject &target)override{return native->bind(target);}
  bool verify(const QRect &rect,const std::optional<QPoint> &point,bool key)override{return !aborted && native->verify(rect,point,key);}
  bool idle()const override{return !aborted && native->idle();}bool inject(const QJsonObject &event)override{return native->inject(event);}
  bool physicalInput()override{return native->physicalInput();}
};
}
int main(int argc,char **argv) {
  QApplication app(argc,argv);Log coreLog;const auto args=app.arguments();
  auto arg=[&](const QString &name){const int i=args.indexOf(name);return i>=0 && i+1<args.size()?args[i+1]:QString{};};
  if(!args.contains("--fixture") || arg("--output").isEmpty() || arg("--log").isEmpty())return 2;
  QFile file(arg("--log"));if(!file.open(QIODevice::WriteOnly|QIODevice::Text))return 2;QTextStream log(&file);
  if(args.contains("--headed-authorized") || args.contains("--verify-core-guard")) {
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);if(snapshot==INVALID_HANDLE_VALUE){log<<"Cannot verify input-sharing processes; no fixture or input.\n";return 3;}
    PROCESSENTRY32W entry{};entry.dwSize=sizeof(entry);bool running=false;
    for(BOOL more=Process32FirstW(snapshot,&entry);more;more=Process32NextW(snapshot,&entry)) {
      const auto name=QString::fromWCharArray(entry.szExeFile).toLower();
      if(QStringList{"deskflow.exe","deskflow-core.exe","deskflow-daemon.exe"}.contains(name)) {running=true;log<<"Input-sharing prerequisite: close "<<name<<" PID "<<entry.th32ProcessID<<"\n";}
    }
    CloseHandle(snapshot);if(running){log<<"REFUSED: existing Deskflow process; no fixture shown and zero injection attempts.\n";return 3;}
  }
  auto native=createNativeControl();
  log<<"Native control availability: "<<native->available()<<"\n";log.flush();
  if(!args.contains("--headed-authorized")) {
    log<<"No headed authorization: no fixture window shown and zero injection attempts.\n";
    log<<"Unselected desktop rejection: "<<(!native->available()?"PASS":"FAIL (run through hidden runner)")<<"\n";
    return native->available()?3:0;
  }
  if(!native->available()){log<<"Active unlocked desktop unavailable; no injection attempted.\n";return 3;}
  bool physicalAbort=false;native=std::make_unique<ObservedNative>(std::move(native),physicalAbort);
  std::function<void()> release;
  QTimer watchdog;watchdog.setSingleShot(true);QObject::connect(&watchdog,&QTimer::timeout,&app,[&]{log<<"Fixture exceeded 20-second total deadline\n";log.flush();if(release)release();std::exit(5);});watchdog.start(20000);
  QWidget fixture;fixture.setWindowTitle("Deskflow owned input acceptance fixture");fixture.resize(640,400);
  auto *layout=new QVBoxLayout(&fixture);auto *title=new QLabel("OWNED FIXTURE - native input acceptance",&fixture);layout->addWidget(title);
  auto *button=new QPushButton("Click target: waiting",&fixture);button->setMinimumHeight(70);layout->addWidget(button);
  auto *edit=new QLineEdit(&fixture);edit->setPlaceholderText("Native typing target");layout->addWidget(edit);
  auto *slider=new QSlider(Qt::Horizontal,&fixture);slider->setRange(0,100);slider->setValue(0);layout->addWidget(slider);
  auto *wheel=new QSpinBox(&fixture);wheel->setRange(0,100);wheel->setValue(50);layout->addWidget(wheel);
  auto *status=new QLabel("No input delivered",&fixture);layout->addWidget(status);
  int clicks=0;QObject::connect(button,&QPushButton::clicked,&fixture,[&]{++clicks;button->setText("Click target: activated");});
  fixture.show();fixture.raise();fixture.activateWindow();
  if(!QTest::qWaitFor([&]{return GetForegroundWindow()==reinterpret_cast<HWND>(fixture.winId());},3000)){log<<"Owned foreground signal failed; no injection attempted.\n";return 4;}
  const auto hwnd=reinterpret_cast<HWND>(fixture.winId());RECT r{};DwmGetWindowAttribute(hwnd,DWMWA_EXTENDED_FRAME_BOUNDS,&r,sizeof(r));
  FILETIME a{},b{},c{},d{};GetProcessTimes(GetCurrentProcess(),&a,&b,&c,&d);
  control_fixture::ChannelFixture channel(std::move(native));
  if(physicalAbort){log<<"Physical input observed; fixture aborted before control grant.\n";return 4;}
  QTimer heartbeat;QObject::connect(&heartbeat,&QTimer::timeout,&app,[&]{
    if(physicalAbort){log<<"Physical input observed; fixture abort.\n";log.flush();if(release)release();std::exit(4);}
    channel.local.send(message("Heartbeat",channel.identity()));channel.remote->send(message("Heartbeat",channel.identity()));
  });heartbeat.start(250);
  const QString session=channel.session,source=channel.source,lease=channel.lease;
  release=[&]{channel.service.reset();};
  const QJsonObject target{{"session",session},{"source",source},{"kind","window"},{"handle",QString::number(quintptr(hwnd),16)},
    {"pid",qint64(GetCurrentProcessId())},{"birth",QString::number((quint64(a.dwHighDateTime)<<32)|a.dwLowDateTime,16)},{"device",""}};
  QJsonObject geometry{{"session",session},{"source",source},{"frame",1},{"epoch",0},{"geometry",1},
    {"x",int(r.left)},{"y",int(r.top)},{"width",int(r.right-r.left)},{"height",int(r.bottom-r.top)},{"valid",true}};
  channel.target(target,geometry);
  struct RemoteInput {
    control_fixture::ChannelFixture &channel;
    bool &aborted;
    bool input(const QJsonObject &event,qint64){return !aborted && channel.remote->send(message("ControlInput",event));}
    void revoke(){channel.local.send(message("RevokeControl",channel.identity()));}
  } owner{channel,physicalAbort};
  auto screenPoint=[&](QWidget *widget,const QPoint &point){
    const auto local=widget->mapTo(&fixture,point);POINT origin{};ClientToScreen(hwnd,&origin);
    return QPoint(origin.x+qRound(local.x()*fixture.devicePixelRatioF()),origin.y+qRound(local.y()*fixture.devicePixelRatioF()));
  };
  int sequence=0;
  auto event=[&](QJsonObject e){e["session"]=session;e["source"]=source;e["lease"]=lease;e["sequence"]=++sequence;e["frame"]=1;e["epoch"]=0;e["geometry"]=1;return e;};
  const auto point=screenPoint(button,button->rect().center());
  auto click=event({{"kind","button"},{"x",point.x()},{"y",point.y()},{"code",1},{"down",true}});
  owner.input(click,0);
  const bool viewOnly=QTest::qWaitFor([&]{return control_fixture::ChannelFixture::has(channel.remoteMessages,"Error");},3000);
  log<<"Authenticated loopback TLS/private-IPC control rejection observed: "<<viewOnly<<"\n";
  fixture.grab().save(arg("--output")+"/native-before.png");
  if(!viewOnly){log<<"Viewing-only broker rejection failed\n";return 4;}
  channel.grant();
  const bool down=owner.input(click,1);click["down"]=false;click["sequence"]=++sequence;const bool up=owner.input(click,2);
  if(!down || !up || !QTest::qWaitFor([&]{return clicks==1;},3000)){log<<"Native click signal failed\n";return 4;}
  const auto typing=screenPoint(edit,edit->rect().center());
  for(bool held:{true,false})if(!owner.input(event({{"kind","button"},{"x",typing.x()},{"y",typing.y()},{"code",1},{"down",held}}),3)){log<<"Typing focus click failed\n";return 4;}
  if(!QTest::qWaitFor([&]{return edit->hasFocus();},3000)){log<<"Owned typing focus signal failed\n";return 4;}
  for(const auto letter:QString("DESKFLOW"))for(bool held:{true,false})if(!owner.input(event({{"kind","key"},{"code",letter.unicode()},{"down",held}}),4)){log<<"Native typing injection rejected\n";return 4;}
  if(!QTest::qWaitFor([&]{return edit->text().compare("deskflow",Qt::CaseInsensitive)==0;},3000)){log<<"Native text signal failed: "<<edit->text()<<"\n";return 4;}
  const auto dragStart=screenPoint(slider,QPoint(8,slider->height()/2)),dragEnd=screenPoint(slider,QPoint(slider->width()-12,slider->height()/2));
  if(!owner.input(event({{"kind","button"},{"x",dragStart.x()},{"y",dragStart.y()},{"code",1},{"down",true}}),5) ||
     !owner.input(event({{"kind","move"},{"x",dragEnd.x()},{"y",dragEnd.y()}}),6) ||
     !owner.input(event({{"kind","button"},{"x",dragEnd.x()},{"y",dragEnd.y()},{"code",1},{"down",false}}),7) ||
     !QTest::qWaitFor([&]{return slider->value()>80;},3000)){log<<"Native drag signal failed\n";return 4;}
  const auto scroll=screenPoint(wheel,wheel->rect().center());
  if(!owner.input(event({{"kind","wheel"},{"x",scroll.x()},{"y",scroll.y()},{"dx",0},{"dy",120}}),8) ||
     !QTest::qWaitFor([&]{return wheel->value()==51;},3000)){log<<"Native wheel signal failed\n";return 4;}
  if(!owner.input(event({{"kind","key"},{"code",162},{"down",true}}),9) ||
     !QTest::qWaitFor([&]{return (GetAsyncKeyState(VK_LCONTROL)&0x8000)!=0;},3000)){log<<"Control key was not observed held\n";return 4;}
  log<<"Held Ctrl observed before revoke: true\n";owner.revoke();
  if(!QTest::qWaitFor([&]{return !(GetAsyncKeyState(VK_LCONTROL)&0x8000);},3000)){log<<"Control key release failed\n";return 4;}
  status->setText("Native click / typing / wheel / drag passed; held Ctrl released");
  fixture.grab().save(arg("--output")+"/native-after.png");
  log<<"Owned HWND "<<target["handle"].toString()<<" PID "<<GetCurrentProcessId()<<" geometry "<<geometry["x"].toInt()<<","<<geometry["y"].toInt()<<" "<<geometry["width"].toInt()<<"x"<<geometry["height"].toInt()<<"\n";
  log<<"PASS: view-only rejected; clicks="<<clicks<<" actual text="<<edit->text()<<" drag slider="<<slider->value()<<" wheel value="<<wheel->value()<<" held Ctrl released="<<!(GetAsyncKeyState(VK_LCONTROL)&0x8000)<<"\n";
  return 0;
}
