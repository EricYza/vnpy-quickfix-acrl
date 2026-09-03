/* -*- C++ -*- */

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

#ifndef FIX_SOCKETCONNECTION_H
#define FIX_SOCKETCONNECTION_H

#ifdef _MSC_VER
#pragma warning(disable : 4503 4355 4786 4290)
#endif

#include "Mutex.h"
#include "Parser.h"
#include "Responder.h"
#include "SessionID.h"
#include "SocketMonitor.h"
#include "Utility.h"
#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
#include <chrono>
#endif
#include <set>

namespace FIX {
class SocketAcceptor;
class SocketServer;
class SocketConnector;
class SocketInitiator;
class Session;

/// Encapsulates a socket file descriptor (single-threaded).
class SocketConnection : Responder {
public:
  typedef std::set<SessionID> Sessions;
#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
  /**
   * @brief Tells the acceptor scan whether a direct read made progress, should retry later, or must disconnect.
   */
  enum class DirectReadResult { Data, WouldBlock, Disconnect };

  /**
   * @brief Tells the acceptor scan whether the send queue drained, remains pending, or must disconnect.
   */
  enum class DirectWriteResult { Complete, Pending, Disconnect };
#endif

  SocketConnection(socket_handle s, Sessions sessions, SocketMonitor *pMonitor);
  SocketConnection(SocketInitiator &, const SessionID &, socket_handle, SocketMonitor *);
  virtual ~SocketConnection();

  socket_handle getSocket() const { return m_socket; }
  Session *getSession() const { return m_pSession; }

  bool read(SocketConnector &s);
  bool read(SocketAcceptor &, SocketServer &);
#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
  DirectReadResult readDirect(SocketAcceptor &, SocketServer &);
  DirectWriteResult processQueueDirect();

  /**
   * @brief Selects direct-scan ownership for this connection's receive and send progress.
   *
   * @param enabled `true` to bypass poll-based writes and let `runDirectScanOnce()` service this connection.
   */
  void setDirectReadPoll(bool enabled) { m_directReadPoll = enabled; }
#endif
  bool processQueue();

  void signal() {
    Locker l(m_mutex);
    if (m_sendQueue.size() == 1) {
      m_pMonitor->signal(m_socket);
    }
  }

  void unsignal() {
    Locker l(m_mutex);
    if (m_sendQueue.size() == 0) {
      m_pMonitor->unsignal(m_socket);
    }
  }

  void onTimeout();

private:
  typedef std::deque<std::string, ALLOCATOR<std::string>> Queue;

  bool isValidSession();
  ssize_t readFromSocket() EXCEPT(SocketRecvFailed);
  bool readMessage(std::string &msg);
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
  void readMessages(SocketMonitor &s, SocketAcceptor *acceptor);
#else
  void readMessages(SocketMonitor &s);
#endif
  bool send(const std::string &);
  void disconnect();

  socket_handle m_socket;
  char m_buffer[BUFSIZ];

  Parser m_parser;
  Queue m_sendQueue;
  ssize_t m_sendLength;
  Sessions m_sessions;
  Session *m_pSession;
  SocketMonitor *m_pMonitor;
  Mutex m_mutex;
#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
  bool m_directReadPoll = false;
  std::chrono::steady_clock::time_point m_directLogonDeadline
      = std::chrono::steady_clock::now() + std::chrono::seconds(1);
#endif
#ifdef _MSC_VER
  fd_set m_fds;
#endif
};
} // namespace FIX

#endif // FIX_SOCKETCONNECTION_H
