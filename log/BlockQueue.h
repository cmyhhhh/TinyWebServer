/*************************************************************
 *循环数组实现的阻塞队列，m_back = (queueBack + 1) % queueMaxSize;
 *线程安全，每个操作前都要先加互斥锁，操作完后，再解锁
 **************************************************************/

#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include "../lock/locker.h"

template <class T>
class BlockQueue
{
public:
    BlockQueue(int queueMaxSize = 1000) : queueMaxSize(queueMaxSize)
    {
        if (queueMaxSize <= 0)
        {
            exit(-1);
        }

        blockQueueArray = new T[queueMaxSize];
        queueSize = 0;
        queueFront = -1;
        queueBack = -1;
    }

    ~BlockQueue()
    {
        mutex.lock();
        if (blockQueueArray != NULL)
            delete[] blockQueueArray;

        mutex.unlock();
    }

    void clear()
    {
        mutex.lock();
        queueSize = 0;
        queueFront = -1;
        queueBack = -1;
        mutex.unlock();
    }

    // 判断队列是否满了
    bool full()
    {
        mutex.lock();
        if (queueSize >= queueMaxSize)
        {
            mutex.unlock();
            return true;
        }
        mutex.unlock();
        return false;
    }

    // 判断队列是否为空
    bool empty()
    {
        mutex.lock();
        if (0 == queueSize)
        {
            mutex.unlock();
            return true;
        }
        mutex.unlock();
        return false;
    }

    // 返回队首元素
    bool front(T &value)
    {
        mutex.lock();
        if (0 == queueSize)
        {
            mutex.unlock();
            return false;
        }
        value = blockQueueArray[queueFront];
        mutex.unlock();
        return true;
    }

    // 返回队尾元素
    bool back(T &value)
    {
        mutex.lock();
        if (0 == queueSize)
        {
            mutex.unlock();
            return false;
        }
        value = blockQueueArray[queueBack];
        mutex.unlock();
        return true;
    }

    int size()
    {
        int tmp = 0;
        mutex.lock();
        tmp = queueSize;
        mutex.unlock();
        return tmp;
    }

    int maxSize()
    {
        int tmp = 0;
        mutex.lock();
        tmp = queueMaxSize;
        mutex.unlock();
        return tmp;
    }

    // 往队列添加元素，需要将所有使用队列的线程先唤醒
    // 当有元素push进队列,相当于生产者生产了一个元素
    // 若当前没有线程等待条件变量,则唤醒无意义
    bool push(const T &item)
    {

        mutex.lock();
        if (queueSize >= queueMaxSize)
        {
            condMutex.broadcast();
            mutex.unlock();
            return false;
        }

        queueBack = (queueBack + 1) % queueMaxSize;
        blockQueueArray[queueBack] = item;

        queueSize++;

        condMutex.broadcast();
        mutex.unlock();
        return true;
    }

    // pop时,如果当前队列没有元素,将会等待条件变量
    bool pop(T &item)
    {

        mutex.lock();
        while (queueSize <= 0)
        {
            if (!condMutex.wait(mutex.get()))
            {
                mutex.unlock();
                return false;
            }
        }
        queueFront = (queueFront + 1) % queueMaxSize;
        item = blockQueueArray[queueFront];
        queueSize--;
        mutex.unlock();
        return true;
    }

    // 增加了超时处理
    bool pop(T &item, int ms_timeout)
    {
        struct timespec t = {0, 0};
        struct timeval now = {0, 0};
        gettimeofday(&now, NULL);
        mutex.lock();
        if (queueSize <= 0)
        {
            t.tv_sec = now.tv_sec + ms_timeout / 1000;
            t.tv_nsec = (ms_timeout % 1000) * 1000;
            if (!condMutex.timewait(mutex.get(), t))
            {
                mutex.unlock();
                return false;
            }
        }

        if (queueSize <= 0)
        {
            mutex.unlock();
            return false;
        }

        queueFront = (queueFront + 1) % queueMaxSize;
        item = blockQueueArray[queueFront];
        queueSize--;
        mutex.unlock();
        return true;
    }

private:
    locker mutex;
    cond condMutex;

    T *blockQueueArray;
    int queueSize;
    int queueMaxSize;
    int queueFront;
    int queueBack;
};

#endif
