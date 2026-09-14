// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
namespace deskflow::streaming {
// Printable source-layout symbol for the normalized control virtual-key code.
inline int printableControlSymbol(int code) {
  if(code>=65 && code<=90)return 'a'+code-65;
  if(code==32 || (code>=48 && code<=57))return code;
  switch(code){case 186:return ';';case 187:return '=';case 188:return ',';case 189:return '-';case 190:return '.';case 191:return '/';case 192:return '`';case 219:return '[';case 220:return '\\';case 221:return ']';case 222:return '\'';default:return 0;}
}
}
