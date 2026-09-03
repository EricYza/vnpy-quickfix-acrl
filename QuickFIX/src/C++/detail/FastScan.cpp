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
#include "stdafx.h"
#else
#include "config.h"
#endif

#include "detail/FastScan.h"

#if defined(QUICKFIX_SIMD_FIELD_SCAN) || defined(QUICKFIX_SIMD_STREAM_PARSER) \
    || defined(QUICKFIX_SIMD_PATTERN_SCAN)
#define QUICKFIX_WANTS_SIMD_FAST_SCAN 1
#endif

#if defined(QUICKFIX_WANTS_SIMD_FAST_SCAN) \
    && (defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#define QUICKFIX_HAS_SSE2_FAST_SCAN 1
#include <emmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

namespace FIX {
namespace detail {

/**
 * @brief Finds the first occurrence of one byte with a scalar left-to-right scan.
 *
 * @param begin First byte in the half-open search range.
 * @param end One past the final byte in the search range.
 * @param target Byte value to locate.
 * @return Pointer to the first matching byte, or `end` when no match exists.
 */
const char *findCharScalar(const char *begin, const char *end, char target) {
  while (begin != end) {
    if (*begin == target) {
      return begin;
    }
    ++begin;
  }
  return end;
}

#if defined(QUICKFIX_HAS_SSE2_FAST_SCAN)
namespace {

/**
 * @brief Returns the index of the lowest matching byte represented by an SSE2 comparison mask.
 *
 * Bit zero corresponds to the first byte in the loaded 16-byte chunk. Callers invoke this helper only for a nonzero
 * mask.
 *
 * @param mask Nonzero 16-bit byte-match mask stored in an unsigned integer.
 * @return Zero-based index of the first set bit.
 */
int firstSetBit(unsigned int mask) {
#if defined(_MSC_VER)
  unsigned long index = 0;
  _BitScanForward(&index, mask);
  return static_cast<int>(index);
#else
  return __builtin_ctz(mask);
#endif
}

} // namespace
#endif

/**
 * @brief Finds one target byte by comparing 16 candidate bytes per SSE2 iteration.
 *
 * Each 16-byte chunk is compared with a vector containing 16 copies of `target`. `_mm_movemask_epi8()` converts the
 * comparison result into a bit mask, and the lowest set bit identifies the first matching byte. Any tail shorter than
 * 16 bytes uses the scalar implementation. Builds without SSE2 support use the scalar implementation for the entire
 * range.
 *
 * @param begin First byte in the half-open search range.
 * @param end One past the final byte in the search range.
 * @param target Byte value to locate.
 * @return Pointer to the first matching byte, or `end` when no match exists.
 */
const char *findCharSimd(const char *begin, const char *end, char target) {
#if defined(QUICKFIX_HAS_SSE2_FAST_SCAN)
  const __m128i targetVector = _mm_set1_epi8(target);
  const char *current = begin;

  while (end - current >= 16) {
    const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(current));
    const __m128i matched = _mm_cmpeq_epi8(chunk, targetVector);
    const int mask = _mm_movemask_epi8(matched);
    if (mask != 0) {
      return current + firstSetBit(static_cast<unsigned int>(mask));
    }
    current += 16;
  }

  return findCharScalar(current, end, target);
#else
  return findCharScalar(begin, end, target);
#endif
}

/**
 * @brief Selects the scalar or SIMD single-byte scanner at compile time.
 *
 * Stream framing and field scanning enable the SIMD implementation independently through
 * `QUICKFIX_SIMD_STREAM_PARSER` and `QUICKFIX_SIMD_FIELD_SCAN`.
 *
 * @param begin First byte in the half-open search range.
 * @param end One past the final byte in the search range.
 * @param target Byte value to locate.
 * @return Pointer to the first matching byte, or `end` when no match exists.
 */
const char *findCharFast(const char *begin, const char *end, char target) {
#if defined(QUICKFIX_SIMD_FIELD_SCAN) || defined(QUICKFIX_SIMD_STREAM_PARSER)
  return findCharSimd(begin, end, target);
#else
  return findCharScalar(begin, end, target);
#endif
}

/**
 * @brief Finds the first complete `SOH 1 0 =` checksum-field marker with a scalar scan.
 *
 * @param begin First possible marker position in the half-open search range.
 * @param end One past the final readable byte.
 * @return Pointer to the marker's SOH byte, or `end` when no complete marker exists.
 */
const char *findSoh10Scalar(const char *begin, const char *end) {
  const char *current = begin;
  while (end - current >= 4) {
    if (current[0] == '\001' && current[1] == '1' && current[2] == '0' && current[3] == '=') {
      return current;
    }
    ++current;
  }
  return end;
}

/**
 * @brief Finds `SOH 1 0 =` by testing 16 possible marker starts in parallel with SSE2.
 *
 * Four unaligned 16-byte loads view the same candidate positions at offsets 0, 1, 2, and 3. Their comparison masks
 * are ANDed, so a bit remains set only when all four bytes at that candidate position form `SOH 1 0 =`. The first set
 * bit returns the earliest marker. A range tail that cannot safely provide all four vector loads is scanned by the
 * scalar implementation; builds without SSE2 use the scalar implementation for the whole range.
 *
 * @param begin First possible marker position in the half-open search range.
 * @param end One past the final readable byte.
 * @return Pointer to the marker's SOH byte, or `end` when no complete marker exists.
 */
const char *findSoh10Simd(const char *begin, const char *end) {
#if defined(QUICKFIX_HAS_SSE2_FAST_SCAN)
  const __m128i sohVector = _mm_set1_epi8('\001');
  const __m128i oneVector = _mm_set1_epi8('1');
  const __m128i zeroVector = _mm_set1_epi8('0');
  const __m128i equalVector = _mm_set1_epi8('=');
  const char *current = begin;

  while (end - current >= 19) {
    const __m128i sohChunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(current));
    const __m128i oneChunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(current + 1));
    const __m128i zeroChunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(current + 2));
    const __m128i equalChunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(current + 3));

    const int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(sohChunk, sohVector))
        & _mm_movemask_epi8(_mm_cmpeq_epi8(oneChunk, oneVector))
        & _mm_movemask_epi8(_mm_cmpeq_epi8(zeroChunk, zeroVector))
        & _mm_movemask_epi8(_mm_cmpeq_epi8(equalChunk, equalVector));
    if (mask != 0) {
      return current + firstSetBit(static_cast<unsigned int>(mask));
    }
    current += 16;
  }

  return findSoh10Scalar(current, end);
#else
  return findSoh10Scalar(begin, end);
#endif
}

/**
 * @brief Selects the scalar or SIMD `SOH 1 0 =` pattern scanner at compile time.
 *
 * @param begin First possible marker position in the half-open search range.
 * @param end One past the final readable byte.
 * @return Pointer to the marker's SOH byte, or `end` when no complete marker exists.
 */
const char *findSoh10Fast(const char *begin, const char *end) {
#if defined(QUICKFIX_SIMD_PATTERN_SCAN)
  return findSoh10Simd(begin, end);
#else
  return findSoh10Scalar(begin, end);
#endif
}

/**
 * @brief Reports whether this build can execute the SSE2 fast-scan implementations.
 *
 * @return `true` when a SIMD scan option requested fast scanning and the compiler target provides SSE2.
 */
bool simdFastScanAvailable() {
#if defined(QUICKFIX_HAS_SSE2_FAST_SCAN)
  return true;
#else
  return false;
#endif
}

} // namespace detail
} // namespace FIX
