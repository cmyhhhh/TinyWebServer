#include "log.h"
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>

Log::Log()
{
    count = 0;
    isAsync = false;
}

Log::~Log()
{
    if (logFile != NULL)
    {
        fclose(logFile);
    }
}
// 异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const std::string &logConfigPath)
{
    try
    {
        boost::property_tree::ptree pt;
        boost::property_tree::ini_parser::read_ini(logConfigPath, pt);
        std::strncpy(logName, pt.get<std::string>("Log.LogName").c_str(), sizeof(logName) - 1);
        std::strncpy(logPath, pt.get<std::string>("Log.LogPath").c_str(), sizeof(logPath) - 1);
        logBufferSize = pt.get<int>("Log.LogBufferSize");
        logSingleFileLine = pt.get<int>("Log.LogSingleFileLine");
        logEnable = pt.get<int>("Log.LogEnable");
        int logQueueSize = pt.get<int>("Log.LogQueueSize");

        // 如果设置了logQueueSize,则设置为异步
        if (logQueueSize > 0)
        {
            isAsync = true;
            logQueue = new BlockQueue<std::string>(logQueueSize);
            pthread_t tid;
            // flushLogThread为回调函数,这里表示创建线程异步写日志
            pthread_create(&tid, NULL, flushLogThread, NULL);
        }

        logBuffer = new char[logBufferSize];
        memset(logBuffer, 0, logBufferSize);

        time_t t = time(NULL);
        struct tm currentTime = *localtime(&t);

        char logRealPath[256] = {0};
        int len = std::strlen(logPath);
        if (len > 0 && logPath[len - 1] == '/')
        {
            // 替换末尾的 '/' 为 '\0'
            logPath[len - 1] = '\0';
        }

        snprintf(logRealPath, 255, "%s/%d_%02d_%02d_%s.log", logPath, currentTime.tm_year + 1900, currentTime.tm_mon + 1, currentTime.tm_mday, logName);

        today = currentTime.tm_mday;
        logFile = fopen(logRealPath, "a");
        if (logFile == NULL)
        {
            return false;
        }

        return true;
    }
    catch (std::exception &e)
    {
        std::cout << "Error: " << e.what() << std::endl;
    }
}

Log *Log::getInstance()
{
    // C++11要求编译器保证内部静态变量的线程安全性,所以不需要加锁
    // 如果使用C++11之前的标准，还是需要加锁
    static Log logInstance;
    return &logInstance;

    // 双检测锁模式
    // 如果只检测一次，在每次调用获取实例的方法时，都需要加锁
    // if(logInstance == NULL){
    //     pthread_mutex_lock(&mutexInstance);
    //     if(logInstance == NULL){
    //         logInstance = new Log();
    //     }
    //     pthread_mutex_unlock(&mutexInstance);
    // }
    // return logInstance;
}

void Log::writeLog(int level, const char *format, ...)
{
    struct timeval now = {0, 0};
    gettimeofday(&now, NULL);
    struct tm currentTime = *localtime(&now.tv_sec);
    char s[16] = {0};
    switch (level)
    {
    case 0:
        strcpy(s, "[debug]:");
        break;
    case 1:
        strcpy(s, "[info]:");
        break;
    case 2:
        strcpy(s, "[warn]:");
        break;
    case 3:
        strcpy(s, "[erro]:");
        break;
    default:
        strcpy(s, "[info]:");
        break;
    }

    logMutex.lock();
    count++;

    if (today != currentTime.tm_mday || count % logSingleFileLine == 0) // everyday log
    {
        char newLog[256] = {0};
        fflush(logFile);
        fclose(logFile);
        char tail[16] = {0};

        snprintf(tail, 16, "%d_%02d_%02d_", currentTime.tm_year + 1900, currentTime.tm_mon + 1, currentTime.tm_mday);

        if (today != currentTime.tm_mday)
        {
            snprintf(newLog, 255, "%s%s%s", logPath, tail, logName);
            today = currentTime.tm_mday;
            count = 0;
        }
        else
        {
            snprintf(newLog, 255, "%s%s%s.%lld", logPath, tail, logName, count / logSingleFileLine);
        }
        logFile = fopen(newLog, "a");
    }

    logMutex.unlock();

    va_list valst;
    va_start(valst, format);

    std::string logStr;
    logMutex.lock();

    // 写入的具体时间内容格式
    int n = snprintf(logBuffer, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ",
                     currentTime.tm_year + 1900, currentTime.tm_mon + 1, currentTime.tm_mday,
                     currentTime.tm_hour, currentTime.tm_min, currentTime.tm_sec, now.tv_usec, s);

    int m = vsnprintf(logBuffer + n, logBufferSize - n - 1, format, valst);
    logBuffer[n + m] = '\n';
    logBuffer[n + m + 1] = '\0';
    logStr = logBuffer;

    logMutex.unlock();

    if (isAsync && !logQueue->full())
    {
        logQueue->push(logStr);
    }
    else
    {
        logMutex.lock();
        fputs(logStr.c_str(), logFile);
        logMutex.unlock();
    }

    va_end(valst);
}

void Log::flush(void)
{
    logMutex.lock();
    // 强制刷新写入流缓冲区
    fflush(logFile);
    logMutex.unlock();
}

void *Log::flushLogThread(void *args)
{
    Log::getInstance()->asyncWriteLog();
}

void *Log::asyncWriteLog()
{
    std::string log;
    while (logQueue->pop(log))
    {
        fputs(log.c_str(), logFile);
    }
}

bool Log::getLogEnable()
{
    return logEnable;
}