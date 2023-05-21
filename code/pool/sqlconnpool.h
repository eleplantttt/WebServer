
#ifndef SQLCONNPOOL_H
#define SQLCONNPOOL_H

#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <semaphore.h>
#include <thread>
#include "../log/log.h"

class SqlConnPool {
public:
    static SqlConnPool *Instance();             // 提供公共方法  单例模式

    MYSQL *GetConn();   //获取连接
    void FreeConn(MYSQL * conn);    //释放连接，放入池子
    int GetFreeConnCount();         //获取空闲连接的数量

    void Init(const char* host, int port,
              const char* user,const char* pwd, 
              const char* dbName, int connSize);
    void ClosePool();

private:
    SqlConnPool();//单例  私有化构造函数
    ~SqlConnPool();

    int MAX_CONN_;          //最大连接数
    int useCount_;          //当前用户数
    int freeCount_;         //空闲用户数

    std::queue<MYSQL *> connQue_;  //队列
    std::mutex mtx_;                //互斥锁
    sem_t semId_;                   //信号量
};


#endif // SQLCONNPOOL_H