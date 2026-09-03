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

#ifdef _MSC_VER
#include "stdafx.h"
#else
#include "config.h"
#endif

#include "Exceptions.h"
#include "SocketServer.h"
#include "Utility.h"
#ifndef _MSC_VER
#include <cerrno>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#include <exception>

namespace FIX {
/**
 * @brief Adapts low-level `SocketMonitor` events to the higher-level `SocketServer::Strategy` callbacks.
 *
 * The wrapper distinguishes listening sockets from connected client sockets. A readable listening socket is
 * accepted and reported through `onConnect()`, while a readable client socket is forwarded through `onData()`.
 */
class ServerWrapper : public SocketMonitor::Strategy {
public:
  /**
   * @brief Creates an event adapter for one `SocketServer::block()` iteration.
   *
   * @param sockets Listening sockets owned by the server.
   * @param server Server passed to each high-level callback.
   * @param strategy High-level callback target, normally `SocketAcceptor`.
   */
  ServerWrapper(std::set<socket_handle> sockets, SocketServer &server, SocketServer::Strategy &strategy)
      : m_sockets(sockets),
        m_server(server),
        m_strategy(strategy) {}

private:
  /**
   * @brief Handles completion of the monitor's non-blocking connect-registration step.
   *
   * Accepted server-side sockets are already reported by `onEvent()`, so no second high-level callback is required.
   *
   * @param socket Connected client socket.
   *
   * The unused `SocketMonitor` argument identifies the monitor that dispatched the event.
   */
  void onConnect(SocketMonitor &, socket_handle socket) {}

  /**
   * @brief Converts a readable event into either `accept()` or application-data processing.
   *
   * @param monitor Monitor that detected the readable socket.
   * @param socket Listening or connected socket marked readable by `poll()`.
   */
  void onEvent(SocketMonitor &monitor, socket_handle socket) {
    if (m_sockets.find(socket) != m_sockets.end()) {
      m_strategy.onConnect(m_server, socket, m_server.accept(socket));
    } else {
      if (!m_strategy.onData(m_server, socket)) {
        onError(monitor, socket);
      }
    }
  }

  /**
   * @brief Forwards a writable client-socket event to the server strategy.
   *
   * @param socket Client socket marked writable by `poll()`.
   *
   * The unused `SocketMonitor` argument identifies the monitor that dispatched the event.
   */
  void onWrite(SocketMonitor &, socket_handle socket) { m_strategy.onWrite(m_server, socket); }

  /**
   * @brief Disconnects and removes a socket after a socket-specific error.
   *
   * @param monitor Monitor that owns the socket.
   * @param socket Socket that reported an error or closed condition.
   */
  void onError(SocketMonitor &monitor, socket_handle socket) {
    m_strategy.onDisconnect(m_server, socket);
    monitor.drop(socket);
  }

  /**
   * @brief Forwards a monitor-wide error to the server strategy.
   *
   * The unused `SocketMonitor` argument identifies the monitor that reported the error.
   */
  void onError(SocketMonitor &) { m_strategy.onError(m_server); }

  /**
   * @brief Forwards the monitor timer tick to the server strategy.
   *
   * The unused `SocketMonitor` argument identifies the monitor whose timeout elapsed.
   */
  void onTimeout(SocketMonitor &) { m_strategy.onTimeout(m_server); };

  typedef std::set<socket_handle> Sockets;

  Sockets m_sockets;
  SocketServer &m_server;
  SocketServer::Strategy &m_strategy;
};

SocketServer::SocketServer(int timeout)
    : m_monitor(timeout) {}

/**
 * @brief Creates one listening socket and registers it for readable events.
 *
 * @param port TCP port on which the acceptor listens.
 * @param reuse Whether to enable address reuse while creating the listening socket.
 * @param noDelay Whether accepted client sockets should disable Nagle's algorithm.
 * @param sendBufSize Requested send-buffer size for accepted sockets, or zero for the operating-system default.
 * @param rcvBufSize Requested receive-buffer size for accepted sockets, or zero for the operating-system default.
 * @return Listening socket registered in the monitor's read set.
 */
socket_handle SocketServer::add(int port, bool reuse, bool noDelay, int sendBufSize, int rcvBufSize)
    EXCEPT(SocketException &) {
  if (m_portToInfo.find(port) != m_portToInfo.end()) {
    return m_portToInfo[port].m_socket;
  }

  socket_handle socket = socket_createAcceptor(port, reuse);
  if (socket == INVALID_SOCKET_HANDLE) {
    throw SocketException();
  }
  if (noDelay) {
    socket_setsockopt(socket, TCP_NODELAY);
  }
  if (sendBufSize) {
    socket_setsockopt(socket, SO_SNDBUF, sendBufSize);
  }
  if (rcvBufSize) {
    socket_setsockopt(socket, SO_RCVBUF, rcvBufSize);
  }
  m_monitor.addRead(socket);

  SocketInfo info(socket, port, noDelay, sendBufSize, rcvBufSize);
  m_socketToInfo[socket] = info;
  m_portToInfo[port] = info;
  return socket;
}

/**
 * @brief Accepts one client and registers its non-blocking socket with the monitor.
 *
 * @param socket Readable listening socket reported by `poll()`.
 * @return Newly accepted client socket, or `INVALID_SOCKET_HANDLE` when acceptance fails.
 */
socket_handle SocketServer::accept(socket_handle socket) {
  SocketInfo info = m_socketToInfo[socket];

  socket_handle result = socket_accept(socket);
  if (info.m_noDelay) {
    socket_setsockopt(result, TCP_NODELAY);
  }
  if (info.m_sendBufSize) {
    socket_setsockopt(result, SO_SNDBUF, info.m_sendBufSize);
  }
  if (info.m_rcvBufSize) {
    socket_setsockopt(result, SO_RCVBUF, info.m_rcvBufSize);
  }
  if (result != INVALID_SOCKET_HANDLE) {
    m_monitor.addConnect(result);
  }
  return result;
}

#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
/**
 * @brief Attempts one non-blocking `accept()` across the configured listening sockets.
 *
 * Interrupted system calls are retried. A listener returning `EAGAIN` or `EWOULDBLOCK` is simply empty for this scan,
 * so the function continues to the next listener. At most one new client is returned per call.
 *
 * @return Accepted socket details, `WouldBlock` when every listener is empty, or `Error` for a real socket failure.
 */
SocketServer::DirectAcceptResult SocketServer::acceptDirect() {
  for (const SocketToInfo::value_type &socketWithInfo : m_socketToInfo) {
    const socket_handle acceptSocket = socketWithInfo.first;
    const SocketInfo &info = socketWithInfo.second;

    socket_handle result;
    do {
      result = socket_accept(acceptSocket);
    } while (!socket_isValid(result) && errno == EINTR);

    if (!socket_isValid(result)) {
      const int errorCode = errno;
      if (errorCode == EAGAIN || errorCode == EWOULDBLOCK) {
        // No connection is pending on this non-blocking listener during the current scan.
        continue;
      }
      return {DirectAcceptStatus::Error, acceptSocket, INVALID_SOCKET_HANDLE, errorCode};
    }

    if (info.m_noDelay) {
      socket_setsockopt(result, TCP_NODELAY);
    }
    if (info.m_sendBufSize) {
      socket_setsockopt(result, SO_SNDBUF, info.m_sendBufSize);
    }
    if (info.m_rcvBufSize) {
      socket_setsockopt(result, SO_RCVBUF, info.m_rcvBufSize);
    }
    // addRead() sets the client non-blocking and retains it for shared socket ownership and cleanup.
    m_monitor.addRead(result);
    return {DirectAcceptStatus::Accepted, acceptSocket, result, 0};
  }

  return {DirectAcceptStatus::WouldBlock, INVALID_SOCKET_HANDLE, INVALID_SOCKET_HANDLE, EAGAIN};
}
#endif

void SocketServer::close() {
  for (const SocketToInfo::value_type &socketWithInfo : m_socketToInfo) {
    socket_handle socket = socketWithInfo.first;
    socket_close(socket);
    socket_invalidate(socket);
  }
}

/**
 * @brief Performs one socket-monitor iteration for all listening and connected sockets.
 *
 * This method does not choose between the blocking baseline and `poll0`; it builds the adapter and delegates both
 * modes to the same `SocketMonitor::block()` implementation.
 *
 * @param strategy High-level callback target, normally `SocketAcceptor`.
 * @param poll When `true`, requests an immediate monitor iteration for `SocketAcceptor::onPoll()`.
 * @param timeout Per-call timeout forwarded to the monitor; the UNIX monitor currently uses its configured timeout.
 * @return `false` if a listening socket is invalid; otherwise `true` after the monitor iteration.
 */
bool SocketServer::block(Strategy &strategy, bool poll, double timeout) {
  std::set<socket_handle> sockets;
  for (const SocketToInfo::value_type &socketWithInfo : m_socketToInfo) {
    if (!socket_isValid(socketWithInfo.first)) {
      return false;
    }
    sockets.insert(socketWithInfo.first);
  }

  // ServerWrapper is the bridge from monitor-level events back to SocketAcceptor callbacks.
  ServerWrapper wrapper(sockets, *this, strategy);
  m_monitor.block(wrapper, poll, timeout);
  return true;
}

/**
 * @brief Enables or disables the monitor's zero-timeout `poll0` behavior.
 *
 * @param enabled `true` to force each operating-system `poll()` call to use a zero timeout.
 */
void SocketServer::setBusyPoll(bool enabled) { m_monitor.setBusyPoll(enabled); }

int SocketServer::socketToPort(socket_handle socket) {
  SocketToInfo::iterator find = m_socketToInfo.find(socket);
  if (find == m_socketToInfo.end()) {
    return 0;
  }
  return find->second.m_port;
}

socket_handle SocketServer::portToSocket(int port) {
  PortToInfo::iterator find = m_portToInfo.find(port);
  if (find == m_portToInfo.end()) {
    return 0;
  }
  return find->second.m_socket;
}
} // namespace FIX
