/****************************************************************************
** Copyright (c) 2001-2014
**
** This file is part of the QuickFIX FIX Engine
**
** This file may be distributed under the terms of the quickfixengine.org
** license as defined by quickfixengine.org and appearing in the file
** LICENSE included in the packaging of this file.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
** See http://www.quickfixengine.org/LICENSE for licensing information.
**
** Contact ask@quickfixengine.org if any conditions of this licensing are
** not clear to you.
**
****************************************************************************/

#ifndef _MSC_VER

#include "detail/DirectSocketWrite.h"

#include <Utility.h>
#include <cerrno>
#include <string>

#include "catch_amalgamated.hpp"

namespace {

class WriteTestSocketPair {
public:
  WriteTestSocketPair() {
    FIX::socket_init();
    const std::pair<FIX::socket_handle, FIX::socket_handle> sockets = FIX::socket_createpair();
    m_reader = sockets.first;
    m_writer = sockets.second;
    FIX::socket_setnonblock(m_reader);
    FIX::socket_setnonblock(m_writer);
  }

  ~WriteTestSocketPair() {
    close(m_reader);
    close(m_writer);
  }

  FIX::socket_handle reader() const { return m_reader; }
  FIX::socket_handle writer() const { return m_writer; }

  void closeReader() { close(m_reader); }

private:
  static void close(FIX::socket_handle &socket) {
    if (FIX::socket_isValid(socket)) {
      FIX::socket_close(socket);
      FIX::socket_invalidate(socket);
    }
  }

  FIX::socket_handle m_reader = INVALID_SOCKET_HANDLE;
  FIX::socket_handle m_writer = INVALID_SOCKET_HANDLE;
};

} // namespace

TEST_CASE("DirectSocketWriteTests") {
  using FIX::detail::DirectSocketWriteStatus;

  SECTION("preserves partial progress across a full nonblocking send buffer") {
    WriteTestSocketPair sockets;
    REQUIRE(FIX::socket_setsockopt(sockets.writer(), SO_SNDBUF, 4096) == 0);
    REQUIRE(FIX::socket_setsockopt(sockets.reader(), SO_RCVBUF, 4096) == 0);

    const std::string payload(4 * 1024 * 1024, 'W');
    std::size_t written = 0;
    bool partialWrite = false;
    bool wouldBlock = false;

    for (int attempt = 0; attempt < 10000 && !wouldBlock; ++attempt) {
      const std::size_t requested = payload.size() - written;
      const FIX::detail::DirectSocketWriteResult result
          = FIX::detail::directSocketWrite(sockets.writer(), payload.data() + written, requested);
      if (result.status == DirectSocketWriteStatus::Progress) {
        REQUIRE(result.bytes > 0);
        partialWrite = partialWrite || static_cast<std::size_t>(result.bytes) < requested;
        written += static_cast<std::size_t>(result.bytes);
      } else {
        REQUIRE(result.status == DirectSocketWriteStatus::WouldBlock);
        wouldBlock = true;
      }
    }

    REQUIRE(partialWrite);
    REQUIRE(wouldBlock);
    REQUIRE(written > 0);
    REQUIRE(written < payload.size());

    const FIX::detail::DirectSocketWriteResult stillBlocked
        = FIX::detail::directSocketWrite(sockets.writer(), payload.data() + written, payload.size() - written);
    CHECK(stillBlocked.status == DirectSocketWriteStatus::WouldBlock);
    CHECK(stillBlocked.bytes == 0);
    CHECK((stillBlocked.errorCode == EAGAIN || stillBlocked.errorCode == EWOULDBLOCK));

    std::string received;
    received.reserve(payload.size());
    char buffer[16384];
    bool resumedAfterWouldBlock = false;

    for (int attempt = 0; attempt < 200000 && received.size() < payload.size(); ++attempt) {
      while (true) {
        const ssize_t bytes = FIX::socket_recv(sockets.reader(), buffer, sizeof(buffer));
        if (bytes > 0) {
          received.append(buffer, static_cast<std::size_t>(bytes));
          continue;
        }
        REQUIRE(bytes < 0);
        REQUIRE((errno == EAGAIN || errno == EWOULDBLOCK));
        break;
      }

      if (written < payload.size()) {
        const FIX::detail::DirectSocketWriteResult result = FIX::detail::directSocketWrite(
            sockets.writer(), payload.data() + written, payload.size() - written);
        if (result.status == DirectSocketWriteStatus::Progress) {
          REQUIRE(result.bytes > 0);
          resumedAfterWouldBlock = true;
          written += static_cast<std::size_t>(result.bytes);
        } else {
          REQUIRE(result.status == DirectSocketWriteStatus::WouldBlock);
        }
      }
    }

    CHECK(resumedAfterWouldBlock);
    CHECK(written == payload.size());
    CHECK(received == payload);
  }

  SECTION("reports a closed peer separately from an idle socket") {
    WriteTestSocketPair sockets;
    sockets.closeReader();
    const std::string payload = "closed-peer";

    const FIX::detail::DirectSocketWriteResult result
        = FIX::detail::directSocketWrite(sockets.writer(), payload.data(), payload.size());

    CHECK(result.status == DirectSocketWriteStatus::PeerClosed);
    CHECK(result.bytes == 0);
    CHECK((result.errorCode == EPIPE || result.errorCode == ECONNRESET || result.errorCode == ENOTCONN));
  }

  SECTION("preserves an unexpected socket error") {
    const std::string payload = "invalid-socket";
    const FIX::detail::DirectSocketWriteResult result
        = FIX::detail::directSocketWrite(INVALID_SOCKET_HANDLE, payload.data(), payload.size());

    CHECK(result.status == DirectSocketWriteStatus::Error);
    CHECK(result.bytes == 0);
    CHECK(result.errorCode == EBADF);
  }
}

#endif
