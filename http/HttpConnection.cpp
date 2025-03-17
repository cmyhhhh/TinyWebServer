#include "HttpConnection.h"

#include <mysql/mysql.h>
#include <fstream>

// 定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker usersLock;
std::map<std::string, std::string> users;

void HttpConnection::initMysqlUser(MysqlConnectionPool *connPool)
{
    // 先从连接池中取一个连接
    MYSQL *mysql = NULL;
    MysqlConnectionPoolRAII mysqlcon(&mysql, connPool);

    // 在user表中检索username, passwd数据, 浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    // 从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    // // 返回结果集中的列数
    // int num_fields = mysql_num_fields(result);

    // // 返回所有字段结构的数组
    // MYSQL_FIELD *fields = mysql_fetch_fields(result);

    // 从结果集中获取下一行, 将对应的用户名和密码, 存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        std::string temp1(row[0]);
        std::string temp2(row[1]);
        users[temp1] = temp2;
    }
}

// 从内核事件表删除描socket
void removeSocketFd(int epollfd, int socketfd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, socketfd, 0);
    close(socketfd);
}

// 将事件重置为EPOLLONESHOT
void modEventFd(int epollfd, int socketfd, int ev, int TRIGMode)
{
    epoll_event socketEvent;
    socketEvent.data.fd = socketfd;

    if (1 == TRIGMode)
        socketEvent.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        socketEvent.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, socketfd, &socketEvent);
}

int HttpConnection::connectCount = 0;
int HttpConnection::epollFd = -1;

// 关闭连接, 关闭一个连接, 客户总量减一
void HttpConnection::closeConnect(bool real_close)
{
    if (real_close && (socketFd != -1))
    {
        printf("close %d\n", socketFd);
        removeSocketFd(epollFd, socketFd);
        socketFd = -1;
        connectCount--;
    }
}

// 初始化连接,外部调用初始化套接字地址
void HttpConnection::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode)
{
    socketFd = sockfd;
    socketAddress = addr;

    Utils::addEventFd(epollFd, socketFd, true, epollTrigMode);
    connectCount++;

    // 当浏览器出现连接重置时, 可能是网站根目录出错或
    // http响应格式出错或者访问的文件中内容完全为空
    rootPath = root;
    epollTrigMode = TRIGMode;

    init();
}

// 初始化新接受的连接
// checkState默认为分析请求行状态
void HttpConnection::init()
{
    mysql = NULL;
    bytesToSend = 0;
    bytesHaveSend = 0;
    checkState = CHECK_STATE_REQUESTLINE;
    linger = false;
    method = GET;
    url = 0;
    version = 0;
    contentLength = 0;
    host = 0;
    startLine = 0;
    checkedIdx = 0;
    readIdx = 0;
    writeIdx = 0;
    cgi = 0;
    state = 0;
    timer_flag = 0;
    improv = 0;

    memset(readBuf, '\0', READ_BUFFER_SIZE);
    memset(writeBuf, '\0', WRITE_BUFFER_SIZE);
    memset(realFile, '\0', FILENAME_LEN);
}

// 从状态机, 用于分析出一行内容
// 返回值为行的读取状态, 有LINE_OK,LINE_BAD,LINE_OPEN
HttpConnection::LINE_STATUS HttpConnection::parseLine()
{
    char temp;
    for (; checkedIdx < readIdx; ++checkedIdx)
    {
        temp = readBuf[checkedIdx];
        // 在HTTP报文中, 每一行的数据由\r\n作为结束字符
        // 如果读到\r, 则判断是否是\r\n, 是则返回LINE_OK, 否则返回LINE_BAD
        if (temp == '\r')
        {
            // \r下一个不是\n, 接收不完整, 需要继续接收
            if ((checkedIdx + 1) == readIdx)
                return LINE_OPEN;
            // 符合\r\n, 将\r\n改为\0\0, 返回LINE_OK
            // \0\0目的是为了读取该行数据
            else if (readBuf[checkedIdx + 1] == '\n')
            {
                readBuf[checkedIdx++] = '\0';
                readBuf[checkedIdx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        // 如果读到\n, 则判断是否是\r\n, 是则返回LINE_OK, 否则返回LINE_BAD
        else if (temp == '\n')
        {
            if (checkedIdx > 1 && readBuf[checkedIdx - 1] == '\r')
            {
                readBuf[checkedIdx - 1] = '\0';
                readBuf[checkedIdx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    // 没有找到\r\n, 返回LINE_OPEN继续接收数据
    return LINE_OPEN;
}

// 循环读取客户数据, 直到无数据可读或对方关闭连接
// 非阻塞ET工作模式下, 需要一次性将数据读完
bool HttpConnection::read()
{
    if (readIdx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytesRead = 0;

    // LT读取数据
    if (0 == epollTrigMode)
    {
        bytesRead = recv(socketFd, readBuf + readIdx, READ_BUFFER_SIZE - readIdx, 0);
        readIdx += bytesRead;

        if (bytesRead <= 0)
        {
            return false;
        }

        return true;
    }
    // ET读数据
    else
    {
        while (true)
        {
            bytesRead = recv(socketFd, readBuf + readIdx, READ_BUFFER_SIZE - readIdx, 0);
            if (bytesRead == -1)
            {
                // EAGAIN资源暂时不可用, EAGAIN 和 EWOULDBLOCK 是相同的错误码（值相同）, 只是语义不同。
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytesRead == 0)
            {
                return false;
            }
            readIdx += bytesRead;
        }
        return true;
    }
}

// 解析http请求行, 获得请求方法, 目标url及http版本号
// 例如: GET /index.html HTTP/1.1
HttpConnection::HTTP_CODE HttpConnection::parseRequestLine(char *text)
{
    // 各个部分之间通过\t或空格分隔
    url = strpbrk(text, " \t");
    if (!url)
    {
        return BAD_REQUEST;
    }
    *url++ = '\0';
    // 目前只支持GET和POST
    if (strcasecmp(text, "GET") == 0)
        method = GET;
    else if (strcasecmp(text, "POST") == 0)
    {
        method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    // 去除连续的多余\t或空格分隔, 再去找下一个\t或空格分隔
    url += strspn(url, " \t");
    version = strpbrk(url, " \t");
    if (!version)
        return BAD_REQUEST;
    *version++ = '\0';
    version += strspn(version, " \t");
    // 目前支持HTTP/1.1
    if (strcasecmp(version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    // 检查路由的http://
    if (strncasecmp(url, "http://", 7) == 0)
    {
        url += 7;
        url = strchr(url, '/');
    }
    // 检查路由的https://
    if (strncasecmp(url, "https://", 8) == 0)
    {
        url += 8;
        url = strchr(url, '/');
    }
    // 一般的不会带有上述两种符号, 直接是单独的/或/后面带访问资源
    if (!url || url[0] != '/')
        return BAD_REQUEST;
    // 当url为/时, 显示index.html
    if (strlen(url) == 1)
        strcat(url, "index.html");
    checkState = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

// 解析http请求的一个头部信息
HttpConnection::HTTP_CODE HttpConnection::parseHeaders(char *text)
{
    // 遇到空行, 表示头部字段解析完毕
    if (text[0] == '\0')
    {
        // POST请求有content, 需要继续处理
        if (contentLength != 0)
        {
            checkState = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // GET请求没有content
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        // 跳过空格和\t字符
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        contentLength = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

// 判断http请求是否被完整读入
HttpConnection::HTTP_CODE HttpConnection::parseContent(char *text)
{
    // 判断当前接收到的数据content是否完整
    if (readIdx >= (contentLength + checkedIdx))
    {
        text[contentLength] = '\0';
        // POST请求中最后为输入的用户名和密码
        contentString = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

HttpConnection::HTTP_CODE HttpConnection::processRead()
{
    LINE_STATUS lineStatus = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    // 循环读取每部分数据, 请求行和请求头结尾都有/r/n, 好判断是否解析完成。
    // 但content部分没有/r/n, 所以需要判断是否是content部分, 是则单独处理。
    while ((checkState == CHECK_STATE_CONTENT && lineStatus == LINE_OK) || ((lineStatus = parseLine()) == LINE_OK))
    {
        text = getLine();
        startLine = checkedIdx;
        LOG_INFO("%s", text);
        switch (checkState)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parseRequestLine(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parseHeaders(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return doRequest();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parseContent(text);
            if (ret == GET_REQUEST)
                return doRequest();
            lineStatus = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

// 处理http请求
HttpConnection::HTTP_CODE HttpConnection::doRequest()
{
    strcpy(realFile, rootPath);
    int rootPathLen = strlen(rootPath);
    // 找到URL中的/位置, 以/开头
    const char *p = strrchr(url, '/');

    // 处理cgi, 目前URL有8种情况
    // / 跳转到index.html, 即导航页面
    // /0 跳转到register.html, 即注册页面
    // /1 跳转到login.html, 即登录页面
    // /2CGISQL.cgi 登录校验, 验证成功跳转到welcome.html, 即资源请求成功页面; 否则跳转到loginError.html, 即登录失败页面。
    // /3CGISQL.cgi 注册检测, 验证成功跳转到login.html, 即登录页面; 否则跳转到registerError.html, 即注册失败页面。
    // /5 跳转到picture.html, 即图片请求页面
    // /6 跳转到video.html, 即视频请求页面
    // 其余直接返回对应资源

    // 登录和注册校验功能
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        // 根据标志判断是登录检测还是注册检测
        char flag = url[1];

        // 可以调用CGI文件处理, 但目前没有实现CGI
        // char *urlToRealFile = (char *)malloc(sizeof(char) * 200);
        // strcpy(urlToRealFile, "/");
        // strcat(urlToRealFile, url + 2);
        // strncpy(realFile + rootPathLen, urlToRealFile, FILENAME_LEN - rootPathLen - 1);
        // free(urlToRealFile);

        // 将用户名和密码提取出来
        // user=123&password=123
        char name[100], password[100];
        int i;
        for (i = 5; contentString[i] != '&'; ++i)
            name[i - 5] = contentString[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; contentString[i] != '\0'; ++i, ++j)
            password[j] = contentString[i];
        password[j] = '\0';

        // 如果是注册, 先检测数据库中是否有重名的, 没有重名, 进行增加
        if (*(p + 1) == '3')
        {
            if (users.find(name) == users.end())
            {
                char *sql_insert = (char *)malloc(sizeof(char) * 200);
                strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES('");
                strcat(sql_insert, name);
                strcat(sql_insert, "', '");
                strcat(sql_insert, password);
                strcat(sql_insert, "')");
                usersLock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(std::pair<std::string, std::string>(name, password));
                usersLock.unlock();

                if (!res)
                    strcpy(url, "/login.html");
                else
                    strcpy(url, "/registerError.html");
            }
            else
                strcpy(url, "/registerError.html");
        }
        // 如果是登录, 直接判断
        // 若浏览器端输入的用户名和密码在表中可以查找到, 返回1, 否则返回0
        else
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(url, "/welcome.html");
            else
                strcpy(url, "/loginError.html");
        }
    }

    char *urlToRealFile = (char *)malloc(sizeof(char) * 200);
    switch (*(p + 1))
    {
    case '0':
        strcpy(urlToRealFile, "/register.html");
        break;
    case '1':
        strcpy(urlToRealFile, "/login.html");
        break;
    case '5':
        strcpy(urlToRealFile, "/picture.html");
        break;
    case '6':
        strcpy(urlToRealFile, "/video.html");
        break;
    default:
        strcpy(urlToRealFile, url);
        break;
    }
    strncpy(realFile + rootPathLen, urlToRealFile, strlen(urlToRealFile));
    free(urlToRealFile);

    // 不存在该文件
    if (stat(realFile, &fileStat) < 0)
        return NO_RESOURCE;
    // 其他用户可读, 当前权限不可读
    if (!(fileStat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    // 是否为目录
    if (S_ISDIR(fileStat.st_mode))
        return BAD_REQUEST;

    int fd = open(realFile, O_RDONLY);
    fileAddress = (char *)mmap(0, fileStat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}
void HttpConnection::unmap()
{
    if (fileAddress)
    {
        munmap(fileAddress, fileStat.st_size);
        fileAddress = 0;
    }
}
bool HttpConnection::write()
{
    int temp = 0;

    if (bytesToSend == 0)
    {
        modEventFd(epollFd, socketFd, EPOLLIN, epollTrigMode);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(socketFd, fielLovec, fielLovecCount);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modEventFd(epollFd, socketFd, EPOLLOUT, epollTrigMode);
                return true;
            }
            unmap();
            return false;
        }

        bytesHaveSend += temp;
        bytesToSend -= temp;
        if (bytesHaveSend >= fielLovec[0].iov_len)
        {
            fielLovec[0].iov_len = 0;
            fielLovec[1].iov_base = fileAddress + (bytesHaveSend - writeIdx);
            fielLovec[1].iov_len = bytesToSend;
        }
        else
        {
            fielLovec[0].iov_base = writeBuf + bytesHaveSend;
            fielLovec[0].iov_len = fielLovec[0].iov_len - bytesHaveSend;
        }

        if (bytesToSend <= 0)
        {
            unmap();
            modEventFd(epollFd, socketFd, EPOLLIN, epollTrigMode);

            if (linger)
            {
                init();
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
// 新建响应数据
bool HttpConnection::addResponse(const char *format, ...)
{
    // 写入内容超过写缓冲区
    if (writeIdx >= WRITE_BUFFER_SIZE)
        return false;
    va_list argList;
    va_start(argList, format);
    int len = vsnprintf(writeBuf + writeIdx, WRITE_BUFFER_SIZE - 1 - writeIdx, format, argList);
    // 写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - writeIdx))
    {
        va_end(argList);
        return false;
    }
    writeIdx += len;
    va_end(argList);

    LOG_INFO("request:%s", writeBuf);

    return true;
}
// 新建响应行
bool HttpConnection::addStatusLine(int status, const char *title)
{
    return addResponse("%s %d %s\r\n", "HTTP/1.1", status, title);
}
// 新建响应头
bool HttpConnection::addHeaders(int content_len)
{
    return addContentLength(content_len) && addLinger() &&
           addContentType() && addBlankLine();
}
bool HttpConnection::addContentLength(int content_len)
{
    return addResponse("Content-Length:%d\r\n", content_len);
}
bool HttpConnection::addContentType()
{
    return addResponse("Content-Type:%s\r\n", "text/html");
}
bool HttpConnection::addLinger()
{
    return addResponse("Connection:%s\r\n", (linger == true) ? "keep-alive" : "close");
}
bool HttpConnection::addBlankLine()
{
    return addResponse("%s", "\r\n");
}
bool HttpConnection::addContent(const char *content)
{
    return addResponse("%s", content);
}
bool HttpConnection::processWrite(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        addStatusLine(500, error_500_title);
        addHeaders(strlen(error_500_form));
        if (!addContent(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        addStatusLine(404, error_404_title);
        addHeaders(strlen(error_404_form));
        if (!addContent(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        addStatusLine(403, error_403_title);
        addHeaders(strlen(error_403_form));
        if (!addContent(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        addStatusLine(200, ok_200_title);
        if (fileStat.st_size != 0)
        {
            addHeaders(fileStat.st_size);
            fielLovec[0].iov_base = writeBuf;
            fielLovec[0].iov_len = writeIdx;
            fielLovec[1].iov_base = fileAddress;
            fielLovec[1].iov_len = fileStat.st_size;
            fielLovecCount = 2;
            bytesToSend = writeIdx + fileStat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            addHeaders(strlen(ok_string));
            if (!addContent(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    fielLovec[0].iov_base = writeBuf;
    fielLovec[0].iov_len = writeIdx;
    fielLovecCount = 1;
    bytesToSend = writeIdx;
    return true;
}
void HttpConnection::process()
{
    HTTP_CODE readRet = processRead();
    // 请求不完整, 还需要继续读
    if (readRet == NO_REQUEST)
    {
        LOG_INFO("client(%s) request is incomplete", inet_ntoa(socketAddress.sin_addr));
        modEventFd(epollFd, socketFd, EPOLLIN, epollTrigMode);
        return;
    }
    // http内容被processRead()解析完, web服务器需要处理请求并返回给client
    bool write_ret = processWrite(readRet);
    if (!write_ret)
    {
        closeConnect();
    }
    modEventFd(epollFd, socketFd, EPOLLOUT, epollTrigMode);
}
