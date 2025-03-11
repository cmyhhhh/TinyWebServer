#ifndef CONFIG_MYSQL_POOL
#define CONFIG_MYSQL_POOL

#include <string>
#include <mysql/mysql.h>
#include <queue>
#include <mutex>
#include <semaphore.h>
#include <thread>

class MysqlConnectionPool
{
private:
    MysqlConnectionPool();
    ~MysqlConnectionPool();
    std::queue<MYSQL *> connList;
    std::mutex mtx;
    unsigned int useCount;
    unsigned int freeCount;
    sem_t semId;
    std::string user;
    std::string passwd;
    std::string databaseName;

public:
    static MysqlConnectionPool *getInstance();
    bool Init(const std::string &mysqlConfigPath);
    MYSQL *getConnection();
    bool freeConnection(MYSQL *con);
    void ClosePool();
    std::string getUser();
    std::string getPasswd();
    std::string getDatabaseName();
};

class MysqlConnectionPoolRAII
{
public:
    MysqlConnectionPoolRAII(MYSQL **sql, MysqlConnectionPool *connpool)
    {
        *sql = connpool->getConnection();
        sql_ = *sql;
        connpool_ = connpool;
    }

    ~MysqlConnectionPoolRAII()
    {
        if (sql_)
        {
            connpool_->freeConnection(sql_);
        }
    }

private:
    MYSQL *sql_;
    MysqlConnectionPool *connpool_;
};

#endif