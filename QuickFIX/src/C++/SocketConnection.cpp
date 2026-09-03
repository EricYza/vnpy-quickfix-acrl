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
#include <poll.h>
#endif

#include "Session.h"
#include "SocketAcceptor.h"
#include "SocketConnection.h"
#include "SocketConnector.h"
#include "SocketInitiator.h"
#include "Utility.h"
#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
#include "detail/DirectSocketRead.h"
#include "detail/DirectSocketWrite.h"
#endif

namespace FIX {
SocketConnection::SocketConnection(socket_handle s, Sessions sessions, SocketMonitor *pMonitor)
    : m_socket(s),
      m_sendLength(0),
      m_sessions(sessions),
      m_pSession(0),
      m_pMonitor(pMonitor) {
#ifdef _MSC_VER
  FD_ZERO(&m_fds);
  FD_SET(m_socket, &m_fds);
#endif
}

SocketConnection::SocketConnection(
    SocketInitiator &i,
    const SessionID &sessionID,
    socket_handle s,
    SocketMonitor *pMonitor)
    : m_socket(s),
      m_sendLength(0),
      m_pSession(i.getSession(sessionID, *this)),
      m_pMonitor(pMonitor) {
#ifdef _MSC_VER
  FD_ZERO(&m_fds);
  FD_SET(m_socket, &m_fds);
#endif
  m_sessions.insert(sessionID);
}

SocketConnection::~SocketConnection() {
  if (m_pSession) {
    Session::unregisterSession(m_pSession->getSessionID());
  }
}

/**
 * @brief Enqueues one serialized FIX message for non-blocking delivery.
 *
 * The original path immediately tries `processQueue()` and asks `SocketMonitor` for future writable events. Direct
 * mode leaves the message in the same queue because `runDirectScanOnce()` owns all subsequent send attempts.
 *
 * @param message Complete serialized FIX message to append to the connection's send queue.
 * @return Always `true`; delivery progress is handled asynchronously by the selected network path.
 */
bool SocketConnection::send(const std::string &message) {
  Locker l(m_mutex);

  m_sendQueue.push_back(message);
#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
  if (m_directReadPoll) {
    // The direct scan will attempt this queue; do not register the socket with poll-based POLLOUT handling.
    return true;
  }
#endif
  processQueue();
  signal();
  return true;
}

bool SocketConnection::processQueue() {
  Locker l(m_mutex);

  if (!m_sendQueue.size()) {
    return true;
  }

#ifdef _MSC_VER
  struct timeval timeout = {0, 0};
  fd_set writeset = m_fds;
  if (select(0, 0, &writeset, 0, &timeout) <= 0) {
    return false;
  }
#else
  struct pollfd pfd = {m_socket, POLLOUT, 0};
  if (poll(&pfd, 1, 0) <= 0) {
    return false;
  }
#endif

  const std::string &msg = m_sendQueue.front();

  ssize_t result = socket_send(m_socket, msg.c_str() + m_sendLength, msg.length() - m_sendLength);

  if (result > 0) {
    m_sendLength += result;
  }

  if (m_sendLength == msg.length()) {
    m_sendLength = 0;
    m_sendQueue.pop_front();
  }

  return !m_sendQueue.size();
}

#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
/**
 * @brief Makes one direct non-blocking send attempt for the message at the front of the queue.
 *
 * `m_sendLength` is the byte offset already accepted by the kernel for the current front message. A partial send or
 * `WouldBlock` preserves both the message and offset so the next scan resumes at the first unsent byte.
 *
 * @return `Complete` when the queue is empty, `Pending` when more bytes remain, or `Disconnect` on a closed peer or
 * non-recoverable socket error.
 */
SocketConnection::DirectWriteResult SocketConnection::processQueueDirect() {
  Locker l(m_mutex);

  if (m_sendQueue.empty()) {
    return DirectWriteResult::Complete;
  }

  const std::string &message = m_sendQueue.front();
  // Resume from m_sendLength instead of retransmitting the prefix accepted during an earlier scan.
  const detail::DirectSocketWriteResult result
      = detail::directSocketWrite(m_socket, message.data() + m_sendLength, message.size() - m_sendLength);

  if (result.status == detail::DirectSocketWriteStatus::Progress) {
    // Keep a partial-send offset across scans; only a fully sent message may leave the queue.
    m_sendLength += result.bytes;
    if (m_sendLength == static_cast<ssize_t>(message.size())) {
      m_sendLength = 0;
      m_sendQueue.pop_front();
    }
    return m_sendQueue.empty() ? DirectWriteResult::Complete : DirectWriteResult::Pending;
  }

  if (result.status == detail::DirectSocketWriteStatus::WouldBlock) {
    // A full kernel send buffer is temporary, so retain the exact queue state for the next scan.
    return DirectWriteResult::Pending;
  }
  return DirectWriteResult::Disconnect;
}
#endif

void SocketConnection::disconnect() {
  if (m_pMonitor) {
    m_pMonitor->drop(m_socket);
  }
}

bool SocketConnection::read(SocketConnector &s) {
  if (!m_pSession) {
    return false;
  }

  try {
    readFromSocket();
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
    readMessages(s.getMonitor(), 0);
#else
    readMessages(s.getMonitor());
#endif
  } catch (SocketRecvFailed &e) {
    m_pSession->getLog()->onEvent(e.what());
    return false;
  }
  return true;
}

bool SocketConnection::read(SocketAcceptor &acceptor, SocketServer &server) {
  std::string message;
  try {
    if (!m_pSession) {
#if _MSC_VER
      struct timeval timeout = {1, 0};
      fd_set readset = m_fds;
#else
      int timeout = 1000; // 1000ms = 1 second
      struct pollfd pfd = {m_socket, POLLIN | POLLPRI, 0};
#endif

      while (!readMessage(message)) {
#if _MSC_VER
        int result = select(0, &readset, 0, 0, &timeout);
#else
        int result = poll(&pfd, 1, timeout);
#endif
        if (result > 0) {
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
          const ssize_t received = readFromSocket();
          acceptor.recordNetworkReceive(static_cast<std::uint64_t>(received));
#else
          readFromSocket();
#endif
        } else if (result == 0) {
          return false;
        } else if (result < 0) {
          return false;
        }
      }

      m_pSession = Session::lookupSession(message, true);
      if (!isValidSession()) {
        m_pSession = 0;
        if (acceptor.getLog()) {
          acceptor.getLog()->onEvent("Session not found for incoming message: " + message);
          acceptor.getLog()->onIncoming(message);
        }
      }
      if (m_pSession) {
        m_pSession = acceptor.getSession(message, *this);
      }
      if (m_pSession) {
        m_pSession->next(message, UtcTimeStamp::now());
      }
      if (!m_pSession) {
        server.getMonitor().drop(m_socket);
        return false;
      }

      if (m_pSession->isAcceptor()) {
        std::string remote_address = socket_peername(m_socket);
        if (!m_pSession->getAllowedRemoteAddresses().empty() && !m_pSession->inAllowedRemoteAddresses(remote_address)) {
          m_pSession->getLog()->onEvent("Deny connections to the acceptor from " + remote_address);
          return false;
        }
        m_pSession->getLog()->onEvent("Allows connections to the acceptor from " + remote_address);
      }

      Session::registerSession(m_pSession->getSessionID());
      return true;
    } else {
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
      const ssize_t received = readFromSocket();
      acceptor.recordNetworkReceive(static_cast<std::uint64_t>(received));
      readMessages(server.getMonitor(), &acceptor);
#else
      readFromSocket();
      readMessages(server.getMonitor());
#endif
      return true;
    }
  } catch (SocketRecvFailed &e) {
    if (m_pSession) {
      m_pSession->getLog()->onEvent(e.what());
    }
    server.getMonitor().drop(m_socket);
  } catch (InvalidMessage &) {
    server.getMonitor().drop(m_socket);
  }
  return false;
}

#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
/**
 * @brief Makes one direct non-blocking receive attempt and feeds any bytes into the normal FIX processing path.
 *
 * Parser state survives across calls, so fragmented Logon and application messages continue in later scan
 * iterations. Once a complete Logon identifies the Session, message dispatch reuses `readMessages()` and
 * `Session::next()` exactly as the original socket path does.
 *
 * @param acceptor Acceptor used for Session lookup, connection policy, logging, and optional diagnostics.
 * @param server Server providing shared monitor ownership for the established connection.
 * @return `Data` after receiving bytes, `WouldBlock` when this scan found no bytes, or `Disconnect` for timeout,
 * peer closure, malformed session setup, or a non-recoverable socket error.
 */
SocketConnection::DirectReadResult SocketConnection::readDirect(SocketAcceptor &acceptor, SocketServer &server) {
  const detail::DirectSocketReadResult socketResult = detail::directSocketRead(m_socket, m_buffer, sizeof(m_buffer));

  if (socketResult.status == detail::DirectSocketReadStatus::WouldBlock) {
    // EAGAIN means no data in this scan, not a broken connection; only the Logon deadline can turn it into a close.
    if (!m_pSession && std::chrono::steady_clock::now() >= m_directLogonDeadline) {
      if (acceptor.getLog()) {
        acceptor.getLog()->onEvent("Timed out waiting for Logon on direct-read connection");
      }
      return DirectReadResult::Disconnect;
    }
    return DirectReadResult::WouldBlock;
  }

  if (socketResult.status == detail::DirectSocketReadStatus::PeerClosed
      || socketResult.status == detail::DirectSocketReadStatus::Error) {
    if (m_pSession) {
      SocketRecvFailed error(socketResult.status == detail::DirectSocketReadStatus::PeerClosed ? 0 : -1);
      m_pSession->getLog()->onEvent(error.what());
    }
    return DirectReadResult::Disconnect;
  }

  // Parser retains incomplete trailing bytes, allowing recv boundaries to split a FIX message safely.
  m_parser.addToStream(m_buffer, static_cast<std::size_t>(socketResult.bytes));
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS)
  acceptor.recordNetworkReceive(static_cast<std::uint64_t>(socketResult.bytes));
#endif

  try {
    if (!m_pSession) {
      std::string message;
      if (!readMessage(message)) {
        return DirectReadResult::Data;
      }

      m_pSession = Session::lookupSession(message, true);
      if (!isValidSession()) {
        m_pSession = 0;
        if (acceptor.getLog()) {
          acceptor.getLog()->onEvent("Session not found for incoming message: " + message);
          acceptor.getLog()->onIncoming(message);
        }
      }
      if (m_pSession) {
        m_pSession = acceptor.getSession(message, *this);
      }
      if (m_pSession) {
        m_pSession->next(message, UtcTimeStamp::now());
      }
      if (!m_pSession) {
        return DirectReadResult::Disconnect;
      }

      if (m_pSession->isAcceptor()) {
        const std::string remoteAddress = socket_peername(m_socket);
        if (!m_pSession->getAllowedRemoteAddresses().empty()
            && !m_pSession->inAllowedRemoteAddresses(remoteAddress)) {
          m_pSession->getLog()->onEvent("Deny connections to the acceptor from " + remoteAddress);
          return DirectReadResult::Disconnect;
        }
        m_pSession->getLog()->onEvent("Allows connections to the acceptor from " + remoteAddress);
      }

      Session::registerSession(m_pSession->getSessionID());
    }

#if defined(QUICKFIX_NETWORK_DIAGNOSTICS)
    readMessages(server.getMonitor(), &acceptor);
#else
    readMessages(server.getMonitor());
#endif
    return DirectReadResult::Data;
  } catch (InvalidMessage &) {
    return DirectReadResult::Disconnect;
  }
}
#endif

bool SocketConnection::isValidSession() {
  if (m_pSession == 0) {
    return false;
  }
  SessionID sessionID = m_pSession->getSessionID();
  if (Session::isSessionRegistered(sessionID)) {
    return false;
  }
  return !(m_sessions.find(sessionID) == m_sessions.end());
}

ssize_t SocketConnection::readFromSocket() EXCEPT(SocketRecvFailed) {
  ssize_t size = socket_recv(m_socket, m_buffer, sizeof(m_buffer));
  if (size <= 0) {
    throw SocketRecvFailed(size);
  }
  m_parser.addToStream(m_buffer, size);
  return size;
}

/**
 * @brief Attempts to extract one complete FIX message from the connection's persistent Parser buffer.
 *
 * @param msg Destination for the complete wire message.
 * @return `false` only when more stream bytes are required; `true` after extraction or a parser error that the
 * surrounding connection/session path must handle.
 */
bool SocketConnection::readMessage(std::string &msg) {
  try {
    return m_parser.readFixMessage(msg);
  } catch (MessageParseError &) {}
  return true;
}

#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
void SocketConnection::readMessages(SocketMonitor &socketMonitor, SocketAcceptor *acceptor) {
#else
void SocketConnection::readMessages(SocketMonitor &socketMonitor) {
#endif
  if (!m_pSession) {
    return;
  }

  std::string message;
  while (readMessage(message)) {
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
    if (acceptor) {
      acceptor->recordParsedMessage();
    }
#endif
    try {
      m_pSession->next(message, UtcTimeStamp::now());
    } catch (InvalidMessage &) {
      if (!m_pSession->isLoggedOn()) {
        socketMonitor.drop(m_socket);
      }
    }
  }
}

/**
 * @brief Forwards one periodic network tick into the existing FIX Session timer state machine.
 *
 * Blocking and `poll0` receive this tick from `SocketMonitor`; direct mode calls the same function through its
 * `steady_clock` compensation in `runDirectScanOnce()`.
 */
void SocketConnection::onTimeout() {
  if (m_pSession) {
    m_pSession->next(UtcTimeStamp::now());
  }
}
} // namespace FIX
