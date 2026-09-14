// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "Audio.h"
#include <QtEndian>
namespace deskflow::streaming {
struct AudioClipping { quint32 start=0,end=0; };
inline QByteArray encodeAudioClipping(AudioClipping clip) {
  QByteArray bytes(16,'\0'); qToBigEndian(quint64(0x44464c4f57414331ULL),bytes.data());
  qToBigEndian(clip.start,bytes.data()+8); qToBigEndian(clip.end,bytes.data()+12); return bytes;
}
inline std::optional<AudioClipping> decodeAudioClipping(const QByteArray &bytes) {
  if (bytes.size()!=16 || qFromBigEndian<quint64>(bytes.constData())!=0x44464c4f57414331ULL) return {};
  AudioClipping clip{qFromBigEndian<quint32>(bytes.constData()+8),qFromBigEndian<quint32>(bytes.constData()+12)};
  if (clip.start>960 || clip.end>960) return {}; return clip;
}
inline bool clipDecodedAudio(AudioBlock &block,AudioClipping clip) {
  if (!validAudioBlock(block) || quint64(clip.start)+clip.end>=quint64(block.samples.size()/8)) return false;
  block.samples=block.samples.mid(clip.start*8,block.samples.size()-(clip.start+clip.end)*8);
  block.durationNs=block.samples.size()/8*GST_SECOND/48000;
  // Priming/padding are codec samples, not source time. Preserve media and capture PTS.
  return true;
}
}
