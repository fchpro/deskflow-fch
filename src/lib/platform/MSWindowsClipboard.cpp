/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsClipboard.h"

#include "base/Log.h"
#include "platform/MSWindowsClipboardBitmapConverter.h"
#include "platform/MSWindowsClipboardFacade.h"
#include "platform/MSWindowsClipboardHTMLConverter.h"
#include "platform/MSWindowsClipboardUTF16Converter.h"

#include <QScopeGuard>

namespace {
HANDLE legacyV5Bitmap(HANDLE bitmap)
{
  const auto size = GlobalSize(bitmap);
  if (size < sizeof(BITMAPV5HEADER))
    return nullptr;
  const auto *source = static_cast<const char *>(GlobalLock(bitmap));
  if (!source)
    return nullptr;
  const auto unlock = qScopeGuard([&] { GlobalUnlock(bitmap); });
  BITMAPV5HEADER header{};
  memcpy(&header, source, sizeof(header));
  // This is a lossless re-layout of the standard sRGB BGRA screenshot case.
  // Other masks, palettes and colour profiles require their original V5 data.
  if (header.bV5Size != sizeof(header) || header.bV5Width <= 0 || header.bV5Height == 0 ||
      header.bV5Planes != 1 || header.bV5BitCount != 32 || header.bV5Compression != BI_BITFIELDS ||
      header.bV5RedMask != 0x00ff0000 || header.bV5GreenMask != 0x0000ff00 ||
      header.bV5BlueMask != 0x000000ff || header.bV5AlphaMask != 0xff000000 ||
      header.bV5CSType != LCS_sRGB || header.bV5ClrUsed != 0 || header.bV5ProfileData != 0 ||
      header.bV5ProfileSize != 0)
    return nullptr;
  const auto height = header.bV5Height < 0 ? -int64_t(header.bV5Height) : int64_t(header.bV5Height);
  const auto pixelsSize = uint64_t(header.bV5Width) * uint64_t(height) * 4;
  if (pixelsSize + sizeof(header) != size)
    return nullptr;

  const auto result = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixelsSize);
  auto *target = result ? static_cast<char *>(GlobalLock(result)) : nullptr;
  if (!target) {
    if (result)
      GlobalFree(result);
    LOG_WARN("failed to allocate standard clipboard bitmap");
    return nullptr;
  }
  BITMAPINFOHEADER legacy{};
  memcpy(&legacy, &header, sizeof(legacy));
  legacy.biSize = sizeof(legacy);
  legacy.biCompression = BI_RGB;
  memcpy(target, &legacy, sizeof(legacy));
  memcpy(target + sizeof(legacy), source + sizeof(header), pixelsSize);
  GlobalUnlock(result);
  return result;
}

HANDLE duplicateV5Bitmap(HANDLE bitmap)
{
  const auto size = GlobalSize(bitmap);
  if (size < sizeof(BITMAPV5HEADER))
    return nullptr;

  const auto *source = static_cast<const char *>(GlobalLock(bitmap));
  if (!source)
    return nullptr;

  DWORD headerSize = 0;
  memcpy(&headerSize, source, sizeof(headerSize));
  HANDLE result = nullptr;
  if (headerSize == sizeof(BITMAPV5HEADER)) {
    result = GlobalAlloc(GMEM_MOVEABLE, size);
    auto *target = result ? GlobalLock(result) : nullptr;
    if (target) {
      memcpy(target, source, size);
      GlobalUnlock(result);
    } else {
      if (result)
        GlobalFree(result);
      result = nullptr;
      LOG_WARN("failed to allocate V5 clipboard bitmap");
    }
  }
  GlobalUnlock(bitmap);
  return result;
}
} // namespace

//
// MSWindowsClipboard
//

UINT MSWindowsClipboard::s_ownershipFormat = 0;

MSWindowsClipboard::MSWindowsClipboard(HWND window)
    : m_window(window),
      m_time(0),
      m_facade(new MSWindowsClipboardFacade()),
      m_deleteFacade(true)
{
  // add converters, most desired first
  m_converters.push_back(new MSWindowsClipboardUTF16Converter);
  m_converters.push_back(new MSWindowsClipboardBitmapConverter);
  m_converters.push_back(new MSWindowsClipboardHTMLConverter);
}

MSWindowsClipboard::~MSWindowsClipboard()
{
  clearConverters();

  // dependency injection causes confusion over ownership, so we need
  // logic to decide whether or not we delete the facade. there must
  // be a more elegant way of doing this.
  if (m_deleteFacade)
    delete m_facade;
}

void MSWindowsClipboard::setFacade(IMSWindowsClipboardFacade &facade)
{
  delete m_facade;
  m_facade = &facade;
  m_deleteFacade = false;
}

bool MSWindowsClipboard::emptyUnowned()
{
  LOG_DEBUG("empty clipboard");

  // empty the clipboard (and take ownership)
  if (!EmptyClipboard()) {
    // unable to cause this in integ tests, but this error has never
    // actually been reported by users.
    LOG_WARN("failed to grab clipboard");
    return false;
  }

  return true;
}

bool MSWindowsClipboard::empty()
{
  if (!emptyUnowned()) {
    return false;
  }

  // mark clipboard as being owned by deskflow
  HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE | GMEM_DDESHARE, 1);
  if (nullptr == SetClipboardData(getOwnershipFormat(), data)) {
    LOG_WARN("failed to set clipboard data");
    GlobalFree(data);
    return false;
  }

  return true;
}

void MSWindowsClipboard::add(Format format, const std::string &data)
{
  // exit early if there is no data to prevent spurious "failed to convert clipboard data" errors
  if (data.empty()) {
    LOG_DEBUG("not adding 0 bytes to clipboard format: %d", format);
    return;
  }
  bool isSucceeded = false;
  // convert data to win32 form
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    IMSWindowsClipboardConverter *converter = *index;

    // skip converters for other formats
    if (converter->getFormat() == format) {
      HANDLE win32Data = converter->fromIClipboard(data);
      if (win32Data != nullptr) {
        LOG_DEBUG("add %d bytes to clipboard format: %d", data.size(), format);
        // Windows advertises CF_DIBV5 for CF_DIB but cannot synthesize it
        // from a macOS V5 BITFIELDS DIB. Publish the matching format explicitly,
        // retaining CF_DIB for existing consumers and all V5 pixels/colour data.
        // Inspect the converted handle so repaired legacy DIBs stay CF_DIB only.
        HANDLE v5Data = format == Format::Bitmap ? duplicateV5Bitmap(win32Data) : nullptr;
        if (v5Data) {
          // CF_DIB readers can assume BITFIELDS masks follow the header;
          // V5 embeds them instead. Provide a standard INFOHEADER layout while
          // keeping the full original colour/alpha representation in CF_DIBV5.
          if (HANDLE legacyData = legacyV5Bitmap(win32Data)) {
            GlobalFree(win32Data);
            win32Data = legacyData;
          }
        }
        m_facade->write(win32Data, converter->getWin32Format());
        if (v5Data)
          m_facade->write(v5Data, CF_DIBV5);
        isSucceeded = true;
        break;
      } else {
        LOG_DEBUG("failed to convert clipboard data to platform format");
      }
    }
  }

  if (!isSucceeded) {
    LOG_DEBUG("missed clipboard data convert for format: %d", format);
  }
}

bool MSWindowsClipboard::open(Time time) const
{
  LOG_DEBUG("open clipboard");

  // The clipboard is a global mutex on Windows. We aren't always going to
  // get the lock on the first try, so try a few times before giving up.
  // Based on Chromium's ScopedClipboard::Acquire() retry loop.
  static const int kMaxRetries = 5;
  static const int kRetryDelayMs = 5;

  for (int i = 0; i < kMaxRetries; ++i) {
    if (OpenClipboard(m_window)) {
      std::scoped_lock lock{m_mutex};
      m_time = time;
      return true;
    }

    if (i < kMaxRetries - 1) {
      LOG_DEBUG("failed to open clipboard (attempt %d/%d, error=%d), retrying", i + 1, kMaxRetries, GetLastError());
      Sleep(kRetryDelayMs);
    }
  }

  LOG_WARN("failed to open clipboard after %d attempts: %d", kMaxRetries, GetLastError());
  return false;
}

void MSWindowsClipboard::close() const
{
  LOG_DEBUG("close clipboard");
  CloseClipboard();
}

IClipboard::Time MSWindowsClipboard::getTime() const
{
  std::scoped_lock lock{m_mutex};
  return m_time;
}

bool MSWindowsClipboard::has(Format format) const
{
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    IMSWindowsClipboardConverter *converter = *index;
    if (converter->getFormat() == format) {
      if (IsClipboardFormatAvailable(converter->getWin32Format())) {
        return true;
      }
    }
  }
  return false;
}

std::string MSWindowsClipboard::get(Format format) const
{
  // find the converter for the first clipboard format we can handle
  IMSWindowsClipboardConverter *converter = nullptr;
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {

    converter = *index;
    if (converter->getFormat() == format) {
      break;
    }
    converter = nullptr;
  }

  // if no converter then we don't recognize any formats
  if (converter == nullptr) {
    LOG_WARN("no converter for format %d", format);
    return std::string();
  }

  // get a handle to the clipboard data
  HANDLE win32Data = GetClipboardData(converter->getWin32Format());
  if (win32Data == nullptr) {
    // nb: can't cause this using integ tests; this is only caused when
    // the selected converter returns an invalid format -- which you
    // cannot cause using public functions.
    return std::string();
  }

  // convert
  return converter->toIClipboard(win32Data);
}

void MSWindowsClipboard::clearConverters()
{
  for (ConverterList::iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    delete *index;
  }
  m_converters.clear();
}

bool MSWindowsClipboard::isOwnedByDeskflow()
{
  // create ownership format if we haven't yet
  if (s_ownershipFormat == 0) {
    s_ownershipFormat = RegisterClipboardFormat(TEXT("Deskflow Ownership"));
  }
  return (IsClipboardFormatAvailable(getOwnershipFormat()) != 0);
}

UINT MSWindowsClipboard::getOwnershipFormat()
{
  // create ownership format if we haven't yet
  if (s_ownershipFormat == 0) {
    s_ownershipFormat = RegisterClipboardFormat(TEXT("Deskflow Ownership"));
  }

  // return the format
  return s_ownershipFormat;
}
