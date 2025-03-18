#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
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
#include <map>

#include "../lock/locker.h"
#include "../mysql/mysqlConnectPool.h"
#include "../timer/connectTimer.h"
#include "../log/log.h"

class HttpConnection
{
public:
    static const int FILENAME_LEN = 200;
    // 读缓冲区的大小
    static const int READ_BUFFER_SIZE = 2048;
    // 写缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;
    // 请求方法，目前只用到GET和POST
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    // 主状态机的状态，即对请求行、请求头、请求体进行分析
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    // 报文解析的结果
    enum HTTP_CODE
    {
        NO_REQUEST,        // 请求不完整, 跳转主线程继续监测读事件
        GET_REQUEST,       // 获得了完整的HTTP请求, 调用do_request完成请求资源映射
        BAD_REQUEST,       // HTTP请求报文有语法错误, 调用process_write返回错误响应报文
        NO_RESOURCE,       // 请求资源不存在, 调用process_write返回错误响应报文
        FORBIDDEN_REQUEST, // 请求资源被禁止, 调用process_write返回错误响应报文
        FILE_REQUEST,      // 请求资源可以正常访问, 调用process_write返回响应报文
        INTERNAL_ERROR,    // 服务器内部错误
        CLOSED_CONNECTION  // 连接关闭
    };
    // 从状态机的状态
    enum LINE_STATUS
    {
        LINE_OK = 0, // 对http请求行和请求头每行解析成功
        LINE_BAD,    // 对http请求行和请求头每行解析失败
        LINE_OPEN    // 完成http报文解析
    };

public:
    HttpConnection() {}
    ~HttpConnection() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int);
    void closeConnect(bool real_close = true);
    void process();
    bool read();
    bool write();
    sockaddr_in *get_address()
    {
        return &socketAddress;
    }
    void initMysqlUser(MysqlConnectionPool *connPool);
    int failRW;     // 读写失败，调用timer关闭该链接
    int completeRW; // 1为读写完成，0为未完成

private:
    void init();
    HTTP_CODE processRead();
    bool processWrite(HTTP_CODE ret);
    HTTP_CODE parseRequestLine(char *text);
    HTTP_CODE parseHeaders(char *text);
    HTTP_CODE parseContent(char *text);
    HTTP_CODE doRequest();
    char *getLine() { return readBuf + startLine; };
    LINE_STATUS parseLine();
    void unmap();
    bool addResponse(const char *format, ...);
    bool addContent(const char *content);
    bool addStatusLine(int status, const char *title);
    bool addHeaders(int content_length);
    bool addContentType();
    bool addContentLength(int content_length);
    bool addLinger();
    bool addBlankLine();

public:
    static int epollFd;      // epoll文件描述符
    static int connectCount; // client连接数量
    MYSQL *mysql;            // 用户名和密码数据库
    int state;               // 读为0, 写为1

private:
    // socket信息
    int socketFd;              // 套接字文件描述符
    sockaddr_in socketAddress; // 套接字地址

    // 读写缓冲区
    char readBuf[READ_BUFFER_SIZE];   // 读缓冲区
    long readIdx;                     // 指示读缓冲区已经读入的数据长度
    long checkedIdx;                  // 当前正在解析的字符在readBuf中的位置
    int startLine;                    // readBuf中已经解析的字节数
    char writeBuf[WRITE_BUFFER_SIZE]; // 写缓冲区
    int writeIdx;                     // 写缓冲区长度

    // 请求报文的信息
    METHOD method;               // 请求方法
    char realFile[FILENAME_LEN]; // 请求的文件名
    char *url;                   // url
    char *version;               // http版本
    char *host;                  // 域名
    long contentLength;          // 消息体的长度，不包括头部
    bool linger;                 // 是否保持连接
    char *contentString;         // 存储请求体数据
    CHECK_STATE checkState;      // 解析请求报文当前所处的状态

    // 响应报文的信息
    char *fileAddress;         // 被请求文件被mmap到的地址
    struct stat fileStat;      // 被请求文件在本地的状态
    struct iovec fielLovec[2]; // 存储写缓冲区和被请求文件的iovector
    int fielLovecCount;        // iovector的数量
    int cgi;                   // 当前请求是否为POST
    int bytesToSend;           // 剩余发送的数据
    int bytesHaveSend;         // 已发送的数据
    char *rootPath;            // 根目录

    // std::map<std::string, std::string> m_users;
    int epollTrigMode; // epoll触发模式，1边缘触发，0水平触发
};

#endif
