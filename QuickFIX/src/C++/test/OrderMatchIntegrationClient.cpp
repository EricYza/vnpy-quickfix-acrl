/****************************************************************************
** Copyright (c) 2001-2014
**
** This file is part of the QuickFIX FIX Engine
**
** This file may be distributed under the terms of the quickfixengine.org
** license as defined by quickfixengine.org and appearing in the file
** LICENSE included in the packaging of this file.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING ANY
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
** See http://www.quickfixengine.org/LICENSE for licensing information.
**
** Contact ask@quickfixengine.org if any conditions of this licensing are
** not clear to you.
****************************************************************************/

#include "quickfix/Application.h"
#include "quickfix/MessageStore.h"
#include "quickfix/Session.h"
#include "quickfix/SessionSettings.h"
#include "quickfix/SocketInitiator.h"
#include "quickfix/fix42/Heartbeat.h"
#include "quickfix/fix42/MarketDataRequest.h"
#include "quickfix/fix42/MarketDataSnapshotFullRefresh.h"
#include "quickfix/fix42/NewOrderSingle.h"
#include "quickfix/fix42/OrderCancelRequest.h"
#include "quickfix/fix42/TestRequest.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct ReceivedMessage {
  FIX::SessionID sessionID;
  FIX::Message message;
};

std::string messageType(const FIX::Message &message) {
  FIX::MsgType type;
  message.getHeader().getField(type);
  return type.getValue();
}

void require(bool condition, const std::string &description) {
  if (!condition) {
    throw std::runtime_error(description);
  }
}

double numericField(const FIX::Message &message, int tag) {
  return std::stod(message.getField(tag));
}

class ClientApplication : public FIX::NullApplication {
public:
  void onLogon(const FIX::SessionID &sessionID) override {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_loggedOn.insert(sessionID);
    m_condition.notify_all();
  }

  void onLogout(const FIX::SessionID &sessionID) override {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_loggedOn.erase(sessionID);
    ++m_logoutCount;
    m_condition.notify_all();
  }

  void fromAdmin(const FIX::Message &message, const FIX::SessionID &sessionID)
      EXCEPT(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::RejectLogon) override {
    record(message, sessionID);
  }

  void fromApp(const FIX::Message &message, const FIX::SessionID &sessionID)
      EXCEPT(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::UnsupportedMessageType)
          override {
    record(message, sessionID);
  }

  void waitForLogons(std::size_t expected, std::chrono::seconds timeout) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_condition.wait_for(lock, timeout, [&] { return m_loggedOn.size() >= expected; })) {
      throw std::runtime_error("timed out waiting for QuickFIX client Logon");
    }
  }

  void waitForLogouts(std::size_t expected, std::chrono::seconds timeout) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_condition.wait_for(lock, timeout, [&] { return m_logoutCount >= expected; })) {
      throw std::runtime_error("timed out waiting for QuickFIX client Logout");
    }
  }

  FIX::Message waitForMessage(
      const FIX::SessionID &sessionID,
      const std::function<bool(const FIX::Message &)> &predicate,
      std::chrono::seconds timeout,
      const std::string &description) {
    std::unique_lock<std::mutex> lock(m_mutex);
    const auto findMessage = [&]() {
      for (auto iterator = m_messages.begin(); iterator != m_messages.end(); ++iterator) {
        if (iterator->sessionID == sessionID && predicate(iterator->message)) {
          FIX::Message message = iterator->message;
          m_messages.erase(iterator);
          return std::make_pair(true, message);
        }
      }
      return std::make_pair(false, FIX::Message());
    };

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
      auto result = findMessage();
      if (result.first) {
        return result.second;
      }
      if (m_condition.wait_until(lock, deadline) == std::cv_status::timeout) {
        result = findMessage();
        if (result.first) {
          return result.second;
        }
        throw std::runtime_error("timed out waiting for " + description);
      }
    }
  }

private:
  void record(const FIX::Message &message, const FIX::SessionID &sessionID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_messages.push_back({sessionID, message});
    m_condition.notify_all();
  }

  std::mutex m_mutex;
  std::condition_variable m_condition;
  std::set<FIX::SessionID> m_loggedOn;
  std::size_t m_logoutCount = 0;
  std::vector<ReceivedMessage> m_messages;
};

std::string makeInitiatorSettings(int port, const std::string &dictionary) {
  std::ostringstream stream;
  stream << "[DEFAULT]\n"
         << "ConnectionType=initiator\n"
         << "SocketConnectHost=127.0.0.1\n"
         << "SocketConnectPort=" << port << "\n"
         << "SocketNodelay=Y\n"
         << "ReconnectInterval=1\n"
         << "StartTime=00:00:00\n"
         << "EndTime=00:00:00\n"
         << "HeartBtInt=2\n"
         << "CheckLatency=N\n"
         << "PersistMessages=N\n"
         << "ResetOnLogon=Y\n"
         << "UseDataDictionary=Y\n"
         << "DataDictionary=" << dictionary << "\n"
         << "\n"
         << "[SESSION]\n"
         << "BeginString=FIX.4.2\n"
         << "SenderCompID=CLIENT1\n"
         << "TargetCompID=ORDERMATCH\n"
         << "\n"
         << "[SESSION]\n"
         << "BeginString=FIX.4.2\n"
         << "SenderCompID=CLIENT2\n"
         << "TargetCompID=ORDERMATCH\n";
  return stream.str();
}

bool executionReportMatches(const FIX::Message &message, const std::string &clientOrderID, char status) {
  return messageType(message) == FIX::MsgType_ExecutionReport && message.isSetField(FIX::FIELD::ClOrdID)
         && message.getField(FIX::FIELD::ClOrdID) == clientOrderID && message.isSetField(FIX::FIELD::OrdStatus)
         && message.getField(FIX::FIELD::OrdStatus) == std::string(1, status);
}

FIX::Message waitForExecutionReport(
    ClientApplication &application,
    const FIX::SessionID &sessionID,
    const std::string &clientOrderID,
    char status) {
  return application.waitForMessage(
      sessionID,
      [&](const FIX::Message &message) { return executionReportMatches(message, clientOrderID, status); },
      std::chrono::seconds(5),
      "ExecutionReport ClOrdID=" + clientOrderID + " OrdStatus=" + std::string(1, status));
}

void sendToTarget(FIX::Message &message, const FIX::SessionID &sessionID) {
  if (!FIX::Session::sendToTarget(message, sessionID)) {
    throw std::runtime_error("Session::sendToTarget returned false for " + sessionID.toString());
  }
}

void exerciseOrderMatch(ClientApplication &application) {
  const FIX::SessionID client1("FIX.4.2", "CLIENT1", "ORDERMATCH");
  const FIX::SessionID client2("FIX.4.2", "CLIENT2", "ORDERMATCH");

  FIX42::TestRequest testRequest(FIX::TestReqID("ROUNDTRIP"));
  sendToTarget(testRequest, client1);
  const FIX::Message heartbeatResponse = application.waitForMessage(
      client1,
      [](const FIX::Message &message) {
        return messageType(message) == FIX::MsgType_Heartbeat && message.isSetField(FIX::FIELD::TestReqID)
               && message.getField(FIX::FIELD::TestReqID) == "ROUNDTRIP";
      },
      std::chrono::seconds(5),
      "Heartbeat response to TestRequest");
  require(heartbeatResponse.getField(FIX::FIELD::TestReqID) == "ROUNDTRIP", "Heartbeat TestReqID mismatch");

  FIX42::NewOrderSingle buy(
      FIX::ClOrdID("B1"),
      FIX::HandlInst(FIX::HandlInst_AUTOMATED_EXECUTION_NO_INTERVENTION),
      FIX::Symbol("LNUX"),
      FIX::Side(FIX::Side_BUY),
      FIX::TransactTime::now(),
      FIX::OrdType(FIX::OrdType_LIMIT));
  buy.set(FIX::OrderQty(100));
  buy.set(FIX::Price(10.0));
  buy.set(FIX::TimeInForce(FIX::TimeInForce_DAY));
  sendToTarget(buy, client1);

  const FIX::Message buyNew = waitForExecutionReport(application, client1, "B1", FIX::OrdStatus_NEW);
  require(numericField(buyNew, FIX::FIELD::LeavesQty) == 100, "NEW buy LeavesQty mismatch");
  require(numericField(buyNew, FIX::FIELD::CumQty) == 0, "NEW buy CumQty mismatch");

  FIX42::NewOrderSingle sell(
      FIX::ClOrdID("S1"),
      FIX::HandlInst(FIX::HandlInst_AUTOMATED_EXECUTION_NO_INTERVENTION),
      FIX::Symbol("LNUX"),
      FIX::Side(FIX::Side_SELL),
      FIX::TransactTime::now(),
      FIX::OrdType(FIX::OrdType_LIMIT));
  sell.set(FIX::OrderQty(40));
  sell.set(FIX::Price(9.0));
  sell.set(FIX::TimeInForce(FIX::TimeInForce_DAY));
  sendToTarget(sell, client2);

  const FIX::Message sellNew = waitForExecutionReport(application, client2, "S1", FIX::OrdStatus_NEW);
  require(numericField(sellNew, FIX::FIELD::LeavesQty) == 40, "NEW sell LeavesQty mismatch");

  const FIX::Message buyPartial
      = waitForExecutionReport(application, client1, "B1", FIX::OrdStatus_PARTIALLY_FILLED);
  const FIX::Message sellFilled = waitForExecutionReport(application, client2, "S1", FIX::OrdStatus_FILLED);
  require(numericField(buyPartial, FIX::FIELD::LeavesQty) == 60, "partial buy LeavesQty mismatch");
  require(numericField(buyPartial, FIX::FIELD::CumQty) == 40, "partial buy CumQty mismatch");
  require(numericField(buyPartial, FIX::FIELD::LastShares) == 40, "partial buy LastShares mismatch");
  require(numericField(sellFilled, FIX::FIELD::LeavesQty) == 0, "filled sell LeavesQty mismatch");
  require(numericField(sellFilled, FIX::FIELD::CumQty) == 40, "filled sell CumQty mismatch");
  require(numericField(sellFilled, FIX::FIELD::LastShares) == 40, "filled sell LastShares mismatch");

  FIX42::OrderCancelRequest cancel(
      FIX::OrigClOrdID("B1"),
      FIX::ClOrdID("CXL-B1"),
      FIX::Symbol("LNUX"),
      FIX::Side(FIX::Side_BUY),
      FIX::TransactTime::now());
  cancel.set(FIX::OrderQty(60));
  sendToTarget(cancel, client1);

  const FIX::Message canceled = waitForExecutionReport(application, client1, "B1", FIX::OrdStatus_CANCELED);
  require(numericField(canceled, FIX::FIELD::LeavesQty) == 0, "canceled buy LeavesQty mismatch");
  require(numericField(canceled, FIX::FIELD::CumQty) == 40, "canceled buy CumQty mismatch");

  FIX42::MarketDataRequest marketDataRequest(
      FIX::MDReqID("MD1"), FIX::SubscriptionRequestType(FIX::SubscriptionRequestType_SNAPSHOT), FIX::MarketDepth(1));
  FIX42::MarketDataRequest::NoMDEntryTypes entryType;
  entryType.set(FIX::MDEntryType(FIX::MDEntryType_BID));
  marketDataRequest.addGroup(entryType);
  entryType.set(FIX::MDEntryType(FIX::MDEntryType_OFFER));
  marketDataRequest.addGroup(entryType);
  FIX42::MarketDataRequest::NoRelatedSym relatedSymbol;
  relatedSymbol.set(FIX::Symbol("LNUX"));
  marketDataRequest.addGroup(relatedSymbol);
  sendToTarget(marketDataRequest, client1);

  const FIX::Message snapshotMessage = application.waitForMessage(
      client1,
      [](const FIX::Message &message) {
        return messageType(message) == FIX::MsgType_MarketDataSnapshotFullRefresh
               && message.isSetField(FIX::FIELD::MDReqID) && message.getField(FIX::FIELD::MDReqID) == "MD1";
      },
      std::chrono::seconds(5),
      "MarketDataSnapshotFullRefresh");
  FIX42::MarketDataSnapshotFullRefresh snapshot(snapshotMessage);
  FIX::NoMDEntries entryCount;
  snapshot.get(entryCount);
  require(entryCount.getValue() == 3, "market-data entry count mismatch");

  const double expectedPrices[] = {10.0, 10.5, 10.25};
  const double expectedSizes[] = {100.0, 100.0, 10.0};
  const char expectedTypes[] = {FIX::MDEntryType_BID, FIX::MDEntryType_OFFER, FIX::MDEntryType_TRADE};
  for (int index = 1; index <= entryCount; ++index) {
    FIX42::MarketDataSnapshotFullRefresh::NoMDEntries entry;
    snapshot.getGroup(index, entry);
    FIX::MDEntryType type;
    FIX::MDEntryPx price;
    FIX::MDEntrySize size;
    entry.get(type);
    entry.get(price);
    entry.get(size);
    require(type.getValue() == expectedTypes[index - 1], "market-data entry type mismatch");
    require(price.getValue() == expectedPrices[index - 1], "market-data entry price mismatch");
    require(size.getValue() == expectedSizes[index - 1], "market-data entry size mismatch");
  }

  for (const FIX::SessionID &sessionID : {client1, client2}) {
    application.waitForMessage(
        sessionID,
        [](const FIX::Message &message) {
          return messageType(message) == FIX::MsgType_Heartbeat && !message.isSetField(FIX::FIELD::TestReqID);
        },
        std::chrono::seconds(5),
        "periodic Heartbeat");
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: " << argv[0] << " PORT FIX42_XML\n";
    return 2;
  }

  try {
    const int port = std::stoi(argv[1]);
    std::istringstream settingsStream(makeInitiatorSettings(port, argv[2]));
    FIX::SessionSettings settings(settingsStream);
    ClientApplication application;
    FIX::MemoryStoreFactory storeFactory;
    FIX::SocketInitiator initiator(application, storeFactory, settings);

    initiator.start();
    application.waitForLogons(2, std::chrono::seconds(8));
    exerciseOrderMatch(application);
    initiator.stop();
    application.waitForLogouts(2, std::chrono::seconds(5));

    std::cout << "quickfix_client=pass\n"
              << "logon=pass\n"
              << "test_request=pass\n"
              << "new_order=pass\n"
              << "cross_session_match=pass\n"
              << "cancel=pass\n"
              << "market_data=pass\n"
              << "periodic_heartbeat=pass\n"
              << "logout=pass\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "quickfix_client=fail\n"
              << "error=" << error.what() << "\n";
    return 1;
  }
}
