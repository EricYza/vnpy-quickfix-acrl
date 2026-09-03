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

#include "FieldConvertors.h"
#include "Parser.h"
#include "Utility.h"
#include "detail/FastScan.h"
#include <algorithm>
#include <cstring>
#include <limits>

namespace FIX {

#if defined(QUICKFIX_SIMD_STREAM_PARSER)
namespace {

/**
 * @brief Finds the first `8=` BeginString marker in the accumulated TCP stream.
 *
 * The SIMD helper locates candidate `'8'` bytes and the following byte is checked for `'='`.
 *
 * @param buffer Parser-owned stream bytes that may contain noise, partial messages, or multiple messages.
 * @return Offset of the marker's `'8'`, or `std::string::npos` when no complete marker exists.
 */
std::string::size_type findBeginMarkerFast(const std::string &buffer) {
  if (buffer.size() < 2) {
    return std::string::npos;
  }

  const char *data = buffer.data();
  const char *current = data;
  const char *lastCandidate = data + buffer.size() - 1;

  while (current < lastCandidate) {
    const char *candidate = detail::findCharFast(current, lastCandidate, '8');
    if (candidate == lastCandidate) {
      return std::string::npos;
    }
    if (candidate[1] == '=') {
      return static_cast<std::string::size_type>(candidate - data);
    }
    current = candidate + 1;
  }

  return std::string::npos;
}

/**
 * @brief Finds a field marker consisting of SOH followed by a fixed byte pattern.
 *
 * Candidate SOH bytes are located with `findCharFast()`, then the short pattern is verified with `memcmp()`. Stream
 * framing uses this helper for `SOH 9 =` and, when pattern SIMD is disabled, for `SOH 1 0 =`.
 *
 * @param buffer Parser-owned stream bytes to search.
 * @param start Offset of the first possible SOH candidate.
 * @param pattern Bytes required immediately after the candidate SOH.
 * @param patternLength Number of bytes in `pattern`.
 * @return Offset of the matching SOH byte, or `std::string::npos` when no complete marker exists.
 */
std::string::size_type findSohPatternFast(
    const std::string &buffer,
    std::string::size_type start,
    const char *pattern,
    std::size_t patternLength) {
  const char *data = buffer.data();
  const char *current = data + start;
  const char *end = data + buffer.size();

  while (current < end) {
    const char *candidate = detail::findCharFast(current, end, '\001');
    if (candidate == end) {
      return std::string::npos;
    }
    if (static_cast<std::size_t>(end - candidate - 1) >= patternLength
        && std::memcmp(candidate + 1, pattern, patternLength) == 0) {
      return static_cast<std::string::size_type>(candidate - data);
    }
    current = candidate + 1;
  }

  return std::string::npos;
}

/**
 * @brief Finds the next SOH field delimiter with the compile-time-selected byte scanner.
 *
 * @param buffer Parser-owned stream bytes to search.
 * @param start Offset at which the search begins.
 * @return Offset of the first SOH byte, or `std::string::npos` when none exists.
 */
std::string::size_type findSohFast(const std::string &buffer, std::string::size_type start) {
  const char *data = buffer.data();
  const char *end = data + buffer.size();
  const char *result = detail::findCharFast(data + start, end, '\001');
  if (result == end) {
    return std::string::npos;
  }
  return static_cast<std::string::size_type>(result - data);
}

/**
 * @brief Finds the `SOH 1 0 =` checksum-field marker near the BodyLength-derived boundary.
 *
 * `QUICKFIX_SIMD_PATTERN_SCAN` uses the four-byte SIMD pattern matcher. Without that independent option, the function
 * reuses the SOH-candidate scanner followed by a scalar comparison with `"10="`.
 *
 * @param buffer Parser-owned stream bytes to search.
 * @param start Offset of the first possible checksum marker.
 * @return Offset of the marker's SOH byte, or `std::string::npos` when no complete marker exists.
 */
std::string::size_type findChecksumPatternFast(const std::string &buffer, std::string::size_type start) {
#if defined(QUICKFIX_SIMD_PATTERN_SCAN)
  const char *data = buffer.data();
  const char *end = data + buffer.size();
  const char *result = detail::findSoh10Fast(data + start, end);
  if (result == end) {
    return std::string::npos;
  }
  return static_cast<std::string::size_type>(result - data);
#else
  return findSohPatternFast(buffer, start, "10=", 3);
#endif
}

/**
 * @brief Parses the decimal BodyLength value without allocating a temporary string.
 *
 * Empty values, nondigits, and values that overflow `int` are rejected.
 *
 * @param buffer Parser-owned stream containing the BodyLength digits.
 * @param begin Offset of the first BodyLength digit.
 * @param end Offset of the SOH immediately after the final digit.
 * @param length Receives the parsed non-negative length on success.
 * @return `true` when all digits form a representable integer; otherwise `false`.
 */
bool parseLengthFast(const std::string &buffer, std::string::size_type begin, std::string::size_type end, int &length) {
  if (begin == end) {
    return false;
  }

  int value = 0;
  for (std::string::size_type pos = begin; pos < end; ++pos) {
    const char c = buffer[pos];
    if (c < '0' || c > '9') {
      return false;
    }

    const int digit = c - '0';
    if (value > (std::numeric_limits<int>::max() - digit) / 10) {
      return false;
    }
    value = (value * 10) + digit;
  }

  length = value;
  return true;
}

/**
 * @brief Attempts to extract one complete FIX message with the stream-framing fast path.
 *
 * The function locates `8=`, finds `SOH 9 =`, parses BodyLength, jumps to the expected checksum region, finds
 * `SOH 1 0 =`, and finally finds the checksum field's terminating SOH. On success it copies exactly one complete
 * message to `str` and erases all consumed stream bytes. On any incomplete or unsupported shape it returns `false`
 * without modifying `str` or `buffer`, allowing `Parser::readFixMessage()` to run the original scalar parser.
 *
 * @param str Receives one complete FIX wire message on success.
 * @param buffer Persistent Parser stream containing zero, one, or multiple messages.
 * @return `true` when one complete message was extracted; otherwise `false`.
 */
bool tryReadFixMessageFast(std::string &str, std::string &buffer) {
  const std::string::size_type beginPos = findBeginMarkerFast(buffer);
  if (beginPos == std::string::npos) {
    return false;
  }

  const std::string::size_type lengthFieldPos = findSohPatternFast(buffer, beginPos, "9=", 2);
  if (lengthFieldPos == std::string::npos) {
    return false;
  }

  const std::string::size_type lengthBegin = lengthFieldPos + 3;
  const std::string::size_type lengthEnd = findSohFast(buffer, lengthBegin);
  if (lengthEnd == std::string::npos) {
    return false;
  }

  int length = 0;
  if (!parseLengthFast(buffer, lengthBegin, lengthEnd, length)) {
    return false;
  }

  const std::string::size_type bodyBegin = lengthEnd + 1;
  const std::string::size_type bodyLength = static_cast<std::string::size_type>(length);
  if (bodyBegin > std::numeric_limits<std::string::size_type>::max() - bodyLength) {
    return false;
  }

  const std::string::size_type checksumSearchStart = bodyBegin + bodyLength;
  if (buffer.size() < checksumSearchStart) {
    return false;
  }

  const std::string::size_type checksumFieldPos = findChecksumPatternFast(buffer, checksumSearchStart - 1);
  if (checksumFieldPos == std::string::npos) {
    return false;
  }

  const std::string::size_type checksumBegin = checksumFieldPos + 4;
  const std::string::size_type checksumEnd = findSohFast(buffer, checksumBegin);
  if (checksumEnd == std::string::npos) {
    return false;
  }

  const std::string::size_type messageEnd = checksumEnd + 1;
  str.assign(buffer, beginPos, messageEnd - beginPos);
  buffer.erase(0, messageEnd);
  return true;
}

} // namespace
#endif

/**
 * @brief Extracts BodyLength for the original scalar stream-framing path.
 *
 * @param length Receives the parsed tag 9 value.
 * @param pos Receives the offset of the first body byte on success.
 * @param buffer Persistent Parser stream being examined.
 * @return `true` when a complete BodyLength field was parsed; `false` when its marker or delimiter is not yet present.
 */
bool Parser::extractLength(int &length, std::string::size_type &pos, const std::string &buffer)
    EXCEPT(MessageParseError) {
  if (!buffer.size()) {
    return false;
  }

  std::string::size_type startPos = buffer.find("\0019=", 0);
  if (startPos == std::string::npos) {
    return false;
  }
  startPos += 3;
  std::string::size_type endPos = buffer.find("\001", startPos);
  if (endPos == std::string::npos) {
    return false;
  }

  std::string strLength(buffer, startPos, endPos - startPos);

  try {
    length = IntConvertor::convert(strLength);
    if (length < 0) {
      throw MessageParseError();
    }
  } catch (FieldConvertError &) {
    throw MessageParseError();
  }

  pos = endPos + 1;
  return true;
}

/**
 * @brief Removes one complete FIX message from the accumulated byte stream.
 *
 * When `QUICKFIX_SIMD_STREAM_PARSER` is enabled, the fast framing attempt runs first. A fast-path miss immediately
 * continues into the original `std::string::find()` implementation, preserving the baseline parser as a correctness
 * fallback. Successful extraction removes exactly the consumed bytes; an incomplete stream returns `false`.
 *
 * @param str Receives one complete FIX wire message.
 * @return `true` when a message was extracted; `false` when more stream bytes are required.
 */
bool Parser::readFixMessage(std::string &str) EXCEPT(MessageParseError) {
#if defined(QUICKFIX_SIMD_STREAM_PARSER)
  if (tryReadFixMessageFast(str, m_buffer)) {
    return true;
  }
#endif

  std::string::size_type pos = 0;

  if (m_buffer.length() < 2) {
    return false;
  }
  pos = m_buffer.find("8=");
  if (pos == std::string::npos) {
    return false;
  }
  m_buffer.erase(0, pos);

  int length = 0;

  try {
    if (extractLength(length, pos, m_buffer)) {
      pos += length;
      if (m_buffer.size() < pos) {
        return false;
      }

      pos = m_buffer.find("\00110=", pos - 1);
      if (pos == std::string::npos) {
        return false;
      }
      pos += 4;
      pos = m_buffer.find("\001", pos);
      if (pos == std::string::npos) {
        return false;
      }
      pos += 1;

      str.assign(m_buffer, 0, pos);
      m_buffer.erase(0, pos);
      return true;
    }
  } catch (MessageParseError &e) {
    if (length > 0) {
      m_buffer.erase(0, pos + length);
    } else {
      m_buffer.erase();
    }

    throw e;
  }

  return false;
}
} // namespace FIX
