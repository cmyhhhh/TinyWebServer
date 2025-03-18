#include "connectTimer.h"
#include "../http/HttpConnection.h"

TimerList::TimerList()
{
    head = new Timer();
    tail = new Timer();
    head->next = tail;
    tail->prev = head;
}
TimerList::~TimerList()
{
    while (head->next)
    {
        head = head->next;
        delete head->prev;
    }
    delete head;
}

// 添加定时器
void TimerList::addTimer(Timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (head->next == tail)
    {
        head->next = timer;
        tail->prev = timer;
        timer->prev = head;
        timer->next = tail;
        return;
    }
    Timer *cur = head->next;
    while (cur != tail && timer->expire > cur->expire)
    {
        cur = cur->next;
    }
    timer->prev = cur->prev;
    cur->prev->next = timer;
    timer->next = cur;
    cur->prev = timer;
    return;
}

// 任务发生变化时, 调整定时器
void TimerList::adjustTimer(Timer *timer)
{
    if (!timer)
    {
        return;
    }
    // 在链表尾或超时值仍小于后一个节点不调整
    if (timer->next == tail || (timer->expire < timer->next->expire))
    {
        return;
    }

    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    addTimer(timer);
}
// 删除定时器
void TimerList::deleteTimer(Timer *timer)
{
    if (!timer)
    {
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

// 删除超时事件
void TimerList::tick()
{
    if (head->next == tail)
    {
        return;
    }

    time_t cur = time(NULL);
    Timer *curTimer = head->next;
    while (curTimer!=tail)
    {
        if (cur < curTimer->expire)
        {
            break;
        }
        curTimer->timerHandler(curTimer->userData);
        head->next = curTimer->next;
        delete curTimer;
        curTimer = head->next;
    }
}

void Utils::init(int timeslot)
{
    TIMESLOT = timeslot;
}

// 对文件描述符设置非阻塞
int Utils::setNonBlocking(int socketfd)
{
    int flag = fcntl(socketfd, F_GETFL);
    if (flag == -1)
        return -1;
    flag |= O_NONBLOCK;
    if (fcntl(socketfd, F_SETFL, flag) == -1)
        return -1;
    return 0;
}

// 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addEventFd(int epollfd, int socketfd, bool oneshot, int mode)
{
    epoll_event socketEvent;
    socketEvent.data.fd = socketfd;

    if (1 == mode)
        // 边缘触发，监控 数据输入 和 远程挂断或连接关闭
        socketEvent.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        // 默认水平触发，监控 数据输入 和 远程挂断或连接关闭
        socketEvent.events = EPOLLIN | EPOLLRDHUP;

    // socket数据可能未处理完，有新数据到，导致该事件被不同线程再次触发。
    // 开启EPOLLONESHOT，确保每个socket连接被处理时，只处理一次。
    // 若想要再次处理，需要重新使用EPOLL_CTL_MOD重新配置文件描述符.
    // EPOLLONESHOT会挂起
    if (oneshot)
        socketEvent.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, socketfd, &socketEvent);
    setNonBlocking(socketfd);
}

// 信号处理函数, 只给webserver发送信号, 不进行处理, 避免信号被屏蔽太久
// 不调用非可重入函数, 不依赖全局变量, 恢复全局状态
void Utils::sigHandler(int sig)
{
    // 为保证函数的可重入性，保留原来的errno(全局变量)
    int save_errno = errno;
    int msg = sig;
    send(pipeFd[1], (char *)&msg, 1, 0);
    errno = save_errno;
}

// 设置信号函数
void Utils::addSig(int sig, void(handler)(int), bool restart)
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::cleanTimeoutConnect()
{
    timerList.tick();
    alarm(TIMESLOT);
}

void Utils::sendSocketMessage(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int *Utils::pipeFd = 0;
int Utils::epollFd = 0;

class Utils;
// 删除超时http连接
void timeoutDelete(ClientData* userData)
{
    epoll_ctl(Utils::epollFd, EPOLL_CTL_DEL, userData->sockfd, 0);
    assert(userData);
    close(userData->sockfd);
    HttpConnection::connectCount--;
}
