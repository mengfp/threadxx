/*
 * threadxx.h
 *
 *  Created on: Oct 30, 2015
 *      Author: mengfp
 */

#ifndef THREADXX_H_
#define THREADXX_H_

#include <atomic>
#include <condition_variable>
#include <queue>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
enum THREAD_PRIORITY {
  THREAD_PRIORITY_HIGHEST,
  THREAD_PRIORITY_ABOVE_NORMAL,
  THREAD_PRIORITY_NORMAL,
  THREAD_PRIORITY_BELOW_NORMAL,
  THREAD_PRIORITY_LOWEST,
  THREAD_PRIORITY_IDLE
};
inline bool SetThreadPriority(pthread_t pt, int priority) {
  sched_param param{0};
  int policy = (priority == THREAD_PRIORITY_IDLE ? SCHED_IDLE : SCHED_OTHER);
  return !pthread_setschedparam(pt, policy, &param);
}
#endif

#define THREADXX_BEGIN namespace threadxx {
#define THREADXX_END }

THREADXX_BEGIN

inline void yield() {
  std::this_thread::yield();
}

class Spinlock {
 private:
  std::atomic_flag flag = ATOMIC_FLAG_INIT;

 public:
  void Lock() {
    while (flag.test_and_set(std::memory_order_acquire)) yield();
  }

  bool Try() {
    return !flag.test_and_set(std::memory_order_acquire);
  }

  void Unlock() {
    flag.clear(std::memory_order_release);
  }

  void lock() {
    Lock();
  }

  bool try_lock() {
    return Try();
  }

  void unlock() {
    Unlock();
  }
};

class Condition {
 private:
  volatile bool value = false;
  Spinlock spinlock;
  std::condition_variable_any cv;

 public:
  bool Get() {
    return value;
  }

  void Set() {
    if (value) {
      return;
    }
    {
      std::unique_lock<Spinlock> lock(spinlock);
      value = true;
    }
    cv.notify_all();
  }

  void Reset() {
    value = false;
  }

  bool Wait(int msec = -1) {
    if (value) {
      return true;
    }
    std::unique_lock<Spinlock> lock(spinlock);
    if (value) {
      return true;
    }
    if (msec < 0) {
      cv.wait(lock);
    } else {
      cv.wait_for(lock, std::chrono::milliseconds(msec));
    }
    return value;
  }
};

class Semaphore {
 private:
  volatile int count;
  Spinlock spinlock;
  std::condition_variable_any cv;

 private:
  bool Try() {
    return count ? (--count, true) : false;
  }

 public:
  Semaphore(int ninit = 0) : count(ninit) {
  }

  void Post() {
    {
      std::unique_lock<Spinlock> lock(spinlock);
      ++count;
    }
    cv.notify_one();
  }

  void Post(int n) {
    {
      std::unique_lock<Spinlock> lock(spinlock);
      count += n;
    }
    cv.notify_all();
  }

  void Wait() {
    std::unique_lock<Spinlock> lock(spinlock);
    while (!Try()) cv.wait(lock);
  }

  bool TryWait() {
    std::unique_lock<Spinlock> lock(spinlock);
    return Try();
  }

  bool TimedWait(int msec) {
    std::unique_lock<Spinlock> lock(spinlock);
    if (Try()) return true;
    cv.wait_for(lock, std::chrono::milliseconds(msec));
    return Try();
  }

  bool Wait(int msec) {
    if (msec < 0)
      return Wait(), true;
    else if (msec == 0)
      return TryWait();
    else
      return TimedWait(msec);
  }
};

template <typename T>
class SafeQueue : public std::queue<T> {
 private:
  Spinlock spinlock;

 public:
  bool IsEmpty() {
    std::unique_lock<Spinlock> lock(spinlock);
    return std::queue<T>::empty();
  }

  void Push(const T& t) {
    std::unique_lock<Spinlock> lock(spinlock);
    std::queue<T>::push(t);
  }

  bool Pop(T& t) {
    std::unique_lock<Spinlock> lock(spinlock);
    if (std::queue<T>::empty()) return false;
    t = std::queue<T>::front();
    std::queue<T>::pop();
    return true;
  }
};

template <typename T>
class BlockingQueue : public std::queue<T> {
 private:
  Spinlock spinlock;
  std::condition_variable_any cv;

 private:
  bool Try(T& t) {
    if (std::queue<T>::empty()) return false;
    t = std::queue<T>::front();
    std::queue<T>::pop();
    return true;
  }

 public:
  bool IsEmpty() {
    std::unique_lock<Spinlock> lock(spinlock);
    return std::queue<T>::empty();
  }

  void Push(const T& t) {
    {
      std::unique_lock<Spinlock> lock(spinlock);
      std::queue<T>::push(t);
    }
    cv.notify_one();
  }

  void Pop(T& t) {
    std::unique_lock<Spinlock> lock(spinlock);
    while (!Try(t)) cv.wait(lock);
  }

  bool TryPop(T& t) {
    std::unique_lock<Spinlock> lock(spinlock);
    return Try(t);
  }

  bool TimedPop(T& t, int timeout) {
    std::unique_lock<Spinlock> lock(spinlock);
    if (Try(t)) return true;
    cv.wait_for(lock, std::chrono::milliseconds(timeout));
    return Try(t);
  }

  bool Pop(T& t, int timeout) {
    if (timeout < 0)
      return Pop(t), true;
    else if (timeout == 0)
      return TryPop(t);
    else
      return TimedPop(t, timeout);
  }
};

class Message {
 public:
  int type;
  long long first;
  long long second;
  Semaphore* sem;

 public:
  Message(int type = 0, long long first = 0, long long second = 0)
    : type(type), first(first), second(second), sem(nullptr) {
  }

  virtual ~Message() {
  }
};

class MessageQueue : public BlockingQueue<Message*> {
 public:
  ~MessageQueue() {
    Message* p;
    while (TryPop(p)) delete p;
  }

  void Push(Message* p) {
    BlockingQueue<Message*>::Push(p);
  }
};

enum THREAD_MSG_RESERVED { TMSG_TIMEOUT = -1, TMSG_QUIT = -2 };

class Thread {
 public:
  Thread(MessageQueue* mq = 0) : timeout(-1), quit(false) {
    if (mq) {
      local_mq = 0;
      this->mq = mq;
    } else {
      local_mq = new MessageQueue;
      this->mq = local_mq;
    }
    th = new std::thread(ThreadProc, this);
  }

  virtual ~Thread() {
    Quit();
    if (local_mq) {
      delete local_mq;
    }
  }

  void Quit() {
    if (th) {
      mq->Push(new Message(TMSG_QUIT));
      th->join();
      delete th;
      th = 0;
    }
  }

  void SetTimeout(int timeout) {
    mq->Push(new Message(TMSG_TIMEOUT, timeout));
  }

  bool PostMessage(Message* p, bool wait = false) {
    if (th && p && p->type >= 0) {
      if (wait) {
        Semaphore sem;
        p->sem = &sem;
        mq->Push(p);
        sem.Wait();
        return true;
      } else {
        mq->Push(p);
        return true;
      }
    } else {
      delete p;
      return false;
    }
  }

  bool PostMessage(int type,
                   long long first = 0,
                   long long second = 0,
                   bool wait = false) {
    if (th && type >= 0) {
      auto p = new Message(type, first, second);
      if (wait) {
        Semaphore sem;
        p->sem = &sem;
        mq->Push(p);
        sem.Wait();
        return true;
      } else {
        mq->Push(p);
        return true;
      }
    } else {
      return false;
    }
  }

  void SetPriority(int priority) {
    if (th) {
      SetThreadPriority(th->native_handle(), priority);
    }
  }

 protected:
  virtual void OnMessage(Message* p) {
  }

  virtual void OnMessage(Message& msg) {
  }

  virtual void OnIdle() {
  }

  virtual void OnTimeout() {
  }

  virtual void OnQuit() {
  }

 private:
  MessageQueue* local_mq;
  MessageQueue* mq;
  std::thread* th;
  volatile int timeout;
  volatile bool quit;

 private:
  static void ThreadProc(Thread* p) {
    p->Run();
  }

  void ProcessMessage(Message* p) {
    if (p->type >= 0) {
      OnMessage(p);
      OnMessage(*p);
    } else if (p->type == TMSG_TIMEOUT) {
      timeout = (int)p->first;
    } else if (p->type == TMSG_QUIT) {
      quit = true;
    }
  }

  void Run() {
    Message* p = nullptr;
    while (true) {
      if (mq->Pop(p, timeout) && p) {
        ProcessMessage(p);
        if (p->sem) {
          p->sem->Post();
        }
        delete p;
        if (quit) {
          OnQuit();
          return;
        }
        while (mq->TryPop(p) && p) {
          ProcessMessage(p);
          if (p->sem) {
            p->sem->Post();
          }
          delete p;
          if (quit) {
            OnQuit();
            return;
          }
        }
        OnIdle();
      } else {
        OnTimeout();
      }
    }
  }
};

THREADXX_END

#endif /* THREADXX_H_ */
