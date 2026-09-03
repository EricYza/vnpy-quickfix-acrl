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

#include "Utility.h"
#include <cstddef>

namespace FIX::detail {

/**
 * @brief Classifies one non-blocking direct send attempt.
 *
 * `WouldBlock` preserves the queued message for another scan; `PeerClosed` and `Error` require connection cleanup.
 */
enum class DirectSocketWriteStatus {
  Progress,
  WouldBlock,
  PeerClosed,
  Error
};

/**
 * @brief Carries the classified send outcome, byte count, and operating-system error code.
 */
struct DirectSocketWriteResult {
  DirectSocketWriteStatus status;
  ssize_t bytes;
  int errorCode;
};

DirectSocketWriteResult directSocketWrite(socket_handle socket, const char *data, std::size_t length) noexcept;

} // namespace FIX::detail
