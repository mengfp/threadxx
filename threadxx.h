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
enum THREAD_PRIORITY
{
	THREAD_PRIORITY_HIGHEST,
	THREAD_PRIORITY_ABOVE_NORMAL,
	THREAD_PRIORITY_NORMAL,
	THREAD_PRIORITY_BELOW_NORMAL,
	THREAD_PRIORITY_LOWEST,
	THREAD_PRIORITY_IDLE
};
inline bool SetThreadPriority(pthread_t pt, int priority)
{
	sched_param param{ 0 };
	int policy = (priority == THREAD_PRIORITY_IDLE ? SCHED_IDLE : SCHED_OTHER);
	return !pthread_setschedparam(pt, policy, &param);
}
#endif

#define THREADXX_BEGIN namespace threadxx {
#define THREADXX_END }

THREADXX_BEGIN

inline void yield()
{
	std::this_thread::yield();
}

class Spinlock
{
private:
	std::atomic_flag flag;

public:
	Spinlock()
	{
		flag.clear();
	}

	void Lock()
	{
		while (flag.test_and_set(std::memory_order_acquire))
			yield();
	}

	bool Try()
	{
		return !flag.test_and_set(std::memory_order_acquire);
	}

	void Unlock()
	{
		flag.clear(std::memory_order_release);
	}
	
	void lock()
	{
		Lock();
	}
	
	bool try_lock()
	{
		return Try();
	}
	
	void unlock()
	{
		Unlock();
	}
};

template <typename T> class Lock
{
private:
	T& lock;

public:
	Lock(T& lock) : lock(lock)
	{
		lock.Lock();
	}

	~Lock()
	{
		lock.Unlock();
	}
};

class Semaphore
{
private:
	volatile int count;
	std::mutex mtx;
	std::condition_variable cv;

private:
	int Try()
	{
		if (count == 0)
		{
			return -1;
		}
		else
		{
			--count;
			return 0;
		}
	}

public:
	Semaphore(int ninit = 0) :
		count(ninit)
	{
	}

	int Post()
	{
		mtx.lock();
		++count;
		mtx.unlock();
		cv.notify_one();
		return 0;
	}

	int Post(int n)
	{
		mtx.lock();
		count += n;
		mtx.unlock();
		while (n > 0)
		{
			cv.notify_one();
			--n;
		}
		return 0;
	}

	int Wait()
	{
		std::unique_lock < std::mutex > lock(mtx);
		while (Try())
			cv.wait(lock);
		return 0;
	}

	int TryWait()
	{
		std::unique_lock < std::mutex > lock(mtx);
		return Try();
	}

	int TimedWait(int msec)
	{
		std::unique_lock < std::mutex > lock(mtx);
		if (Try())
		{
			cv.wait_for(lock, std::chrono::milliseconds(msec));
			return Try();
		}
		return 0;
	}

	int Wait(int msec)
	{
		if (msec < 0)
			return Wait();
		else if (msec == 0)
			return TryWait();
		else
			return TimedWait(msec);
	}
};

template<typename T> class SafeQueue : public std::queue<T>
{
private:
	Spinlock spin;

public:
	int IsEmpty()
	{
		spin.Lock();
		int r = std::queue<T>::empty();
		spin.Unlock();
		return r;
	}

	void Push(T t)
	{
		spin.Lock();
		std::queue<T>::push(t);
		spin.Unlock();
	}

	int Pop(T& t)
	{
		int r = 0;
		spin.Lock();
		if (std::queue<T>::empty())
			r = -1;
		else
		{
			t = std::queue<T>::front();
			std::queue<T>::pop();
		}
		spin.Unlock();
		return r;
	}
};

template<typename T> class BlockingQueue : public SafeQueue<T>
{
private:
	Semaphore sem;

public:
	void Push(T t)
	{
		SafeQueue<T>::Push(t);
		sem.Post();
	}

	int Pop(T& t, int timeout = -1)
	{
		if (sem.Wait(timeout) == 0)
			return SafeQueue<T>::Pop(t);
		else
			return -1;
	}
};

class Message
{
public:
	int type;
	long long wParam;
	long long lParam;

public:
	Message(int type = 0, long long wParam = 0, long long lParam = 0) :
		type(type), wParam(wParam), lParam(lParam)
	{
	}

	virtual ~Message()
	{
	}
};

class MessageQueue : public BlockingQueue <Message*>
{
public:
	~MessageQueue()
	{
		Message* p;
		while (Pop(p, 0) == 0)
			delete p;
	}

	void Push(Message* p)
	{
		BlockingQueue <Message*>::Push(p);
	}

	void Push(int type, long long wParam = 0, long long lParam = 0)
	{
		Push(new Message(type, wParam, lParam));
	}
};

enum THREAD_MSG_RESERVED
{
	TMSG_TIMEOUT = -1,
	TMSG_QUIT = -2
};

class Thread
{
public:
	Thread(MessageQueue* mq = 0) : timeout(-1), quit(false)
	{
		if (mq)
		{
			local_mq = 0;
			this->mq = mq;
		}
		else
		{
			local_mq = new MessageQueue;
			this->mq = local_mq;
		}
		th = new std::thread(ThreadProc, this);
	}

	virtual ~Thread()
	{
		Quit();
		if (local_mq)
			delete local_mq;
	}

	void Quit()
	{
		if (th)
		{
			mq->Push(new Message(TMSG_QUIT));
			th->join();
			delete th;
			th = 0;
		}
	}

	void SetTimeout(int timeout)
	{
		mq->Push(new Message(TMSG_TIMEOUT, timeout));
	}

	int PostMessage(Message* p)
	{
		if (th && p && p->type >= 0)
		{
			mq->Push(p);
			return 0;
		}
		else
		{
			delete p;
			return -1;
		}
	}

	int PostMessage(int type, long long wParam = 0, long long lParam = 0)
	{
		if (th && type >= 0)
		{
			mq->Push(new Message(type, wParam, lParam));
			return 0;
		}
		else
			return -1;
	}

	void SetPriority(int priority)
	{
		if (th)
			SetThreadPriority(th->native_handle(), priority);
	}

protected:
	virtual void OnMessage(Message* p)
	{
	}

	virtual void OnMessage(Message& msg)
	{
	}

	virtual void OnIdle()
	{
	}

	virtual void OnTimeout()
	{
	}

	virtual void OnQuit()
	{
	}

private:
	MessageQueue* local_mq;
	MessageQueue* mq;
	std::thread* th;
	int timeout;
	bool quit;

private:
	static void* ThreadProc(void* p)
	{
		return ((Thread*)p)->Run();
	}

	void ProcessMessage(Message* p)
	{
		if (p->type >= 0)
		{
			OnMessage(p);
			OnMessage(*p);
		}
		else if (p->type == TMSG_TIMEOUT)
			timeout = (int)p->wParam;
		else
			quit = true;
		delete p;
	}

	void* Run()
	{
		while (!quit)
		{
			Message* p = nullptr;
			if (mq->Pop(p, timeout) == 0 && p)
				ProcessMessage(p);
			else
				OnTimeout();
			while (true)
			{
				if (mq->Pop(p, 0) == 0 && p)
					ProcessMessage(p);
				else
				{
					OnIdle();
					if (mq->IsEmpty())
						break;
				}
			}
		}
		OnQuit();
		return 0;
	}
};

THREADXX_END

#endif /* THREADXX_H_ */
