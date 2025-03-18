#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <list>
#include <cstdio>
#include <exception>
#include <pthread.h>
#include "../lock/locker.h"
#include "../mysql/mysqlConnectPool.h"

template <typename T>
class ThreadPool
{
public:
    ThreadPool(const std::string &configPath, int actorModel);
    ~ThreadPool();
    bool append(T *request, int state = 0);

private:
    /*工作线程运行的函数，它不断从工作队列中取出任务并执行之*/
    static void *worker(void *arg);
    void run();

private:
    unsigned int threadNumber; // 线程池中的线程数
    unsigned int maxRequests;  // 请求队列中允许的最大请求数
    pthread_t *threads;        // 描述线程池的数组，其大小为threadNumber
    std::queue<T *> workqueue; // 请求队列
    locker workqueueLocker;    // 保护请求队列的互斥锁
    sem taskNumSem;            // 是否有任务需要处理
    int actorModel;            // 模型切换
};

template <typename T>
ThreadPool<T>::ThreadPool(const std::string &configPath, int actorModel) : actorModel(actorModel)
{
    try
    {
        boost::property_tree::ptree pt;
        boost::property_tree::ini_parser::read_ini(configPath, pt);
        threadNumber = pt.get<unsigned int>("ThreadPool.ThreadNumber");
        maxRequests = pt.get<unsigned int>("ThreadPool.MaxRequests");
        if (threadNumber <= 0 || maxRequests <= 0)
            throw std::exception();
        threads = new pthread_t[threadNumber];
        if (!threads)
            throw std::exception();
        for (int i = 0; i < threadNumber; i++)
        {
            if (pthread_create(threads + i, NULL, worker, this) != 0)
            {
                delete[] threads;
                throw std::exception();
            }
            if (pthread_detach(threads[i]))
            {
                delete[] threads;
                throw std::exception();
            }
        }
    }
    catch (std::exception &e)
    {
        std::cout << e.what() << std::endl;
    }
}

template <typename T>
ThreadPool<T>::~ThreadPool()
{
    delete[] threads;
}

template <typename T>
bool ThreadPool<T>::append(T *request, int state)
{
    workqueueLocker.lock();
    if (workqueue.size() >= maxRequests)
    {
        workqueueLocker.unlock();
        return false;
    }
    request->state = state;
    workqueue.push(request);
    workqueueLocker.unlock();
    taskNumSem.post();
    return true;
}

template <typename T>
void *ThreadPool<T>::worker(void *arg)
{
    ThreadPool *pool = (ThreadPool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void ThreadPool<T>::run()
{
    while (true)
    {
        taskNumSem.wait();
        workqueueLocker.lock();
        if (workqueue.empty())
        {
            workqueueLocker.unlock();
            continue;
        }
        T *request = std::move(workqueue.front());
        workqueue.pop();
        workqueueLocker.unlock();
        if (!request)
            continue;
        if (1 == actorModel)
        {
            if (0 == request->state)
            {
                if (request->read())
                {
                    request->completeRW = 1;
                    MysqlConnectionPoolRAII mysqlcon(&request->mysql, MysqlConnectionPool::getInstance());
                    request->process();
                }
                else
                {
                    request->completeRW = 1;
                    request->failRW = 1;
                }
            }
            else
            {
                if (request->write())
                {
                    request->completeRW = 1;
                }
                else
                {
                    request->completeRW = 1;
                    request->failRW = 1;
                }
            }
        }
        else
        {
            MysqlConnectionPoolRAII mysqlcon(&request->mysql, MysqlConnectionPool::getInstance());
            request->process();
        }
    }
}
#endif
