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

#ifndef FIX_SOCKETMONITOR_UNIX_H
#define FIX_SOCKETMONITOR_UNIX_H

#ifndef _MSC_VER

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <poll.h>
#include <queue>
#include <set>

#include "Utility.h"

namespace FIX {
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS)
struct SocketMonitorDiagnostics {
  std::uint64_t pollCalls = 0;
  std::uint64_t pollWaitNanoseconds = 0;
  std::uint64_t pollImmediateReturns = 0;
  std::uint64_t pollBlockingReturns = 0;
  std::uint64_t pollContextSampleFailures = 0;
};
#endif

/// Monitors events on a collection of sockets.
class SocketMonitor {
public:
  class Strategy;

  SocketMonitor(int timeout = 0);
  virtual ~SocketMonitor();

  bool addConnect(socket_handle socket);
  bool addRead(socket_handle socket);
  bool addWrite(socket_handle socket);
  bool drop(socket_handle socket);
#if defined(QUICKFIX_DIRECT_READ_POLL)
  bool dropDirect(socket_handle socket);
#endif
  void signal(socket_handle socket);
  void unsignal(socket_handle socket);
  void block(Strategy &strategy, bool poll = 0, double timeout = 0.0);
  void setBusyPoll(bool enabled);
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS)
  void setDiagnosticsEnabled(bool enabled);
  void resetDiagnostics();
  SocketMonitorDiagnostics diagnostics() const;
#endif

  size_t numSockets() { return m_readSockets.size() - 1; }

private:
  typedef std::set<socket_handle> Sockets;
  typedef std::queue<socket_handle> Queue;

  void setsockopt();
  bool bind();
  bool listen();
  void buildSet(const Sockets &, struct pollfd *pfds, short events);
  inline int getTimeval(bool poll, double timeout);
  inline bool sleepIfEmpty(bool poll);
  bool busyPollTimeoutElapsed();

  void processRead(Strategy &, socket_handle socket_fd);
  void processWrite(Strategy &, socket_handle socket_fd);
  void processError(Strategy &, socket_handle socket_fd);
  void processPollList(Strategy &strategy, struct pollfd *pfds, unsigned pfds_size);

  int m_timeout;
  bool m_busyPoll;
  using Clock = std::chrono::steady_clock;
  Clock::time_point m_ticks;
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS)
  std::atomic<bool> m_diagnosticsEnabled{false};
  std::atomic<std::uint64_t> m_pollCalls{0};
  std::atomic<std::uint64_t> m_pollWaitNanoseconds{0};
  std::atomic<std::uint64_t> m_pollImmediateReturns{0};
  std::atomic<std::uint64_t> m_pollBlockingReturns{0};
  std::atomic<std::uint64_t> m_pollContextSampleFailures{0};
#endif

  socket_handle m_signal;
  socket_handle m_interrupt;
  Sockets m_connectSockets;
  Sockets m_readSockets;
  Sockets m_writeSockets;
  Queue m_dropped;

public:
  class Strategy {
  public:
    virtual ~Strategy() {}
    virtual void onConnect(SocketMonitor &, socket_handle socket) = 0;
    virtual void onEvent(SocketMonitor &, socket_handle socket) = 0;
    virtual void onWrite(SocketMonitor &, socket_handle socket) = 0;
    virtual void onError(SocketMonitor &, socket_handle socket) = 0;
    virtual void onError(SocketMonitor &) = 0;
    virtual void onTimeout(SocketMonitor &) {}
  };
};
} // namespace FIX

#endif //_MSC_VER

#endif // FIX_SOCKETMONITOR_UNIX_H
