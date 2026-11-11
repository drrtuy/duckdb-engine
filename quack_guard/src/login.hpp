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

/* Parsing of what a Quack client sends at login, and of peer addresses. */

#pragma once

#include "quack_guard/authorizer.hpp"

#include <string>

namespace quack_guard
{

/* MariaDB allows 128 characters; in bytes that is 128 * 4 for utf8mb4.
   The authorizer applies the exact limit. */
static constexpr size_t MAX_USER_BYTES= 512;
static constexpr size_t MAX_ROLE_BYTES= 512;
static constexpr size_t MAX_PROOF_BYTES= 1024;

/*
  Quack's auth string is "user[/role]:password". In user and role, '%XX'
  escapes any byte except NUL (needed for ':', '/' or '%' in a name); the
  password is
  everything after the first unescaped ':'. An empty user is allowed (the
  authorizer decides about anonymous accounts). False if malformed or too
  long.
*/
bool ParseAuthString(const std::string &auth, ClientClaims &claims);

/*
  The canonical form of a peer address as the authorizer sees it:
  IPv4-mapped IPv6 ("::ffff:10.0.0.5") becomes IPv4, an IPv6 zone id
  ("%eth0") is dropped. Empty if `raw` is not a numeric address.
*/
std::string CanonicalPeer(const std::string &raw);

/* Overwrite a secret before its memory is released. */
void Wipe(std::string &secret);

} /* namespace quack_guard */
