#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <iostream>
#include <string>
#include <stdarg.h>
#include <pthread.h>
#include "BlockQueue.h"


class Log
{
public:
    // C++11以后,使用局部变量懒汉不用加锁
    static Log *getInstance();

    // 创建异步线程写日志
    static void *flushLogThread(void *args);

    bool init(const std::string &logConfigPath);

    void writeLog(int level, const char *format, ...);

    void flush(void);

    bool getLogEnable();

private:
    Log();
    virtual ~Log();
    void *asyncWriteLog();

    char logPath[128];             // 路径名
    char logName[128];             // log文件名
    int logSingleFileLine;         // 日志最大行数
    int logBufferSize;             // 日志缓冲区大小
    long long count;               // 日志行数记录
    int today;                     // 因为按天分类,记录当前时间是那一天
    FILE *logFile;                 // 打开log的文件指针
    char *logBuffer;               // 日志缓冲区
    BlockQueue<std::string> *logQueue; // 阻塞队列
    bool isAsync;                  // 是否同步标志位
    locker logMutex;
    int logEnable; // 开启日志

    // 第二种单例懒汉模式实现方式
    // static Log *logInstance;
    // static pthread_mutex_t mutexInstance;
};

#define LOG_DEBUG(format, ...)                                   \
    if (true == Log::getInstance()->getLogEnable())                                          \
    {                                                            \
        Log::getInstance()->writeLog(0, format, ##__VA_ARGS__); \
        Log::getInstance()->flush();                             \
    }
#define LOG_INFO(format, ...)                                    \
    if (true == Log::getInstance()->getLogEnable())                                          \
    {                                                            \
        Log::getInstance()->writeLog(1, format, ##__VA_ARGS__); \
        Log::getInstance()->flush();                             \
    }
#define LOG_WARN(format, ...)                                    \
    if (true == Log::getInstance()->getLogEnable())                                          \
    {                                                            \
        Log::getInstance()->writeLog(2, format, ##__VA_ARGS__); \
        Log::getInstance()->flush();                             \
    }
#define LOG_ERROR(format, ...)                                   \
    if (true == Log::getInstance()->getLogEnable())                                          \
    {                                                            \
        Log::getInstance()->writeLog(3, format, ##__VA_ARGS__); \
        Log::getInstance()->flush();                             \
    }

#endif
