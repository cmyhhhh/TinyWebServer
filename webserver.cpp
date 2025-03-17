#include "webserver.h"
#include "mysql/mysqlConnectPool.h"

WebServer::WebServer()
{
    // http_conn类对象
    users = new HttpConnection[MAX_FD];

    // root文件夹路径
    char server_path[200];
    getcwd(server_path, 200);
    char root[6] = "/root";
    webRoot = (char *)malloc(strlen(server_path) + strlen(root) + 1);
    strcpy(webRoot, server_path);
    strcat(webRoot, root);

    // 定时器
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    MysqlConnectionPool::getInstance()->ClosePool();
    close(epollFd);
    close(listenFd);
    close(pipeFd[1]);
    close(pipeFd[0]);
    delete[] users;
    delete[] users_timer;
    delete threadPool;
}

void WebServer::init(int port, std::string configPath, int log_write,
                     int opt_linger, int trigmode, int sql_num,
                     int thread_num, int close_log, int actor_model)
{
    // 初始化日志模块
    if (!Log::getInstance()->init(configPath))
    {
        std::cout << "log init failed!" << std::endl;
    }

    // 初始化web服务器配置
    try
    {
        boost::property_tree::ptree pt;
        boost::property_tree::ini_parser::read_ini(configPath, pt);
        listenTriggerMode = pt.get<int>("Web.ListenTriggerMode");
        connectTriggerMode = pt.get<int>("Web.ConnectTriggerMode");
        httpPort = pt.get<int>("Web.Port");
        lingerTime = pt.get<int>("Web.LingerTime");
        actorModel = pt.get<int>("Web.ActorModel");
    }
    catch (std::exception &e)
    {
        LOG_ERROR("%s", "Web init failed!");
    }

    // 初始化数据库连接池
    if (!MysqlConnectionPool::getInstance()->Init(configPath))
    {
        LOG_ERROR("%s", "mysql connection pool init failed!");
    }

    // 初始化线程池
    threadPool = new ThreadPool<HttpConnection>(configPath, actorModel);

    eventListen();
}

void WebServer::eventListen()
{
    // 创建socket用于监听是否有client连接
    listenFd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenFd < 0)
    {
        LOG_ERROR("%s", "listen socket error");
        return;
    }

    // 优雅关闭连接
    struct linger tmp = {0, 0};
    if (lingerTime > 0)
    {
        tmp.l_onoff = 1;
        tmp.l_linger = lingerTime;
    }
    setsockopt(listenFd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    // htonl:host byte order to network byte order long类型
    // htons:host byte order to network byte order short类型
    // 网络字节序是大端序
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(httpPort);

    int flag = 1;
    // 允许绑定到正在被 TIME_WAIT 状态占用的地址和端口
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    Utils::setNonBlocking(listenFd);
    ret = bind(listenFd, (struct sockaddr *)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(listenFd, 5);
    assert(ret >= 0);

    utils.init(TIMESLOT);

    // epoll创建内核事件表
    epoll_event events[MAX_EVENT_NUMBER];
    epollFd = epoll_create(5);
    assert(epollFd != -1);

    Utils::addEventFd(epollFd, listenFd, false, listenTriggerMode);
    HttpConnection::epollFd = epollFd;

    // 创建一对已连接的套接字。这两个套接字彼此连接，可以用于进程间通信（IPC）
    // pipe是半双工，而socket是双工的
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipeFd);
    assert(ret != -1);
    Utils::setNonBlocking(pipeFd[1]);
    Utils::addEventFd(epollFd, pipeFd[0], false, 0);

    utils.addsig(SIGPIPE, SIG_IGN);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);

    // 工具类,信号和描述符基础操作
    Utils::u_pipefd = pipeFd;
    Utils::u_epollfd = epollFd;
}

void WebServer::timer(int connfd, struct sockaddr_in clientAddress)
{
    MysqlConnectionPool *connPool = MysqlConnectionPool::getInstance();
    users[connfd].init(connfd, clientAddress, webRoot, connectTriggerMode);

    // 初始化client_data数据
    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
    users_timer[connfd].address = clientAddress;
    users_timer[connfd].sockfd = connfd;
    util_timer *timer = new util_timer;
    timer->user_data = &users_timer[connfd];
    timer->cb_func = cb_func;
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[connfd].timer = timer;
    utils.m_timer_lst.add_timer(timer);
}

// 若有数据传输，则将定时器往后延迟3个单位
// 并对新的定时器在链表上的位置进行调整
void WebServer::adjust_timer(util_timer *timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    LOG_INFO("%s", "adjust timer once");
}

void WebServer::deal_timer(util_timer *timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);
    if (timer)
    {
        utils.m_timer_lst.del_timer(timer);
    }

    LOG_INFO("close fd %d", users_timer[sockfd].sockfd);
}

bool WebServer::dealClientConnect()
{
    struct sockaddr_in clientAddress;
    socklen_t clientAddrlength = sizeof(clientAddress);

    while (1)
    {
        int connectFd = accept(listenFd, (struct sockaddr *)&clientAddress, &clientAddrlength);
        if (connectFd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            else
            {
                LOG_ERROR("accept error from %s:%d, errno is %d",
                          inet_ntoa(clientAddress.sin_addr), ntohs(clientAddress.sin_port), errno);
            }
        }
        if (HttpConnection::connectCount >= MAX_FD)
        {
            utils.show_error(connectFd, "too many clients");
            LOG_ERROR("%s", "Internal server busy");
            break;
        }
        timer(connectFd, clientAddress);
    }
}

bool WebServer::dealwithsignal(bool &timeout, bool &stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(pipeFd[0], signals, sizeof(signals), 0);
    if (ret == -1)
    {
        return false;
    }
    else if (ret == 0)
    {
        return false;
    }
    else
    {
        for (int i = 0; i < ret; ++i)
        {
            switch (signals[i])
            {
            case SIGALRM:
            {
                timeout = true;
                break;
            }
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            }
        }
    }
    return true;
}

void WebServer::dealClientRead(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;

    // reactor
    if (1 == actorModel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        // 若监测到读事件，将该事件放入请求队列
        threadPool->append(users + sockfd, 0);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        // proactor
        if (users[sockfd].read())
        {
            LOG_INFO("deal with the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            // 若监测到读事件，将该事件放入请求队列
            threadPool->append(users + sockfd);

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::dealClientWrite(int sockfd)
{
    util_timer *timer = users_timer[sockfd].timer;
    // reactor
    if (1 == actorModel)
    {
        if (timer)
        {
            adjust_timer(timer);
        }

        threadPool->append(users + sockfd, 1);

        while (true)
        {
            if (1 == users[sockfd].improv)
            {
                if (1 == users[sockfd].timer_flag)
                {
                    deal_timer(timer, sockfd);
                    users[sockfd].timer_flag = 0;
                }
                users[sockfd].improv = 0;
                break;
            }
        }
    }
    else
    {
        // proactor
        if (users[sockfd].write())
        {
            LOG_INFO("send data to the client(%s)", inet_ntoa(users[sockfd].get_address()->sin_addr));

            if (timer)
            {
                adjust_timer(timer);
            }
        }
        else
        {
            deal_timer(timer, sockfd);
        }
    }
}

void WebServer::start()
{
    bool timeout = false;
    bool stop_server = false;

    while (!stop_server)
    {
        // timeout 为 -1，epoll_wait 会一直阻塞
        int number = epoll_wait(epollFd, events, MAX_EVENT_NUMBER, -1);
        if (number < 0 && errno != EINTR)
        {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        for (int i = 0; i < number; i++)
        {
            int sockfd = events[i].data.fd;

            // 处理新到的客户连接
            if (sockfd == listenFd)
            {
                dealClientConnect();
            }
            // 服务器端关闭连接
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 移除对应的定时器
                util_timer *timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            // 处理信号
            else if ((sockfd == pipeFd[0]) && (events[i].events & EPOLLIN))
            {
                bool flag = dealwithsignal(timeout, stop_server);
                if (false == flag)
                    LOG_ERROR("%s", "dealClientConnect failure");
            }
            // 处理客户连接上接收到的数据
            else if (events[i].events & EPOLLIN)
            {
                dealClientRead(sockfd);
            }
            else if (events[i].events & EPOLLOUT)
            {
                dealClientWrite(sockfd);
            }
        }
        if (timeout)
        {
            utils.timer_handler();

            LOG_INFO("%s", "timer tick");

            timeout = false;
        }
    }
}
