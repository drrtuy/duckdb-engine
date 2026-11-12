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

#define MYSQL_SERVER 1
#include "bridge.h"

#include <mutex>

namespace quack_bridge
{

std::atomic<size_t> acl_queue_size{64};
std::atomic<uint64_t> acl_timeout_ms{5000};

static std::mutex bridge_lock;
static std::shared_ptr<MariaDBAuthorizer> bridge;

bool StartBridge(handlerton *engine)
{
  std::lock_guard<std::mutex> guard(bridge_lock);
  if (bridge)
    return true;
  auto authorizer= std::make_shared<MariaDBAuthorizer>(
      engine, BridgeSettings{&acl_queue_size, &acl_timeout_ms});
  if (!authorizer->Start())
    return false;
  bridge= std::move(authorizer);
  return true;
}

void StopBridge()
{
  std::shared_ptr<MariaDBAuthorizer> stopping;
  {
    std::lock_guard<std::mutex> guard(bridge_lock);
    stopping= std::move(bridge);
  }
  if (stopping)
    stopping->Stop();
}

std::shared_ptr<MariaDBAuthorizer> Bridge()
{
  std::lock_guard<std::mutex> guard(bridge_lock);
  return bridge;
}

} /* namespace quack_bridge */
