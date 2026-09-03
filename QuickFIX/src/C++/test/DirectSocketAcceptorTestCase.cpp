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

#include "TestHelper.h"

#include <MessageStore.h>
#include <Parser.h>
#include <Session.h>
#include <SessionSettings.h>
#include <SocketAcceptor.h>
#include <Utility.h>
#include <fix42/Heartbeat.h>
#include <fix42/Logon.h>
#include <fix42/Logout.h>
#include <fix42/NewOrderSingle.h>
#include <fix42/News.h>
#include <cerrno>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"

namespace {

class DirectTestApplication : public FIX::NullApplication {
public:
  void onLogon(const FIX::SessionID &sessionID) override {
    ++logons;
    ++sessionLogons[sessionID];
  }

  void onLogout(const FIX::SessionID &) override { ++logouts; }

  void fromApp(const FIX::Message &, const FIX::SessionID &sessionID)
      EXCEPT(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::UnsupportedMessageType)
          override {
    ++applicationMessages;
    ++sessionApplicationMessages[sessionID];
    if (!largeResponsePayload.empty()) {
      FIX42::News response(FIX::Headline("DIRECT-BACKPRESSURE"));
      response.set(FIX::RawDataLength(static_cast<int>(largeResponsePayload.size())));
      response.set(FIX::RawData(largeResponsePayload));
      if (FIX::Session::sendToTarget(response, sessionID)) {
        ++acceptedLargeResponses;
        largeResponseWire = response.toString();
      }
    }
    for (int i = 0; i < responsesPerApplicationMessage; ++i) {
      FIX42::Heartbeat response;
      response.set(FIX::TestReqID("DIRECT-RESPONSE-" + std::to_string(i)));
      if (FIX::Session::sendToTarget(response, sessionID)) {
        ++acceptedResponses;
      }
    }
  }

  int logons = 0;
  int logouts = 0;
  int applicationMessages = 0;
  int responsesPerApplicationMessage = 0;
  int acceptedResponses = 0;
  int acceptedLargeResponses = 0;
  std::string largeResponsePayload;
  std::string largeResponseWire;
  std::map<FIX::SessionID, int> sessionLogons;
  std::map<FIX::SessionID, int> sessionApplicationMessages;
};

class DirectAcceptorFixture {
public:
  ~DirectAcceptorFixture() {
    if (acceptor) {
      acceptor->stop(true);
    }
    for (FIX::socket_handle &socket : clientSockets) {
      if (FIX::socket_isValid(socket)) {
        FIX::socket_close(socket);
        FIX::socket_invalidate(socket);
      }
    }
  }

  FIX::socket_handle connect(uint16_t port, int receiveBufferSize = 0) {
    const FIX::socket_handle socket = FIX::createSocket(port, "127.0.0.1");
    if (FIX::socket_isValid(socket)) {
      FIX::socket_setsockopt(socket, TCP_NODELAY);
      if (receiveBufferSize > 0) {
        FIX::socket_setsockopt(socket, SO_RCVBUF, receiveBufferSize);
      }
    }
    clientSockets.push_back(socket);
    return socket;
  }

  void closeClient(FIX::socket_handle socket) {
    for (FIX::socket_handle &tracked : clientSockets) {
      if (tracked == socket) {
        FIX::socket_close(tracked);
        FIX::socket_invalidate(tracked);
        return;
      }
    }
  }

  DirectTestApplication application;
  FIX::MemoryStoreFactory storeFactory;
  std::unique_ptr<FIX::SocketAcceptor> acceptor;
  std::vector<FIX::socket_handle> clientSockets;
};

FIX::SessionSettings directAcceptorSettings(int sendBufferSize = 0, int logoutTimeout = 2) {
  std::ostringstream configuration;
  configuration << "[DEFAULT]\n"
                << "ConnectionType=acceptor\n"
                << "SocketAcceptPort=0\n"
                << "SocketReuseAddress=Y\n"
                << "SocketNodelay=Y\n"
                << "SocketBusyPollMode=direct\n"
                << "StartTime=00:00:00\n"
                << "EndTime=00:00:00\n"
                << "UseDataDictionary=N\n"
                << "CheckLatency=N\n"
                << "PersistMessages=N\n"
                << "LogoutTimeout=" << logoutTimeout << "\n";
  if (sendBufferSize > 0) {
    configuration << "SocketSendBufferSize=" << sendBufferSize << "\n";
  }
  configuration << "[SESSION]\n"
                << "BeginString=FIX.4.2\n"
                << "SenderCompID=ACCEPT\n"
                << "TargetCompID=INITIATOR\n"
                << "HeartBtInt=30\n";
  std::istringstream stream(configuration.str());
  return FIX::SessionSettings(stream);
}

FIX::SessionSettings directMultiAcceptorSettings() {
  std::istringstream stream(
      "[DEFAULT]\n"
      "ConnectionType=acceptor\n"
      "SocketAcceptPort=0\n"
      "SocketReuseAddress=Y\n"
      "SocketNodelay=Y\n"
      "SocketBusyPollMode=direct\n"
      "StartTime=00:00:00\n"
      "EndTime=00:00:00\n"
      "UseDataDictionary=N\n"
      "CheckLatency=N\n"
      "PersistMessages=N\n"
      "[SESSION]\n"
      "BeginString=FIX.4.2\n"
      "SenderCompID=ACCEPT\n"
      "TargetCompID=INITIATOR_A\n"
      "HeartBtInt=30\n"
      "[SESSION]\n"
      "BeginString=FIX.4.2\n"
      "SenderCompID=ACCEPT\n"
      "TargetCompID=INITIATOR_B\n"
      "HeartBtInt=30\n");
  return FIX::SessionSettings(stream);
}

std::string makeLogon(
    int sequenceNumber = 1,
    const std::string &senderCompID = "INITIATOR",
    int heartBtInt = 30) {
  FIX42::Logon message;
  message.getHeader().set(FIX::SenderCompID(senderCompID));
  message.getHeader().set(FIX::TargetCompID("ACCEPT"));
  message.getHeader().set(FIX::MsgSeqNum(sequenceNumber));
  message.getHeader().set(FIX::SendingTime::now());
  message.set(FIX::EncryptMethod(FIX::EncryptMethod_NONE_OTHER));
  message.set(FIX::HeartBtInt(heartBtInt));
  return message.toString();
}

std::string makeHeartbeat(
    int sequenceNumber,
    const std::string &testRequestID = std::string(),
    const std::string &senderCompID = "INITIATOR") {
  FIX42::Heartbeat message;
  message.getHeader().set(FIX::SenderCompID(senderCompID));
  message.getHeader().set(FIX::TargetCompID("ACCEPT"));
  message.getHeader().set(FIX::MsgSeqNum(sequenceNumber));
  message.getHeader().set(FIX::SendingTime::now());
  if (!testRequestID.empty()) {
    message.set(FIX::TestReqID(testRequestID));
  }
  return message.toString();
}

std::string makeLogout(int sequenceNumber, const std::string &senderCompID = "INITIATOR") {
  FIX42::Logout message;
  message.getHeader().set(FIX::SenderCompID(senderCompID));
  message.getHeader().set(FIX::TargetCompID("ACCEPT"));
  message.getHeader().set(FIX::MsgSeqNum(sequenceNumber));
  message.getHeader().set(FIX::SendingTime::now());
  return message.toString();
}

std::string makeNewOrderSingle(
    int sequenceNumber = 2,
    const std::string &senderCompID = "INITIATOR",
    const std::string &clientOrderID = "DIRECT-ORDER-1") {
  FIX42::NewOrderSingle message(
      FIX::ClOrdID(clientOrderID),
      FIX::HandlInst(FIX::HandlInst_AUTOMATED_EXECUTION_NO_INTERVENTION),
      FIX::Symbol("LNUX"),
      FIX::Side(FIX::Side_BUY),
      FIX::TransactTime::now(),
      FIX::OrdType(FIX::OrdType_MARKET));
  message.getHeader().set(FIX::SenderCompID(senderCompID));
  message.getHeader().set(FIX::TargetCompID("ACCEPT"));
  message.getHeader().set(FIX::MsgSeqNum(sequenceNumber));
  message.getHeader().set(FIX::SendingTime::now());
  message.set(FIX::OrderQty(100));
  return message.toString();
}

void sendAll(FIX::socket_handle socket, const std::string &data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const ssize_t sent = FIX::socket_send(socket, data.data() + offset, data.size() - offset);
    REQUIRE(sent > 0);
    offset += static_cast<std::size_t>(sent);
  }
}

std::vector<std::string> readMessages(
    FIX::socket_handle socket,
    std::size_t expectedMessages,
    FIX::SocketAcceptor *acceptor = nullptr,
    int maxAttempts = 2000) {
  FIX::Parser parser;
  std::vector<std::string> messages;

  for (int attempt = 0; attempt < maxAttempts && messages.size() < expectedMessages; ++attempt) {
    if (acceptor) {
      REQUIRE(acceptor->poll());
    }
    int available = 0;
    REQUIRE(FIX::socket_fionread(socket, available));
    if (available > 0) {
      std::string data(static_cast<std::size_t>(available), '\0');
      const ssize_t received = FIX::socket_recv(socket, data.data(), data.size());
      REQUIRE(received > 0);
      parser.addToStream(data.data(), static_cast<std::size_t>(received));

      std::string message;
      while (parser.readFixMessage(message)) {
        messages.push_back(message);
      }
    }
    if (messages.size() < expectedMessages) {
      FIX::process_sleep(0.001);
    }
  }
  return messages;
}

std::string messageType(const std::string &wireMessage) {
  FIX::Message message(wireMessage, false);
  FIX::MsgType type;
  message.getHeader().getField(type);
  return type.getValue();
}

bool waitForDisconnect(FIX::socket_handle socket, FIX::SocketAcceptor *acceptor = nullptr, int maxAttempts = 2000) {
  FIX::socket_setnonblock(socket);
  char byte;
  for (int attempt = 0; attempt < maxAttempts; ++attempt) {
    if (acceptor) {
      acceptor->poll();
    }
    const ssize_t received = FIX::socket_recv(socket, &byte, sizeof(byte));
    if (received == 0) {
      return true;
    }
    if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      return true;
    }
    FIX::process_sleep(0.001);
  }
  return false;
}

} // namespace

TEST_CASE("DirectSocketAcceptorTests") {
  DirectAcceptorFixture fixture;
  FIX::SessionSettings settings = directAcceptorSettings();
  fixture.acceptor = std::make_unique<FIX::SocketAcceptor>(fixture.application, fixture.storeFactory, settings);

  REQUIRE(fixture.acceptor->poll());
  const FIX::SessionID sessionID("FIX.4.2", "ACCEPT", "INITIATOR");
  const auto port = fixture.acceptor->sessionToPort().find(sessionID);
  REQUIRE(port != fixture.acceptor->sessionToPort().end());

  FIX::socket_handle clientSocket = fixture.connect(port->second);
  REQUIRE(FIX::socket_isValid(clientSocket));
  REQUIRE(fixture.acceptor->poll());

  const std::string logon = makeLogon();
  const std::size_t split = logon.size() / 2;
  sendAll(clientSocket, logon.substr(0, split));
  FIX::process_sleep(0.01);
  REQUIRE(fixture.acceptor->poll());
  CHECK(fixture.application.logons == 0);

  sendAll(clientSocket, logon.substr(split));
  FIX::process_sleep(0.01);
  REQUIRE(fixture.acceptor->poll());
  CHECK(fixture.application.logons == 1);

  sendAll(clientSocket, makeNewOrderSingle());
  FIX::process_sleep(0.01);
  REQUIRE(fixture.acceptor->poll());
  CHECK(fixture.application.applicationMessages == 1);

  fixture.closeClient(clientSocket);
  FIX::process_sleep(0.01);
  REQUIRE(fixture.acceptor->poll());

  clientSocket = fixture.connect(port->second);
  REQUIRE(FIX::socket_isValid(clientSocket));
  REQUIRE(fixture.acceptor->poll());

  sendAll(clientSocket, makeLogon(3));
  FIX::process_sleep(0.01);
  REQUIRE(fixture.acceptor->poll());
  CHECK(fixture.application.logons == 2);
}

TEST_CASE("DirectMultiSocketAcceptorTests") {
  DirectAcceptorFixture fixture;
  FIX::SessionSettings settings = directMultiAcceptorSettings();
  fixture.acceptor = std::make_unique<FIX::SocketAcceptor>(fixture.application, fixture.storeFactory, settings);

  REQUIRE(fixture.acceptor->poll());
  const FIX::SessionID sessionA("FIX.4.2", "ACCEPT", "INITIATOR_A");
  const FIX::SessionID sessionB("FIX.4.2", "ACCEPT", "INITIATOR_B");
  const auto portA = fixture.acceptor->sessionToPort().find(sessionA);
  const auto portB = fixture.acceptor->sessionToPort().find(sessionB);
  REQUIRE(portA != fixture.acceptor->sessionToPort().end());
  REQUIRE(portB != fixture.acceptor->sessionToPort().end());
  REQUIRE(portA->second == portB->second);

  FIX::socket_handle clientA = fixture.connect(portA->second);
  FIX::socket_handle clientB = fixture.connect(portB->second);
  REQUIRE(FIX::socket_isValid(clientA));
  REQUIRE(FIX::socket_isValid(clientB));
  REQUIRE(fixture.acceptor->poll());
  REQUIRE(fixture.acceptor->poll());

  const std::string logonA = makeLogon(1, "INITIATOR_A");
  const std::size_t logonSplit = logonA.size() / 2;
  sendAll(clientA, logonA.substr(0, logonSplit));
  sendAll(clientB, makeLogon(1, "INITIATOR_B"));
  FIX::process_sleep(0.01);
  REQUIRE(fixture.acceptor->poll());
  CHECK(fixture.application.sessionLogons[sessionA] == 0);
  CHECK(fixture.application.sessionLogons[sessionB] == 1);

  sendAll(clientA, logonA.substr(logonSplit));
  FIX::process_sleep(0.01);
  REQUIRE(fixture.acceptor->poll());
  CHECK(fixture.application.sessionLogons[sessionA] == 1);
  CHECK(fixture.application.sessionLogons[sessionB] == 1);
  REQUIRE(readMessages(clientA, 1).size() == 1);
  REQUIRE(readMessages(clientB, 1).size() == 1);

  constexpr int burstMessages = 64;
  std::string burst;
  for (int sequenceNumber = 2; sequenceNumber < burstMessages + 2; ++sequenceNumber) {
    burst += makeNewOrderSingle(
        sequenceNumber,
        "INITIATOR_A",
        "DIRECT-A-" + std::to_string(sequenceNumber));
  }
  sendAll(clientA, burst);
  sendAll(clientB, makeNewOrderSingle(2, "INITIATOR_B", "DIRECT-B-2"));
  FIX::process_sleep(0.01);
  REQUIRE(fixture.acceptor->poll());
  CHECK(fixture.application.sessionApplicationMessages[sessionB] == 1);
  CHECK(fixture.application.sessionApplicationMessages[sessionA] > 0);

  for (int attempt = 0;
       attempt < 10 && fixture.application.sessionApplicationMessages[sessionA] < burstMessages;
       ++attempt) {
    REQUIRE(fixture.acceptor->poll());
  }
  REQUIRE(fixture.application.sessionApplicationMessages[sessionA] == burstMessages);

  FIX::Session *sessionBState = FIX::Session::lookupSession(sessionB);
  REQUIRE(sessionBState != nullptr);
  REQUIRE(sessionBState->isLoggedOn());
  REQUIRE(sessionBState->getExpectedTargetNum() == 3);

  fixture.application.responsesPerApplicationMessage = 3;
  sendAll(clientB, makeNewOrderSingle(3, "INITIATOR_B", "DIRECT-B-3"));
  FIX::process_sleep(0.01);
  REQUIRE(fixture.acceptor->poll());
  REQUIRE(fixture.acceptor->poll());
  REQUIRE(fixture.acceptor->poll());
  INFO("expected target=" << sessionBState->getExpectedTargetNum());
  REQUIRE(fixture.application.sessionApplicationMessages[sessionB] == 2);
  REQUIRE(fixture.application.acceptedResponses == 3);
  REQUIRE(readMessages(clientB, 3).size() == 3);
  fixture.application.responsesPerApplicationMessage = 0;

  fixture.closeClient(clientA);
  FIX::process_sleep(0.01);
  REQUIRE(fixture.acceptor->poll());

  sendAll(clientB, makeNewOrderSingle(4, "INITIATOR_B", "DIRECT-B-4"));
  FIX::process_sleep(0.01);
  REQUIRE(fixture.acceptor->poll());
  CHECK(fixture.application.sessionApplicationMessages[sessionB] == 3);

  clientA = fixture.connect(portA->second);
  REQUIRE(FIX::socket_isValid(clientA));
  REQUIRE(fixture.acceptor->poll());
  sendAll(clientA, makeLogon(burstMessages + 2, "INITIATOR_A"));
  FIX::process_sleep(0.01);
  REQUIRE(fixture.acceptor->poll());
  CHECK(fixture.application.sessionLogons[sessionA] == 2);
  CHECK(fixture.application.sessionLogons[sessionB] == 1);
}

TEST_CASE("DirectSocketWriteBackpressureTests") {
  DirectAcceptorFixture fixture;
  FIX::SessionSettings settings = directAcceptorSettings(4096);
  fixture.application.largeResponsePayload.assign(4 * 1024 * 1024, 'R');
  fixture.acceptor = std::make_unique<FIX::SocketAcceptor>(fixture.application, fixture.storeFactory, settings);

  REQUIRE(fixture.acceptor->poll());
  const FIX::SessionID sessionID("FIX.4.2", "ACCEPT", "INITIATOR");
  const auto port = fixture.acceptor->sessionToPort().find(sessionID);
  REQUIRE(port != fixture.acceptor->sessionToPort().end());

  FIX::socket_handle clientSocket = fixture.connect(port->second);
  REQUIRE(FIX::socket_isValid(clientSocket));
  REQUIRE(fixture.acceptor->poll());
  sendAll(clientSocket, makeLogon());
  REQUIRE(fixture.acceptor->poll());
  REQUIRE(readMessages(clientSocket, 1).size() == 1);

  sendAll(clientSocket, makeNewOrderSingle());
  REQUIRE(fixture.acceptor->poll());
  REQUIRE(fixture.application.applicationMessages == 1);
  REQUIRE(fixture.application.acceptedLargeResponses == 1);
  REQUIRE(fixture.application.largeResponseWire.size() > fixture.application.largeResponsePayload.size());

  for (int scan = 0; scan < 128; ++scan) {
    REQUIRE(fixture.acceptor->poll());
  }

  FIX::Session *session = FIX::Session::lookupSession(sessionID);
  REQUIRE(session != nullptr);
  CHECK(session->isLoggedOn());

  int immediatelyAvailable = 0;
  REQUIRE(FIX::socket_fionread(clientSocket, immediatelyAvailable));
  REQUIRE(immediatelyAvailable > 0);
  CHECK(static_cast<std::size_t>(immediatelyAvailable) < fixture.application.largeResponseWire.size());

  const std::vector<std::string> responses = readMessages(clientSocket, 1, fixture.acceptor.get(), 100000);
  std::string largeResponse;
  for (const std::string &response : responses) {
    if (messageType(response) == FIX42::News::MsgType().getValue()) {
      largeResponse = response;
      break;
    }
  }
  REQUIRE_FALSE(largeResponse.empty());
  CHECK(largeResponse == fixture.application.largeResponseWire);

  fixture.application.largeResponsePayload.clear();
  sendAll(clientSocket, makeNewOrderSingle(3, "INITIATOR", "DIRECT-AFTER-BACKPRESSURE"));
  REQUIRE(fixture.acceptor->poll());
  CHECK(fixture.application.applicationMessages == 2);
  CHECK(session->isLoggedOn());
}

TEST_CASE("DirectSocketSessionTimerTests") {
  DirectAcceptorFixture fixture;
  FIX::SessionSettings settings = directAcceptorSettings(0, 1);
  fixture.acceptor = std::make_unique<FIX::SocketAcceptor>(fixture.application, fixture.storeFactory, settings);

  REQUIRE(fixture.acceptor->poll());
  const FIX::SessionID sessionID("FIX.4.2", "ACCEPT", "INITIATOR");
  const auto port = fixture.acceptor->sessionToPort().find(sessionID);
  REQUIRE(port != fixture.acceptor->sessionToPort().end());

  FIX::socket_handle clientSocket = fixture.connect(port->second);
  REQUIRE(FIX::socket_isValid(clientSocket));
  REQUIRE(fixture.acceptor->poll());
  sendAll(clientSocket, makeLogon(1, "INITIATOR", 1));
  REQUIRE(fixture.acceptor->poll());
  REQUIRE(readMessages(clientSocket, 1).size() == 1);

  FIX::process_sleep(1.1);
  REQUIRE(fixture.acceptor->poll());
  REQUIRE(fixture.acceptor->poll());
  const std::vector<std::string> heartbeat = readMessages(clientSocket, 1);
  REQUIRE(heartbeat.size() == 1);
  CHECK(messageType(heartbeat.front()) == FIX::MsgType_Heartbeat);

  FIX::process_sleep(1.1);
  REQUIRE(fixture.acceptor->poll());
  REQUIRE(fixture.acceptor->poll());
  const std::vector<std::string> testRequest = readMessages(clientSocket, 1);
  REQUIRE(testRequest.size() == 1);
  REQUIRE(messageType(testRequest.front()) == FIX::MsgType_TestRequest);
  FIX::Message parsedTestRequest(testRequest.front(), false);
  FIX::TestReqID testRequestID;
  REQUIRE(parsedTestRequest.getFieldIfSet(testRequestID));
  CHECK(testRequestID.getValue() == "TEST");

  sendAll(clientSocket, makeHeartbeat(2, "TEST"));
  REQUIRE(fixture.acceptor->poll());
  FIX::Session *session = FIX::Session::lookupSession(sessionID);
  REQUIRE(session != nullptr);
  REQUIRE(session->getExpectedTargetNum() == 3);

  session->logout("DIRECT-TIMER-LOGOUT");
  FIX::process_sleep(1.1);
  REQUIRE(fixture.acceptor->poll());
  REQUIRE(fixture.acceptor->poll());
  const std::vector<std::string> logout = readMessages(clientSocket, 1);
  REQUIRE(logout.size() == 1);
  CHECK(messageType(logout.front()) == FIX::MsgType_Logout);

  FIX::process_sleep(1.1);
  REQUIRE(fixture.acceptor->poll());
  REQUIRE(waitForDisconnect(clientSocket, fixture.acceptor.get()));
  CHECK_FALSE(session->isLoggedOn());
  CHECK(fixture.application.logouts == 1);
}

TEST_CASE("DirectSocketResendRequestTests") {
  DirectAcceptorFixture fixture;
  FIX::SessionSettings settings = directAcceptorSettings();
  fixture.acceptor = std::make_unique<FIX::SocketAcceptor>(fixture.application, fixture.storeFactory, settings);

  REQUIRE(fixture.acceptor->poll());
  const FIX::SessionID sessionID("FIX.4.2", "ACCEPT", "INITIATOR");
  const auto port = fixture.acceptor->sessionToPort().find(sessionID);
  REQUIRE(port != fixture.acceptor->sessionToPort().end());

  FIX::socket_handle clientSocket = fixture.connect(port->second);
  REQUIRE(FIX::socket_isValid(clientSocket));
  REQUIRE(fixture.acceptor->poll());
  sendAll(clientSocket, makeLogon());
  REQUIRE(fixture.acceptor->poll());
  REQUIRE(readMessages(clientSocket, 1).size() == 1);

  sendAll(clientSocket, makeNewOrderSingle(3, "INITIATOR", "DIRECT-QUEUED-3"));
  REQUIRE(fixture.acceptor->poll());
  CHECK(fixture.application.applicationMessages == 0);

  const std::vector<std::string> resendRequest = readMessages(clientSocket, 1);
  REQUIRE(resendRequest.size() == 1);
  REQUIRE(messageType(resendRequest.front()) == FIX::MsgType_ResendRequest);
  FIX::Message parsedResendRequest(resendRequest.front(), false);
  FIX::BeginSeqNo beginSequenceNumber;
  FIX::EndSeqNo endSequenceNumber;
  REQUIRE(parsedResendRequest.getFieldIfSet(beginSequenceNumber));
  REQUIRE(parsedResendRequest.getFieldIfSet(endSequenceNumber));
  CHECK(beginSequenceNumber.getValue() == 2);
  CHECK(endSequenceNumber.getValue() == 0);

  sendAll(clientSocket, makeNewOrderSingle(2, "INITIATOR", "DIRECT-RECOVERED-2"));
  REQUIRE(fixture.acceptor->poll());
  CHECK(fixture.application.applicationMessages == 2);
  FIX::Session *session = FIX::Session::lookupSession(sessionID);
  REQUIRE(session != nullptr);
  CHECK(session->getExpectedTargetNum() == 4);
}

TEST_CASE("DirectSocketLogonTimeoutTests") {
  DirectAcceptorFixture fixture;
  FIX::SessionSettings settings = directAcceptorSettings();
  fixture.acceptor = std::make_unique<FIX::SocketAcceptor>(fixture.application, fixture.storeFactory, settings);

  REQUIRE(fixture.acceptor->poll());
  const FIX::SessionID sessionID("FIX.4.2", "ACCEPT", "INITIATOR");
  const auto port = fixture.acceptor->sessionToPort().find(sessionID);
  REQUIRE(port != fixture.acceptor->sessionToPort().end());

  FIX::socket_handle clientSocket = fixture.connect(port->second);
  REQUIRE(FIX::socket_isValid(clientSocket));
  REQUIRE(fixture.acceptor->poll());
  FIX::process_sleep(1.1);
  REQUIRE(fixture.acceptor->poll());
  CHECK(waitForDisconnect(clientSocket, fixture.acceptor.get()));
  CHECK(fixture.application.logons == 0);
}

TEST_CASE("DirectSocketGracefulStopTests") {
  DirectAcceptorFixture fixture;
  FIX::SessionSettings settings = directAcceptorSettings();
  fixture.acceptor = std::make_unique<FIX::SocketAcceptor>(fixture.application, fixture.storeFactory, settings);
  fixture.acceptor->start();

  const FIX::SessionID sessionID("FIX.4.2", "ACCEPT", "INITIATOR");
  const auto port = fixture.acceptor->sessionToPort().find(sessionID);
  REQUIRE(port != fixture.acceptor->sessionToPort().end());
  FIX::socket_handle clientSocket = fixture.connect(port->second);
  REQUIRE(FIX::socket_isValid(clientSocket));

  sendAll(clientSocket, makeLogon());
  REQUIRE(readMessages(clientSocket, 1, nullptr, 4000).size() == 1);

  const std::chrono::steady_clock::time_point stopStarted = std::chrono::steady_clock::now();
  std::future<void> stopResult
      = std::async(std::launch::async, [&fixture]() { fixture.acceptor->stop(false); });

  const std::vector<std::string> logout = readMessages(clientSocket, 1, nullptr, 4000);
  REQUIRE(logout.size() == 1);
  REQUIRE(messageType(logout.front()) == FIX::MsgType_Logout);
  sendAll(clientSocket, makeLogout(2));

  REQUIRE(stopResult.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
  stopResult.get();
  const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - stopStarted;
  CHECK(elapsed.count() < 5.0);
  CHECK(fixture.acceptor->isStopped());
  CHECK(fixture.application.logouts == 1);
}

#endif
