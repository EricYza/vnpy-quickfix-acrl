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

#include "detail/DirectSocketRead.h"

#include <Parser.h>
#include <Utility.h>
#include <cerrno>
#include <string>

#include "catch_amalgamated.hpp"

namespace {

class TestSocketPair {
public:
  TestSocketPair() {
    const std::pair<FIX::socket_handle, FIX::socket_handle> sockets = FIX::socket_createpair();
    m_reader = sockets.first;
    m_writer = sockets.second;
    FIX::socket_setnonblock(m_reader);
  }

  ~TestSocketPair() {
    close(m_reader);
    close(m_writer);
  }

  FIX::socket_handle reader() const { return m_reader; }
  FIX::socket_handle writer() const { return m_writer; }

  void closeWriter() { close(m_writer); }

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

TEST_CASE("DirectSocketReadTests") {
  using FIX::detail::DirectSocketReadStatus;

  SECTION("returns data without a readiness poll") {
    TestSocketPair sockets;
    const std::string payload = "direct-read";
    REQUIRE(
        FIX::socket_send(sockets.writer(), payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));

    char buffer[32]{};
    const FIX::detail::DirectSocketReadResult result
        = FIX::detail::directSocketRead(sockets.reader(), buffer, sizeof(buffer));

    CHECK(result.status == DirectSocketReadStatus::Data);
    CHECK(result.bytes == static_cast<ssize_t>(payload.size()));
    CHECK(result.errorCode == 0);
    CHECK(std::string(buffer, static_cast<std::size_t>(result.bytes)) == payload);
  }

  SECTION("treats EAGAIN as an idle socket") {
    TestSocketPair sockets;
    char buffer[32]{};

    const FIX::detail::DirectSocketReadResult result
        = FIX::detail::directSocketRead(sockets.reader(), buffer, sizeof(buffer));

    CHECK(result.status == DirectSocketReadStatus::WouldBlock);
    CHECK(result.bytes == 0);
    CHECK((result.errorCode == EAGAIN || result.errorCode == EWOULDBLOCK));
  }

  SECTION("distinguishes peer shutdown from EAGAIN") {
    TestSocketPair sockets;
    sockets.closeWriter();
    char buffer[32]{};

    const FIX::detail::DirectSocketReadResult result
        = FIX::detail::directSocketRead(sockets.reader(), buffer, sizeof(buffer));

    CHECK(result.status == DirectSocketReadStatus::PeerClosed);
    CHECK(result.bytes == 0);
    CHECK(result.errorCode == 0);
  }

  SECTION("preserves a real socket error") {
    char buffer[32]{};
    const FIX::detail::DirectSocketReadResult result
        = FIX::detail::directSocketRead(INVALID_SOCKET_HANDLE, buffer, sizeof(buffer));

    CHECK(result.status == DirectSocketReadStatus::Error);
    CHECK(result.bytes == 0);
    CHECK(result.errorCode == EBADF);
  }

  SECTION("feeds fragmented FIX data into the existing parser") {
    TestSocketPair sockets;
    FIX::Parser parser;
    const std::string firstPart = "8=FIX.4.2\0019=17\00135=4\00136=";
    const std::string secondPart = "88\001123=Y\00110=34\001";
    char buffer[64]{};

    REQUIRE(FIX::socket_send(sockets.writer(), firstPart.data(), firstPart.size())
            == static_cast<ssize_t>(firstPart.size()));
    FIX::detail::DirectSocketReadResult result
        = FIX::detail::directSocketRead(sockets.reader(), buffer, sizeof(buffer));
    REQUIRE(result.status == DirectSocketReadStatus::Data);
    parser.addToStream(buffer, static_cast<std::size_t>(result.bytes));

    std::string message;
    CHECK_FALSE(parser.readFixMessage(message));

    REQUIRE(FIX::socket_send(sockets.writer(), secondPart.data(), secondPart.size())
            == static_cast<ssize_t>(secondPart.size()));
    result = FIX::detail::directSocketRead(sockets.reader(), buffer, sizeof(buffer));
    REQUIRE(result.status == DirectSocketReadStatus::Data);
    parser.addToStream(buffer, static_cast<std::size_t>(result.bytes));

    CHECK(parser.readFixMessage(message));
    CHECK(message == firstPart + secondPart);
  }
}

#endif
