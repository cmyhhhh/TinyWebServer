#include "mysqlConnectPool.h"

MysqlConnectionPool::MysqlConnectionPool()
{
    useCount = 0;
    freeCount = 0;
}

MysqlConnectionPool *MysqlConnectionPool::getInstance()
{
    static MysqlConnectionPool connPool;
    return &connPool;
}

bool MysqlConnectionPool::Init(const std::string &mysqlConfigPath)
{
    try
    {
        boost::property_tree::ptree pt;
        boost::property_tree::ini_parser::read_ini(mysqlConfigPath, pt);
        user = pt.get<std::string>("Mysql.User");
        passwd = pt.get<std::string>("Mysql.Password");
        databaseName = pt.get<std::string>("Mysql.Database");
        std::string host = pt.get<std::string>("Mysql.Host");
        unsigned int port = pt.get<unsigned int>("Mysql.Port");
        unsigned int maxConnetion = pt.get<unsigned int>("Mysql.MaxConnections");
        unsigned int clientflag = pt.get<unsigned int>("Mysql.ClientFlags");
        if (mysql_library_init(0, NULL, NULL))
        {
            return false;
        }
        for (int i = 0; i < maxConnetion; i++)
        {
            MYSQL *con = mysql_init(nullptr);
            if (con == nullptr)
            {
                throw std::runtime_error("mysql_init error");
            }
            con = mysql_real_connect(con, host.c_str(), user.c_str(), passwd.c_str(), databaseName.c_str(), port, nullptr, clientflag);
            if (con == nullptr)
            {
                throw std::runtime_error("mysql_real_connect error");
            }
            connList.push(con);
        }
        freeCount = maxConnetion;
        sem_init(&semId, 0, maxConnetion);
    }
    catch (std::exception &e)
    {
        std::cout << e.what() << std::endl;
        return false;
    }
    return true;
}

MYSQL *MysqlConnectionPool::getConnection()
{
    MYSQL *con = nullptr;
    sem_wait(&semId);
    mtx.lock();
    if (connList.size() > 0)
    {
        con = connList.front();
        connList.pop();
        useCount++;
        freeCount--;
    }
    mtx.unlock();
    return con;
}

bool MysqlConnectionPool::freeConnection(MYSQL *con)
{
    std::lock_guard<std::mutex> lock(mtx);
    if (con != nullptr)
    {
        connList.push(con);
        useCount--;
        freeCount++;
        sem_post(&semId);
        return true;
    }
    return false;
}

void MysqlConnectionPool::ClosePool()
{
    mtx.lock();
    while (!connList.empty())
    {
        MYSQL *con = connList.front();
        connList.pop();
        mysql_close(con);
    }
    freeCount = 0;
    useCount = 0;
    mtx.unlock();
    mysql_library_end();
    sem_destroy(&semId);
}

std::string MysqlConnectionPool::getUser()
{
    return user;
}

std::string MysqlConnectionPool::getPasswd()
{
    return passwd;
}

std::string MysqlConnectionPool::getDatabaseName()
{
    return databaseName;
}

MysqlConnectionPool::~MysqlConnectionPool()
{
    ClosePool();
}