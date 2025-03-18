#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

class Timer;

struct ClientData
{
    sockaddr_in address;
    int sockfd;
    Timer *timer;
};

// 定时器节点
class Timer
{
public:
    Timer() : prev(NULL), next(NULL) {}

public:
    // 超时时间
    time_t expire;

    void (*timerHandler)(ClientData*);
    ClientData *userData;
    Timer *prev;
    Timer *next;
};

// TODO: 链表addTimer时间复杂度为O(n)，使用红黑树使其降为O(log n)
// 定时器双向链表, 按超时时间升序排列
class TimerList
{
public:
    TimerList();
    ~TimerList();

    void addTimer(Timer *timer);
    void adjustTimer(Timer *timer);
    void deleteTimer(Timer *timer);
    void tick();

private:
    Timer *head;
    Timer *tail;
};

class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    // 对文件描述符设置非阻塞
    static int setNonBlocking(int fd);

    // 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    static void addEventFd(int epollfd, int fd, bool one_shot, int TRIGMode);

    // 信号处理函数
    static void sigHandler(int sig);

    // 设置信号函数
    void addSig(int sig, void(handler)(int), bool restart = true);

    // 定时处理任务，重新定时以不断触发SIGALRM信号
    void cleanTimeoutConnect();

    void sendSocketMessage(int connfd, const char *info);

public:
    static int *pipeFd;
    TimerList timerList;
    static int epollFd;
    int TIMESLOT;
};

void timeoutDelete(ClientData *userData);

#endif
