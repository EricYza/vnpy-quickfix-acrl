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

#ifndef FIX_SOCKETACCEPTOR_H
#define FIX_SOCKETACCEPTOR_H

#ifdef _MSC_VER
#pragma warning(disable : 4503 4355 4786 4290)
#endif

#include "Acceptor.h"
#include "SocketConnection.h"
#include "SocketServer.h"
#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
#include <chrono>
#endif
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
#include <atomic>
#include <cstdint>
#endif

namespace FIX {
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
struct SocketAcceptorDiagnostics {
  std::uint64_t pollCalls = 0;
  std::uint64_t pollWaitNanoseconds = 0;
  std::uint64_t pollImmediateReturns = 0;
  std::uint64_t pollBlockingReturns = 0;
  std::uint64_t pollContextSampleFailures = 0;
  std::uint64_t recvCalls = 0;
  std::uint64_t recvBytes = 0;
  std::uint64_t parsedMessages = 0;
};
#endif

/// Socket implementation of Acceptor.
class SocketAcceptor : public Acceptor, SocketServer::Strategy {
  friend class SocketConnection;

public:
  typedef std::map<SessionID, uint16_t> SessionToPort;

  SocketAcceptor(Application &, MessageStoreFactory &, const SessionSettings &) EXCEPT(ConfigError);
  SocketAcceptor(Application &, MessageStoreFactory &, const SessionSettings &, LogFactory &) EXCEPT(ConfigError);

  virtual ~SocketAcceptor();

  const SessionToPort &sessionToPort() { return m_sessionToPort; }
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
  /// Enables or disables acceptor-side poll, recv, and parsed-message counters.
  void setNetworkDiagnosticsEnabled(bool enabled);
  /// Clears acceptor-side diagnostics without changing whether collection is enabled.
  void resetNetworkDiagnostics();
  /// Returns a thread-safe snapshot of acceptor-side network diagnostics.
  SocketAcceptorDiagnostics networkDiagnostics() const;
#endif

private:
  bool readSettings(const SessionSettings &);

  typedef std::set<SessionID> Sessions;
  typedef std::map<uint16_t, Sessions> PortToSessions;
  typedef std::map<socket_handle, SocketConnection *> SocketConnections;

  void onConfigure(const SessionSettings &) EXCEPT(ConfigError);
  void onInitialize(const SessionSettings &) EXCEPT(RuntimeError);

  void onStart();
  bool onPoll();
  void onStop();
#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
  bool runDirectScanOnce();
  void disconnectDirect(socket_handle);
#endif

  void onConnect(SocketServer &, socket_handle, socket_handle);
  void onWrite(SocketServer &, socket_handle);
  bool onData(SocketServer &, socket_handle);
  void onDisconnect(SocketServer &, socket_handle);
  void onError(SocketServer &);
  void onTimeout(SocketServer &);
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
  void recordNetworkReceive(std::uint64_t bytes);
  void recordParsedMessage();
#endif

  SocketServer *m_pServer;
  bool m_busyPoll;
#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
  bool m_directReadPoll;
  std::chrono::steady_clock::time_point m_directTimeoutTick;
#endif
  int m_busyPollCpu;
  PortToSessions m_portToSessions;
  SessionToPort m_sessionToPort;
  SocketConnections m_connections;
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
  std::atomic<bool> m_networkDiagnosticsEnabled{false};
  std::atomic<std::uint64_t> m_recvCalls{0};
  std::atomic<std::uint64_t> m_recvBytes{0};
  std::atomic<std::uint64_t> m_parsedMessages{0};
#endif
};
/*! @} */
} // namespace FIX

#endif // FIX_SOCKETACCEPTOR_H
