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

/*
  Open Quack sessions by connection id. Each entry remembers its peer and
  the authorizer that admitted it; Remove() reports the end of the session
  to that authorizer exactly once.
*/

#pragma once

#include "quack_guard/authorizer.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace quack_guard
{

class SessionRegistry
{
public:
  struct Entry
  {
    std::string peer_ip;
    std::shared_ptr<const Identity> identity;
    std::shared_ptr<QuackAuthorizer> issuer;
  };

  /* False if `max_sessions` are already open or the id is taken. */
  bool Add(const std::string &connection_id, Entry entry, size_t max_sessions);
  /* Copies the entry; false if unknown. */
  bool Find(const std::string &connection_id, Entry &out) const;
  /* Forget the session and tell its authorizer; no-op if unknown. */
  void Remove(const std::string &connection_id);
  std::vector<std::string> Ids() const;
  size_t Count() const;

private:
  mutable std::mutex lock;
  std::map<std::string, Entry> sessions;
};

/* Tell `issuer` that `identity`'s session ended; never throws. */
void NotifyClosed(const std::shared_ptr<QuackAuthorizer> &issuer,
                  const Identity &identity);

} /* namespace quack_guard */
