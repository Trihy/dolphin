// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <queue>
#include <string>
#include <thread>

#include "Common/Thread.h"

// A thread that executes the given function for every item placed into its queue.

namespace Common
{
template <typename T>
class WorkQueueThread
{
public:
  WorkQueueThread() = default;
  WorkQueueThread(const std::string name, std::function<void(T)> function)
  {
    Reset(std::move(name), std::move(function));
  }
  ~WorkQueueThread() { Shutdown(); }

  // Shuts the current work thread down (if any) and starts a new thread with the given function
  // Note: Some consumers of this API push items to the queue before starting the thread.
  void Reset(const std::string& name, std::function<void(T)> function)
  {
    Shutdown();
    std::lock_guard lg(m_lock);
    m_thread_name = std::move(name);
    m_shutdown = false;
    m_function = std::move(function);
    m_thread = std::thread(&WorkQueueThread::ThreadLoop, this);
  }

  // Adds an item to the work queue
  template <typename... Args>
  void EmplaceItem(Args&&... args)
  {
    std::lock_guard lg(m_lock);
    if (m_shutdown)
      return;

    m_items.emplace(std::forward<Args>(args)...);
    m_idle = false;
    m_worker_cond_var.notify_one();
  }

  // Adds an item to the work queue
  void Push(T&& item)
  {
    std::lock_guard lg(m_lock);
    if (m_shutdown)
      return;

    m_items.push(item);
    m_idle = false;
    m_worker_cond_var.notify_one();
  }

  // Adds an item to the work queue
  void Push(const T& item)
  {
    std::lock_guard lg(m_lock);
    if (m_shutdown)
      return;

    m_items.push(item);
    m_idle = false;
    m_worker_cond_var.notify_one();
  }

  // Empties the queue
  // If the worker polls IsCanceling(), it can abort it's work when Cancelling
  void Cancel()
  {
    std::unique_lock lg(m_lock);
    if (m_shutdown)
      return;

    m_cancelling = true;
    m_items = std::queue<T>();
    m_worker_cond_var.notify_one();
  }

  // Tells the worker to shut down when it's queue is empty
  // Blocks until the worker thread exits.
  // If cancel is true, will Cancel before before telling the worker to exit
  void Shutdown(bool cancel = false)
  {
    {
      std::unique_lock lg(m_lock);
      if (m_shutdown || !m_thread.joinable())
        return;

      if (cancel)
      {
        m_cancelling = true;
        m_items = std::queue<T>();
      }

      m_shutdown = true;
      m_worker_cond_var.notify_one();
    }

    m_thread.join();
  }

  // Blocks until all items in the queue have been processed (or cancelled)
  void WaitForCompletion()
  {
    std::unique_lock lg(m_lock);
    if (m_idle)  // Only check m_idle, we want this to work even another thread called Shutdown
      return;

    m_wait_cond_var.wait(lg, [&] { return m_idle; });
  }

  // For the worker to check if it should abort it's work early.
  bool IsCancelling() const { return m_cancelling.load(); }

private:
  void ThreadLoop()
  {
    Common::SetCurrentThreadName(m_thread_name.c_str());

    while (true)
    {
      std::unique_lock lg(m_lock);
      if (m_items.empty())
      {
        m_idle = true;
        m_cancelling = false;
        m_wait_cond_var.notify_all();
        m_worker_cond_var.wait(
            lg, [&] { return m_shutdown || m_cancelling.load() || !m_items.empty(); });

        if (m_shutdown)
          break;
        continue;
      }
      T item{std::move(m_items.front())};
      m_items.pop();
      lg.unlock();

      m_function(std::move(item));
    }
  }

  std::function<void(T)> m_function;
  std::string m_thread_name;
  std::thread m_thread;
  std::mutex m_lock;
  std::queue<T> m_items;
  std::condition_variable m_wait_cond_var;
  std::condition_variable m_worker_cond_var;
  std::atomic<bool> m_cancelling = false;
  bool m_idle = true;
  bool m_shutdown = false;
};

}  // namespace Common
