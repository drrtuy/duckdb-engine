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

#include "session_registry.hpp"

namespace quack_guard
{

void NotifyClosed(const std::shared_ptr<QuackAuthorizer> &issuer,
                  const Identity &identity)
{
  if (!issuer)
    return;
  try
  {
    issuer->sessionClosed(identity);
  }
  catch (...)
  {
  }
}

bool SessionRegistry::Add(const std::string &connection_id, Entry entry,
                          size_t max_sessions)
{
  std::lock_guard<std::mutex> guard(lock);
  if (sessions.size() >= max_sessions || sessions.count(connection_id))
    return false;
  sessions.emplace(connection_id, std::move(entry));
  return true;
}

bool SessionRegistry::Find(const std::string &connection_id, Entry &out) const
{
  std::lock_guard<std::mutex> guard(lock);
  auto it= sessions.find(connection_id);
  if (it == sessions.end())
    return false;
  out= it->second;
  return true;
}

void SessionRegistry::Remove(const std::string &connection_id)
{
  Entry removed;
  {
    std::lock_guard<std::mutex> guard(lock);
    auto it= sessions.find(connection_id);
    if (it == sessions.end())
      return;
    removed= std::move(it->second);
    sessions.erase(it);
  }
  NotifyClosed(removed.issuer, *removed.identity);
}

std::vector<std::string> SessionRegistry::Ids() const
{
  std::lock_guard<std::mutex> guard(lock);
  std::vector<std::string> ids;
  ids.reserve(sessions.size());
  for (auto &entry : sessions)
    ids.push_back(entry.first);
  return ids;
}

size_t SessionRegistry::Count() const
{
  std::lock_guard<std::mutex> guard(lock);
  return sessions.size();
}

} /* namespace quack_guard */
