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
#include "Session.h"
#include "Settings.h"
#include "SocketAcceptor.h"
#include "Utility.h"

#if (defined(QUICKFIX_BUSY_POLL) || defined(QUICKFIX_DIRECT_READ_POLL)) && !defined(_MSC_VER)
#include <cstring>
#include <pthread.h>
#include <sched.h>
#endif

namespace FIX {
#if (defined(QUICKFIX_BUSY_POLL) || defined(QUICKFIX_DIRECT_READ_POLL)) && !defined(_MSC_VER)
namespace {

/**
 * @brief Pins the calling acceptor network thread to one logical CPU.
 *
 * @param cpu Zero-based logical CPU index supplied by `SocketBusyPollCpu`.
 * @return An empty string on success, or the operating-system error text on failure.
 */
std::string setCurrentThreadAffinity(int cpu) {
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu, &cpuset);

  const int result = pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
  if (result == 0) {
    return std::string();
  }
  return std::strerror(result);
}

} // namespace
#endif

SocketAcceptor::SocketAcceptor(Application &application, MessageStoreFactory &factory, const SessionSettings &settings)
    EXCEPT(ConfigError)
    : Acceptor(application, factory, settings),
      m_pServer(0),
      m_busyPoll(false),
#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
      m_directReadPoll(false),
      m_directTimeoutTick(std::chrono::steady_clock::now()),
#endif
      m_busyPollCpu(-1) {}

SocketAcceptor::SocketAcceptor(
    Application &application,
    MessageStoreFactory &factory,
    const SessionSettings &settings,
    LogFactory &logFactory) EXCEPT(ConfigError)
    : Acceptor(application, factory, settings, logFactory),
      m_pServer(0),
      m_busyPoll(false),
#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
      m_directReadPoll(false),
      m_directTimeoutTick(std::chrono::steady_clock::now()),
#endif
      m_busyPollCpu(-1) {}

SocketAcceptor::~SocketAcceptor() {
  SocketConnections::iterator iter;
  for (iter = m_connections.begin(); iter != m_connections.end(); ++iter) {
    delete iter->second;
  }
}

/**
 * @brief Validates acceptor socket and polling settings before network resources are created.
 *
 * This step checks valid mode names, compile-time availability, and CPU index values. The settings are applied later
 * by `onInitialize()`.
 *
 * @param sessionSettings Settings for every FIX session owned by this acceptor.
 */
void SocketAcceptor::onConfigure(const SessionSettings &sessionSettings) EXCEPT(ConfigError) {
  for (const SessionID &sessionID : sessionSettings.getSessions()) {
    const Dictionary &settings = sessionSettings.get(sessionID);
    settings.getInt(SOCKET_ACCEPT_PORT);
    if (settings.has(SOCKET_REUSE_ADDRESS)) {
      settings.getBool(SOCKET_REUSE_ADDRESS);
    }
    if (settings.has(SOCKET_NODELAY)) {
      settings.getBool(SOCKET_NODELAY);
    }
    if (settings.has(SOCKET_BUSY_POLL)) {
      settings.getBool(SOCKET_BUSY_POLL);
    }
    if (settings.has(SOCKET_BUSY_POLL_MODE)) {
      if (settings.has(SOCKET_BUSY_POLL)) {
        throw ConfigError(std::string(SOCKET_BUSY_POLL_MODE) + " cannot be combined with " + SOCKET_BUSY_POLL);
      }

      const std::string mode = settings.getString(SOCKET_BUSY_POLL_MODE);
      if (mode != "blocking" && mode != "poll0" && mode != "direct") {
        throw ConfigError(std::string(SOCKET_BUSY_POLL_MODE) + " must be blocking, poll0, or direct");
      }
#if !defined(QUICKFIX_BUSY_POLL)
      if (mode == "poll0") {
        throw ConfigError(std::string(SOCKET_BUSY_POLL_MODE) + "=poll0 requires QUICKFIX_BUSY_POLL=ON");
      }
#endif
#if !defined(QUICKFIX_DIRECT_READ_POLL) || defined(_MSC_VER)
      if (mode == "direct") {
        throw ConfigError(
            std::string(SOCKET_BUSY_POLL_MODE) + "=direct requires a UNIX build with QUICKFIX_DIRECT_READ_POLL=ON");
      }
#endif
    }
    if (settings.has(SOCKET_BUSY_POLL_CPU)) {
      const int cpu = settings.getInt(SOCKET_BUSY_POLL_CPU);
      if (cpu < 0) {
        throw ConfigError(std::string(SOCKET_BUSY_POLL_CPU) + " must be greater than or equal to zero");
      }
    }
  }
}

#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
void SocketAcceptor::setNetworkDiagnosticsEnabled(bool enabled) {
  if (enabled) {
    if (m_pServer) {
      m_pServer->getMonitor().setDiagnosticsEnabled(true);
    }
    m_networkDiagnosticsEnabled.store(true, std::memory_order_release);
  } else {
    m_networkDiagnosticsEnabled.store(false, std::memory_order_release);
    if (m_pServer) {
      m_pServer->getMonitor().setDiagnosticsEnabled(false);
    }
  }
}

void SocketAcceptor::resetNetworkDiagnostics() {
  m_recvCalls.store(0, std::memory_order_relaxed);
  m_recvBytes.store(0, std::memory_order_relaxed);
  m_parsedMessages.store(0, std::memory_order_relaxed);
  if (m_pServer) {
    m_pServer->getMonitor().resetDiagnostics();
  }
}

SocketAcceptorDiagnostics SocketAcceptor::networkDiagnostics() const {
  SocketAcceptorDiagnostics result;
  if (m_pServer) {
    const SocketMonitorDiagnostics monitor = m_pServer->getMonitor().diagnostics();
    result.pollCalls = monitor.pollCalls;
    result.pollWaitNanoseconds = monitor.pollWaitNanoseconds;
    result.pollImmediateReturns = monitor.pollImmediateReturns;
    result.pollBlockingReturns = monitor.pollBlockingReturns;
    result.pollContextSampleFailures = monitor.pollContextSampleFailures;
  }
  result.recvCalls = m_recvCalls.load(std::memory_order_relaxed);
  result.recvBytes = m_recvBytes.load(std::memory_order_relaxed);
  result.parsedMessages = m_parsedMessages.load(std::memory_order_relaxed);
  return result;
}

void SocketAcceptor::recordNetworkReceive(std::uint64_t bytes) {
  if (!m_networkDiagnosticsEnabled.load(std::memory_order_acquire)) {
    return;
  }
  m_recvCalls.fetch_add(1, std::memory_order_relaxed);
  m_recvBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void SocketAcceptor::recordParsedMessage() {
  if (m_networkDiagnosticsEnabled.load(std::memory_order_acquire)) {
    m_parsedMessages.fetch_add(1, std::memory_order_relaxed);
  }
}
#endif

/**
 * @brief Creates listening sockets and translates session settings into the acceptor's runtime polling mode.
 *
 * All sessions share one `SocketServer` and one network thread, so `SocketBusyPollMode` and `SocketBusyPollCpu` must
 * agree across every configured session. `blocking` leaves both optimized mode flags disabled, `poll0` enables the
 * zero-timeout path in `SocketMonitor::block()`, and `direct` selects the separate direct-scan loop.
 *
 * @param sessionSettings Settings used to create listening sockets and select the polling mode.
 */
void SocketAcceptor::onInitialize(const SessionSettings &sessionSettings) EXCEPT(RuntimeError) {
  uint16_t port = 0;
  m_busyPoll = false;
#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
  m_directReadPoll = false;
  m_directTimeoutTick = std::chrono::steady_clock::now();
#endif
  m_busyPollCpu = -1;
  std::string configuredPollMode;

  try {
    m_pServer = new SocketServer(1);

    for (const SessionID &sessionID : sessionSettings.getSessions()) {
      const Dictionary &settings = sessionSettings.get(sessionID);
      port = (short)settings.getInt(SOCKET_ACCEPT_PORT);

      const bool reuseAddress = settings.has(SOCKET_REUSE_ADDRESS) ? settings.getBool(SOCKET_REUSE_ADDRESS) : true;

      const bool noDelay = settings.has(SOCKET_NODELAY) ? settings.getBool(SOCKET_NODELAY) : false;

      const int sendBufSize = settings.has(SOCKET_SEND_BUFFER_SIZE) ? settings.getInt(SOCKET_SEND_BUFFER_SIZE) : 0;

      const int rcvBufSize = settings.has(SOCKET_RECEIVE_BUFFER_SIZE) ? settings.getInt(SOCKET_RECEIVE_BUFFER_SIZE) : 0;

      if (settings.has(SOCKET_BUSY_POLL)) {
        m_busyPoll = m_busyPoll || settings.getBool(SOCKET_BUSY_POLL);
      }
      if (settings.has(SOCKET_BUSY_POLL_MODE)) {
        const std::string mode = settings.getString(SOCKET_BUSY_POLL_MODE);
        if (!configuredPollMode.empty() && configuredPollMode != mode) {
          delete m_pServer;
          m_pServer = 0;
          throw RuntimeError(std::string(SOCKET_BUSY_POLL_MODE) + " must be the same for all acceptor sessions");
        }
        configuredPollMode = mode;
      }
      if (settings.has(SOCKET_BUSY_POLL_CPU)) {
        const int cpu = settings.getInt(SOCKET_BUSY_POLL_CPU);
        if (m_busyPollCpu >= 0 && m_busyPollCpu != cpu) {
          delete m_pServer;
          m_pServer = 0;
          throw RuntimeError(std::string(SOCKET_BUSY_POLL_CPU) + " must be the same for all acceptor sessions");
        }
        m_busyPollCpu = cpu;
      }

      socket_handle acceptSocket = m_pServer->add(port, reuseAddress, noDelay, sendBufSize, rcvBufSize);
      m_portToSessions[socket_hostport(acceptSocket)].insert(sessionID);
      m_sessionToPort[sessionID] = socket_hostport(acceptSocket);
    }
    // Convert the shared text setting into mutually exclusive runtime flags.
    if (!configuredPollMode.empty()) {
      m_busyPoll = configuredPollMode == "poll0";
#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
      m_directReadPoll = configuredPollMode == "direct";
#endif
    }
#if defined(QUICKFIX_BUSY_POLL)
    m_pServer->setBusyPoll(m_busyPoll);
#endif
  } catch (SocketException &e) {
    delete m_pServer;
    m_pServer = 0;
    throw RuntimeError(
        "Unable to create, bind, or listen to port " + IntConvertor::convert((unsigned short)port) + " (" + e.what()
        + ")");
  }
}

/**
 * @brief Runs the acceptor network thread until the engine is stopped.
 *
 * The blocking baseline and `poll0` both repeatedly call `SocketServer::block()` and therefore share socket-set
 * construction, event dispatch, and callbacks. Their only readiness-loop difference is the timeout passed to the
 * operating-system `poll()` call by `SocketMonitor`. Direct mode uses `runDirectScanOnce()` as a separate branch.
 *
 * When an optimized busy loop is selected, this function also applies the configured CPU affinity before entering
 * that loop.
 */
void SocketAcceptor::onStart() {
#if (defined(QUICKFIX_BUSY_POLL) || defined(QUICKFIX_DIRECT_READ_POLL)) && !defined(_MSC_VER)
  bool busyLoop = m_busyPoll;
#if defined(QUICKFIX_DIRECT_READ_POLL)
  busyLoop = busyLoop || m_directReadPoll;
#endif
  // Affinity applies to the acceptor network thread that is about to execute the busy loop.
  if (busyLoop && m_busyPollCpu >= 0) {
    const std::string error = setCurrentThreadAffinity(m_busyPollCpu);
    if (!error.empty() && getLog()) {
      std::stringstream stream;
      stream << "Unable to set busy-poll thread CPU affinity to " << m_busyPollCpu << ": " << error;
      getLog()->onEvent(stream.str());
    }
  }
#endif

#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
  // Direct mode bypasses SocketMonitor; blocking and poll0 deliberately share the branch below.
  if (m_directReadPoll) {
    while (!isStopped() && m_pServer && runDirectScanOnce()) {}
  } else
#endif
  {
    while (!isStopped() && m_pServer && m_pServer->block(*this)) {}
  }

  if (!m_pServer) {
    return;
  }

  time_t start = 0;
  time_t now = 0;

  ::time(&start);
#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
  if (m_directReadPoll) {
    while (isLoggedOn()) {
      runDirectScanOnce();
      if (::time(&now) - 5 >= start) {
        break;
      }
    }
  } else
#endif
  {
    while (isLoggedOn()) {
      m_pServer->block(*this);
      if (::time(&now) - 5 >= start) {
        break;
      }
    }
  }

  m_pServer->close();
  delete m_pServer;
  m_pServer = 0;
}

#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
/**
 * @brief Executes one complete iteration of the direct acceptor busy loop.
 *
 * One iteration attempts to accept at most one client, visits every active connection once for a non-blocking read
 * and queued write, and advances the existing FIX Session timers when one second has elapsed. `WouldBlock` results
 * mean that a socket made no progress in this iteration; they do not disconnect it.
 *
 * @return `false` when the server no longer exists; otherwise `true` so the caller can begin the next scan.
 */
bool SocketAcceptor::runDirectScanOnce() {
  if (!m_pServer) {
    return false;
  }

  // An empty non-blocking accept is normal; the next busy-loop iteration tries again.
  if (m_connections.size() < m_sessionToPort.size()) {
    const SocketServer::DirectAcceptResult acceptResult = m_pServer->acceptDirect();
    if (acceptResult.status == SocketServer::DirectAcceptStatus::Accepted) {
      onConnect(*m_pServer, acceptResult.acceptSocket, acceptResult.socket);
    } else if (acceptResult.status == SocketServer::DirectAcceptStatus::Error) {
      onError(*m_pServer);
    }
  }

  // Advance before callbacks because disconnectDirect() erases the current connection from this map.
  SocketConnections::iterator connection = m_connections.begin();
  while (connection != m_connections.end()) {
    const SocketConnections::iterator current = connection++;
    const socket_handle socket = current->first;
    SocketConnection *socketConnection = current->second;
    const SocketConnection::DirectReadResult readResult = socketConnection->readDirect(*this, *m_pServer);
    if (readResult == SocketConnection::DirectReadResult::Disconnect) {
      disconnectDirect(socket);
      continue;
    }

    const SocketConnection::DirectWriteResult writeResult = socketConnection->processQueueDirect();
    if (writeResult == SocketConnection::DirectWriteResult::Disconnect) {
      disconnectDirect(socket);
    }
  }

  // Direct mode never reaches SocketMonitor's poll timeout, so it must drive the original Session timers itself.
  const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
  if (now - m_directTimeoutTick >= std::chrono::seconds(1)) {
    onTimeout(*m_pServer);
    m_directTimeoutTick = now;
  }
  return true;
}

/**
 * @brief Closes one direct-mode socket and synchronously removes its QuickFIX connection state.
 *
 * @param socket Client socket that closed or produced a non-recoverable read/write error.
 */
void SocketAcceptor::disconnectDirect(socket_handle socket) {
  if (!m_pServer) {
    return;
  }
  m_pServer->getMonitor().dropDirect(socket);
  onDisconnect(*m_pServer, socket);
}
#endif

/**
 * @brief Performs one externally requested, non-blocking acceptor iteration.
 *
 * @return `false` when the acceptor has stopped or cannot continue; otherwise `true`.
 */
bool SocketAcceptor::onPoll() {
  if (!m_pServer) {
    return false;
  }

  time_t start = 0;
  time_t now = 0;

  if (isStopped()) {
    if (start == 0) {
      ::time(&start);
    }
    if (!isLoggedOn()) {
      start = 0;
      return false;
    }
    if (::time(&now) - 5 >= start) {
      start = 0;
      return false;
    }
  }

#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
  if (m_directReadPoll) {
    runDirectScanOnce();
    return true;
  }
#endif

  m_pServer->block(*this, true);
  return true;
}

void SocketAcceptor::onStop() {
  if (m_pServer) {
    m_pServer->close();
  }
}

/**
 * @brief Registers an accepted client socket as a QuickFIX `SocketConnection`.
 *
 * @param server Server that owns the listening socket and socket monitor.
 * @param a Listening socket that accepted the connection.
 * @param s Newly accepted client socket.
 */
void SocketAcceptor::onConnect(SocketServer &server, socket_handle a, socket_handle s) {
  if (!socket_isValid(s)) {
    return;
  }
  SocketConnections::iterator i = m_connections.find(s);
  if (i != m_connections.end()) {
    return;
  }
  uint16_t port = server.socketToPort(a);
  if (port == 0) {
    port = socket_hostport(a);
  }
  Sessions sessions = m_portToSessions[port];
  SocketConnection *connection = new SocketConnection(s, sessions, &server.getMonitor());
#if defined(QUICKFIX_DIRECT_READ_POLL) && !defined(_MSC_VER)
  // This flag also makes Session responses wait for processQueueDirect() instead of the poll-based write path.
  connection->setDirectReadPoll(m_directReadPoll);
#endif
  m_connections[s] = connection;

  std::stringstream stream;
  stream << "Accepted connection from " << socket_peername(s) << " on port " << port;

  if (getLog()) {
    getLog()->onEvent(stream.str());
  }
}

/**
 * @brief Continues sending queued bytes after `poll()` reports that a client socket is writable.
 *
 * @param server Server that dispatched the writable event.
 * @param s Writable client socket.
 */
void SocketAcceptor::onWrite(SocketServer &server, socket_handle s) {
  SocketConnections::iterator i = m_connections.find(s);
  if (i == m_connections.end()) {
    return;
  }
  SocketConnection *pSocketConnection = i->second;
  if (pSocketConnection->processQueue()) {
    pSocketConnection->unsignal();
  }
}

/**
 * @brief Reads available bytes from a client socket and passes them into the FIX parsing/session path.
 *
 * This is the final callback in the normal `poll()` readiness chain. `SocketConnection::read()` performs the actual
 * `recv()` and forwards complete messages through `Parser`, `Session`, and the application.
 *
 * @param server Server that dispatched the readable event.
 * @param s Readable client socket.
 * @return `true` while the connection remains usable; `false` when it should be disconnected.
 */
bool SocketAcceptor::onData(SocketServer &server, socket_handle s) {
  SocketConnections::iterator i = m_connections.find(s);
  if (i == m_connections.end()) {
    return false;
  }
  SocketConnection *pSocketConnection = i->second;
  return pSocketConnection->read(*this, server);
}

void SocketAcceptor::onDisconnect(SocketServer &, socket_handle s) {
  SocketConnections::iterator i = m_connections.find(s);
  if (i == m_connections.end()) {
    return;
  }
  SocketConnection *pSocketConnection = i->second;

  Session *pSession = pSocketConnection->getSession();
  if (pSession) {
    pSession->disconnect();
  }

  delete pSocketConnection;
  m_connections.erase(s);
}

void SocketAcceptor::onError(SocketServer &) {}

/**
 * @brief Advances timeout handling for every active FIX connection.
 *
 * In the blocking and `poll0` paths this callback originates in `SocketMonitor::block()`. Each connection then
 * advances the existing Session heartbeat, TestRequest, logon, logout, and disconnect timers.
 *
 * The unused `SocketServer` argument identifies the server whose monitor produced the timeout event.
 */
void SocketAcceptor::onTimeout(SocketServer &) {
  SocketConnections::iterator i;
  for (i = m_connections.begin(); i != m_connections.end(); ++i) {
    i->second->onTimeout();
  }
}
} // namespace FIX
