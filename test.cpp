#include <iostream>
#include "ticktock.h"
#include "threadxx.h"

// Erik Rigtorp
struct spinlock {
    std::atomic<bool> lock_ = { 0 };

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

// XYTools
class XySpinLock {
 public:
  XySpinLock() {
    m_lock.clear();
  }

  XySpinLock(const XySpinLock&) = delete;

  XySpinLock(XySpinLock&&) = delete;

  XySpinLock& operator=(const XySpinLock&) = delete;

  ~XySpinLock() = default;

  /**
   * 加锁, 为了能够使用unique_lock, 必须实现lock方法
   */
  void lock() {
    while (m_lock.test_and_set(std::memory_order_acquire))
	  ;
  }

  /**
   * 加锁, 为了能够使用unique_lock, 必须实现try_lock方法
   */
  bool try_lock() {
    return !m_lock.test_and_set(std::memory_order_acquire);
  }

  /**
   * 解锁, 为了能够使用unique_lock, 必须实现unlock方法
   */
  void unlock() {
    m_lock.clear(std::memory_order_release);
  }

 private:
  std::atomic_flag m_lock;
};

//#define spinlock XySpinLock
#define spinlock threadxx::Spinlock

class MyThread : public threadxx::Thread
{
private:
    spinlock& lock;
    uint64_t t;

public:
    MyThread(spinlock& lock) : lock(lock), t(0)
    {
    }

    ~MyThread()
    {
        Quit();
        std::cout << "timer = " << t / 1000000 << std::endl;
    }

    virtual void OnMessage(threadxx::Message&)
    {
        ticktock tt(t);
        for (int i = 0; i < 100000000; i++)
        {
            lock.lock();
            lock.unlock();
        }
    }
};

void test_spinlock()
{
    spinlock lock;

    MyThread t1(lock), t2(lock);
    t1.PostMessage(1);
    t2.PostMessage(1);
}

int main()
{
    test_spinlock();
    return 0;
}
