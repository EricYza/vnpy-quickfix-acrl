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

#include "Message.h"
#include "Utility.h"
#include "Values.h"
#include "detail/FastScan.h"
#include <iomanip>

namespace FIX {

int const headerOrder[] = {FIELD::BeginString, FIELD::BodyLength, FIELD::MsgType};

std::unique_ptr<DataDictionary> Message::s_dataDictionary;

namespace {

std::string::const_iterator findSoh(
    const std::string &string,
    std::string::const_iterator valueStart,
    std::string::const_iterator strEnd) {
#if defined(QUICKFIX_SIMD_FIELD_SCAN)
  (void)strEnd;
  const char *begin = string.data() + (valueStart - string.begin());
  const char *end = string.data() + string.size();
  const char *result = detail::findCharFast(begin, end, '\001');
  return string.begin() + (result - string.data());
#else
  return std::find(valueStart, strEnd, '\001');
#endif
}

} // namespace

#if defined(QUICKFIX_FIXED_LAYOUT_PARSER)
namespace {

/**
 * @brief Selects the FieldMap that receives a field extracted from a fixed-layout template.
 */
enum class FixedFieldTarget { Header, Body, Trailer };

/**
 * @brief Describes one top-level FIX field at compile-time-known wire offsets.
 *
 * A template entry supplies all information needed to construct a `FieldBase` without scanning for `=` or SOH.
 */
struct FixedFieldSpec {
  int tag;                             ///< Numeric FIX tag represented by this entry.
  std::string::size_type fieldOffset;  ///< Offset of the tag's first character.
  std::string::size_type valueOffset;  ///< Offset of the value's first character.
  std::string::size_type valueLength;  ///< Fixed number of value bytes.
  FixedFieldTarget target;             ///< Header, body, or trailer destination.
};

/**
 * @brief Describes one field inside a fixed-layout repeating group.
 */
struct FixedGroupSpec {
  int tag;                             ///< Numeric FIX tag represented by this entry.
  std::string::size_type fieldOffset;  ///< Offset of the tag's first character.
  std::string::size_type valueOffset;  ///< Offset of the value's first character.
  std::string::size_type valueLength;  ///< Fixed number of value bytes.
};

/**
 * @brief Offset template for a 187-byte NOS1 NewOrderSingle message.
 *
 * The template covers all header, body, and trailer fields, including the custom `9001=NOS1` marker.
 */
const FixedFieldSpec FIXED_NOS1_FIELDS[] = {
    {FIELD::BeginString, 0, 2, 7, FixedFieldTarget::Header},
    {FIELD::BodyLength, 10, 12, 3, FixedFieldTarget::Header},
    {FIELD::MsgType, 16, 19, 1, FixedFieldTarget::Header},
    {FIELD::SenderCompID, 21, 24, 6, FixedFieldTarget::Header},
    {FIELD::TargetCompID, 31, 34, 6, FixedFieldTarget::Header},
    {FIELD::MsgSeqNum, 41, 44, 12, FixedFieldTarget::Header},
    {FIELD::SendingTime, 57, 60, 17, FixedFieldTarget::Header},
    {9001, 78, 83, 4, FixedFieldTarget::Body},
    {FIELD::ClOrdID, 88, 91, 18, FixedFieldTarget::Body},
    {FIELD::HandlInst, 110, 113, 1, FixedFieldTarget::Body},
    {FIELD::Symbol, 115, 118, 4, FixedFieldTarget::Body},
    {FIELD::Side, 123, 126, 1, FixedFieldTarget::Body},
    {FIELD::TransactTime, 128, 131, 17, FixedFieldTarget::Body},
    {FIELD::OrderQty, 149, 152, 10, FixedFieldTarget::Body},
    {FIELD::OrdType, 163, 166, 1, FixedFieldTarget::Body},
    {FIELD::TimeInForce, 168, 171, 1, FixedFieldTarget::Body},
    {FIELD::Currency, 173, 176, 3, FixedFieldTarget::Body},
    {FIELD::CheckSum, 180, 183, 3, FixedFieldTarget::Trailer},
};

/**
 * @brief Offset template for a 185-byte CXL1 OrderCancelRequest message.
 *
 * The template covers all header, body, and trailer fields, including the custom `9001=CXL1` marker.
 */
const FixedFieldSpec FIXED_CXL1_FIELDS[] = {
    {FIELD::BeginString, 0, 2, 7, FixedFieldTarget::Header},
    {FIELD::BodyLength, 10, 12, 3, FixedFieldTarget::Header},
    {FIELD::MsgType, 16, 19, 1, FixedFieldTarget::Header},
    {FIELD::SenderCompID, 21, 24, 6, FixedFieldTarget::Header},
    {FIELD::TargetCompID, 31, 34, 6, FixedFieldTarget::Header},
    {FIELD::MsgSeqNum, 41, 44, 12, FixedFieldTarget::Header},
    {FIELD::SendingTime, 57, 60, 17, FixedFieldTarget::Header},
    {9001, 78, 83, 4, FixedFieldTarget::Body},
    {FIELD::OrigClOrdID, 88, 91, 17, FixedFieldTarget::Body},
    {FIELD::ClOrdID, 109, 112, 17, FixedFieldTarget::Body},
    {FIELD::Symbol, 130, 133, 4, FixedFieldTarget::Body},
    {FIELD::Side, 138, 141, 1, FixedFieldTarget::Body},
    {FIELD::TransactTime, 143, 146, 17, FixedFieldTarget::Body},
    {FIELD::OrderQty, 164, 167, 10, FixedFieldTarget::Body},
    {FIELD::CheckSum, 178, 181, 3, FixedFieldTarget::Trailer},
};

/**
 * @brief Top-level offset template for a 288-byte MDW1 MarketDataSnapshotFullRefresh.
 *
 * Repeating-group fields are described separately by `FIXED_MDW1_GROUPS`. Adding those groups also maintains the
 * `NoMDEntries` group-count field.
 */
const FixedFieldSpec FIXED_MDW1_FIELDS[] = {
    {FIELD::BeginString, 0, 2, 7, FixedFieldTarget::Header},
    {FIELD::BodyLength, 10, 12, 3, FixedFieldTarget::Header},
    {FIELD::MsgType, 16, 19, 1, FixedFieldTarget::Header},
    {FIELD::SenderCompID, 21, 24, 6, FixedFieldTarget::Header},
    {FIELD::TargetCompID, 31, 34, 6, FixedFieldTarget::Header},
    {FIELD::MsgSeqNum, 41, 44, 12, FixedFieldTarget::Header},
    {FIELD::SendingTime, 57, 60, 17, FixedFieldTarget::Header},
    {9001, 78, 83, 4, FixedFieldTarget::Body},
    {FIELD::MDReqID, 88, 92, 18, FixedFieldTarget::Body},
    {FIELD::Symbol, 111, 114, 4, FixedFieldTarget::Body},
    {FIELD::CheckSum, 281, 284, 3, FixedFieldTarget::Trailer},
};

/**
 * @brief Three fixed MDEntry groups, each containing type, price, size, and time fields.
 */
const FixedGroupSpec FIXED_MDW1_GROUPS[][4] = {
    {
        {FIELD::MDEntryType, 125, 129, 1},
        {FIELD::MDEntryPx, 131, 135, 13},
        {FIELD::MDEntrySize, 149, 153, 10},
        {FIELD::MDEntryTime, 164, 168, 8},
    },
    {
        {FIELD::MDEntryType, 177, 181, 1},
        {FIELD::MDEntryPx, 183, 187, 13},
        {FIELD::MDEntrySize, 201, 205, 10},
        {FIELD::MDEntryTime, 216, 220, 8},
    },
    {
        {FIELD::MDEntryType, 229, 233, 1},
        {FIELD::MDEntryPx, 235, 239, 13},
        {FIELD::MDEntrySize, 253, 257, 10},
        {FIELD::MDEntryTime, 268, 272, 8},
    },
};

/// Byte offset at which every supported fixed-layout marker field begins.
constexpr std::string::size_type FIXED_LAYOUT_MARKER_FIELD_OFFSET = 78;
/// Marker field size including tag, value, and the terminating SOH.
constexpr std::string::size_type FIXED_LAYOUT_MARKER_FIELD_SIZE = 10;

/// Marker selecting the fixed NewOrderSingle template.
const char FIXED_NOS1_MARKER[] = "9001=NOS1\001";
/// Marker selecting the fixed OrderCancelRequest template.
const char FIXED_CXL1_MARKER[] = "9001=CXL1\001";
/// Marker selecting the fixed MarketDataSnapshotFullRefresh template.
const char FIXED_MDW1_MARKER[] = "9001=MDW1\001";

/**
 * @brief Checks whether the expected marker occupies the fixed marker offset.
 *
 * @param string Complete FIX wire message being considered for a fixed template.
 * @param marker Ten-byte marker expected at `FIXED_LAYOUT_MARKER_FIELD_OFFSET`.
 * @return `true` when all marker bytes match at the fixed offset; otherwise `false`.
 */
bool hasFixedLayoutMarker(const std::string &string, const char *marker) {
  return string.compare(
             FIXED_LAYOUT_MARKER_FIELD_OFFSET, FIXED_LAYOUT_MARKER_FIELD_SIZE, marker, FIXED_LAYOUT_MARKER_FIELD_SIZE)
      == 0;
}

} // namespace
#endif

Message::Message()
    : m_validStructure(true),
      m_tag(0) {}

Message::Message(const message_order &headerOrder, const message_order &trailerOrder, const message_order &order)
    : FieldMap(order),
      m_header(headerOrder),
      m_trailer(trailerOrder),
      m_validStructure(true) {}

Message::Message(const std::string &string, bool validate) EXCEPT(InvalidMessage)
    : m_validStructure(true),
      m_tag(0) {
  setString(string, validate);
}

Message::Message(const std::string &string, const DataDictionary &dataDictionary, bool validate) EXCEPT(InvalidMessage)
    : m_validStructure(true),
      m_tag(0) {
  setString(string, validate, &dataDictionary, &dataDictionary);
}

Message::Message(
    const std::string &string,
    const DataDictionary &sessionDataDictionary,
    const DataDictionary &applicationDataDictionary,
    bool validate) EXCEPT(InvalidMessage)
    : m_validStructure(true),
      m_tag(0) {
  setString(string, validate, &sessionDataDictionary, &applicationDataDictionary);
}

Message::Message(
    const message_order &headerOrder,
    const message_order &trailerOrder,
    const message_order &order,
    const std::string &string,
    const DataDictionary &dataDictionary,
    bool validate) EXCEPT(InvalidMessage)
    : FieldMap(order),
      m_header(headerOrder),
      m_trailer(trailerOrder),
      m_validStructure(true) {
  setString(string, validate, &dataDictionary, &dataDictionary);
}

Message::Message(
    const message_order &headerOrder,
    const message_order &trailerOrder,
    const message_order &order,
    const std::string &string,
    const DataDictionary &sessionDataDictionary,
    const DataDictionary &applicationDataDictionary,
    bool validate) EXCEPT(InvalidMessage)
    : FieldMap(order),
      m_header(headerOrder),
      m_trailer(trailerOrder),
      m_validStructure(true) {
  setStringHeader(string);
  if (isAdmin()) {
    setString(string, validate, &sessionDataDictionary, &sessionDataDictionary);
  } else {
    setString(string, validate, &sessionDataDictionary, &applicationDataDictionary);
  }
}

Message::Message(const BeginString &beginString, const MsgType &msgType)
    : m_validStructure(true),
      m_tag(0) {
  m_header.setField(beginString);
  m_header.setField(msgType);
}

Message::~Message() {}

bool Message::InitializeXML(const std::string &url) {
  try {
    s_dataDictionary.reset(new DataDictionary(url));
    return true;
  } catch (ConfigError &) {
    return false;
  }
}

void Message::reverseRoute(const Header &header) {
  // required routing tags
  BeginString beginString;
  SenderCompID senderCompID;
  TargetCompID targetCompID;

  m_header.removeField(beginString.getTag());
  m_header.removeField(senderCompID.getTag());
  m_header.removeField(targetCompID.getTag());

  if (header.getFieldIfSet(beginString)) {
    if (beginString.getValue().size()) {
      m_header.setField(beginString);
    }

    OnBehalfOfLocationID onBehalfOfLocationID;
    DeliverToLocationID deliverToLocationID;

    m_header.removeField(onBehalfOfLocationID.getTag());
    m_header.removeField(deliverToLocationID.getTag());

    if (beginString >= BeginString_FIX41) {
      if (header.getFieldIfSet(onBehalfOfLocationID)) {
        if (onBehalfOfLocationID.getValue().size()) {
          m_header.setField(DeliverToLocationID(onBehalfOfLocationID));
        }
      }

      if (header.getFieldIfSet(deliverToLocationID)) {
        if (deliverToLocationID.getValue().size()) {
          m_header.setField(OnBehalfOfLocationID(deliverToLocationID));
        }
      }
    }
  }

  if (header.getFieldIfSet(senderCompID)) {
    if (senderCompID.getValue().size()) {
      m_header.setField(TargetCompID(senderCompID));
    }
  }

  if (header.getFieldIfSet(targetCompID)) {
    if (targetCompID.getValue().size()) {
      m_header.setField(SenderCompID(targetCompID));
    }
  }

  // optional routing tags
  OnBehalfOfCompID onBehalfOfCompID;
  OnBehalfOfSubID onBehalfOfSubID;
  DeliverToCompID deliverToCompID;
  DeliverToSubID deliverToSubID;

  m_header.removeField(onBehalfOfCompID.getTag());
  m_header.removeField(onBehalfOfSubID.getTag());
  m_header.removeField(deliverToCompID.getTag());
  m_header.removeField(deliverToSubID.getTag());

  if (header.getFieldIfSet(onBehalfOfCompID)) {
    if (onBehalfOfCompID.getValue().size()) {
      m_header.setField(DeliverToCompID(onBehalfOfCompID));
    }
  }

  if (header.getFieldIfSet(onBehalfOfSubID)) {
    if (onBehalfOfSubID.getValue().size()) {
      m_header.setField(DeliverToSubID(onBehalfOfSubID));
    }
  }

  if (header.getFieldIfSet(deliverToCompID)) {
    if (deliverToCompID.getValue().size()) {
      m_header.setField(OnBehalfOfCompID(deliverToCompID));
    }
  }

  if (header.getFieldIfSet(deliverToSubID)) {
    if (deliverToSubID.getValue().size()) {
      m_header.setField(OnBehalfOfSubID(deliverToSubID));
    }
  }
}

std::string Message::toString(int beginStringField, int bodyLengthField, int checkSumField) const {
  std::string str;
  toString(str, beginStringField, bodyLengthField, checkSumField);
  return str;
}

std::string &Message::toString(std::string &str, int beginStringField, int bodyLengthField, int checkSumField) const {
  // Combined traversal: compute bodyLength and partial checksum in a single pass each (3 traversals instead of 6)
  auto headerLengthAndTotal = m_header.calculateLengthAndTotal(beginStringField, bodyLengthField, checkSumField);
  auto bodyLengthAndTotal = FieldMap::calculateLengthAndTotal(beginStringField, bodyLengthField, checkSumField);
  auto trailerLengthAndTotal = m_trailer.calculateLengthAndTotal(beginStringField, bodyLengthField, checkSumField);

  int bodyLength = headerLengthAndTotal.length + bodyLengthAndTotal.length + trailerLengthAndTotal.length;

  // Add BodyLength field's own checksum contribution without allocating a string.
  // The field wire format is "tag=value\001"; sum the ASCII values of each byte.
  auto sumAsciiDigits = [](int number) {
    int sum = 0;
    do {
      sum += '0' + (number % 10);
      number /= 10;
    } while (number > 0);
    return sum;
  };
  int bodyLengthFieldContrib = sumAsciiDigits(bodyLengthField) + '=' + sumAsciiDigits(bodyLength) + '\001';
  int totalChecksum = (headerLengthAndTotal.total + bodyLengthAndTotal.total + trailerLengthAndTotal.total + bodyLengthFieldContrib) % 256;

  m_header.setField(IntField(bodyLengthField, bodyLength));
  m_trailer.setField(CheckSumField(checkSumField, totalChecksum));

  str.clear();

  str.reserve(bodyLength + 64);

  m_header.calculateString(str);
  FieldMap::calculateString(str);
  m_trailer.calculateString(str);

  return str;
}

std::string Message::toXML() const {
  std::string str;
  toXML(str);
  return str;
}

std::string &Message::toXML(std::string &str) const {
  std::stringstream stream;
  stream << "<message>" << std::endl
         << std::setw(2) << " " << "<header>" << std::endl
         << toXMLFields(getHeader(), 4) << std::setw(2) << " " << "</header>" << std::endl
         << std::setw(2) << " " << "<body>" << std::endl
         << toXMLFields(*this, 4) << std::setw(2) << " " << "</body>" << std::endl
         << std::setw(2) << " " << "<trailer>" << std::endl
         << toXMLFields(getTrailer(), 4) << std::setw(2) << " " << "</trailer>" << std::endl
         << "</message>";

  return str = stream.str();
}

std::string Message::toXMLFields(const FieldMap &fields, int space) const {
  std::stringstream stream;
  std::string name;
  for (const FieldMap::value_type &field : fields) {
    int tag = field.getTag();
    std::string value = field.getString();

    stream << std::setw(space) << " " << "<field ";
    if (s_dataDictionary.get() && s_dataDictionary->getFieldName(tag, name)) {
      stream << "name=\"" << name << "\" ";
    }
    stream << "number=\"" << tag << "\"";
    if (s_dataDictionary.get() && s_dataDictionary->getValueName(tag, value, name)) {
      stream << " enum=\"" << name << "\"";
    }
    stream << ">";
    stream << "<![CDATA[" << value << "]]>";
    stream << "</field>" << std::endl;
  }

  for (const FieldMap::g_value_type &group : fields.groups()) {
    for (const FieldMap *groupFields : group.second) {
      stream << std::setw(space) << " " << "<group>" << std::endl
             << toXMLFields(*groupFields, space + 2) << std::setw(space) << " " << "</group>" << std::endl;
    }
  }

  return stream.str();
}

#if defined(QUICKFIX_FIXED_LAYOUT_PARSER)
/**
 * @brief Attempts to populate this Message from one of the supported fixed-offset templates.
 *
 * Template eligibility intentionally checks only the complete message length and the ten-byte tag 9001 marker at its
 * fixed offset. It does not rescan other tags or delimiters and does not perform generic type, BodyLength, or CheckSum
 * validation. Once a template matches, `FieldBase` objects are constructed directly from the template offsets.
 *
 * A length or marker mismatch returns `false` before adding any fields, allowing `Message::setString()` to continue
 * with the original generic parser.
 *
 * @param string Complete serialized FIX message to match and decode.
 * @return `true` when NOS1, CXL1, or MDW1 was decoded by offset; otherwise `false`.
 */
bool Message::setFixedLayoutString(const std::string &string) {
  const FixedFieldSpec *fields = nullptr;
  std::size_t fieldCount = 0;
  bool hasMarketDataGroups = false;

  if (string.size() == 187 && hasFixedLayoutMarker(string, FIXED_NOS1_MARKER)) {
    fields = FIXED_NOS1_FIELDS;
    fieldCount = sizeof(FIXED_NOS1_FIELDS) / sizeof(FIXED_NOS1_FIELDS[0]);
  } else if (string.size() == 185 && hasFixedLayoutMarker(string, FIXED_CXL1_MARKER)) {
    fields = FIXED_CXL1_FIELDS;
    fieldCount = sizeof(FIXED_CXL1_FIELDS) / sizeof(FIXED_CXL1_FIELDS[0]);
  } else if (string.size() == 288 && hasFixedLayoutMarker(string, FIXED_MDW1_MARKER)) {
    fields = FIXED_MDW1_FIELDS;
    fieldCount = sizeof(FIXED_MDW1_FIELDS) / sizeof(FIXED_MDW1_FIELDS[0]);
    hasMarketDataGroups = true;
  } else {
    return false;
  }

  /**
   * @brief Constructs one FieldBase from precomputed tag and value offsets.
   *
   * @param tag Numeric FIX tag supplied by the selected template.
   * @param fieldOffset Offset of the tag's first byte.
   * @param valueOffset Offset of the value's first byte.
   * @param valueLength Fixed number of value bytes.
   * @return FieldBase covering the tag, value, and terminating SOH.
   */
  auto makeField = [&string](int tag, std::string::size_type fieldOffset, std::string::size_type valueOffset,
                             std::string::size_type valueLength) {
    const std::string::const_iterator tagStart = string.begin() + fieldOffset;
    const std::string::const_iterator valueStart = string.begin() + valueOffset;
    const std::string::const_iterator valueEnd = valueStart + valueLength;
    return FieldBase(tag, valueStart, valueEnd, tagStart, valueEnd + 1);
  };

  for (std::size_t i = 0; i < fieldCount; ++i) {
    const FixedFieldSpec &field = fields[i];
    FieldBase fieldBase = makeField(field.tag, field.fieldOffset, field.valueOffset, field.valueLength);
    if (field.target == FixedFieldTarget::Header) {
      m_header.appendField(fieldBase);
    } else if (field.target == FixedFieldTarget::Trailer) {
      m_trailer.appendField(fieldBase);
    } else {
      appendField(fieldBase);
    }
  }

  if (hasMarketDataGroups) {
    for (const auto &groupFields : FIXED_MDW1_GROUPS) {
      Group group(FIELD::NoMDEntries, FIELD::MDEntryType, message_order(
                                                          FIELD::MDEntryType,
                                                          FIELD::MDEntryPx,
                                                          FIELD::MDEntrySize,
                                                          FIELD::MDEntryTime,
                                                          0));
      for (const FixedGroupSpec &field : groupFields) {
        group.appendField(makeField(field.tag, field.fieldOffset, field.valueOffset, field.valueLength));
      }
      addGroup(group);
    }
  }

  m_header.sortFields();
  sortFields();
  m_trailer.sortFields();
  return true;
}
#endif

/**
 * @brief Parses a complete serialized FIX message into header, body, trailer, and repeating groups.
 *
 * When fixed-layout support is compiled in, this function first attempts `setFixedLayoutString()`. A successful
 * template match returns immediately and therefore bypasses generic field scanning and validation. A template miss
 * continues through the original `extractField()` and `setGroup()` path without requiring a different caller.
 *
 * @param string Complete serialized FIX message to parse.
 * @param doValidation Enables the generic parser's structural, BodyLength, and CheckSum validation.
 * @param pSessionDataDictionary Optional dictionary for session fields and groups.
 * @param pApplicationDataDictionary Optional dictionary for application fields and groups.
 */
void Message::setString(
    const std::string &string,
    bool doValidation,
    const DataDictionary *pSessionDataDictionary,
    const DataDictionary *pApplicationDataDictionary) EXCEPT(InvalidMessage) {
  clear();

#if defined(QUICKFIX_FIXED_LAYOUT_PARSER)
  if (setFixedLayoutString(string)) {
    return;
  }
#endif

  std::string::size_type pos = 0;
  int count = 0;

  FIX::MsgType msg;

  field_type type = header;

  while (pos < string.size()) {
    FieldBase field = extractField(string, pos, pSessionDataDictionary, pApplicationDataDictionary);
    if (count < 3 && headerOrder[count++] != field.getTag()) {
      if (doValidation) {
        throw InvalidMessage("Header fields out of order");
      }
    }

    if (isHeaderField(field, pSessionDataDictionary)) {
      if (type != header) {
        if (m_tag == 0) {
          m_tag = field.getTag();
        }
        m_validStructure = false;
      }

      if (field.getTag() == FIELD::MsgType) {
        msg.setString(field.getString());
        if (isAdminMsgType(msg)) {
          pApplicationDataDictionary = pSessionDataDictionary;
        }
      }

      m_header.appendField(field);

      if (pSessionDataDictionary) {
        setGroup("_header_", field, string, pos, getHeader(), *pSessionDataDictionary);
      }
    } else if (isTrailerField(field, pSessionDataDictionary)) {
      type = trailer;
      m_trailer.appendField(field);

      if (pSessionDataDictionary) {
        setGroup("_trailer_", field, string, pos, getTrailer(), *pSessionDataDictionary);
      }
    } else {
      if (type == trailer) {
        if (m_tag == 0) {
          m_tag = field.getTag();
        }
        m_validStructure = false;
      }

      type = body;
      appendField(field);

      if (pApplicationDataDictionary) {
        setGroup(msg, field, string, pos, *this, *pApplicationDataDictionary);
      }
    }
  }

  // sort fields
  m_header.sortFields();
  sortFields();
  m_trailer.sortFields();

  if (doValidation) {
    validate();
  }
}

void Message::setGroup(
    const std::string &msg,
    const FieldBase &field,
    const std::string &string,
    std::string::size_type &pos,
    FieldMap &map,
    const DataDictionary &dataDictionary) {
  int group = field.getTag();
  int delim;
  const DataDictionary *pDD = 0;
  if (!dataDictionary.getGroup(msg, group, delim, pDD)) {
    return;
  }
  std::unique_ptr<Group> pGroup;

  while (pos < string.size()) {
    std::string::size_type oldPos = pos;
    FieldBase field = extractField(string, pos, &dataDictionary, &dataDictionary, pGroup.get());

    // Start a new group because...
    if ( // found delimiter
        (field.getTag() == delim) ||
        // no delimiter, but field belongs to group OR field already processed
        (pDD->isField(field.getTag()) && (pGroup.get() == 0 || pGroup->isSetField(field.getTag())))) {
      if (pGroup.get()) {
        map.addGroupPtr(group, pGroup.release(), false);
      }
      pGroup.reset(new Group(field.getTag(), delim, pDD->getOrderedFields()));
    } else if (!pDD->isField(field.getTag())) {
      if (pGroup.get()) {
        map.addGroupPtr(group, pGroup.release(), false);
      }
      pos = oldPos;
      return;
    }

    if (!pGroup.get()) {
      return;
    }
    pGroup->addField(field);
    setGroup(msg, field, string, pos, *pGroup, *pDD);
  }
}

bool Message::setStringHeader(const std::string &string) {
  clear();

  std::string::size_type pos = 0;
  int count = 0;

  while (pos < string.size()) {
    FieldBase field = extractField(string, pos);
    if (count < 3 && headerOrder[count++] != field.getTag()) {
      return false;
    }

    if (isHeaderField(field)) {
      m_header.appendField(field);
    } else {
      break;
    }
  }

  m_header.sortFields();
  return true;
}

bool Message::isHeaderField(int field) {
  switch (field) {
  case FIELD::BeginString:
  case FIELD::BodyLength:
  case FIELD::MsgType:
  case FIELD::SenderCompID:
  case FIELD::TargetCompID:
  case FIELD::OnBehalfOfCompID:
  case FIELD::DeliverToCompID:
  case FIELD::SecureDataLen:
  case FIELD::MsgSeqNum:
  case FIELD::SenderSubID:
  case FIELD::SenderLocationID:
  case FIELD::TargetSubID:
  case FIELD::TargetLocationID:
  case FIELD::OnBehalfOfSubID:
  case FIELD::OnBehalfOfLocationID:
  case FIELD::DeliverToSubID:
  case FIELD::DeliverToLocationID:
  case FIELD::PossDupFlag:
  case FIELD::PossResend:
  case FIELD::SendingTime:
  case FIELD::OrigSendingTime:
  case FIELD::XmlDataLen:
  case FIELD::XmlData:
  case FIELD::MessageEncoding:
  case FIELD::LastMsgSeqNumProcessed:
  case FIELD::OnBehalfOfSendingTime:
  case FIELD::ApplVerID:
  case FIELD::CstmApplVerID:
  case FIELD::NoHops:
    return true;
  default:
    return false;
  };
}

bool Message::isHeaderField(const FieldBase &field, const DataDictionary *pD) {
  return isHeaderField(field.getTag(), pD);
}

bool Message::isHeaderField(int field, const DataDictionary *pD) {
  if (isHeaderField(field)) {
    return true;
  }
  if (pD) {
    return pD->isHeaderField(field);
  }
  return false;
}

bool Message::isTrailerField(int field) {
  switch (field) {
  case FIELD::SignatureLength:
  case FIELD::Signature:
  case FIELD::CheckSum:
    return true;
  default:
    return false;
  };
}

bool Message::isTrailerField(const FieldBase &field, const DataDictionary *pD) {
  return isTrailerField(field.getTag(), pD);
}

bool Message::isTrailerField(int field, const DataDictionary *pD) {
  if (isTrailerField(field)) {
    return true;
  }
  if (pD) {
    return pD->isTrailerField(field);
  }
  return false;
}

SessionID Message::getSessionID(const std::string &qualifier) const EXCEPT(FieldNotFound) {
  return SessionID(
      getHeader().getField<BeginString>(),
      getHeader().getField<SenderCompID>(),
      getHeader().getField<TargetCompID>(),
      qualifier);
}

void Message::setSessionID(const SessionID &sessionID) {
  getHeader().setField(sessionID.getBeginString());
  getHeader().setField(sessionID.getSenderCompID());
  getHeader().setField(sessionID.getTargetCompID());
}

void Message::validate() const {
  try {
    const BodyLength &aBodyLength = FIELD_GET_REF(m_header, BodyLength);

    const size_t expectedLength = static_cast<size_t>(aBodyLength);
    const size_t receivedLength = bodyLength();

    if (expectedLength != receivedLength) {
      std::stringstream text;
      text << "Expected BodyLength=" << expectedLength << ", Received BodyLength=" << receivedLength;
      throw InvalidMessage(text.str());
    }

    const CheckSum &aCheckSum = FIELD_GET_REF(m_trailer, CheckSum);

    const int expectedChecksum = (int)aCheckSum;
    const int receivedChecksum = checkSum();

    if (expectedChecksum != receivedChecksum) {
      std::stringstream text;
      text << "Expected CheckSum=" << expectedChecksum << ", Received CheckSum=" << receivedChecksum;
      throw InvalidMessage(text.str());
    }
  } catch (FieldNotFound &e) {
    const std::string fieldName = (e.field == FIX::FIELD::BodyLength) ? "BodyLength" : "CheckSum";
    throw InvalidMessage(fieldName + std::string(" is missing"));
  } catch (IncorrectDataFormat &e) {
    const std::string fieldName = (e.field == FIX::FIELD::BodyLength) ? "BodyLength" : "CheckSum";
    throw InvalidMessage(fieldName + std::string(" has wrong format: ") + e.detail);
  }
}

FIX::FieldBase Message::extractField(
    const std::string &string,
    std::string::size_type &pos,
    const DataDictionary *pSessionDD /*= 0*/,
    const DataDictionary *pAppDD /*= 0*/,
    const Group *pGroup /*= 0*/) const {
  std::string::const_iterator const tagStart = string.begin() + pos;
  std::string::const_iterator const strEnd = string.end();

  std::string::const_iterator const equalSign = std::find(tagStart, strEnd, '=');
  if (equalSign == strEnd) {
    throw InvalidMessage("Equal sign not found in field");
  }

  int field = 0;
  if (!IntConvertor::convert(tagStart, equalSign, field)) {
    throw InvalidMessage(std::string("Field tag is invalid: ") + std::string(tagStart, equalSign));
  }

  std::string::const_iterator const valueStart = equalSign + 1;

  std::string::const_iterator soh = findSoh(string, valueStart, strEnd);
  if (soh == strEnd) {
    throw InvalidMessage("SOH not found at end of field");
  }

  if (IsDataField(field, pSessionDD, pAppDD)) {
    // Assume length field is 1 less.
    int lenField = field - 1;
    // Special case for Signature which violates above assumption.
    if (field == FIELD::Signature) {
      lenField = FIELD::SignatureLength;
    }

    // identify part of the message that should contain length field
    const FieldMap *location = pGroup;
    if (!location) {
      if (isHeaderField(lenField, pSessionDD)) {
        location = &m_header;
      } else if (isTrailerField(lenField, pSessionDD)) {
        location = &m_trailer;
      } else {
        location = this;
      }
    }

    try {
      const FieldBase &fieldLength = location->reverse_find(lenField);
      soh = valueStart + IntConvertor::convert(fieldLength.getString());
    } catch (FieldNotFound &) {
      throw InvalidMessage(
          std::string("Data length field ") + IntConvertor::convert(lenField)
          + std::string(" was not found for data field ") + IntConvertor::convert(field));
    } catch (FieldConvertError &e) {
      throw InvalidMessage(
          std::string("Unable to determine SOH for data field ") + IntConvertor::convert(field) + std::string(": ")
          + e.what());
    }
  }

  std::string::const_iterator const tagEnd = soh + 1;
  pos = std::distance(string.begin(), tagEnd);

  return FieldBase(field, valueStart, soh, tagStart, tagEnd);
}

} // namespace FIX
