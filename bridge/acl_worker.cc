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
#include "acl_worker.h"

#include <my_global.h>
#include "sql_class.h"
#include "sql_lex.h"

#include <chrono>

MYSQL_THD create_background_thd();
void destroy_background_thd(MYSQL_THD thd);
void *thd_attach_thd(MYSQL_THD thd);
void thd_detach_thd(void *mysys_var);

namespace quack_bridge
{

using quack_guard::Reason;
using quack_guard::Verdict;

struct AclWorker::Request
{
  Job job;
  std::atomic<bool> abandoned{false};
  std::mutex lock;
  std::condition_variable done_cv;
  bool done= false;
  Verdict verdict;  /* deny by default */
};

AclWorker::~AclWorker() { Stop(); }

bool AclWorker::Start()
{
  std::unique_lock<std::mutex> guard(lock);
  if (running)
    return true;
  stopping= false;
  init= Init::Pending;
  try
  {
    thread= std::thread([this] { Loop(); });
  }
  catch (...)
  {
    return false;
  }
  init_cv.wait(guard, [&] { return init != Init::Pending; });
  if (init == Init::Failed)
  {
    guard.unlock();
    thread.join();
    return false;
  }
  running= true;
  return true;
}

void AclWorker::Stop()
{
  std::deque<std::shared_ptr<Request>> pending;
  std::thread stopping_thread;
  {
    std::lock_guard<std::mutex> guard(lock);
    if (!running)
      return;
    running= false;
    stopping= true;
    pending.swap(queue);
    stopping_thread= std::move(thread);
    wake.notify_all();
  }
  for (auto &request : pending)
  {
    std::lock_guard<std::mutex> guard(request->lock);
    request->verdict= Verdict::Deny(Reason::Shutdown);
    request->done= true;
    request->done_cv.notify_all();
  }
  if (stopping_thread.joinable())
    stopping_thread.join();
}

Verdict AclWorker::Run(Job job, size_t queue_size, uint64_t timeout_ms)
{
  if (timeout_ms == 0)
    return Verdict::Deny(Reason::Timeout);
  std::shared_ptr<Request> request;
  try
  {
    request= std::make_shared<Request>();
    request->job= std::move(job);
  }
  catch (...)
  {
    return Verdict::Deny(Reason::Internal);
  }
  {
    std::lock_guard<std::mutex> guard(lock);
    if (!running || stopping)
      return Verdict::Deny(Reason::Shutdown);
    if (queue.size() >= queue_size)
      return Verdict::Deny(Reason::Saturated);
    try
    {
      queue.push_back(request);
    }
    catch (...)
    {
      return Verdict::Deny(Reason::Internal);
    }
    wake.notify_one();
  }
  std::unique_lock<std::mutex> wait(request->lock);
  if (!request->done_cv.wait_for(wait, std::chrono::milliseconds(timeout_ms),
                                 [&] { return request->done; }))
  {
    /* Still queued: take it out; running: told to give up, its verdict
       is dropped */
    request->abandoned= true;
    std::lock_guard<std::mutex> guard(lock);
    for (auto it= queue.begin(); it != queue.end(); ++it)
      if (*it == request)
      {
        queue.erase(it);
        break;
      }
    return Verdict::Deny(Reason::Timeout);
  }
  return request->verdict;
}

void AclWorker::Loop()
{
  char stack_top;  /* the base of this thread's stack, for THD::thread_stack */
  THD *thd= nullptr;
  void *saved_mysys_var= nullptr;
  if (!my_thread_init())
  {
    thd= create_background_thd();
    if (thd)
    {
      saved_mysys_var= thd_attach_thd(thd);
      thd->thread_stack= &stack_top;
    }
  }
  {
    std::lock_guard<std::mutex> guard(lock);
    init= thd ? Init::Ready : Init::Failed;
    init_cv.notify_all();
  }
  if (!thd)
  {
    my_thread_end();
    return;
  }
  for (;;)
  {
    std::shared_ptr<Request> request;
    {
      std::unique_lock<std::mutex> guard(lock);
      wake.wait(guard, [&] { return stopping || !queue.empty(); });
      if (stopping)
        break;
      request= queue.front();
      queue.pop_front();
    }
    Verdict verdict= Verdict::Deny(Reason::Internal);
    if (!request->abandoned)
    {
      /* Each request is a statement of its own: lex_end() releases the
         plugin locks ha_table_exists() takes */
      lex_start(thd);
      try
      {
        verdict= request->job(thd, request->abandoned);
      }
      catch (...)
      {
        verdict= Verdict::Deny(Reason::Internal);
      }
      lex_end(thd->lex);
      /* Leave nothing behind for the next request */
      thd->security_ctx= &thd->main_security_ctx;
      thd->clear_error();
      free_root(thd->mem_root, MYF(MY_KEEP_PREALLOC));
    }
    std::lock_guard<std::mutex> guard(request->lock);
    request->verdict= verdict;
    request->done= true;
    request->done_cv.notify_all();
  }
  thd_detach_thd(saved_mysys_var);
  destroy_background_thd(thd);
  my_thread_end();
}

} /* namespace quack_bridge */
