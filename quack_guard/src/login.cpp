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

#include "login.hpp"

#include <algorithm>
#include <arpa/inet.h>

namespace quack_guard
{

static int hex_value(char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/* Unescape s[begin, end) into out; false on a bad escape or if too long. */
static bool unescape(const std::string &s, size_t begin, size_t end,
                     size_t max_bytes, std::string &out)
{
  out.clear();
  for (size_t i= begin; i < end; i++)
  {
    char c= s[i];
    if (c == '%')
    {
      if (i + 2 >= end)
        return false;
      int hi= hex_value(s[i + 1]), lo= hex_value(s[i + 2]);
      if (hi < 0 || lo < 0)
        return false;
      c= (char) (hi * 16 + lo);
      i+= 2;
      if (c == '\0')
        return false;  /* C-string APIs would cut the name here */
    }
    out.push_back(c);
    if (out.size() > max_bytes)
      return false;
  }
  return true;
}

bool ParseAuthString(const std::string &auth, ClientClaims &claims)
{
  /* Names are escaped, so the first ':' and '/' are delimiters. */
  auto colon= auth.find(':');
  if (colon == std::string::npos)
    return false;
  auto slash= auth.find('/');
  size_t user_end= (slash != std::string::npos && slash < colon) ? slash : colon;
  if (!unescape(auth, 0, user_end, MAX_USER_BYTES, claims.user))
    return false;
  if (user_end == slash && slash < colon)
  {
    if (!unescape(auth, slash + 1, colon, MAX_ROLE_BYTES, claims.role) ||
        claims.role.empty())
      return false;
  }
  else
    claims.role.clear();
  if (auth.size() - colon - 1 > MAX_PROOF_BYTES)
    return false;
  claims.proof.assign(auth, colon + 1, std::string::npos);
  return true;
}

std::string CanonicalPeer(const std::string &raw)
{
  std::string address= raw;
  auto zone= address.find('%');
  if (zone != std::string::npos)
    address.resize(zone);
  unsigned char buf[16];
  if (inet_pton(AF_INET, address.c_str(), buf) == 1)
    return address;
  if (inet_pton(AF_INET6, address.c_str(), buf) != 1)
    return std::string();
  static const unsigned char v4_mapped[12]= {0, 0, 0, 0, 0, 0,
                                            0, 0, 0, 0, 0xff, 0xff};
  char text[INET6_ADDRSTRLEN];
  if (std::equal(v4_mapped, v4_mapped + 12, buf))
    return inet_ntop(AF_INET, buf + 12, text, sizeof(text)) ? text
                                                            : std::string();
  return inet_ntop(AF_INET6, buf, text, sizeof(text)) ? text : std::string();
}

void Wipe(std::string &secret)
{
  volatile char *p= secret.empty() ? nullptr : &secret[0];
  for (size_t i= 0; i < secret.size(); i++)
    p[i]= 0;
  secret.clear();
}

} /* namespace quack_guard */
