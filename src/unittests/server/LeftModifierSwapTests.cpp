/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Fakhri Chahed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/LeftModifierSwap.h"
#include "arch/Arch.h"
#include "base/Log.h"
#include "deskflow/IKeyState.h"
#include <QTest>
#include <memory>
#ifdef Q_OS_WIN
#include "platform/MSWindowsKeyState.h"
#include "../deskflow/MockEventQueue.h"

class CapturedKeyEvents : public MockEventQueue
{
public:
  std::unique_ptr<IKeyState::KeyInfo> last;
  void addEvent(Event &&event) override
  {
    last.reset(IKeyState::KeyInfo::alloc(*static_cast<IKeyState::KeyInfo *>(event.getData())));
  }
};
#endif

class LeftModifierSwapTests : public QObject
{
  Q_OBJECT
  Arch m_arch;
  Log m_log;
private Q_SLOTS:
  void initTestCase() { m_arch.init(); }
  void keys_data()
  {
    QTest::addColumn<uint32_t>("input");
    QTest::addColumn<uint32_t>("expected");
    QTest::newRow("left-control") << kKeyControl_L << kKeySuper_L;
    QTest::newRow("left-windows") << kKeySuper_L << kKeyControl_L;
    QTest::newRow("right-control") << kKeyControl_R << kKeyControl_R;
    QTest::newRow("right-windows") << kKeySuper_R << kKeySuper_R;
    QTest::newRow("letter") << uint32_t('c') << uint32_t('c');
    QTest::newRow("release-by-button") << kKeyNone << kKeyNone;
  }
  void keys()
  {
    QFETCH(uint32_t, input);
    QFETCH(uint32_t, expected);
    QCOMPARE(LeftModifierSwap("mac").map("mac", false, input, 0, {}).first, expected);
  }
  void masks_data()
  {
    QTest::addColumn<uint32_t>("left");
    QTest::addColumn<uint32_t>("right");
    QTest::addColumn<uint32_t>("input");
    QTest::addColumn<uint32_t>("expected");
    constexpr auto C = KeyModifierControl;
    constexpr auto W = KeyModifierSuper;
    const uint32_t physical[] = {0, C, W, C | W};
    // Rows = left side; columns = right side. Explicit shortcut expectations.
    const uint32_t output[4][4] = {{0, C, W, C | W}, {W, C | W, W, C | W},
                                  {C, C, C | W, C | W}, {C | W, C | W, C | W, C | W}};
    constexpr auto extra = KeyModifierShift | KeyModifierAlt | KeyModifierCapsLock;
    for (int l = 0; l < 4; ++l) {
      for (int r = 0; r < 4; ++r) {
        auto name = QString("left-%1-right-%2").arg(l).arg(r).toLatin1();
        QTest::newRow(name) << physical[l] << physical[r] << (physical[l] | physical[r] | extra)
                            << (output[l][r] | extra);
      }
    }
    QTest::newRow("altgr-suppressed-control") << C << uint32_t(0) << KeyModifierAltGr << KeyModifierAltGr;
    QTest::newRow("released-control") << uint32_t(0) << uint32_t(0) << uint32_t(0) << uint32_t(0);
    QTest::newRow("synthetic-control") << uint32_t(0) << uint32_t(0) << C << C;
    QTest::newRow("synthetic-windows") << uint32_t(0) << uint32_t(0) << W << W;
  }
  void masks()
  {
    QFETCH(uint32_t, left);
    QFETCH(uint32_t, right);
    QFETCH(uint32_t, input);
    QFETCH(uint32_t, expected);
    QCOMPARE(LeftModifierSwap("mac").map("mac", false, 'c', input, {left, right}).second, expected);
  }
  void scope_data()
  {
    QTest::addColumn<QString>("target");
    QTest::addColumn<QString>("destination");
    QTest::addColumn<bool>("primary");
    QTest::newRow("local-windows") << "mac" << "mac" << true;
    QTest::newRow("other-client") << "mac" << "other" << false;
    QTest::newRow("disabled") << "" << "mac" << false;
  }
  void scope()
  {
    QFETCH(QString, target);
    QFETCH(QString, destination);
    QFETCH(bool, primary);
    const auto actual = LeftModifierSwap(target.toStdString()).map(
        destination.toStdString(), primary, kKeyControl_L, KeyModifierControl, {KeyModifierControl, 0});
    QCOMPARE(actual.first, kKeyControl_L);
    QCOMPARE(actual.second, KeyModifierControl);
  }
#ifdef Q_OS_WIN
  void windowsCapture_data()
  {
    QTest::addColumn<bool>("press");
    QTest::addColumn<bool>("repeat");
    QTest::addColumn<uint32_t>("virtualKey");
    QTest::addColumn<uint32_t>("left");
    QTest::addColumn<uint32_t>("right");
    for (int action = 0; action < 3; ++action) {
      const uint32_t keys[] = {VK_LCONTROL, VK_RCONTROL, VK_LWIN, VK_RWIN};
      const uint32_t l[] = {KeyModifierControl, 0, KeyModifierSuper, 0};
      const uint32_t r[] = {0, KeyModifierControl, 0, KeyModifierSuper};
      for (int side = 0; side < 4; ++side) {
        auto name = QString("action-%1-side-%2").arg(action).arg(side).toLatin1();
        QTest::newRow(name) << (action != 1) << (action == 2) << keys[side] << l[side] << r[side];
      }
    }
  }
  void windowsCapture()
  {
    QFETCH(bool, press);
    QFETCH(bool, repeat);
    QFETCH(uint32_t, virtualKey);
    QFETCH(uint32_t, left);
    QFETCH(uint32_t, right);
    CapturedKeyEvents events;
    MSWindowsKeyState state(nullptr, nullptr, &events, {"en"}, false);
    state.updateKeyMap();
    auto button = state.virtualKeyToButton(virtualKey);
    state.onKey(button, true, left | right);
    state.sendKeyEvent(nullptr, press, repeat, 'c', left | right, 1, 46);
    // Physical state may change before the server consumes the queued event.
    state.onKey(button, false, 0);
    QVERIFY(events.last != nullptr);
    QCOMPARE(events.last->m_modifierSides.left, left);
    QCOMPARE(events.last->m_modifierSides.right, right);
  }
#endif
  void eventEqualityIncludesSides_data()
  {
    QTest::addColumn<bool>("left");
    QTest::newRow("left") << true;
    QTest::newRow("right") << false;
  }
  void eventEqualityIncludesSides()
  {
    QFETCH(bool, left);
    std::unique_ptr<IKeyState::KeyInfo> a(IKeyState::KeyInfo::alloc('c', KeyModifierControl, 46, 1));
    std::unique_ptr<IKeyState::KeyInfo> b(IKeyState::KeyInfo::alloc(*a));
    if (left) b->m_modifierSides.left = KeyModifierControl;
    else b->m_modifierSides.right = KeyModifierControl;
    QVERIFY(!IKeyState::KeyInfo::equal(a.get(), b.get()));
  }
  void copiedEventPreservesSides()
  {
    std::unique_ptr<IKeyState::KeyInfo> source(IKeyState::KeyInfo::alloc('c', KeyModifierControl, 46, 1));
    source->m_modifierSides = {KeyModifierControl, KeyModifierSuper};
    std::unique_ptr<IKeyState::KeyInfo> copy(IKeyState::KeyInfo::alloc(*source));
    QCOMPARE(copy->m_modifierSides.left, KeyModifierControl);
    QCOMPARE(copy->m_modifierSides.right, KeyModifierSuper);
  }
};

QTEST_GUILESS_MAIN(LeftModifierSwapTests)
#include "LeftModifierSwapTests.moc"
