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

#pragma once

namespace FIX {
namespace detail {

const char *findCharScalar(const char *begin, const char *end, char target);
const char *findCharSimd(const char *begin, const char *end, char target);
const char *findCharFast(const char *begin, const char *end, char target);
const char *findSoh10Scalar(const char *begin, const char *end);
const char *findSoh10Simd(const char *begin, const char *end);
const char *findSoh10Fast(const char *begin, const char *end);
bool simdFastScanAvailable();

} // namespace detail
} // namespace FIX
