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

#include "DirectSocketRead.h"

#include <cerrno>

namespace FIX::detail {

/**
 * @brief Performs one `recv()` attempt on a non-blocking direct-mode socket.
 *
 * The wrapper converts normal non-blocking outcomes into explicit statuses so the scan loop can distinguish an empty
 * socket from a closed peer or a real error without throwing an exception.
 *
 * @param socket Non-blocking client socket to read.
 * @param buffer Destination for received bytes.
 * @param capacity Maximum number of bytes that fit in `buffer`.
 * @return Classified status, received byte count, and operating-system error code when applicable.
 */
DirectSocketReadResult directSocketRead(socket_handle socket, char *buffer, std::size_t capacity) noexcept {
  ssize_t bytes;
  do {
    bytes = socket_recv(socket, buffer, capacity);
  } while (bytes < 0 && errno == EINTR);

  if (bytes > 0) {
    return {DirectSocketReadStatus::Data, bytes, 0};
  }
  if (bytes == 0) {
    return {DirectSocketReadStatus::PeerClosed, 0, 0};
  }

  const int errorCode = errno;
  if (errorCode == EAGAIN || errorCode == EWOULDBLOCK) {
    // The socket is healthy but has no bytes now; the next direct scan will try it again.
    return {DirectSocketReadStatus::WouldBlock, 0, errorCode};
  }
  return {DirectSocketReadStatus::Error, 0, errorCode};
}

} // namespace FIX::detail
