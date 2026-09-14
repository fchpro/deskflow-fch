// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QString>
#include <memory>
namespace deskflow::streaming {
// Streaming-only OS session boundary. Never changes ordinary input or global power state.
// Construct/start/poll/destroy on the media worker. Interruption is sticky until a new start.
class SessionEnvironment {
public:
  virtual ~SessionEnvironment()=default;
  virtual QString start()=0;
  virtual QString poll()=0;
};
std::unique_ptr<SessionEnvironment> createSessionEnvironment();
}
