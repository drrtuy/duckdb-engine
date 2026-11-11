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
  The ACL worker: one thread owning a background THD, on which every bridge
  request runs (MariaDB's ACL functions need a THD). Requests wait in a
  bounded queue; callers wait at most a timeout. Fail closed: a full queue,
  a timeout, a stopped worker all answer "deny" with their own reason.
*/

#pragma once

#include "quack_guard/authorizer.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class THD;

namespace quack_bridge
{

class AclWorker
{
public:
  /* `abandoned` turns true once the caller stopped waiting: a long job
     should check it between steps and give up */
  using Job=
      std::function<quack_guard::Verdict(THD *, const std::atomic<bool> &abandoned)>;

  AclWorker()= default;
  ~AclWorker();

  /* Start the thread and wait until it has its THD; false if it could not
     (then everything denies) */
  bool Start();
  /* Fail queued requests, finish the running one, join. Idempotent. */
  void Stop();

  /*
    Run `job` on the worker and wait for its verdict. `queue_size` and
    `timeout_ms` are read per call (0 is legal: always Saturated / Timeout).
    The job must own copies of everything it uses: after a timeout it may
    still run, and its result is dropped.
  */
  quack_guard::Verdict Run(Job job, size_t queue_size, uint64_t timeout_ms);

private:
  struct Request;
  void Loop();

  std::mutex lock;
  std::condition_variable wake;
  std::deque<std::shared_ptr<Request>> queue;
  bool running= false;
  bool stopping= false;
  /* Set by the thread once its THD is ready, or failed */
  enum class Init { Pending, Ready, Failed } init= Init::Pending;
  std::condition_variable init_cv;
  std::thread thread;
};

} /* namespace quack_bridge */
