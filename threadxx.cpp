#include "threadxx.h"

#include <iostream>

#include "ticktock.h"

// Erik Rigtorp
struct spinlock {
  std::atomic<bool> lock_ = {0};

  void lock() noexcept {
    for (;;) {
      // Optimistically assume the lock is free on the first try
      if (!lock_.exchange(true, std::memory_order_acquire)) {
        return;
      }
      // Wait for lock to be released without generating cache misses
      while (lock_.load(std::memory_order_relaxed)) {
        // Issue X86 PAUSE or ARM YIELD instruction to reduce contention between
        // hyper-threads
        //__builtin_ia32_pause();
        std::this_thread::yield();
      }
    }
  }

  bool try_lock() noexcept {
    // First do a relaxed load to check if lock is free in order to prevent
    // unnecessary cache misses if someone does while(!try_lock())
    return !lock_.load(std::memory_order_relaxed) &&
           !lock_.exchange(true, std::memory_order_acquire);
  }

  void unlock() noexcept {
    lock_.store(false, std::memory_order_release);
  }
};

template <class Spinlock>
void test_spinlock() {
  class MyThread : public threadxx::Thread {
   private:
    Spinlock& lock;
    uint64_t t;

   public:
    MyThread(Spinlock& lock) : lock(lock), t(0) {
    }

    ~MyThread() {
      Quit();
      std::cout << "timer = " << t / 1000000 << std::endl;
    }

    virtual void OnMessage(threadxx::Message&) {
      ticktock tt(t);
      for (int i = 0; i < 100000000; i++) {
        lock.lock();
        lock.unlock();
      }
    }
  };

  Spinlock lock;

  MyThread t1(lock), t2(lock);
  t1.PostMessage(1);
  t2.PostMessage(1);
}

// threadxx::Semaphore - previous implement
class Semaphore {
 private:
  int count;
  std::mutex mtx;
  std::condition_variable cv;

 private:
  int Try() {
    if (count == 0) {
      return -1;
    } else {
      --count;
      return 0;
    }
  }

 public:
  Semaphore(int ninit = 0) : count(ninit) {
  }

  int Post() {
    mtx.lock();
    ++count;
    mtx.unlock();
    cv.notify_one();
    return 0;
  }

  int Post(int n) {
    mtx.lock();
    count += n;
    mtx.unlock();
    while (n > 0) {
      cv.notify_one();
      --n;
    }
    return 0;
  }

  int Wait() {
    std::unique_lock<std::mutex> lock(mtx);
    while (Try()) cv.wait(lock);
    return 0;
  }

  int TryWait() {
    std::unique_lock<std::mutex> lock(mtx);
    return Try();
  }

  int TimedWait(int msec) {
    std::unique_lock<std::mutex> lock(mtx);
    if (Try()) {
      cv.wait_for(lock, std::chrono::milliseconds(msec));
      return Try();
    }
    return 0;
  }

  int Wait(int msec) {
    if (msec < 0)
      return Wait();
    else if (msec == 0)
      return TryWait();
    else
      return TimedWait(msec);
  }
};

template <class Sema>
void test_semaphore() {
  class MyThread : public threadxx::Thread {
   private:
    Sema& sema;
    uint64_t t;

   public:
    MyThread(Sema& sema) : sema(sema), t(0) {
    }

    ~MyThread() {
      Quit();
      std::cout << "timer = " << t / 1000000 << std::endl;
    }

    void OnMessage(threadxx::Message&) {
      ticktock tt(t);
      for (int i = 0; i < 10000000; i++) {
        sema.Wait();
        sema.Post();
      }
    }
  };

  Sema sema(1);

  MyThread t1(sema), t2(sema);
  t1.PostMessage(1);
  t2.PostMessage(1);
}

// threadxx::BlockingQueue - previouse implement
template <typename T>
class BlockingQueue : public threadxx::SafeQueue<T> {
 private:
  threadxx::Semaphore sem;

 public:
  void Push(const T& t) {
    threadxx::SafeQueue<T>::Push(t);
    sem.Post();
  }

  int Pop(T& t, int timeout = -1) {
    if (sem.Wait(timeout) == 0)
      return threadxx::SafeQueue<T>::Pop(t);
    else
      return -1;
  }
};

template <class Q>
void test_blocking_queue() {
  class MyThread : public threadxx::Thread {
   private:
    Q& q;
    uint64_t t;

   public:
    MyThread(Q& q) : q(q), t(0) {
    }

    ~MyThread() {
      Quit();
      std::cout << "timer = " << t / 1000000 << std::endl;
    }

    void OnMessage(threadxx::Message&) {
      ticktock tt(t);
      for (int i = 0; i < 10000000; i++) {
        int x = 0;
        q.Pop(x);
        q.Push(x + 1);
      }
    }
  };

  Q q;
  q.Push(0);

  MyThread t1(q), t2(q);
  t1.PostMessage(1);
  t2.PostMessage(1);
}

int main() {
  std::cout << "Testing spinlock" << std::endl;
  test_spinlock<spinlock>();

  std::cout << "Testing threadxx::Spinlock" << std::endl;
  test_spinlock<threadxx::Spinlock>();

  std::cout << "Testing Semaphore" << std::endl;
  test_semaphore<Semaphore>();

  std::cout << "Testing threadxx::Semaphore" << std::endl;
  test_semaphore<threadxx::Semaphore>();

  std::cout << "Testing BlockingQueue" << std::endl;
  test_blocking_queue<BlockingQueue<int>>();

  std::cout << "Testing threadxx::BlockingQueue" << std::endl;
  test_blocking_queue<threadxx::BlockingQueue<int>>();

  return 0;
}
