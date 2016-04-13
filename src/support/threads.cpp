/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>

#include <iostream>
#include <cstdlib>

#include "threads.h"


namespace wasm {

// Global thread information

bool setMainThreadId = false;
std::thread::id mainThreadId;

struct MainThreadNoter {
  MainThreadNoter() {
    // global ctors are called on main thread
    mainThreadId = std::this_thread::get_id();
    setMainThreadId = true;
  }
};

static MainThreadNoter noter;

static ThreadPool* pool = nullptr;


// Thread

Thread::Thread(std::function<void ()> onReady) : onReady(onReady) {
  // main thread object's constructor itself can
  // happen before onMainThread is ready
  assert(onMainThread());
  thread = std::unique_ptr<std::thread>(new std::thread(mainLoop, this));
}

Thread::~Thread() {
  assert(onMainThread());
  done = true;
  {
    std::lock_guard<std::mutex> lock(mutex);
    condition.notify_one();
  }
  thread->join();
}

void Thread::runTasks(std::function<void* ()> getTask_,
                      std::function<void (void*)> runTask_) {
  // TODO: fancy work stealing
  assert(onMainThread());
  getTask = getTask_;
  runTask = runTask_;
  {
    std::lock_guard<std::mutex> lock(mutex);
    condition.notify_one();
  }
}

bool Thread::onMainThread() {
  // mainThread Id might not be set yet if we are in a global ctor, but
  // that is on the main thread anyhow
  return !setMainThreadId || std::this_thread::get_id() == mainThreadId;
}

std::mutex debugLock;

void Thread::mainLoop(void *self_) {
  auto* self = static_cast<Thread*>(self_);
  while (1) {
    {
      std::unique_lock<std::mutex> lock(self->mutex);

      self->onReady();
{  std::lock_guard<std::mutex> lock(debugLock); std::cerr << self << " notified ready\n"; }
      self->condition.wait(lock);
    }
{  std::lock_guard<std::mutex> lock(debugLock); std::cerr << self<< " action!!!\n"; }
    if (self->done) break;
{  std::lock_guard<std::mutex> lock(debugLock); std::cerr << self<< " run all tasks\n"; }
    // run tasks until they are all done
    while (1) {
{  std::lock_guard<std::mutex> lock(debugLock); std::cerr << self<< " get a task!!!\n"; }
      auto task = self->getTask();
{  std::lock_guard<std::mutex> lock(debugLock); std::cerr << self<< "   got " << task << "\n"; }
      if (!task) break;
{  std::lock_guard<std::mutex> lock(debugLock); std::cerr << self<< " run task!!!\n"; }
      self->runTask(task);
    }
  }
}


// ThreadPool

ThreadPool::ThreadPool(size_t num) {
  if (num == 1) return; // no multiple cores, don't create threads
  std::unique_lock<std::mutex> lock(mutex);
  ready = 0;
  for (size_t i = 0; i < num; i++) {
    threads.emplace_back(std::unique_ptr<Thread>(new Thread([&] {
      auto old = ready.fetch_add(1);
{  std::lock_guard<std::mutex> lock(debugLock); std::cerr <<  " a new thread is ready, total " << old << "\n"; }
      if (old + 1 == threads.size()) {
{  std::lock_guard<std::mutex> lock(debugLock); std::cerr <<  "  all ready!\n"; }
        condition.notify_one();
      }
    })));
  }
  condition.wait(lock);
}

ThreadPool* ThreadPool::get() {
  if (!pool) {
    assert(Thread::onMainThread());
    size_t num = std::thread::hardware_concurrency();
    if (num < 2) num = 1;
    pool = new ThreadPool(num);
    atexit([&]() {
      delete pool;
    });
  }
  return pool;
}

void ThreadPool::runTasks(std::function<void* ()> getTask,
                          std::vector<std::function<void (void*)>>& runTaskers) {
  // If no multiple cores, or on a side thread, do not use worker threads
  if (threads.size() == 0 || !Thread::onMainThread()) {
    // just run sequentially
    assert(runTaskers.size() > 0);
    while (1) {
      auto task = getTask();
      if (!task) break;
      runTaskers[0](task);
    }
    return;
  }
  // run in parallel on threads
  // TODO: fancy work stealing
  assert(runTaskers.size() == threads.size());
{  std::lock_guard<std::mutex> lock(debugLock); std::cerr << "pool run tasks on " << threads.size() << " threads:\n"; }
  assert(!running);
  running = true;
  std::unique_lock<std::mutex> lock(mutex);
  ready = 0;
  for (size_t i = 0; i < threads.size(); i++) {
    threads[i]->runTasks([&]() -> void* {
{  std::lock_guard<std::mutex> lock(debugLock); std::cerr << "get a task, lock\n"; }
      std::lock_guard<std::mutex> lock(mutex); // TODO: we really could use atomix
{  std::lock_guard<std::mutex> lock(debugLock); std::cerr << "       locked\n"; }
      auto ret = getTask();
      if (ret == nullptr) {
        auto old = ready.fetch_add(1);
{  std::lock_guard<std::mutex> lock(debugLock); std::cerr << "       finished: " << old << "\n"; }
        if (old + 1 == threads.size()) {
{  std::lock_guard<std::mutex> lock(debugLock); std::cerr << "       all finished!\n"; }
          condition.notify_one();
        }
      }
      return ret;
    }, runTaskers[i]);
  }
{  std::lock_guard<std::mutex> lock(debugLock); std::cerr << "main thread waiting\n"; }
  condition.wait(lock);
{  std::lock_guard<std::mutex> lock(debugLock); std::cerr << "main thread continuing.............\n"; }
  running = false;
}

size_t ThreadPool::size() {
  return threads.size();
}

bool ThreadPool::isRunning() {
  return pool && pool->running;
}

} // namespace wasm

