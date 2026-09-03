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

#ifdef _MSC_VER
#pragma warning(disable : 4503 4355 4786)
#include "stdafx.h"
#else
#include "config.h"
#endif

#include "Application.h"
#include "DataDictionary.h"
#include "FieldConvertors.h"
#include "Message.h"
#include "MessageStore.h"
#include "Parser.h"
#include "Session.h"
#include "SessionID.h"
#include "SessionSettings.h"
#include "SocketAcceptor.h"
#include "SocketInitiator.h"
#include "Utility.h"
#include "Values.h"
#include "detail/FastScan.h"
#include "fix42/MarketDataSnapshotFullRefresh.h"
#include "fix42/NewOrderSingle.h"
#include "fix42/OrderCancelRequest.h"
#include "fix42/QuoteRequest.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

const char SOH = '\001';
const char *BeginString = "FIX.4.2";
const char *ClientCompID = "CLIENT";
const char *ServerCompID = "SERVER";

enum class Mode { Parse, Server, Both };
enum class ClientMode { Raw, Quickfix };
enum class MessageKind { NewOrderSingle, OrderCancelRequest, MarketDataSnapshot, QuoteRequest };

/**
 * @brief Runtime configuration shared by message generation, benchmark execution, and result reporting.
 */
struct Options {
  Mode mode = Mode::Both;
  ClientMode clientMode = ClientMode::Raw;
  MessageKind messageKind = MessageKind::NewOrderSingle;
  std::uint64_t messages = 100000;
  std::uint64_t warmup = 10000;
  int port = 0;
  int quoteGroups = 10;
  int sendBufferSize = 0;
  int receiveBufferSize = 0;
  int serverWaitSeconds = 30;
  bool validate = false;
  bool selfTestFastScan = false;
  bool selfTestParser = false;
  bool selfTestCorrectness = false;
  bool fixedLayout = false;
  bool busyPoll = false;
  bool directReadPoll = false;
  bool networkDiagnostics = false;
  int busyPollCpu = -1;
  std::string dataDictionaryPath = "spec/FIX42.xml";
};

/**
 * @brief Move-only RAII owner for the raw benchmark client's socket handle.
 */
class SocketGuard {
public:
  SocketGuard() = default;
  explicit SocketGuard(FIX::socket_handle socket)
      : m_socket(socket) {}
  SocketGuard(const SocketGuard &) = delete;
  SocketGuard &operator=(const SocketGuard &) = delete;
  SocketGuard(SocketGuard &&rhs) noexcept
      : m_socket(rhs.m_socket) {
    rhs.m_socket = INVALID_SOCKET_HANDLE;
  }
  SocketGuard &operator=(SocketGuard &&rhs) noexcept {
    if (this != &rhs) {
      reset(rhs.m_socket);
      rhs.m_socket = INVALID_SOCKET_HANDLE;
    }
    return *this;
  }

  ~SocketGuard() { reset(); }

  FIX::socket_handle get() const { return m_socket; }

  /**
   * @brief Closes the currently owned socket and optionally adopts another one.
   *
   * @param socket New socket to own, or `INVALID_SOCKET_HANDLE` to leave the guard empty.
   */
  void reset(FIX::socket_handle socket = INVALID_SOCKET_HANDLE) {
    if (FIX::socket_isValid(m_socket)) {
      FIX::socket_close(m_socket);
    }
    m_socket = socket;
  }

private:
  FIX::socket_handle m_socket = INVALID_SOCKET_HANDLE;
};

/**
 * @brief Minimal QuickFIX application used to observe Logon state and count fully processed application messages.
 *
 * The server benchmarks stop timing only after `received` reaches the requested message count.
 */
class BenchmarkApplication : public FIX::NullApplication {
public:
  std::atomic<std::uint64_t> received{0};
  std::atomic<bool> loggedOn{false};

  void onLogon(const FIX::SessionID &) override { loggedOn.store(true, std::memory_order_release); }
  void onLogout(const FIX::SessionID &) override { loggedOn.store(false, std::memory_order_release); }

  void fromApp(const FIX::Message &, const FIX::SessionID &)
      EXCEPT(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::UnsupportedMessageType)
          override {
    received.fetch_add(1, std::memory_order_relaxed);
  }
};

/**
 * @brief Returns the current UTC timestamp in FIX wire format.
 *
 * @return Timestamp suitable for SendingTime and TransactTime fields.
 */
std::string timestamp() { return FIX::UtcTimeStampConvertor::convert(FIX::UtcTimeStamp::now()); }

/**
 * @brief Formats an integer as a zero-padded decimal value for fixed-width benchmark fields.
 *
 * @param value The non-negative integer to format.
 * @param width The minimum number of output characters.
 * @return The formatted decimal string.
 */
std::string fixedNumber(std::uint64_t value, int width) {
  std::ostringstream stream;
  stream << std::setw(width) << std::setfill('0') << value;
  return stream.str();
}

/**
 * @brief Encodes one FIX field as `tag=value<SOH>`.
 *
 * @param tag The numeric FIX tag.
 * @param value The field value without a trailing SOH delimiter.
 * @return The encoded wire field.
 */
std::string field(int tag, const std::string &value) {
  return std::to_string(tag) + "=" + value + SOH;
}

/**
 * @brief Builds a complete FIX.4.2 wire message from ordered body and standard-header fields.
 *
 * Tags 8 and 9 are prepended automatically. Tag 10 is calculated over every byte before the checksum field.
 *
 * @param fields Ordered fields beginning with tag 35 and excluding tags 8, 9, and 10.
 * @return A complete FIX message terminated by the checksum field and SOH.
 */
std::string buildFixMessage(const std::vector<std::pair<int, std::string>> &fields) {
  std::string body;
  for (const auto &entry : fields) {
    body += field(entry.first, entry.second);
  }

  std::string message = field(8, BeginString) + field(9, std::to_string(body.size())) + body;
  unsigned int checksum = 0;
  for (unsigned char c : message) {
    checksum += c;
  }

  std::ostringstream checksumField;
  checksumField << "10=" << std::setw(3) << std::setfill('0') << (checksum % 256) << SOH;
  message += checksumField.str();
  return message;
}

/**
 * @brief Builds the common application-message header fields used by ordinary raw benchmark messages.
 *
 * @param msgType FIX MsgType value for tag 35.
 * @param seqNum Session sequence number for tag 34.
 * @param now SendingTime value shared by tags 52 and message-specific time fields.
 * @return Ordered header fields, excluding tags 8 and 9.
 */
std::vector<std::pair<int, std::string>>
headerFields(const std::string &msgType, std::uint64_t seqNum, const std::string &now) {
  return {
      {35, msgType},
      {49, ClientCompID},
      {56, ServerCompID},
      {34, std::to_string(seqNum)},
      {52, now},
  };
}

/**
 * @brief Builds common header fields with a 12-character sequence number for fixed-layout messages.
 *
 * Fixed-layout samples keep field order and value widths stable. Their body adds tag 9001 to select an offset
 * template in `Message::setFixedLayoutString()`.
 *
 * @param msgType FIX MsgType value for tag 35.
 * @param seqNum Session sequence number to pad to 12 decimal characters.
 * @param now SendingTime value for tag 52.
 * @return Ordered fixed-width header fields, excluding tags 8 and 9.
 */
std::vector<std::pair<int, std::string>>
fixedHeaderFields(const std::string &msgType, std::uint64_t seqNum, const std::string &now) {
  return {
      {35, msgType},
      {49, ClientCompID},
      {56, ServerCompID},
      {34, fixedNumber(seqNum, 12)},
      {52, now},
  };
}

/**
 * @brief Builds ordinary NewOrderSingle body and header fields for the raw client.
 *
 * @param seqNum FIX sequence number and ClOrdID suffix.
 * @param now SendingTime and TransactTime value.
 * @param options Reserved for the common message-builder interface.
 * @return Ordered fields excluding tags 8, 9, and 10.
 */
std::vector<std::pair<int, std::string>>
newOrderSingleFields(
    std::uint64_t seqNum,
    const std::string &now,
    [[maybe_unused]] const Options &options) {
  std::vector<std::pair<int, std::string>> fields = headerFields("D", seqNum, now);
  fields.emplace_back(11, "ORDER-" + std::to_string(seqNum));
  fields.emplace_back(21, "1");
  fields.emplace_back(55, "LNUX");
  fields.emplace_back(54, "1");
  fields.emplace_back(60, now);
  fields.emplace_back(38, "100");
  fields.emplace_back(40, "1");
  fields.emplace_back(59, "0");
  fields.emplace_back(15, "USD");
  return fields;
}

/**
 * @brief Builds the fixed-width NOS1 NewOrderSingle template used by the offset parser.
 *
 * @param seqNum FIX sequence number padded to 12 characters.
 * @param now SendingTime and TransactTime value.
 * @param options Reserved for the common message-builder interface.
 * @return Ordered fields including the `9001=NOS1` template marker.
 */
std::vector<std::pair<int, std::string>>
fixedNewOrderSingleFields(
    std::uint64_t seqNum,
    const std::string &now,
    [[maybe_unused]] const Options &options) {
  const std::string seq = fixedNumber(seqNum, 12);
  std::vector<std::pair<int, std::string>> fields = fixedHeaderFields("D", seqNum, now);
  fields.emplace_back(9001, "NOS1");
  fields.emplace_back(11, "ORDER-" + seq);
  fields.emplace_back(21, "1");
  fields.emplace_back(55, "LNUX");
  fields.emplace_back(54, "1");
  fields.emplace_back(60, now);
  fields.emplace_back(38, "0000000100");
  fields.emplace_back(40, "1");
  fields.emplace_back(59, "0");
  fields.emplace_back(15, "USD");
  return fields;
}

/**
 * @brief Builds ordinary OrderCancelRequest body and header fields for the raw client.
 *
 * @param seqNum FIX sequence number and cancel identifier suffix.
 * @param now SendingTime and TransactTime value.
 * @param options Reserved for the common message-builder interface.
 * @return Ordered fields excluding tags 8, 9, and 10.
 */
std::vector<std::pair<int, std::string>>
orderCancelRequestFields(
    std::uint64_t seqNum,
    const std::string &now,
    [[maybe_unused]] const Options &options) {
  std::vector<std::pair<int, std::string>> fields = headerFields("F", seqNum, now);
  fields.emplace_back(41, "ORIG-" + std::to_string(seqNum > 2 ? seqNum - 1 : 1));
  fields.emplace_back(11, "CNCL-" + std::to_string(seqNum));
  fields.emplace_back(55, "LNUX");
  fields.emplace_back(54, "1");
  fields.emplace_back(60, now);
  fields.emplace_back(38, "100");
  return fields;
}

/**
 * @brief Builds the fixed-width CXL1 OrderCancelRequest template used by the offset parser.
 *
 * @param seqNum FIX sequence number used for padded original and cancel identifiers.
 * @param now SendingTime and TransactTime value.
 * @param options Reserved for the common message-builder interface.
 * @return Ordered fields including the `9001=CXL1` template marker.
 */
std::vector<std::pair<int, std::string>>
fixedOrderCancelRequestFields(
    std::uint64_t seqNum,
    const std::string &now,
    [[maybe_unused]] const Options &options) {
  const std::string seq = fixedNumber(seqNum, 12);
  const std::string origSeq = fixedNumber(seqNum > 2 ? seqNum - 1 : 1, 12);
  std::vector<std::pair<int, std::string>> fields = fixedHeaderFields("F", seqNum, now);
  fields.emplace_back(9001, "CXL1");
  fields.emplace_back(41, "ORIG-" + origSeq);
  fields.emplace_back(11, "CNCL-" + seq);
  fields.emplace_back(55, "LNUX");
  fields.emplace_back(54, "1");
  fields.emplace_back(60, now);
  fields.emplace_back(38, "0000000100");
  return fields;
}

/**
 * @brief Appends the three benchmark market-data repeating groups.
 *
 * @param fields Destination field sequence.
 * @param fixedLayout Selects padded fixed-width price and size values when true.
 */
void appendMarketDataEntries(std::vector<std::pair<int, std::string>> &fields, bool fixedLayout) {
  const char *prices[] = {"123.45", "123.46", "123.455"};
  const char *fixedPrices[] = {"00000123.4500", "00000123.4600", "00000123.4550"};
  const char *sizes[] = {"500", "400", "100"};
  const char *fixedSizes[] = {"0000000500", "0000000400", "0000000100"};
  const char *types[] = {"0", "1", "2"};

  for (int i = 0; i < 3; ++i) {
    fields.emplace_back(269, types[i]);
    fields.emplace_back(270, fixedLayout ? fixedPrices[i] : prices[i]);
    fields.emplace_back(271, fixedLayout ? fixedSizes[i] : sizes[i]);
    fields.emplace_back(273, "12:34:56");
  }
}

/**
 * @brief Builds an ordinary MarketDataSnapshotFullRefresh with three repeating-group entries.
 *
 * @param seqNum FIX sequence number and MDReqID suffix.
 * @param now SendingTime value.
 * @param options Supplies the value-width mode passed to the repeating-group builder.
 * @return Ordered header, body, and flattened repeating-group fields.
 */
std::vector<std::pair<int, std::string>>
marketDataSnapshotFields(std::uint64_t seqNum, const std::string &now, const Options &options) {
  std::vector<std::pair<int, std::string>> fields = headerFields("W", seqNum, now);
  fields.emplace_back(262, "MDREQ-" + std::to_string(seqNum));
  fields.emplace_back(55, "LNUX");
  fields.emplace_back(268, "3");
  appendMarketDataEntries(fields, options.fixedLayout);
  return fields;
}

/**
 * @brief Builds the fixed-width MDW1 market-data template and its three repeating groups.
 *
 * @param seqNum FIX sequence number padded for tag 34 and MDReqID.
 * @param now SendingTime value.
 * @param options Supplies fixed-width market-data values.
 * @return Ordered fields including the `9001=MDW1` template marker.
 */
std::vector<std::pair<int, std::string>>
fixedMarketDataSnapshotFields(std::uint64_t seqNum, const std::string &now, const Options &options) {
  const std::string seq = fixedNumber(seqNum, 12);
  std::vector<std::pair<int, std::string>> fields = fixedHeaderFields("W", seqNum, now);
  fields.emplace_back(9001, "MDW1");
  fields.emplace_back(262, "MDREQ-" + seq);
  fields.emplace_back(55, "LNUX");
  fields.emplace_back(268, "3");
  appendMarketDataEntries(fields, options.fixedLayout);
  return fields;
}

/**
 * @brief Builds a QuoteRequest whose configurable groups exercise longer generic parsing paths.
 *
 * @param seqNum FIX sequence number and QuoteReqID suffix.
 * @param now SendingTime and group TransactTime value.
 * @param options Supplies the number of NoRelatedSym groups.
 * @return Ordered header, body, and flattened repeating-group fields.
 */
std::vector<std::pair<int, std::string>>
quoteRequestFields(std::uint64_t seqNum, const std::string &now, const Options &options) {
  std::vector<std::pair<int, std::string>> fields = headerFields("R", seqNum, now);
  fields.emplace_back(131, "QR-" + std::to_string(seqNum));
  fields.emplace_back(146, std::to_string(options.quoteGroups));
  for (int i = 0; i < options.quoteGroups; ++i) {
    fields.emplace_back(55, "IBM" + std::to_string(i));
    fields.emplace_back(200, "202612");
    fields.emplace_back(201, "0");
    fields.emplace_back(202, "120");
    fields.emplace_back(54, "1");
    fields.emplace_back(38, "100");
    fields.emplace_back(40, "1");
    fields.emplace_back(60, now);
    fields.emplace_back(15, "USD");
  }
  return fields;
}

/**
 * @brief Constructs one complete raw-client application message for the selected message kind and layout.
 *
 * @param seqNum FIX session sequence number for the generated message.
 * @param options Selects the message kind, fixed-layout form, and repeating-group count.
 * @return A complete serialized FIX wire message.
 */
std::string makeApplicationMessage(std::uint64_t seqNum, const Options &options) {
  const std::string now = timestamp();
  switch (options.messageKind) {
  case MessageKind::NewOrderSingle:
    return buildFixMessage(
        options.fixedLayout ? fixedNewOrderSingleFields(seqNum, now, options)
                            : newOrderSingleFields(seqNum, now, options));
  case MessageKind::OrderCancelRequest:
    return buildFixMessage(
        options.fixedLayout ? fixedOrderCancelRequestFields(seqNum, now, options)
                            : orderCancelRequestFields(seqNum, now, options));
  case MessageKind::MarketDataSnapshot:
    return buildFixMessage(
        options.fixedLayout ? fixedMarketDataSnapshotFields(seqNum, now, options)
                            : marketDataSnapshotFields(seqNum, now, options));
  case MessageKind::QuoteRequest:
    return buildFixMessage(quoteRequestFields(seqNum, now, options));
  }
  throw std::runtime_error("unknown message kind");
}

/**
 * @brief Constructs the raw client's initial FIX Logon message with sequence number 1.
 *
 * @return A complete serialized FIX Logon message.
 */
std::string makeLogonMessage() {
  const std::string now = timestamp();
  std::vector<std::pair<int, std::string>> fields = headerFields("A", 1, now);
  fields.emplace_back(98, "0");
  fields.emplace_back(108, "30");
  return buildFixMessage(fields);
}

/**
 * @brief Maps a message-kind enum to the stable label printed in benchmark results.
 *
 * @param kind Message kind to describe.
 * @return Static result label.
 */
const char *messageKindName(MessageKind kind) {
  switch (kind) {
  case MessageKind::NewOrderSingle:
    return "new-order-single";
  case MessageKind::OrderCancelRequest:
    return "order-cancel-request";
  case MessageKind::MarketDataSnapshot:
    return "market-data-snapshot";
  case MessageKind::QuoteRequest:
    return "quote-request";
  }
  return "unknown";
}

/**
 * @brief Maps a client-mode enum to the stable label printed in benchmark results.
 *
 * @param mode Raw socket or QuickFIX initiator mode.
 * @return Static result label.
 */
const char *clientModeName(ClientMode mode) {
  return mode == ClientMode::Raw ? "raw" : "quickfix";
}

/**
 * @brief Formats the configured acceptor CPU only when an active polling mode can use it.
 *
 * @param options Poll mode and optional CPU index.
 * @return CPU number or `none`.
 */
std::string busyPollCpuName(const Options &options) {
  return (options.busyPoll || options.directReadPoll) && options.busyPollCpu >= 0
             ? std::to_string(options.busyPollCpu)
             : "none";
}

/**
 * @brief Returns the effective acceptor polling strategy for result reporting.
 *
 * @param options Busy-poll and direct-read flags.
 * @return `blocking`, `poll0`, or `direct`.
 */
const char *socketPollModeName(const Options &options) {
  if (options.directReadPoll) {
    return "direct";
  }
  return options.busyPoll ? "poll0" : "blocking";
}

/**
 * @brief Converts benchmark options into an in-memory QuickFIX acceptor configuration.
 *
 * @param options Controls validation, socket buffers, poll mode, and optional CPU affinity.
 * @param port Requested accept port. Zero lets the operating system select an available port.
 * @return Text in QuickFIX SessionSettings format.
 */
std::string makeAcceptorConfig(const Options &options, int port) {
  std::ostringstream stream;
  stream << "[DEFAULT]\n"
         << "ConnectionType=acceptor\n"
         << "SocketAcceptPort=" << port << "\n"
         << "SocketReuseAddress=Y\n"
         << "SocketNodelay=Y\n"
         << "StartTime=00:00:00\n"
         << "EndTime=00:00:00\n"
         << "CheckLatency=N\n"
         << "PersistMessages=N\n"
         << "UseDataDictionary=" << (options.validate ? "Y" : "N") << "\n";

  if (options.sendBufferSize > 0) {
    stream << "SendBufferSize=" << options.sendBufferSize << "\n";
  }
  if (options.receiveBufferSize > 0) {
    stream << "ReceiveBufferSize=" << options.receiveBufferSize << "\n";
  }
  if (options.directReadPoll) {
    stream << "SocketBusyPollMode=direct\n";
  } else if (options.busyPoll) {
    stream << "SocketBusyPoll=Y\n";
  }
  if ((options.busyPoll || options.directReadPoll) && options.busyPollCpu >= 0) {
    stream << "SocketBusyPollCpu=" << options.busyPollCpu << "\n";
  }
  if (options.validate) {
    stream << "DataDictionary=" << options.dataDictionaryPath << "\n";
    if (options.fixedLayout) {
      stream << "ValidateUserDefinedFields=N\n";
    }
  }

  stream << "[SESSION]\n"
         << "BeginString=" << BeginString << "\n"
         << "SenderCompID=" << ServerCompID << "\n"
         << "TargetCompID=" << ClientCompID << "\n"
         << "HeartBtInt=30\n";
  return stream.str();
}

/**
 * @brief Converts benchmark options into an in-memory QuickFIX initiator configuration.
 *
 * @param options Controls validation and client-side socket buffer sizes.
 * @param port Port already bound by the benchmark acceptor.
 * @return Text in QuickFIX SessionSettings format.
 */
std::string makeInitiatorConfig(const Options &options, int port) {
  std::ostringstream stream;
  stream << "[DEFAULT]\n"
         << "ConnectionType=initiator\n"
         << "SocketConnectHost=127.0.0.1\n"
         << "SocketConnectPort=" << port << "\n"
         << "SocketNodelay=Y\n"
         << "StartTime=00:00:00\n"
         << "EndTime=00:00:00\n"
         << "CheckLatency=N\n"
         << "PersistMessages=N\n"
         << "ReconnectInterval=1\n"
         << "UseDataDictionary=" << (options.validate ? "Y" : "N") << "\n";

  if (options.sendBufferSize > 0) {
    stream << "SendBufferSize=" << options.sendBufferSize << "\n";
  }
  if (options.receiveBufferSize > 0) {
    stream << "ReceiveBufferSize=" << options.receiveBufferSize << "\n";
  }
  if (options.validate) {
    stream << "DataDictionary=" << options.dataDictionaryPath << "\n";
    if (options.fixedLayout) {
      stream << "ValidateUserDefinedFields=N\n";
    }
  }

  stream << "[SESSION]\n"
         << "BeginString=" << BeginString << "\n"
         << "SenderCompID=" << ClientCompID << "\n"
         << "TargetCompID=" << ServerCompID << "\n"
         << "HeartBtInt=30\n";
  return stream.str();
}

/**
 * @brief Prebuilds all raw-client application messages into one contiguous byte stream.
 *
 * Construction happens before timing, so raw server results exclude field formatting and checksum calculation.
 *
 * @param options Selects message count, kind, and layout.
 * @return Concatenated FIX messages with sequence numbers beginning at 2.
 */
std::string makeOutboundStream(const Options &options) {
  std::string sample = makeApplicationMessage(2, options);
  if (options.messages > std::numeric_limits<std::size_t>::max() / sample.size()) {
    throw std::runtime_error("message count is too large to prebuild in memory");
  }

  std::string outbound;
  outbound.reserve(static_cast<std::size_t>(options.messages) * sample.size());
  for (std::uint64_t i = 0; i < options.messages; ++i) {
    outbound += makeApplicationMessage(i + 2, options);
  }
  return outbound;
}

/**
 * @brief Estimates serialized application bytes for the QuickFIX client result report.
 *
 * @param options Selects message count, kind, and layout.
 * @return Sum of equivalent raw message sizes.
 */
std::uint64_t estimateOutboundBytes(const Options &options) {
  std::uint64_t bytes = 0;
  for (std::uint64_t i = 0; i < options.messages; ++i) {
    bytes += makeApplicationMessage(i + 2, options).size();
  }
  return bytes;
}

/**
 * @brief Builds one typed FIX.4.2 message for the QuickFIX initiator benchmark path.
 *
 * Session headers, BodyLength, and CheckSum are intentionally omitted here because `Session::sendToTarget()` supplies
 * and serializes them during the timed loop.
 *
 * @param seqNum Value used to make application identifiers unique. QuickFIX assigns the actual session sequence number.
 * @param options Selects the message kind and repeating-group count.
 * @return A typed QuickFIX application message ready for `Session::sendToTarget()`.
 */
FIX::Message makeQuickfixApplicationMessage(std::uint64_t seqNum, const Options &options) {
  if (options.messageKind == MessageKind::NewOrderSingle) {
    FIX42::NewOrderSingle message(
        FIX::ClOrdID("ORDER-" + std::to_string(seqNum)),
        FIX::HandlInst('1'),
        FIX::Symbol("LNUX"),
        FIX::Side(FIX::Side_BUY),
        FIX::TransactTime::now(),
        FIX::OrdType(FIX::OrdType_MARKET));
    message.set(FIX::OrderQty(100));
    message.set(FIX::TimeInForce(FIX::TimeInForce_DAY));
    message.set(FIX::Currency("USD"));
    return message;
  }

  if (options.messageKind == MessageKind::OrderCancelRequest) {
    FIX42::OrderCancelRequest message(
        FIX::OrigClOrdID("ORIG-" + std::to_string(seqNum > 2 ? seqNum - 1 : 1)),
        FIX::ClOrdID("CNCL-" + std::to_string(seqNum)),
        FIX::Symbol("LNUX"),
        FIX::Side(FIX::Side_BUY),
        FIX::TransactTime::now());
    message.set(FIX::OrderQty(100));
    return message;
  }

  if (options.messageKind == MessageKind::MarketDataSnapshot) {
    FIX42::MarketDataSnapshotFullRefresh message(FIX::Symbol("LNUX"));
    const char *types[] = {"0", "1", "2"};
    const char *prices[] = {"123.45", "123.46", "123.455"};
    const char *sizes[] = {"500", "400", "100"};
    for (int i = 0; i < 3; ++i) {
      FIX42::MarketDataSnapshotFullRefresh::NoMDEntries noMDEntries;
      noMDEntries.setField(269, types[i]);
      noMDEntries.setField(270, prices[i]);
      noMDEntries.setField(271, sizes[i]);
      noMDEntries.setField(273, "12:34:56");
      message.addGroup(noMDEntries);
    }
    return message;
  }

  FIX42::QuoteRequest message(FIX::QuoteReqID("QR-" + std::to_string(seqNum)));
  FIX42::QuoteRequest::NoRelatedSym noRelatedSym;
  for (int i = 0; i < options.quoteGroups; ++i) {
    noRelatedSym.set(FIX::Symbol("IBM" + std::to_string(i)));
    noRelatedSym.set(FIX::MaturityMonthYear("202612"));
    noRelatedSym.set(FIX::PutOrCall(FIX::PutOrCall_PUT));
    noRelatedSym.set(FIX::StrikePrice(120));
    noRelatedSym.set(FIX::Side(FIX::Side_BUY));
    noRelatedSym.set(FIX::OrderQty(100));
    noRelatedSym.set(FIX::OrdType(FIX::OrdType_MARKET));
    noRelatedSym.set(FIX::TransactTime::now());
    noRelatedSym.set(FIX::Currency("USD"));
    message.addGroup(noRelatedSym);
    noRelatedSym.clear();
  }
  return message;
}

/**
 * @brief Computes a steady-clock interval in seconds.
 *
 * @param start Beginning of the measured interval.
 * @param end End of the measured interval.
 * @return Fractional elapsed seconds.
 */
double elapsedSeconds(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end) {
  return std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
}

/**
 * @brief Prints the measured throughput together with every option needed to identify the experiment.
 *
 * @param mode Result mode label, currently `parse` or `server`.
 * @param options Configuration used by the measured run.
 * @param messages Number of application messages completed during the interval.
 * @param bytes Number of application bytes represented by those messages.
 * @param seconds Measured wall-clock duration in seconds.
 */
void printResult(
    const std::string &mode,
    const Options &options,
    std::uint64_t messages,
    std::uint64_t bytes,
    double seconds) {
  const double messagesPerSecond = static_cast<double>(messages) / seconds;
  const double mibPerSecond = static_cast<double>(bytes) / seconds / (1024.0 * 1024.0);
  const double nsPerMessage = seconds * 1000000000.0 / static_cast<double>(messages);

  std::cout << "mode=" << mode << "\n"
            << "client=" << clientModeName(options.clientMode) << "\n"
            << "message=" << messageKindName(options.messageKind) << "\n"
            << "messages=" << messages << "\n"
            << "bytes=" << bytes << "\n"
            << "validate=" << (options.validate ? "yes" : "no") << "\n"
            << "fixed_layout=" << (options.fixedLayout ? "yes" : "no") << "\n"
            << "socket_poll_mode=" << socketPollModeName(options) << "\n"
            << "busy_poll=" << (options.busyPoll ? "yes" : "no") << "\n"
            << "direct_read_poll=" << (options.directReadPoll ? "yes" : "no") << "\n"
            << "busy_poll_cpu=" << busyPollCpuName(options) << "\n"
            << std::fixed << std::setprecision(6) << "seconds=" << seconds << "\n"
            << std::setprecision(2) << "messages_per_second=" << messagesPerSecond << "\n"
            << "MiB_per_second=" << mibPerSecond << "\n"
            << "ns_per_message=" << nsPerMessage << "\n";
}

#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
/**
 * @brief Resets and enables acceptor diagnostics immediately before a timed server interval.
 *
 * @param acceptor Acceptor whose network thread supplies poll, recv, and parse counters.
 * @param options Enables collection only when `networkDiagnostics` is true.
 */
void startNetworkDiagnostics(FIX::SocketAcceptor &acceptor, const Options &options) {
  if (!options.networkDiagnostics) {
    return;
  }
  acceptor.setNetworkDiagnosticsEnabled(false);
  acceptor.resetNetworkDiagnostics();
  acceptor.setNetworkDiagnosticsEnabled(true);
}

/**
 * @brief Stops diagnostic collection and takes a stable counter snapshot.
 *
 * @param acceptor Acceptor that owns the counters.
 * @param options Determines whether diagnostics were requested.
 * @return Collected counters, or a zero-initialized snapshot when diagnostics are disabled.
 */
FIX::SocketAcceptorDiagnostics stopNetworkDiagnostics(FIX::SocketAcceptor &acceptor, const Options &options) {
  if (!options.networkDiagnostics) {
    return FIX::SocketAcceptorDiagnostics();
  }
  acceptor.setNetworkDiagnosticsEnabled(false);
  return acceptor.networkDiagnostics();
}

/**
 * @brief Prints raw network counters and derived batching metrics.
 *
 * @param options Determines whether diagnostic output is enabled.
 * @param diagnostics Counter snapshot captured after the timed interval.
 */
void printNetworkDiagnostics(const Options &options, const FIX::SocketAcceptorDiagnostics &diagnostics) {
  if (!options.networkDiagnostics) {
    return;
  }

  const double messagesPerRecv = diagnostics.recvCalls
      ? static_cast<double>(diagnostics.parsedMessages) / static_cast<double>(diagnostics.recvCalls)
      : 0.0;
  const double averageBytesPerRecv = diagnostics.recvCalls
      ? static_cast<double>(diagnostics.recvBytes) / static_cast<double>(diagnostics.recvCalls)
      : 0.0;
  const double averagePollWaitNanoseconds = diagnostics.pollCalls
      ? static_cast<double>(diagnostics.pollWaitNanoseconds) / static_cast<double>(diagnostics.pollCalls)
      : 0.0;

  std::cout << "network_diagnostics=yes\n"
            << "poll_calls=" << diagnostics.pollCalls << "\n"
            << "poll_wait_nanoseconds=" << diagnostics.pollWaitNanoseconds << "\n"
            << "poll_immediate_returns=" << diagnostics.pollImmediateReturns << "\n"
            << "poll_blocking_returns=" << diagnostics.pollBlockingReturns << "\n"
            << "poll_context_sample_failures=" << diagnostics.pollContextSampleFailures << "\n"
            << "recv_calls=" << diagnostics.recvCalls << "\n"
            << "recv_bytes=" << diagnostics.recvBytes << "\n"
            << "parsed_messages=" << diagnostics.parsedMessages << "\n"
            << std::fixed << std::setprecision(2) << "messages_per_recv=" << messagesPerRecv << "\n"
            << "average_bytes_per_recv=" << averageBytesPerRecv << "\n"
            << "average_poll_wait_nanoseconds=" << averagePollWaitNanoseconds << "\n";
}
#endif

/**
 * @brief Converts a scan result pointer into an offset suitable for scalar/SIMD comparison.
 *
 * @param begin First byte in the tested range.
 * @param end One-past-the-last byte; also represents a not-found result.
 * @param position Pointer returned by a scan implementation.
 * @return Zero-based offset from `begin`, or the range length when not found.
 */
std::size_t offsetOf(const char *begin, const char *end, const char *position) {
  return position == end ? static_cast<std::size_t>(end - begin) : static_cast<std::size_t>(position - begin);
}

/**
 * @brief Checks one character-search case against the scalar implementation.
 *
 * @param buffer Storage containing the tested range and any alignment prefix.
 * @param beginOffset Offset of the tested range in `buffer`.
 * @param length Number of bytes available to each implementation.
 * @param target Character to locate.
 * @param cases Running count incremented after a successful comparison.
 */
void verifyFastScanCase(
    const std::string &buffer,
    std::size_t beginOffset,
    std::size_t length,
    char target,
    std::uint64_t &cases) {
  const char *begin = buffer.data() + beginOffset;
  const char *end = begin + length;
  const char *scalar = FIX::detail::findCharScalar(begin, end, target);
  const char *simd = FIX::detail::findCharSimd(begin, end, target);
  const char *fast = FIX::detail::findCharFast(begin, end, target);

  const std::size_t scalarOffset = offsetOf(begin, end, scalar);
  if (offsetOf(begin, end, simd) != scalarOffset || offsetOf(begin, end, fast) != scalarOffset) {
    std::ostringstream error;
    error << "fast scan mismatch: begin=" << beginOffset << ", length=" << length << ", target="
          << static_cast<int>(static_cast<unsigned char>(target)) << ", scalar=" << scalarOffset
          << ", simd=" << offsetOf(begin, end, simd) << ", fast=" << offsetOf(begin, end, fast);
    throw std::runtime_error(error.str());
  }
  ++cases;
}

/**
 * @brief Checks one `SOH10=` pattern-search case against the scalar implementation.
 *
 * @param buffer Storage containing the tested range and any alignment prefix.
 * @param beginOffset Offset of the tested range in `buffer`.
 * @param length Number of bytes available to each implementation.
 * @param cases Running count incremented after a successful comparison.
 */
void verifySoh10ScanCase(
    const std::string &buffer,
    std::size_t beginOffset,
    std::size_t length,
    std::uint64_t &cases) {
  const char *begin = buffer.data() + beginOffset;
  const char *end = begin + length;
  const char *scalar = FIX::detail::findSoh10Scalar(begin, end);
  const char *simd = FIX::detail::findSoh10Simd(begin, end);
  const char *fast = FIX::detail::findSoh10Fast(begin, end);

  const std::size_t scalarOffset = offsetOf(begin, end, scalar);
  if (offsetOf(begin, end, simd) != scalarOffset || offsetOf(begin, end, fast) != scalarOffset) {
    std::ostringstream error;
    error << "SOH10 pattern scan mismatch: begin=" << beginOffset << ", length=" << length
          << ", scalar=" << scalarOffset << ", simd=" << offsetOf(begin, end, simd)
          << ", fast=" << offsetOf(begin, end, fast);
    throw std::runtime_error(error.str());
  }
  ++cases;
}

/**
 * @brief Compares scalar, explicit SIMD, and compile-time-selected scan results over boundary-heavy inputs.
 *
 * Prefixes vary pointer alignment; lengths exercise scalar tails; matches appear at the start, middle, end, and at
 * multiple positions.
 */
void runFastScanSelfTest() {
  const std::vector<char> targets = {'=', SOH, 'X'};
  std::uint64_t charCases = 0;
  std::uint64_t patternCases = 0;

  for (char target : targets) {
    for (std::size_t prefix = 0; prefix < 32; ++prefix) {
      for (std::size_t length = 0; length <= 192; ++length) {
        std::string buffer(prefix + length, 'a');
        verifyFastScanCase(buffer, prefix, length, target, charCases);

        if (length > 0) {
          const std::size_t positions[] = {0, length / 2, length - 1};
          for (std::size_t position : positions) {
            buffer.assign(prefix + length, 'a');
            buffer[prefix + position] = target;
            verifyFastScanCase(buffer, prefix, length, target, charCases);
          }

          if (length > 2) {
            buffer.assign(prefix + length, 'a');
            buffer[prefix + 1] = target;
            buffer[prefix + length - 1] = target;
            verifyFastScanCase(buffer, prefix, length, target, charCases);
          }
        }
      }
    }
  }

  for (std::size_t prefix = 0; prefix < 32; ++prefix) {
    for (std::size_t length = 0; length <= 192; ++length) {
      std::string buffer(prefix + length, 'a');
      verifySoh10ScanCase(buffer, prefix, length, patternCases);

      if (length >= 4) {
        const std::size_t positions[] = {0, (length - 4) / 2, length - 4};
        for (std::size_t position : positions) {
          buffer.assign(prefix + length, 'a');
          buffer.replace(prefix + position, 4, "\00110=", 4);
          verifySoh10ScanCase(buffer, prefix, length, patternCases);
        }

        if (length >= 8) {
          buffer.assign(prefix + length, 'a');
          buffer.replace(prefix + 1, 4, "\00110=", 4);
          buffer.replace(prefix + length - 4, 4, "\00110=", 4);
          verifySoh10ScanCase(buffer, prefix, length, patternCases);
        }
      }
    }
  }

  std::cout << "fast_scan_self_test=pass\n"
            << "char_cases=" << charCases << "\n"
            << "pattern_cases=" << patternCases << "\n"
            << "cases=" << (charCases + patternCases) << "\n"
            << "simd_available=" << (FIX::detail::simdFastScanAvailable() ? "yes" : "no") << "\n";
}

/**
 * @brief Requires Parser to emit exactly the expected complete wire message.
 *
 * @param parser Parser containing previously supplied stream bytes.
 * @param expected Exact wire message that must be emitted next.
 * @param label Human-readable case name included in failures.
 */
void requireParserRead(FIX::Parser &parser, const std::string &expected, const std::string &label) {
  std::string actual;
  if (!parser.readFixMessage(actual)) {
    throw std::runtime_error("parser self-test failed to read: " + label);
  }
  if (actual != expected) {
    throw std::runtime_error("parser self-test message mismatch: " + label);
  }
}

/**
 * @brief Requires Parser to report that no complete message is currently buffered.
 *
 * @param parser Parser to query.
 * @param label Human-readable case name included in failures.
 */
void requireParserEmpty(FIX::Parser &parser, const std::string &label) {
  std::string actual;
  if (parser.readFixMessage(actual)) {
    throw std::runtime_error("parser self-test unexpectedly read a message: " + label);
  }
}

/**
 * @brief Verifies Parser message framing independently from Message field parsing.
 *
 * Cases cover one message, packed messages, a fragmented message, a garbage prefix, and a larger repeating-group
 * message.
 */
void runParserSelfTest() {
  Options orderOptions;
  const std::string firstOrder = makeApplicationMessage(2, orderOptions);
  const std::string secondOrder = makeApplicationMessage(3, orderOptions);

  std::uint64_t cases = 0;

  {
    FIX::Parser parser;
    parser.addToStream(firstOrder);
    requireParserRead(parser, firstOrder, "single message");
    requireParserEmpty(parser, "single message drained");
    cases += 2;
  }

  {
    FIX::Parser parser;
    parser.addToStream(firstOrder + secondOrder);
    requireParserRead(parser, firstOrder, "first packed message");
    requireParserRead(parser, secondOrder, "second packed message");
    requireParserEmpty(parser, "packed messages drained");
    cases += 3;
  }

  {
    FIX::Parser parser;
    const std::size_t split = firstOrder.size() / 2;
    parser.addToStream(firstOrder.data(), split);
    requireParserEmpty(parser, "partial message");
    parser.addToStream(firstOrder.data() + split, firstOrder.size() - split);
    requireParserRead(parser, firstOrder, "completed partial message");
    cases += 2;
  }

  {
    FIX::Parser parser;
    parser.addToStream("junk-before-message");
    parser.addToStream(firstOrder);
    requireParserRead(parser, firstOrder, "garbage prefix");
    cases += 1;
  }

  {
    Options quoteOptions;
    quoteOptions.messageKind = MessageKind::QuoteRequest;
    const std::string quoteRequest = makeApplicationMessage(2, quoteOptions);
    FIX::Parser parser;
    parser.addToStream(quoteRequest);
    requireParserRead(parser, quoteRequest, "quote request");
    cases += 1;
  }

  std::cout << "parser_self_test=pass\n"
            << "cases=" << cases << "\n"
            << "simd_available=" << (FIX::detail::simdFastScanAvailable() ? "yes" : "no") << "\n";
}

/**
 * @brief Expected wire bytes and semantic values for one generated correctness-test message.
 */
struct ExpectedMessage {
  MessageKind kind = MessageKind::NewOrderSingle;
  std::uint64_t seqNum = 0;
  int quoteGroups = 0;
  bool fixedLayout = false;
  std::string now;
  std::string wire;
};

/**
 * @brief Aggregated work completed by the end-to-end correctness self-test.
 */
struct CorrectnessStats {
  std::uint64_t messages = 0;
  std::uint64_t bytes = 0;
  std::uint64_t fieldChecks = 0;
};

/**
 * @brief Builds one deterministic message together with the values later used to verify it.
 *
 * @param kind Application message type to generate.
 * @param seqNum FIX sequence number and identifier suffix.
 * @param now Stable timestamp shared by all messages in one test run.
 * @param options Supplies fixed-layout and repeating-group settings.
 * @return Expected wire bytes and verification metadata.
 */
ExpectedMessage makeExpectedMessage(
    MessageKind kind,
    std::uint64_t seqNum,
    const std::string &now,
    const Options &options) {
  Options messageOptions = options;
  messageOptions.messageKind = kind;

  ExpectedMessage expected;
  expected.kind = kind;
  expected.seqNum = seqNum;
  expected.quoteGroups = messageOptions.quoteGroups;
  expected.fixedLayout = messageOptions.fixedLayout;
  expected.now = now;
  if (kind == MessageKind::NewOrderSingle) {
    expected.wire = buildFixMessage(
        messageOptions.fixedLayout ? fixedNewOrderSingleFields(seqNum, now, messageOptions)
                                   : newOrderSingleFields(seqNum, now, messageOptions));
  } else if (kind == MessageKind::OrderCancelRequest) {
    expected.wire = buildFixMessage(
        messageOptions.fixedLayout ? fixedOrderCancelRequestFields(seqNum, now, messageOptions)
                                   : orderCancelRequestFields(seqNum, now, messageOptions));
  } else if (kind == MessageKind::MarketDataSnapshot) {
    expected.wire = buildFixMessage(
        messageOptions.fixedLayout ? fixedMarketDataSnapshotFields(seqNum, now, messageOptions)
                                   : marketDataSnapshotFields(seqNum, now, messageOptions));
  } else {
    expected.wire = buildFixMessage(quoteRequestFields(seqNum, now, messageOptions));
  }
  return expected;
}

/**
 * @brief Reads a top-level field directly from expected wire bytes without using the parser under test.
 *
 * @param wire Complete expected FIX message.
 * @param tag Numeric tag to locate.
 * @return Field value between `=` and SOH.
 */
std::string wireFieldValue(const std::string &wire, int tag) {
  const std::string wanted = std::to_string(tag);
  std::size_t pos = 0;

  while (pos < wire.size()) {
    const std::size_t equalSign = wire.find('=', pos);
    const std::size_t fieldEnd = wire.find(SOH, pos);
    if (equalSign == std::string::npos || fieldEnd == std::string::npos || equalSign > fieldEnd) {
      break;
    }
    if (wire.compare(pos, equalSign - pos, wanted) == 0) {
      return wire.substr(equalSign + 1, fieldEnd - equalSign - 1);
    }
    pos = fieldEnd + 1;
  }

  throw std::runtime_error("expected wire field not found: " + wanted);
}

/**
 * @brief Requires one parsed field to exist and equal its independently expected string value.
 *
 * @param map Header, body, trailer, or repeating group containing the field.
 * @param tag Numeric field tag.
 * @param expected Required value.
 * @param label Context included in an error message.
 * @param fieldChecks Running successful-assertion count.
 */
void requireFieldValue(
    const FIX::FieldMap &map,
    int tag,
    const std::string &expected,
    const std::string &label,
    std::uint64_t &fieldChecks) {
  try {
    const std::string actual = map.getField(tag);
    if (actual != expected) {
      std::ostringstream error;
      error << label << " tag " << tag << " mismatch: expected '" << expected << "', actual '" << actual << "'";
      throw std::runtime_error(error.str());
    }
  } catch (FIX::FieldNotFound &) {
    std::ostringstream error;
    error << label << " missing tag " << tag;
    throw std::runtime_error(error.str());
  }
  ++fieldChecks;
}

/**
 * @brief Requires a FieldMap to contain exactly the expected number of fields.
 *
 * @param map Header, body, trailer, or repeating group to inspect.
 * @param expected Required field count.
 * @param label Context included in an error message.
 * @param fieldChecks Running successful-assertion count.
 */
void requireTotalFields(
    const FIX::FieldMap &map,
    std::size_t expected,
    const std::string &label,
    std::uint64_t &fieldChecks) {
  const std::size_t actual = map.totalFields();
  if (actual != expected) {
    std::ostringstream error;
    error << label << " field count mismatch: expected " << expected << ", actual " << actual;
    throw std::runtime_error(error.str());
  }
  ++fieldChecks;
}

/**
 * @brief Requires a repeating-group count to match the generated message.
 *
 * @param map Message or group that owns the repeating group.
 * @param tag Repeating-group count tag.
 * @param expected Required number of group instances.
 * @param label Context included in an error message.
 * @param fieldChecks Running successful-assertion count.
 */
void requireGroupCount(
    const FIX::FieldMap &map,
    int tag,
    std::size_t expected,
    const std::string &label,
    std::uint64_t &fieldChecks) {
  const std::size_t actual = map.groupCount(tag);
  if (actual != expected) {
    std::ostringstream error;
    error << label << " group count mismatch for tag " << tag << ": expected " << expected << ", actual " << actual;
    throw std::runtime_error(error.str());
  }
  ++fieldChecks;
}

/**
 * @brief Verifies header and trailer fields shared by every correctness-test message.
 *
 * @param message Parsed QuickFIX message.
 * @param expected Independently generated values and wire bytes.
 * @param fieldChecks Running successful-assertion count.
 */
void verifyCommonFields(const FIX::Message &message, const ExpectedMessage &expected, std::uint64_t &fieldChecks) {
  const char *msgType = "";
  switch (expected.kind) {
  case MessageKind::NewOrderSingle:
    msgType = "D";
    break;
  case MessageKind::OrderCancelRequest:
    msgType = "F";
    break;
  case MessageKind::MarketDataSnapshot:
    msgType = "W";
    break;
  case MessageKind::QuoteRequest:
    msgType = "R";
    break;
  }

  requireTotalFields(message.getHeader(), 7, "header", fieldChecks);
  requireFieldValue(message.getHeader(), 8, BeginString, "header", fieldChecks);
  requireFieldValue(message.getHeader(), 9, wireFieldValue(expected.wire, 9), "header", fieldChecks);
  requireFieldValue(message.getHeader(), 35, msgType, "header", fieldChecks);
  requireFieldValue(message.getHeader(), 49, ClientCompID, "header", fieldChecks);
  requireFieldValue(message.getHeader(), 56, ServerCompID, "header", fieldChecks);
  requireFieldValue(
      message.getHeader(),
      34,
      expected.fixedLayout ? fixedNumber(expected.seqNum, 12) : std::to_string(expected.seqNum),
      "header",
      fieldChecks);
  requireFieldValue(message.getHeader(), 52, expected.now, "header", fieldChecks);

  requireTotalFields(message.getTrailer(), 1, "trailer", fieldChecks);
  requireFieldValue(message.getTrailer(), 10, wireFieldValue(expected.wire, 10), "trailer", fieldChecks);
}

/**
 * @brief Verifies every NewOrderSingle body field, including fixed-layout tag 9001 when enabled.
 *
 * @param message Parsed QuickFIX message.
 * @param expected Expected sequence number, timestamp, and layout.
 * @param fieldChecks Running successful-assertion count.
 */
void verifyNewOrderSingleFields(
    const FIX::Message &message,
    const ExpectedMessage &expected,
    std::uint64_t &fieldChecks) {
  requireTotalFields(message, expected.fixedLayout ? 10 : 9, "new-order-single body", fieldChecks);
  if (expected.fixedLayout) {
    requireFieldValue(message, 9001, "NOS1", "new-order-single body", fieldChecks);
  }
  requireFieldValue(
      message,
      11,
      expected.fixedLayout ? "ORDER-" + fixedNumber(expected.seqNum, 12) : "ORDER-" + std::to_string(expected.seqNum),
      "new-order-single body",
      fieldChecks);
  requireFieldValue(message, 21, "1", "new-order-single body", fieldChecks);
  requireFieldValue(message, 55, "LNUX", "new-order-single body", fieldChecks);
  requireFieldValue(message, 54, "1", "new-order-single body", fieldChecks);
  requireFieldValue(message, 60, expected.now, "new-order-single body", fieldChecks);
  requireFieldValue(message, 38, expected.fixedLayout ? "0000000100" : "100", "new-order-single body", fieldChecks);
  requireFieldValue(message, 40, "1", "new-order-single body", fieldChecks);
  requireFieldValue(message, 59, "0", "new-order-single body", fieldChecks);
  requireFieldValue(message, 15, "USD", "new-order-single body", fieldChecks);
}

/**
 * @brief Verifies every OrderCancelRequest body field and fixed-width identifiers.
 *
 * @param message Parsed QuickFIX message.
 * @param expected Expected sequence number, timestamp, and layout.
 * @param fieldChecks Running successful-assertion count.
 */
void verifyOrderCancelRequestFields(
    const FIX::Message &message,
    const ExpectedMessage &expected,
    std::uint64_t &fieldChecks) {
  requireTotalFields(message, expected.fixedLayout ? 7 : 6, "order-cancel-request body", fieldChecks);
  if (expected.fixedLayout) {
    requireFieldValue(message, 9001, "CXL1", "order-cancel-request body", fieldChecks);
  }

  const std::string origSeq =
      expected.fixedLayout ? fixedNumber(expected.seqNum > 2 ? expected.seqNum - 1 : 1, 12)
                           : std::to_string(expected.seqNum > 2 ? expected.seqNum - 1 : 1);
  const std::string seq = expected.fixedLayout ? fixedNumber(expected.seqNum, 12) : std::to_string(expected.seqNum);
  requireFieldValue(message, 41, "ORIG-" + origSeq, "order-cancel-request body", fieldChecks);
  requireFieldValue(message, 11, "CNCL-" + seq, "order-cancel-request body", fieldChecks);
  requireFieldValue(message, 55, "LNUX", "order-cancel-request body", fieldChecks);
  requireFieldValue(message, 54, "1", "order-cancel-request body", fieldChecks);
  requireFieldValue(message, 60, expected.now, "order-cancel-request body", fieldChecks);
  requireFieldValue(message, 38, expected.fixedLayout ? "0000000100" : "100", "order-cancel-request body", fieldChecks);
}

/**
 * @brief Verifies QuoteRequest body fields and every NoRelatedSym repeating-group entry.
 *
 * @param message Parsed QuickFIX message.
 * @param expected Expected sequence number and group count.
 * @param fieldChecks Running successful-assertion count.
 */
void verifyQuoteRequestFields(
    const FIX::Message &message,
    const ExpectedMessage &expected,
    std::uint64_t &fieldChecks) {
  requireTotalFields(
      message,
      2 + static_cast<std::size_t>(expected.quoteGroups) * 9,
      "quote-request body",
      fieldChecks);
  requireFieldValue(message, 131, "QR-" + std::to_string(expected.seqNum), "quote-request body", fieldChecks);
  requireFieldValue(message, 146, std::to_string(expected.quoteGroups), "quote-request body", fieldChecks);
  requireGroupCount(message, 146, static_cast<std::size_t>(expected.quoteGroups), "quote-request body", fieldChecks);

  for (int i = 0; i < expected.quoteGroups; ++i) {
    FIX42::QuoteRequest::NoRelatedSym group;
    try {
      message.getGroup(i + 1, group);
    } catch (FIX::FieldNotFound &) {
      std::ostringstream error;
      error << "quote-request missing group " << (i + 1);
      throw std::runtime_error(error.str());
    }

    const std::string label = "quote-request group " + std::to_string(i + 1);
    requireTotalFields(group, 9, label, fieldChecks);
    requireFieldValue(group, 55, "IBM" + std::to_string(i), label, fieldChecks);
    requireFieldValue(group, 200, "202612", label, fieldChecks);
    requireFieldValue(group, 201, "0", label, fieldChecks);
    requireFieldValue(group, 202, "120", label, fieldChecks);
    requireFieldValue(group, 54, "1", label, fieldChecks);
    requireFieldValue(group, 38, "100", label, fieldChecks);
    requireFieldValue(group, 40, "1", label, fieldChecks);
    requireFieldValue(group, 60, expected.now, label, fieldChecks);
    requireFieldValue(group, 15, "USD", label, fieldChecks);
  }
}

/**
 * @brief Verifies MarketDataSnapshot body fields and all three NoMDEntries groups.
 *
 * @param message Parsed QuickFIX message.
 * @param expected Expected sequence number and fixed-layout state.
 * @param fieldChecks Running successful-assertion count.
 */
void verifyMarketDataSnapshotFields(
    const FIX::Message &message,
    const ExpectedMessage &expected,
    std::uint64_t &fieldChecks) {
  const char *prices[] = {"123.45", "123.46", "123.455"};
  const char *fixedPrices[] = {"00000123.4500", "00000123.4600", "00000123.4550"};
  const char *sizes[] = {"500", "400", "100"};
  const char *fixedSizes[] = {"0000000500", "0000000400", "0000000100"};
  const char *types[] = {"0", "1", "2"};

  requireTotalFields(message, expected.fixedLayout ? 16 : 15, "market-data-snapshot body", fieldChecks);
  if (expected.fixedLayout) {
    requireFieldValue(message, 9001, "MDW1", "market-data-snapshot body", fieldChecks);
    requireFieldValue(
        message,
        262,
        "MDREQ-" + fixedNumber(expected.seqNum, 12),
        "market-data-snapshot body",
        fieldChecks);
  } else {
    requireFieldValue(
        message,
        262,
        "MDREQ-" + std::to_string(expected.seqNum),
        "market-data-snapshot body",
        fieldChecks);
  }
  requireFieldValue(message, 55, "LNUX", "market-data-snapshot body", fieldChecks);
  requireFieldValue(message, 268, "3", "market-data-snapshot body", fieldChecks);
  requireGroupCount(message, 268, 3, "market-data-snapshot body", fieldChecks);

  for (int i = 0; i < 3; ++i) {
    FIX42::MarketDataSnapshotFullRefresh::NoMDEntries group;
    try {
      message.getGroup(i + 1, group);
    } catch (FIX::FieldNotFound &) {
      std::ostringstream error;
      error << "market-data-snapshot missing group " << (i + 1);
      throw std::runtime_error(error.str());
    }

    const std::string label = "market-data-snapshot group " + std::to_string(i + 1);
    requireTotalFields(group, 4, label, fieldChecks);
    requireFieldValue(group, 269, types[i], label, fieldChecks);
    requireFieldValue(group, 270, expected.fixedLayout ? fixedPrices[i] : prices[i], label, fieldChecks);
    requireFieldValue(group, 271, expected.fixedLayout ? fixedSizes[i] : sizes[i], label, fieldChecks);
    requireFieldValue(group, 273, "12:34:56", label, fieldChecks);
  }
}

/**
 * @brief Verifies one message at the wire, dictionary, field, and repeating-group levels.
 *
 * @param actualWire Exact message boundary emitted by Parser.
 * @param expected Independently generated message and semantic expectations.
 * @param dictionary FIX.4.2 dictionary used for parsing and validation.
 * @param stats Aggregated counters updated after a successful verification.
 */
void verifyParsedMessage(
    const std::string &actualWire,
    const ExpectedMessage &expected,
    const FIX::DataDictionary &dictionary,
    CorrectnessStats &stats) {
  if (actualWire != expected.wire) {
    std::ostringstream error;
    error << "parser emitted different wire bytes for " << messageKindName(expected.kind) << " seq " << expected.seqNum;
    throw std::runtime_error(error.str());
  }

  FIX::Message message;
  message.setString(actualWire, true, &dictionary);
  FIX::DataDictionary::validate(message, &dictionary, &dictionary);

  verifyCommonFields(message, expected, stats.fieldChecks);
  if (expected.kind == MessageKind::NewOrderSingle) {
    verifyNewOrderSingleFields(message, expected, stats.fieldChecks);
  } else if (expected.kind == MessageKind::OrderCancelRequest) {
    verifyOrderCancelRequestFields(message, expected, stats.fieldChecks);
  } else if (expected.kind == MessageKind::MarketDataSnapshot) {
    verifyMarketDataSnapshotFields(message, expected, stats.fieldChecks);
  } else {
    verifyQuoteRequestFields(message, expected, stats.fieldChecks);
  }

  ++stats.messages;
  stats.bytes += actualWire.size();
}

/**
 * @brief Drains all complete Parser messages and matches each one with the next expected message.
 *
 * @param parser Parser receiving arbitrarily chunked stream data.
 * @param expectedMessages FIFO preserving generated message order.
 * @param dictionary FIX.4.2 dictionary used for semantic verification.
 * @param stats Aggregated counters updated for every drained message.
 */
void drainParsedMessages(
    FIX::Parser &parser,
    std::deque<ExpectedMessage> &expectedMessages,
    const FIX::DataDictionary &dictionary,
    CorrectnessStats &stats) {
  std::string actualWire;
  while (parser.readFixMessage(actualWire)) {
    if (expectedMessages.empty()) {
      throw std::runtime_error("parser emitted more messages than expected");
    }
    verifyParsedMessage(actualWire, expectedMessages.front(), dictionary, stats);
    expectedMessages.pop_front();
  }
}

/**
 * @brief Selects a deterministic byte chunk size used to mimic changing recv boundaries.
 *
 * @param index Monotonic chunk index.
 * @return Selected number of bytes to append to Parser.
 */
std::size_t nextCorrectnessChunkSize(std::uint64_t index) {
  static const std::size_t chunks[] = {1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610, 987, 1597, 4096, 8191};
  return chunks[index % (sizeof(chunks) / sizeof(chunks[0]))];
}

/**
 * @brief Selects how many complete FIX messages are packed into the next generated batch.
 *
 * @param index Monotonic batch index.
 * @return Selected number of messages.
 */
std::uint64_t nextCorrectnessBatchSize(std::uint64_t index) {
  static const std::uint64_t batches[] = {1, 2, 5, 13, 32, 64};
  return batches[index % (sizeof(batches) / sizeof(batches[0]))];
}

/**
 * @brief Simulates a byte stream of packed and fragmented messages of one application type.
 *
 * Irregular chunk and batch sizes model arbitrary recv boundaries while preserving expected message order.
 *
 * @param kind Message type generated for this pass.
 * @param options Supplies message count, layout, and repeating-group settings.
 * @param dictionary FIX.4.2 dictionary used for parsing and validation.
 * @return Total verified messages, bytes, and field checks.
 */
CorrectnessStats runCorrectnessSelfTestForKind(
    MessageKind kind,
    const Options &options,
    const FIX::DataDictionary &dictionary) {
  FIX::Parser parser;
  std::deque<ExpectedMessage> expectedMessages;
  CorrectnessStats stats;
  const std::string now = timestamp();
  std::uint64_t generated = 0;
  std::uint64_t batchIndex = 0;
  std::uint64_t chunkIndex = 0;

  parser.addToStream("noise-before-first-fix-message");
  drainParsedMessages(parser, expectedMessages, dictionary, stats);

  while (generated < options.messages) {
    const std::uint64_t remaining = options.messages - generated;
    const std::uint64_t batchMessages = std::min(remaining, nextCorrectnessBatchSize(batchIndex++));
    std::string batch;

    for (std::uint64_t i = 0; i < batchMessages; ++i) {
      ExpectedMessage expected = makeExpectedMessage(kind, generated + i + 2, now, options);
      batch += expected.wire;
      expectedMessages.push_back(std::move(expected));
    }

    std::size_t offset = 0;
    while (offset < batch.size()) {
      const std::size_t chunkSize = std::min(batch.size() - offset, nextCorrectnessChunkSize(chunkIndex++));
      parser.addToStream(batch.data() + offset, chunkSize);
      offset += chunkSize;
      drainParsedMessages(parser, expectedMessages, dictionary, stats);
    }

    generated += batchMessages;
  }

  drainParsedMessages(parser, expectedMessages, dictionary, stats);
  if (!expectedMessages.empty()) {
    throw std::runtime_error("parser did not emit all expected messages");
  }

  std::string extra;
  if (parser.readFixMessage(extra)) {
    throw std::runtime_error("parser emitted an unexpected trailing message");
  }

  return stats;
}

/**
 * @brief Verifies that a fixed-layout marker with the wrong message length falls back to generic field parsing.
 *
 * @param options Base benchmark settings used to construct the fixed-layout message.
 * @param dictionary FIX.4.2 dictionary used to validate the fallback result.
 * @return Counters for the one verified fallback message.
 */
CorrectnessStats runFixedLayoutFallbackSelfTest(const Options &options, const FIX::DataDictionary &dictionary) {
  Options messageOptions = options;
  messageOptions.fixedLayout = true;
  const std::string now = timestamp();
  std::vector<std::pair<int, std::string>> fields = fixedNewOrderSingleFields(2, now, messageOptions);
  fields.emplace_back(9002, "FALLBACK");
  const std::string wire = buildFixMessage(fields);
  if (wire.size() == 187) {
    throw std::runtime_error("fixed-layout fallback self-test did not change message length");
  }

  FIX::Parser parser;
  parser.addToStream(wire);
  std::string actualWire;
  if (!parser.readFixMessage(actualWire)) {
    throw std::runtime_error("fixed-layout fallback self-test did not read a message");
  }
  if (actualWire != wire) {
    throw std::runtime_error("fixed-layout fallback self-test parser wire mismatch");
  }

  CorrectnessStats stats;
  FIX::Message message;
  message.setString(actualWire, true, &dictionary);
  FIX::DataDictionary::validate(message, &dictionary, &dictionary);
  requireFieldValue(message, 9001, "NOS1", "fixed-layout fallback body", stats.fieldChecks);
  requireFieldValue(message, 9002, "FALLBACK", "fixed-layout fallback body", stats.fieldChecks);
  requireFieldValue(message, 11, "ORDER-" + fixedNumber(2, 12), "fixed-layout fallback body", stats.fieldChecks);
  requireTotalFields(message, 11, "fixed-layout fallback body", stats.fieldChecks);

  std::string trailing;
  if (parser.readFixMessage(trailing)) {
    throw std::runtime_error("fixed-layout fallback self-test emitted a trailing message");
  }

  stats.messages = 1;
  stats.bytes = actualWire.size();
  return stats;
}

/**
 * @brief Verifies that the correct total length with tag 9001 at the wrong offset also uses generic parsing.
 *
 * @param options Base benchmark settings; retained for a uniform fallback-test interface.
 * @param dictionary FIX.4.2 dictionary used to validate the fallback result.
 * @return Counters for the one verified fallback message.
 */
CorrectnessStats runFixedLayoutMarkerFallbackSelfTest(
    const Options &options,
    const FIX::DataDictionary &dictionary) {
  (void)options;
  const std::string now = timestamp();
  const std::string seq = fixedNumber(2, 12);
  std::vector<std::pair<int, std::string>> fields = fixedHeaderFields("D", 2, now);
  fields.emplace_back(11, "ORDER-" + seq);
  fields.emplace_back(9001, "NOS1");
  fields.emplace_back(21, "1");
  fields.emplace_back(55, "LNUX");
  fields.emplace_back(54, "1");
  fields.emplace_back(60, now);
  fields.emplace_back(38, "0000000100");
  fields.emplace_back(40, "1");
  fields.emplace_back(59, "0");
  fields.emplace_back(15, "USD");

  const std::string wire = buildFixMessage(fields);
  if (wire.size() != 187) {
    throw std::runtime_error("fixed-layout marker fallback self-test did not preserve message length");
  }
  if (wire.compare(78, 5, "9001=") == 0) {
    throw std::runtime_error("fixed-layout marker fallback self-test did not move marker offset");
  }

  FIX::Parser parser;
  parser.addToStream(wire);
  std::string actualWire;
  if (!parser.readFixMessage(actualWire)) {
    throw std::runtime_error("fixed-layout marker fallback self-test did not read a message");
  }
  if (actualWire != wire) {
    throw std::runtime_error("fixed-layout marker fallback self-test parser wire mismatch");
  }

  CorrectnessStats stats;
  FIX::Message message;
  message.setString(actualWire, true, &dictionary);
  FIX::DataDictionary::validate(message, &dictionary, &dictionary);
  requireFieldValue(message, 9001, "NOS1", "fixed-layout marker fallback body", stats.fieldChecks);
  requireFieldValue(message, 11, "ORDER-" + seq, "fixed-layout marker fallback body", stats.fieldChecks);
  requireTotalFields(message, 10, "fixed-layout marker fallback body", stats.fieldChecks);

  std::string trailing;
  if (parser.readFixMessage(trailing)) {
    throw std::runtime_error("fixed-layout marker fallback self-test emitted a trailing message");
  }

  stats.messages = 1;
  stats.bytes = actualWire.size();
  return stats;
}

/**
 * @brief Runs the full Parser-to-Message correctness suite selected by the command line.
 *
 * Fixed-layout mode verifies all three templates and both fallback conditions. Normal mode verifies ordinary messages
 * and a configurable repeating-group QuoteRequest.
 *
 * @param options Selects message count, fixed-layout mode, dictionary, and group count.
 */
void runCorrectnessSelfTest(const Options &options) {
  FIX::DataDictionary dictionary(options.dataDictionaryPath);
  if (options.fixedLayout) {
    dictionary.checkUserDefinedFields(false);
  }

  if (options.fixedLayout) {
    const CorrectnessStats orderStats =
        runCorrectnessSelfTestForKind(MessageKind::NewOrderSingle, options, dictionary);
    const CorrectnessStats cancelStats =
        runCorrectnessSelfTestForKind(MessageKind::OrderCancelRequest, options, dictionary);
    const CorrectnessStats marketDataStats =
        runCorrectnessSelfTestForKind(MessageKind::MarketDataSnapshot, options, dictionary);
    const CorrectnessStats lengthFallbackStats = runFixedLayoutFallbackSelfTest(options, dictionary);
    const CorrectnessStats markerFallbackStats = runFixedLayoutMarkerFallbackSelfTest(options, dictionary);

    std::cout << "correctness_self_test=pass\n"
              << "messages="
              << (orderStats.messages + cancelStats.messages + marketDataStats.messages
                  + lengthFallbackStats.messages + markerFallbackStats.messages)
              << "\n"
              << "new_order_single_messages=" << orderStats.messages << "\n"
              << "order_cancel_request_messages=" << cancelStats.messages << "\n"
              << "market_data_snapshot_messages=" << marketDataStats.messages << "\n"
              << "fallback_messages=" << (lengthFallbackStats.messages + markerFallbackStats.messages) << "\n"
              << "length_mismatch_fallback_messages=" << lengthFallbackStats.messages << "\n"
              << "marker_mismatch_fallback_messages=" << markerFallbackStats.messages << "\n"
              << "fixed_layout=yes\n"
              << "bytes="
              << (orderStats.bytes + cancelStats.bytes + marketDataStats.bytes + lengthFallbackStats.bytes
                  + markerFallbackStats.bytes)
              << "\n"
              << "field_checks="
              << (orderStats.fieldChecks + cancelStats.fieldChecks + marketDataStats.fieldChecks
                  + lengthFallbackStats.fieldChecks + markerFallbackStats.fieldChecks)
              << "\n"
              << "parser_wire_compare=yes\n"
              << "message_field_compare=yes\n"
              << "length_mismatch_fallback=yes\n"
              << "marker_mismatch_fallback=yes\n"
              << "data_dictionary_validate=yes\n"
              << "simd_available=" << (FIX::detail::simdFastScanAvailable() ? "yes" : "no") << "\n";
    return;
  }

  const CorrectnessStats orderStats = runCorrectnessSelfTestForKind(MessageKind::NewOrderSingle, options, dictionary);
  const CorrectnessStats quoteStats = runCorrectnessSelfTestForKind(MessageKind::QuoteRequest, options, dictionary);

  std::cout << "correctness_self_test=pass\n"
            << "messages=" << (orderStats.messages + quoteStats.messages) << "\n"
            << "new_order_single_messages=" << orderStats.messages << "\n"
            << "quote_request_messages=" << quoteStats.messages << "\n"
            << "quote_groups=" << options.quoteGroups << "\n"
            << "fixed_layout=no\n"
            << "bytes=" << (orderStats.bytes + quoteStats.bytes) << "\n"
            << "field_checks=" << (orderStats.fieldChecks + quoteStats.fieldChecks) << "\n"
            << "parser_wire_compare=yes\n"
            << "message_field_compare=yes\n"
            << "data_dictionary_validate=yes\n"
            << "simd_available=" << (FIX::detail::simdFastScanAvailable() ? "yes" : "no") << "\n";
}

/**
 * @brief Waits until the benchmark application observes a successful FIX Logon.
 *
 * @param application Application whose atomic Logon state is polled.
 * @param timeoutSeconds Maximum wait duration.
 * @return True when Logon arrives before the deadline.
 */
bool waitForLogon(const BenchmarkApplication &application, int timeoutSeconds) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (application.loggedOn.load(std::memory_order_acquire)) {
      return true;
    }
    FIX::process_sleep(0.001);
  }
  return false;
}

/**
 * @brief Waits until the server application has processed the requested number of application messages.
 *
 * @param application Application whose `received` counter is polled.
 * @param expected Required number of completed `fromApp()` callbacks.
 * @param timeoutSeconds Maximum wait duration.
 * @return True when the expected count is reached before the deadline.
 */
bool waitForMessages(const BenchmarkApplication &application, std::uint64_t expected, int timeoutSeconds) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (application.received.load(std::memory_order_relaxed) >= expected) {
      return true;
    }
    FIX::process_sleep(0.001);
  }
  return false;
}

/**
 * @brief Sends a complete prebuilt byte stream, retrying partial successful sends.
 *
 * @param socket Connected raw-client socket.
 * @param data Complete Logon or application-message stream to send.
 */
void sendAll(FIX::socket_handle socket, const std::string &data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    auto result = FIX::socket_send(socket, data.data() + sent, data.size() - sent);
    if (result <= 0) {
      throw std::runtime_error("socket send failed: " + FIX::socket_get_last_error());
    }
    sent += static_cast<std::size_t>(result);
  }
}

/**
 * @brief Connects the raw client to the local benchmark acceptor with bounded retries.
 *
 * @param port Actual port bound by SocketAcceptor.
 * @param options Supplies optional client socket buffer sizes.
 * @return RAII guard owning the connected socket.
 */
SocketGuard connectWithRetry(int port, const Options &options) {
  for (int attempt = 0; attempt < 100; ++attempt) {
    SocketGuard socket(FIX::socket_createConnector());
    if (!FIX::socket_isValid(socket.get())) {
      throw std::runtime_error("socket create failed: " + FIX::socket_get_last_error());
    }

    FIX::socket_setsockopt(socket.get(), TCP_NODELAY);
    if (options.sendBufferSize > 0) {
      FIX::socket_setsockopt(socket.get(), SO_SNDBUF, options.sendBufferSize);
    }
    if (options.receiveBufferSize > 0) {
      FIX::socket_setsockopt(socket.get(), SO_RCVBUF, options.receiveBufferSize);
    }

    if (FIX::socket_connect(socket.get(), "127.0.0.1", port) == 0) {
      return socket;
    }

    socket.reset();
    FIX::process_sleep(0.05);
  }
  throw std::runtime_error("unable to connect to benchmark acceptor");
}

/**
 * @brief Measures repeated `Message::setString()` parsing without socket or Session work.
 *
 * One sample is reused to isolate parser cost. Message construction, dictionary loading, and warmup are outside the
 * measured interval.
 *
 * @param options Selects message kind, layout, validation, warmup count, and measured iterations.
 */
void runParseBenchmark(const Options &options) {
  std::unique_ptr<FIX::DataDictionary> dataDictionary;
  if (options.validate) {
    dataDictionary = std::make_unique<FIX::DataDictionary>(options.dataDictionaryPath);
    if (options.fixedLayout) {
      dataDictionary->checkUserDefinedFields(false);
    }
  }

  const FIX::DataDictionary *dictionary = dataDictionary.get();
  const std::string sample = makeApplicationMessage(2, options);
  FIX::Message message;
  std::uint64_t sink = 0;

  for (std::uint64_t i = 0; i < options.warmup; ++i) {
    message.setString(sample, options.validate, dictionary);
    sink += message.getHeader().isSetField(35) ? 1 : 0;
  }

  const auto start = std::chrono::steady_clock::now();
  for (std::uint64_t i = 0; i < options.messages; ++i) {
    message.setString(sample, options.validate, dictionary);
    sink += message.getHeader().isSetField(35) ? 1 : 0;
  }
  const auto end = std::chrono::steady_clock::now();

  printResult("parse", options, options.messages, sample.size() * options.messages, elapsedSeconds(start, end));
  std::cout << "sample_bytes=" << sample.size() << "\n"
            << "warmup_messages=" << options.warmup << "\n"
            << "sink=" << sink << "\n";
}

/**
 * @brief Measures the acceptor path with a raw socket client and prebuilt FIX wire bytes.
 *
 * Connection setup, Logon, and message construction occur before timing. The measured interval starts before
 * `sendAll()` and ends after the last server `fromApp()` callback.
 *
 * @param options Selects traffic, validation, socket settings, poll mode, and message count.
 */
void runRawServerBenchmark(const Options &options) {
  BenchmarkApplication application;
  FIX::MemoryStoreFactory storeFactory;
  const FIX::SessionID sessionID(BeginString, ServerCompID, ClientCompID);

  std::istringstream configStream(makeAcceptorConfig(options, options.port));
  FIX::SessionSettings settings(configStream);
  FIX::SocketAcceptor acceptor(application, storeFactory, settings);
  acceptor.start();

  const auto portEntry = acceptor.sessionToPort().find(sessionID);
  if (portEntry == acceptor.sessionToPort().end()) {
    acceptor.stop(true);
    throw std::runtime_error("benchmark session was not bound to a port");
  }

  SocketGuard socket;
  try {
    socket = connectWithRetry(portEntry->second, options);
    sendAll(socket.get(), makeLogonMessage());

    if (!waitForLogon(application, 5)) {
      throw std::runtime_error("acceptor did not log on within 5 seconds");
    }

    const std::string outbound = makeOutboundStream(options);
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
    startNetworkDiagnostics(acceptor, options);
#endif
    const auto start = std::chrono::steady_clock::now();
    sendAll(socket.get(), outbound);
    if (!waitForMessages(application, options.messages, options.serverWaitSeconds)) {
      std::ostringstream error;
      error << "server received " << application.received.load(std::memory_order_relaxed) << " of " << options.messages
            << " messages before timeout";
      throw std::runtime_error(error.str());
    }
    const auto end = std::chrono::steady_clock::now();
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
    const FIX::SocketAcceptorDiagnostics diagnostics = stopNetworkDiagnostics(acceptor, options);
#endif

    printResult("server", options, options.messages, outbound.size(), elapsedSeconds(start, end));
    std::cout << "port=" << portEntry->second << "\n"
              << "prepared_app_bytes=" << outbound.size() << "\n"
              << "received=" << application.received.load(std::memory_order_relaxed) << "\n";
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
    printNetworkDiagnostics(options, diagnostics);
#endif
  } catch (...) {
    acceptor.stop(true);
    throw;
  }

  acceptor.stop(true);
}

/**
 * @brief Measures a full QuickFIX SocketInitiator-to-SocketAcceptor path.
 *
 * The measured interval includes typed message construction, `Session::sendToTarget()` serialization and queuing,
 * socket transfer, server parsing, Session processing, and the final `fromApp()` callbacks.
 *
 * @param options Selects traffic, validation, socket settings, poll mode, and message count.
 */
void runQuickfixServerBenchmark(const Options &options) {
  BenchmarkApplication serverApplication;
  BenchmarkApplication clientApplication;
  FIX::MemoryStoreFactory serverStoreFactory;
  FIX::MemoryStoreFactory clientStoreFactory;
  const FIX::SessionID serverSessionID(BeginString, ServerCompID, ClientCompID);
  const FIX::SessionID clientSessionID(BeginString, ClientCompID, ServerCompID);

  std::istringstream acceptorConfigStream(makeAcceptorConfig(options, options.port));
  FIX::SessionSettings acceptorSettings(acceptorConfigStream);
  FIX::SocketAcceptor acceptor(serverApplication, serverStoreFactory, acceptorSettings);
  acceptor.start();

  const auto portEntry = acceptor.sessionToPort().find(serverSessionID);
  if (portEntry == acceptor.sessionToPort().end()) {
    acceptor.stop(true);
    throw std::runtime_error("benchmark session was not bound to a port");
  }

  std::unique_ptr<FIX::SocketInitiator> initiator;
  try {
    std::istringstream initiatorConfigStream(makeInitiatorConfig(options, portEntry->second));
    FIX::SessionSettings initiatorSettings(initiatorConfigStream);
    initiator = std::make_unique<FIX::SocketInitiator>(clientApplication, clientStoreFactory, initiatorSettings);
    initiator->start();

    if (!waitForLogon(serverApplication, 5) || !waitForLogon(clientApplication, 5)) {
      throw std::runtime_error("initiator and acceptor did not log on within 5 seconds");
    }

    const std::uint64_t outboundBytes = estimateOutboundBytes(options);
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
    startNetworkDiagnostics(acceptor, options);
#endif
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t i = 0; i < options.messages; ++i) {
      FIX::Message message = makeQuickfixApplicationMessage(i + 2, options);
      if (!FIX::Session::sendToTarget(message, clientSessionID)) {
        throw std::runtime_error("Session::sendToTarget returned false");
      }
    }
    if (!waitForMessages(serverApplication, options.messages, options.serverWaitSeconds)) {
      std::ostringstream error;
      error << "server received " << serverApplication.received.load(std::memory_order_relaxed) << " of "
            << options.messages << " messages before timeout";
      throw std::runtime_error(error.str());
    }
    const auto end = std::chrono::steady_clock::now();
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
    const FIX::SocketAcceptorDiagnostics diagnostics = stopNetworkDiagnostics(acceptor, options);
#endif

    printResult("server", options, options.messages, outboundBytes, elapsedSeconds(start, end));
    std::cout << "port=" << portEntry->second << "\n"
              << "estimated_app_bytes=" << outboundBytes << "\n"
              << "received=" << serverApplication.received.load(std::memory_order_relaxed) << "\n";
#if defined(QUICKFIX_NETWORK_DIAGNOSTICS) && !defined(_MSC_VER)
    printNetworkDiagnostics(options, diagnostics);
#endif
  } catch (...) {
    if (initiator) {
      initiator->stop();
    }
    acceptor.stop(true);
    throw;
  }

  initiator->stop();
  acceptor.stop(true);
}

/**
 * @brief Dispatches server mode to the raw or QuickFIX client implementation.
 *
 * @param options Uses `clientMode` to select the measured path.
 */
void runServerBenchmark(const Options &options) {
  if (options.clientMode == ClientMode::Raw) {
    runRawServerBenchmark(options);
  } else {
    runQuickfixServerBenchmark(options);
  }
}

/**
 * @brief Parses an unsigned command-line integer and rejects trailing characters.
 *
 * @param value Raw option value.
 * @param option Option name used in error messages.
 * @return Parsed unsigned integer.
 */
std::uint64_t parseUnsigned(const std::string &value, const std::string &option) {
  try {
    std::size_t parsed = 0;
    unsigned long long result = std::stoull(value, &parsed);
    if (parsed != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return static_cast<std::uint64_t>(result);
  } catch (const std::exception &) {
    throw std::runtime_error("invalid value for --" + option + ": " + value);
  }
}

/**
 * @brief Parses a signed command-line integer and rejects trailing characters.
 *
 * @param value Raw option value.
 * @param option Option name used in error messages.
 * @return Parsed integer.
 */
int parseInt(const std::string &value, const std::string &option) {
  try {
    std::size_t parsed = 0;
    int result = std::stoi(value, &parsed);
    if (parsed != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return result;
  } catch (const std::exception &) {
    throw std::runtime_error("invalid value for --" + option + ": " + value);
  }
}

/**
 * @brief Parses the accepted textual forms of a boolean command-line switch.
 *
 * @param value Inline value after `=`, or an empty string when the switch has no explicit value.
 * @param defaultValue Value returned for an empty string.
 * @return Parsed boolean value.
 */
bool parseBool(const std::string &value, bool defaultValue) {
  if (value.empty()) {
    return defaultValue;
  }
  if (value == "1" || value == "Y" || value == "y" || value == "yes" || value == "true") {
    return true;
  }
  if (value == "0" || value == "N" || value == "n" || value == "no" || value == "false") {
    return false;
  }
  throw std::runtime_error("invalid boolean value: " + value);
}

/**
 * @brief Resolves an option value from `--name=value` or the following argv element.
 *
 * @param argc Total command-line argument count.
 * @param argv Command-line argument vector.
 * @param index Current argument index, advanced when the following element is consumed.
 * @param name Option name used in error messages.
 * @param inlineValue Value found after `=`, if present.
 * @return Resolved option value.
 */
std::string requireValue(int argc, char **argv, int &index, const std::string &name, const std::string &inlineValue) {
  if (!inlineValue.empty()) {
    return inlineValue;
  }
  if (index + 1 >= argc) {
    throw std::runtime_error("missing value for --" + name);
  }
  return argv[++index];
}

/**
 * @brief Prints benchmark command-line help.
 *
 * @param program Executable name displayed in the usage line.
 */
void printUsage(const char *program) {
  std::cout << "Usage: " << program << " [options]\n"
            << "\n"
            << "Options:\n"
            << "  --mode parse|server|both        Benchmark layer (default: both)\n"
            << "  --client raw|quickfix           Server-mode client path (default: raw)\n"
            << "  --message new-order-single|order-cancel-request|market-data-snapshot|quote-request\n"
            << "  --messages N                    Application messages to parse/send (default: 100000)\n"
            << "  --warmup N                      Parse warmup messages (default: 10000)\n"
            << "  --validate[=yes|no]             Enable QuickFIX data dictionary validation\n"
            << "  --fixed-layout[=yes|no]         Use fixed-layout benchmark messages with tag 9001\n"
            << "  --busy-poll[=yes|no]            Enable experimental acceptor busy-poll mode\n"
            << "  --direct-read-poll[=yes|no]     Directly scan acceptor connections with recv\n"
            << "  --busy-poll-cpu N               Bind poll0/direct acceptor thread to CPU N\n"
            << "  --network-diagnostics[=yes|no]  Collect acceptor poll/recv diagnostics\n"
            << "  --data-dictionary PATH          FIX42 XML path when validation is enabled\n"
            << "  --quote-groups N                Repeating groups for quote-request (default: 10)\n"
            << "  --port N                        Server mode accept port, 0 means ephemeral (default: 0)\n"
            << "  --send-buffer-size N            Socket send buffer bytes\n"
            << "  --receive-buffer-size N         Socket receive buffer bytes\n"
            << "  --server-wait-seconds N         Server drain timeout (default: 30)\n"
            << "  --self-test-fast-scan           Verify scalar/SIMD fast-scan helper results\n"
            << "  --self-test-parser              Verify Parser message-boundary behavior\n"
            << "  --self-test-correctness         Verify Parser + Message parsing against expected fields\n"
            << "  --help                          Show this help\n";
}

/**
 * @brief Parses all command-line options and rejects unsupported or contradictory combinations.
 *
 * Validation happens before socket setup so every printed result describes the path that actually ran.
 *
 * @param argc Total command-line argument count.
 * @param argv Command-line argument vector.
 * @return Fully validated benchmark options.
 */
Options parseOptions(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      printUsage(argv[0]);
      std::exit(0);
    }
    if (arg.rfind("--", 0) != 0) {
      throw std::runtime_error("unexpected argument: " + arg);
    }

    arg = arg.substr(2);
    const std::size_t equals = arg.find('=');
    const std::string name = equals == std::string::npos ? arg : arg.substr(0, equals);
    const std::string inlineValue = equals == std::string::npos ? std::string() : arg.substr(equals + 1);

    if (name == "mode") {
      const std::string value = requireValue(argc, argv, i, name, inlineValue);
      if (value == "parse") {
        options.mode = Mode::Parse;
      } else if (value == "server") {
        options.mode = Mode::Server;
      } else if (value == "both") {
        options.mode = Mode::Both;
      } else {
        throw std::runtime_error("invalid mode: " + value);
      }
    } else if (name == "client") {
      const std::string value = requireValue(argc, argv, i, name, inlineValue);
      if (value == "raw") {
        options.clientMode = ClientMode::Raw;
      } else if (value == "quickfix") {
        options.clientMode = ClientMode::Quickfix;
      } else {
        throw std::runtime_error("invalid client: " + value);
      }
    } else if (name == "message") {
      const std::string value = requireValue(argc, argv, i, name, inlineValue);
      if (value == "new-order-single") {
        options.messageKind = MessageKind::NewOrderSingle;
      } else if (value == "order-cancel-request") {
        options.messageKind = MessageKind::OrderCancelRequest;
      } else if (value == "market-data-snapshot") {
        options.messageKind = MessageKind::MarketDataSnapshot;
      } else if (value == "quote-request") {
        options.messageKind = MessageKind::QuoteRequest;
      } else {
        throw std::runtime_error("invalid message: " + value);
      }
    } else if (name == "messages") {
      options.messages = parseUnsigned(requireValue(argc, argv, i, name, inlineValue), name);
    } else if (name == "warmup") {
      options.warmup = parseUnsigned(requireValue(argc, argv, i, name, inlineValue), name);
    } else if (name == "validate") {
      options.validate = parseBool(inlineValue, true);
    } else if (name == "no-validate") {
      options.validate = false;
    } else if (name == "fixed-layout") {
      options.fixedLayout = parseBool(inlineValue, true);
    } else if (name == "no-fixed-layout") {
      options.fixedLayout = false;
    } else if (name == "busy-poll") {
      options.busyPoll = parseBool(inlineValue, true);
    } else if (name == "no-busy-poll") {
      options.busyPoll = false;
    } else if (name == "direct-read-poll") {
      options.directReadPoll = parseBool(inlineValue, true);
    } else if (name == "no-direct-read-poll") {
      options.directReadPoll = false;
    } else if (name == "busy-poll-cpu") {
      options.busyPollCpu = parseInt(requireValue(argc, argv, i, name, inlineValue), name);
      if (options.busyPollCpu < 0) {
        throw std::runtime_error("--busy-poll-cpu must be greater than or equal to zero");
      }
    } else if (name == "network-diagnostics") {
      options.networkDiagnostics = parseBool(inlineValue, true);
    } else if (name == "no-network-diagnostics") {
      options.networkDiagnostics = false;
    } else if (name == "data-dictionary") {
      options.dataDictionaryPath = requireValue(argc, argv, i, name, inlineValue);
    } else if (name == "quote-groups") {
      options.quoteGroups = parseInt(requireValue(argc, argv, i, name, inlineValue), name);
    } else if (name == "port") {
      options.port = parseInt(requireValue(argc, argv, i, name, inlineValue), name);
    } else if (name == "send-buffer-size") {
      options.sendBufferSize = parseInt(requireValue(argc, argv, i, name, inlineValue), name);
    } else if (name == "receive-buffer-size") {
      options.receiveBufferSize = parseInt(requireValue(argc, argv, i, name, inlineValue), name);
    } else if (name == "server-wait-seconds") {
      options.serverWaitSeconds = parseInt(requireValue(argc, argv, i, name, inlineValue), name);
    } else if (name == "self-test-fast-scan") {
      options.selfTestFastScan = true;
    } else if (name == "self-test-parser") {
      options.selfTestParser = true;
    } else if (name == "self-test-correctness") {
      options.selfTestCorrectness = true;
    } else {
      throw std::runtime_error("unknown option: --" + name);
    }
  }

  if (options.messages == 0) {
    throw std::runtime_error("--messages must be greater than zero");
  }
  if (options.quoteGroups <= 0) {
    throw std::runtime_error("--quote-groups must be greater than zero");
  }
  if (options.port < 0 || options.port > 65535) {
    throw std::runtime_error("--port must be between 0 and 65535");
  }
  if (options.serverWaitSeconds <= 0) {
    throw std::runtime_error("--server-wait-seconds must be greater than zero");
  }
  if (options.busyPoll && options.directReadPoll) {
    throw std::runtime_error("--busy-poll and --direct-read-poll are mutually exclusive");
  }
  if (options.busyPollCpu >= 0 && !options.directReadPoll) {
    options.busyPoll = true;
  }
  if (options.directReadPoll && options.mode == Mode::Parse) {
    throw std::runtime_error("--direct-read-poll requires --mode=server or --mode=both");
  }
#if !defined(QUICKFIX_DIRECT_READ_POLL) || defined(_MSC_VER)
  if (options.directReadPoll) {
    throw std::runtime_error("--direct-read-poll requires a UNIX build with QUICKFIX_DIRECT_READ_POLL=ON");
  }
#endif
  if (options.fixedLayout && options.messageKind == MessageKind::QuoteRequest) {
    throw std::runtime_error("--fixed-layout is not supported for quote-request");
  }
  if (options.fixedLayout && options.clientMode == ClientMode::Quickfix
      && (options.mode == Mode::Server || options.mode == Mode::Both)) {
    throw std::runtime_error("--fixed-layout is only supported with --client=raw in server mode");
  }
  if (options.networkDiagnostics && options.mode == Mode::Parse) {
    throw std::runtime_error("--network-diagnostics requires --mode=server or --mode=both");
  }
#if !defined(QUICKFIX_NETWORK_DIAGNOSTICS) || defined(_MSC_VER)
  if (options.networkDiagnostics) {
    throw std::runtime_error("--network-diagnostics requires a UNIX build with QUICKFIX_NETWORK_DIAGNOSTICS=ON");
  }
#endif
  return options;
}

} // namespace

int main(int argc, char **argv) {
#ifndef _MSC_VER
  std::signal(SIGPIPE, SIG_IGN);
#endif

  try {
    const Options options = parseOptions(argc, argv);
    // Self-tests are correctness gates and intentionally do not continue into timed benchmark modes.
    if (options.selfTestFastScan) {
      runFastScanSelfTest();
      return 0;
    }
    if (options.selfTestParser) {
      runParserSelfTest();
      return 0;
    }
    if (options.selfTestCorrectness) {
      runCorrectnessSelfTest(options);
      return 0;
    }

    FIX::socket_init();

    if (options.mode == Mode::Parse || options.mode == Mode::Both) {
      runParseBenchmark(options);
    }
    if (options.mode == Mode::Both) {
      std::cout << "\n";
    }
    if (options.mode == Mode::Server || options.mode == Mode::Both) {
      runServerBenchmark(options);
    }

    FIX::socket_term();
    return 0;
  } catch (const std::exception &exception) {
    FIX::socket_term();
    std::cerr << "fix_parse_benchmark: " << exception.what() << std::endl;
    return 1;
  }
}
