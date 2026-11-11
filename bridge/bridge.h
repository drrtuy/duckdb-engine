/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov <leontodys@gmail.com>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

/* Lifecycle of the bridge inside the plugin. */

#pragma once

#include "mariadb_authorizer.h"

#include <memory>

namespace quack_bridge
{

/* ACL worker limits, set from the plugin's system variables */
extern std::atomic<size_t> acl_queue_size;
extern std::atomic<uint64_t> acl_timeout_ms;

/* Start the authorizer and its worker; false on failure */
bool StartBridge(handlerton *engine);
/* Stop the worker (pending requests are denied). Idempotent. */
void StopBridge();
/* The running authorizer, or null */
std::shared_ptr<MariaDBAuthorizer> Bridge();

} /* namespace quack_bridge */
