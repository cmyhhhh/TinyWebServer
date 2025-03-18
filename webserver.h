#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/HttpConnection.h"

const int MAX_FD = 65536;           // 最大文件描述符
const int MAX_EVENT_NUMBER = 10000; // 最大事件数
const int CONNECT_TIMEOUT = 5;      // 最小超时单位

class WebServer
{
public:
    WebServer();
    ~WebServer();

    void init(std::string configPath);

    void thread_pool();
    void sql_pool();
    void log_write();
    void trig_mode();
    void eventListen();
    void start();
    void timer(int connfd, struct sockaddr_in clientAddress);
    void adjustTimer(Timer *timer);
    void deleteTimer(Timer *timer, int sockfd);
    bool dealClientConnect();
    bool dealSignal(bool &timeout, bool &stop_server);
    void dealClientRead(int sockfd);
    void dealClientWrite(int sockfd);

public:
    // 基础
    int httpPort;
    char *webRoot; // web文件根目录
    int m_log_write;
    int m_close_log;
    int actorModel;

    int pipeFd[2];
    int epollFd;
    HttpConnection *users;

    // 线程池相关
    ThreadPool<HttpConnection> *threadPool;
    int m_thread_num;

    // epoll_event相关
    epoll_event events[MAX_EVENT_NUMBER];

    int listenFd;
    int lingerTime;
    int epollTrigMode;
    int listenTriggerMode;
    int connectTriggerMode;

    // 定时器相关
    ClientData *usersTimer;
    Utils utils;
};
#endif
