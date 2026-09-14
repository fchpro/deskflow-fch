// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QByteArray>
#include <gst/gst.h>
namespace deskflow::streaming {
// Authenticated packet identity is separate from local jitter-buffer timestamps.
class PacketMetadata {
public:
  bool attach(GstBuffer *&buffer,const QByteArray &bytes) {
    if(bytes.size()!=80 && bytes.size()!=96)return false;
    static const auto *info=gst_meta_register_custom_simple("DeskflowAuthenticatedPacketMetadata");
    if(!info)return false;
    buffer=gst_buffer_make_writable(buffer);
    auto *meta=gst_buffer_add_custom_meta(buffer,"DeskflowAuthenticatedPacketMetadata");
    if(!meta)return false;
    auto *owned=g_bytes_new(bytes.constData(),bytes.size());
    gst_structure_set(gst_custom_meta_get_structure(meta),"identity",G_TYPE_BYTES,owned,nullptr);
    g_bytes_unref(owned);
    return true;
  }
  QByteArray read(GstBuffer *buffer) const {
    auto *meta=gst_buffer_get_custom_meta(buffer,"DeskflowAuthenticatedPacketMetadata");
    if(!meta)return {};
    const auto *value=gst_structure_get_value(gst_custom_meta_get_structure(meta),"identity");
    if(!value || !G_VALUE_HOLDS(value,G_TYPE_BYTES))return {};
    gsize size=0; const auto *bytes=static_cast<const char *>(g_bytes_get_data(static_cast<GBytes *>(g_value_get_boxed(value)),&size));
    return QByteArray(bytes,qsizetype(size));
  }
};
}
