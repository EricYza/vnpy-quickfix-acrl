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

#include "DirectSocketWrite.h"

#include <cerrno>

namespace FIX::detail {

/**
 * @brief Performs one `send()` attempt on a non-blocking direct-mode socket.
 *
 * The caller owns queue state and partial-send offsets. This wrapper only classifies the result of one system call,
 * retrying `EINTR` but leaving `EAGAIN` for the next direct scan.
 *
 * @param socket Non-blocking client socket to write.
 * @param data First unsent byte of the queued message.
 * @param length Number of remaining bytes available at `data`.
 * @return Classified status, sent byte count, and operating-system error code when applicable.
 */
DirectSocketWriteResult directSocketWrite(socket_handle socket, const char *data, std::size_t length) noexcept {
  if (length == 0) {
    return {DirectSocketWriteStatus::Progress, 0, 0};
  }

  ssize_t bytes;
  do {
    bytes = socket_send(socket, data, length);
  } while (bytes < 0 && errno == EINTR);

  if (bytes > 0) {
    return {DirectSocketWriteStatus::Progress, bytes, 0};
  }
  if (bytes == 0) {
    return {DirectSocketWriteStatus::PeerClosed, 0, 0};
  }

  const int errorCode = errno;
  if (errorCode == EAGAIN || errorCode == EWOULDBLOCK) {
    // The send buffer is temporarily full; keeping the queue intact makes this a recoverable outcome.
    return {DirectSocketWriteStatus::WouldBlock, 0, errorCode};
  }
  if (errorCode == EPIPE || errorCode == ECONNRESET || errorCode == ENOTCONN) {
    return {DirectSocketWriteStatus::PeerClosed, 0, errorCode};
  }
  return {DirectSocketWriteStatus::Error, 0, errorCode};
}

} // namespace FIX::detail
