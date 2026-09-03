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

#include "config.h"

#include "SocketMonitor.h"
#include "Utility.h"
#include <algorithm>
#include <exception>
#include <iostream>
#include <set>
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS)
#include <sys/resource.h>
#endif

namespace FIX {
/**
 * @brief Creates a UNIX socket monitor and its internal wake-up socket pair.
 *
 * @param timeout Interval in seconds used for normal timeout callbacks and FIX session timer progress.
 */
SocketMonitor::SocketMonitor(int timeout)
    : m_timeout(timeout),
      m_busyPoll(false) {
  socket_init();

  std::pair<socket_handle, socket_handle> sockets = socket_createpair();
  m_signal = sockets.first;
  m_interrupt = sockets.second;
  socket_setnonblock(m_signal);
  socket_setnonblock(m_interrupt);
  m_readSockets.insert(m_interrupt);

  m_ticks = Clock::now();
}

SocketMonitor::~SocketMonitor() {
  Sockets::iterator i;
  for (i = m_readSockets.begin(); i != m_readSockets.end(); ++i) {
    socket_close(*i);
  }

  socket_close(m_signal);
  socket_term();
}

/**
 * @brief Registers a newly accepted non-blocking socket for connect-completion handling.
 *
 * @param s Newly accepted client socket.
 * @return `true` when inserted, or `false` if the socket was already registered.
 */
bool SocketMonitor::addConnect(socket_handle s) {
  socket_setnonblock(s);
  Sockets::iterator i = m_connectSockets.find(s);
  if (i != m_connectSockets.end()) {
    return false;
  }

  m_connectSockets.insert(s);
  return true;
}

/**
 * @brief Registers a non-blocking socket for `POLLIN` readiness checks.
 *
 * Listening sockets and established client sockets both use this set.
 *
 * @param s Socket to monitor for readable data.
 * @return `true` when inserted, or `false` if the socket was already registered.
 */
bool SocketMonitor::addRead(socket_handle s) {
  socket_setnonblock(s);
  Sockets::iterator i = m_readSockets.find(s);
  if (i != m_readSockets.end()) {
    return false;
  }

  m_readSockets.insert(s);
  return true;
}

/**
 * @brief Registers an established non-blocking socket for `POLLOUT` readiness checks.
 *
 * @param s Socket whose send queue still contains bytes.
 * @return `true` when inserted, or `false` if the socket is not readable-registered or is already in the write set.
 */
bool SocketMonitor::addWrite(socket_handle s) {
  if (m_readSockets.find(s) == m_readSockets.end()) {
    return false;
  }

  socket_setnonblock(s);
  Sockets::iterator i = m_writeSockets.find(s);
  if (i != m_writeSockets.end()) {
    return false;
  }

  m_writeSockets.insert(s);
  return true;
}

bool SocketMonitor::drop(socket_handle s) {
  Sockets::iterator i = m_readSockets.find(s);
  Sockets::iterator j = m_writeSockets.find(s);
  Sockets::iterator k = m_connectSockets.find(s);

  if (i != m_readSockets.end() || j != m_writeSockets.end() || k != m_connectSockets.end()) {
    socket_close(s);
    m_readSockets.erase(s);
    m_writeSockets.erase(s);
    m_connectSockets.erase(s);
    m_dropped.push(s);
    return true;
  }
  return false;
}

#if defined(QUICKFIX_DIRECT_READ_POLL)
/**
 * @brief Immediately closes and removes a socket owned by the direct scan path.
 *
 * The normal `drop()` path queues a later monitor error callback. Direct mode invokes `onDisconnect()` itself, so
 * this variant also removes any deferred entry to prevent duplicate connection cleanup.
 *
 * @param s Client socket to close and remove from every monitor set.
 * @return `true` when the socket was present in a monitor set or deferred-drop queue.
 */
bool SocketMonitor::dropDirect(socket_handle s) {
  Sockets::iterator read = m_readSockets.find(s);
  Sockets::iterator write = m_writeSockets.find(s);
  Sockets::iterator connect = m_connectSockets.find(s);
  bool removed = false;

  if (read != m_readSockets.end() || write != m_writeSockets.end() || connect != m_connectSockets.end()) {
    socket_close(s);
    m_readSockets.erase(s);
    m_writeSockets.erase(s);
    m_connectSockets.erase(s);
    removed = true;
  }

  Queue retained;
  while (!m_dropped.empty()) {
    if (m_dropped.front() == s) {
      removed = true;
    } else {
      retained.push(m_dropped.front());
    }
    m_dropped.pop();
  }
  m_dropped.swap(retained);
  return removed;
}
#endif

/**
 * @brief Converts the monitor timeout into the millisecond value expected by `poll()`.
 *
 * @param poll `true` to force an immediate, non-blocking monitor iteration.
 * @param timeout Per-call timeout value; the current UNIX implementation replaces it with `m_timeout`.
 * @return Remaining wait time in milliseconds, or zero for an immediate check.
 */
inline int SocketMonitor::getTimeval(bool poll, double timeout) {
  if (poll) {
    return 0;
  }

  timeout = m_timeout;

  if (!timeout) {
    return 0;
  }

  const double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(Clock::now() - m_ticks).count();
  if (elapsed >= timeout || elapsed == 0.0) {
    m_ticks = Clock::now();
    return (timeout * 1000);
  } else {
    return ((timeout - elapsed) * 1000);
  }
  return (timeout * 1000);
}

/**
 * @brief Sleeps when no sockets are registered and the caller did not request an immediate check.
 *
 * @param poll `true` when the caller requires a non-blocking iteration.
 * @return `true` when this function slept instead of calling `poll()`.
 */
bool SocketMonitor::sleepIfEmpty(bool poll) {
  if (poll) {
    return false;
  }

  if (m_readSockets.empty() && m_writeSockets.empty() && m_connectSockets.empty()) {
    process_sleep(m_timeout);
    return true;
  } else {
    return false;
  }
}

/**
 * @brief Wakes the monitor and requests future `POLLOUT` notifications for one socket.
 *
 * @param socket Socket with queued bytes waiting to be sent.
 */
void SocketMonitor::signal(socket_handle socket) { socket_send(m_signal, (char *)&socket, sizeof(socket)); }

/**
 * @brief Removes a socket from the writable-event set after its send queue is drained.
 *
 * @param s Socket that no longer needs `POLLOUT` notifications.
 */
void SocketMonitor::unsignal(socket_handle s) {
  Sockets::iterator i = m_writeSockets.find(s);
  if (i == m_writeSockets.end()) {
    return;
  }

  m_writeSockets.erase(s);
}

/**
 * @brief Selects whether the operating-system `poll()` call uses a zero timeout.
 *
 * @param enabled `true` for the `poll0` busy loop; `false` for the original timed blocking behavior.
 */
void SocketMonitor::setBusyPoll(bool enabled) { m_busyPoll = enabled; }

#if defined(QUICKFIX_NETWORK_DIAGNOSTICS)
void SocketMonitor::setDiagnosticsEnabled(bool enabled) {
  m_diagnosticsEnabled.store(enabled, std::memory_order_release);
}

void SocketMonitor::resetDiagnostics() {
  m_pollCalls.store(0, std::memory_order_relaxed);
  m_pollWaitNanoseconds.store(0, std::memory_order_relaxed);
  m_pollImmediateReturns.store(0, std::memory_order_relaxed);
  m_pollBlockingReturns.store(0, std::memory_order_relaxed);
  m_pollContextSampleFailures.store(0, std::memory_order_relaxed);
}

SocketMonitorDiagnostics SocketMonitor::diagnostics() const {
  SocketMonitorDiagnostics result;
  result.pollCalls = m_pollCalls.load(std::memory_order_relaxed);
  result.pollWaitNanoseconds = m_pollWaitNanoseconds.load(std::memory_order_relaxed);
  result.pollImmediateReturns = m_pollImmediateReturns.load(std::memory_order_relaxed);
  result.pollBlockingReturns = m_pollBlockingReturns.load(std::memory_order_relaxed);
  result.pollContextSampleFailures = m_pollContextSampleFailures.load(std::memory_order_relaxed);
  return result;
}
#endif

/**
 * @brief Preserves periodic timeout callbacks while `poll0` returns immediately on empty checks.
 *
 * @return `true` once the configured timeout interval has elapsed.
 */
bool SocketMonitor::busyPollTimeoutElapsed() {
  if (!m_timeout) {
    return false;
  }

  const double elapsed = std::chrono::duration_cast<std::chrono::duration<double>>(Clock::now() - m_ticks).count();
  if (elapsed < m_timeout) {
    return false;
  }

  m_ticks = Clock::now();
  return true;
}

/**
 * @brief Performs one readiness check and dispatches the resulting socket events.
 *
 * Read, connect, and write socket sets are converted into one `pollfd` array. In the baseline path `poll()` waits for
 * readiness or the configured timeout. In `poll0`, only the timeout argument becomes zero; construction of `pfds`,
 * interpretation of `revents`, and all strategy callbacks remain unchanged.
 *
 * @param strategy Callback target used to deliver connect, read, write, error, and timeout events.
 * @param should_poll `true` to force an immediate iteration independently of the configured busy-poll mode.
 * @param timeout Per-call timeout forwarded to `getTimeval()`; the UNIX implementation currently uses `m_timeout`.
 */
void SocketMonitor::block(Strategy &strategy, bool should_poll, double timeout) {
  while (m_dropped.size()) {
    strategy.onError(*this, m_dropped.front());
    m_dropped.pop();
    if (m_dropped.size() == 0) {
      return;
    }
  }

  int pfds_size = m_readSockets.size() + m_connectSockets.size() + m_writeSockets.size();
  struct pollfd pfds[pfds_size];
  buildSet(m_readSockets, pfds, POLLPRI | POLLIN);
  buildSet(m_connectSockets, pfds + m_readSockets.size(), POLLOUT | POLLERR);
  buildSet(m_writeSockets, pfds + m_readSockets.size() + m_connectSockets.size(), POLLOUT);

  if (sleepIfEmpty(should_poll)) {
    strategy.onTimeout(*this);
    return;
  }

  int result;
  do {
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS)
    const bool diagnosticsEnabled = m_diagnosticsEnabled.load(std::memory_order_acquire);
    struct rusage usageBefore {};
    const bool sampledContextSwitches = diagnosticsEnabled && getrusage(RUSAGE_THREAD, &usageBefore) == 0;
    const Clock::time_point pollStart = diagnosticsEnabled ? Clock::now() : Clock::time_point();
#endif
    // poll0 changes only this timeout value; the shared event-dispatch path below is unchanged.
    result = poll(pfds, pfds_size, m_busyPoll ? 0 : getTimeval(should_poll, timeout));
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS)
    if (diagnosticsEnabled) {
      const std::uint64_t elapsedNanoseconds = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - pollStart).count());
      m_pollCalls.fetch_add(1, std::memory_order_relaxed);
      m_pollWaitNanoseconds.fetch_add(elapsedNanoseconds, std::memory_order_relaxed);

      struct rusage usageAfter {};
      if (sampledContextSwitches && getrusage(RUSAGE_THREAD, &usageAfter) == 0) {
        if (usageAfter.ru_nvcsw > usageBefore.ru_nvcsw) {
          m_pollBlockingReturns.fetch_add(1, std::memory_order_relaxed);
        } else {
          m_pollImmediateReturns.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        m_pollContextSampleFailures.fetch_add(1, std::memory_order_relaxed);
      }
    }
#endif
  } while (result < 0 && errno == EINTR);

  if (result == 0) {
    if (m_busyPoll && !busyPollTimeoutElapsed()) {
      return;
    }
    strategy.onTimeout(*this);
    return;
  } else if (result > 0) {
    processPollList(strategy, pfds, pfds_size);
  } else {
    strategy.onError(*this);
  }
}

/**
 * @brief Dispatches a readable socket from the `pollfd` array.
 *
 * The internal interrupt socket carries descriptors that should join the write set. Every other readable descriptor
 * is forwarded to `Strategy::onEvent()`, eventually reaching `SocketAcceptor::onData()` for a client socket.
 *
 * @param strategy Callback target for ordinary readable sockets.
 * @param socket_fd Descriptor marked with `POLLIN` or `POLLPRI`.
 */
void SocketMonitor::processRead(Strategy &strategy, socket_handle socket_fd) {
  int s = socket_fd;
  if (s == m_interrupt) {
    socket_handle socket = 0;
    recv(s, (char *)&socket, sizeof(socket), 0);
    addWrite(socket);
  } else {
    strategy.onEvent(*this, s);
  }
}

/**
 * @brief Dispatches a writable socket from the `pollfd` array.
 *
 * A newly accepted socket is first moved from the connect set to the read set. Established sockets are forwarded to
 * `Strategy::onWrite()` so queued bytes can continue sending.
 *
 * @param strategy Callback target for connect completion or queued writes.
 * @param socket_fd Descriptor marked with `POLLOUT`.
 */
void SocketMonitor::processWrite(Strategy &strategy, socket_handle socket_fd) {
  socket_handle s = socket_fd;
  if (m_connectSockets.find(s) != m_connectSockets.end()) {
    m_connectSockets.erase(s);
    m_readSockets.insert(s);
    strategy.onConnect(*this, s);
  } else {
    strategy.onWrite(*this, s);
  }
}

/**
 * @brief Forwards a socket-specific error to the monitor strategy.
 *
 * @param strategy Callback target for the error.
 * @param socket_fd Descriptor marked with `POLLERR` or a terminal `POLLHUP`.
 */
void SocketMonitor::processError(Strategy &strategy, socket_handle socket_fd) { strategy.onError(*this, socket_fd); }

/**
 * @brief Interprets the readiness flags written by `poll()` and dispatches each matching callback.
 *
 * @param strategy Callback target for all resulting events.
 * @param pfds Array whose `events` fields describe requested states and whose `revents` fields contain actual states.
 * @param pfds_size Number of entries in `pfds`.
 */
void SocketMonitor::processPollList(Strategy &strategy, struct pollfd *pfds, unsigned pfds_size) {
  for (unsigned i = 0; i < pfds_size; ++i) {
    if ((pfds[i].revents & POLLIN) || (pfds[i].revents & POLLPRI)) {
      processRead(strategy, pfds[i].fd);
    }

    if ((pfds[i].revents & POLLOUT)) {
      processWrite(strategy, pfds[i].fd);
    }
    if ((pfds[i].revents & POLLERR) || ((pfds[i].revents & POLLHUP) && !(pfds[i].revents & POLLIN))) {
      processError(strategy, pfds[i].fd);
    }
  }
}

/**
 * @brief Appends one socket category to the `pollfd` array used by `poll()`.
 *
 * @param sockets Descriptors belonging to the same read, connect, or write category.
 * @param pfds Destination position for the first descriptor in this category.
 * @param events Readiness flags to request for every descriptor in the category.
 */
void SocketMonitor::buildSet(const Sockets &sockets, struct pollfd *pfds, short events) {
  Sockets::const_iterator iter;
  unsigned int i = 0;
  for (iter = sockets.begin(); iter != sockets.end(); ++iter) {
    pfds[i].fd = *iter;
    pfds[i].events = events;
    pfds[i].revents = 0;
    i += 1;
  }
}

} // namespace FIX

#endif
