// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "base/Log.h"
#include "platform/MSWindowsClipboard.h"

#include <QScopeGuard>
#include <QTest>
#include <QtEndian>

class MSWindowsClipboardNativeBitmapTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase()
  {
    m_window = CreateWindowExW(0, L"STATIC", L"Clipboard bitmap test", 0, 0, 0, 1, 1, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    QVERIFY(m_window != nullptr);
  }

  void cleanupTestCase()
  {
    if (OpenClipboard(m_window)) {
      EmptyClipboard();
      CloseClipboard();
    }
    DestroyWindow(m_window);
  }

  void nativeReaders_data()
  {
    QTest::addColumn<bool>("v5");
    QTest::addColumn<bool>("malformed");
    QTest::newRow("mac-v5-bgra") << true << false;
    QTest::newRow("windows-infoheader") << false << false;
    QTest::newRow("legacy-mac-truncated-header") << false << true;
  }

  void nativeReaders()
  {
    QFETCH(bool, v5);
    QFETCH(bool, malformed);
    const size_t headerSize = v5 ? sizeof(BITMAPV5HEADER) : sizeof(BITMAPINFOHEADER);
    std::string dib(headerSize + 16, '\0');
    auto *raw = reinterpret_cast<quint8 *>(dib.data());
    qToLittleEndian<quint32>(v5 || malformed ? sizeof(BITMAPV5HEADER) : headerSize, raw);
    qToLittleEndian<qint32>(2, raw + 4);
    qToLittleEndian<qint32>(-2, raw + 8);
    qToLittleEndian<quint16>(1, raw + 12);
    qToLittleEndian<quint16>(32, raw + 14);
    qToLittleEndian<quint32>(v5 || malformed ? BI_BITFIELDS : BI_RGB, raw + 16);
    qToLittleEndian<quint32>(16, raw + 20);
    if (v5) {
      qToLittleEndian<quint32>(0x00ff0000, raw + 40);
      qToLittleEndian<quint32>(0x0000ff00, raw + 44);
      qToLittleEndian<quint32>(0x000000ff, raw + 48);
      qToLittleEndian<quint32>(0xff000000, raw + 52);
      qToLittleEndian<quint32>(LCS_sRGB, raw + 56);
    }
    const unsigned char pixels[] = {0x11, 0x22, 0x33, 0xff, 0x44, 0x55, 0x66, 0xff,
                                    0x77, 0x88, 0x99, 0xff, 0xaa, 0xbb, 0xcc, 0xff};
    memcpy(raw + headerSize, pixels, sizeof(pixels));

    MSWindowsClipboard clipboard(m_window);
    QVERIFY(clipboard.open(0));
    auto close = qScopeGuard([&] { clipboard.close(); });
    QVERIFY(clipboard.empty());
    clipboard.add(IClipboard::Format::Bitmap, dib);
    clipboard.close(); // Windows synthesis is established after publication.
    QVERIFY(clipboard.open(0));

    const auto v5Handle = GetClipboardData(CF_DIBV5);
    QVERIFY2(v5Handle != nullptr, "Native CF_DIBV5 retrieval must work, not merely advertise availability");
    if (v5) {
      const auto *bytes = static_cast<const char *>(GlobalLock(v5Handle));
      QVERIFY(bytes != nullptr);
      const std::string copy(bytes, GlobalSize(v5Handle));
      GlobalUnlock(v5Handle);
      QCOMPARE(copy, dib); // Preserve header, masks, alpha and exact pixel bytes.
    }
    QVERIFY(GetClipboardData(CF_DIB) != nullptr);
    const auto bitmap = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
    QVERIFY2(bitmap != nullptr, "Native bitmap consumers such as OLE must receive an image");
    BITMAP object{};
    QVERIFY(GetObjectW(bitmap, sizeof(object), &object) != 0);
    QCOMPARE(object.bmWidth, LONG(2));
    QCOMPARE(object.bmHeight, LONG(2));

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = 2;
    info.bmiHeader.biHeight = -2;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    unsigned char decoded[16]{};
    HDC dc = GetDC(nullptr);
    QVERIFY(dc != nullptr);
    const auto release = qScopeGuard([&] { ReleaseDC(nullptr, dc); });
    QCOMPARE(GetDIBits(dc, bitmap, 0, 2, decoded, &info, DIB_RGB_COLORS), 2);
    for (size_t pixel = 0; pixel < 4; ++pixel)
      for (size_t channel = 0; channel < 3; ++channel)
        QCOMPARE(decoded[pixel * 4 + channel], pixels[pixel * 4 + channel]);
  }

private:
  Log m_log;
  HWND m_window = nullptr;
};

QTEST_MAIN(MSWindowsClipboardNativeBitmapTests)
#include "MSWindowsClipboardNativeBitmapTests.moc"
