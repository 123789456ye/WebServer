#pragma once
#include <mysql/mysql.h>
#include <string>
#include <queue>
#include <mutex>
#include <semaphore.h>
#include <thread>
#include "../log/log.h"

class SqlConnPool {
public:
    static SqlConnPool *instance();

    MYSQL *get_conn();
    void free_conn(MYSQL * conn);
    int get_free_count();

    void init(const char* host, int port,
              const char* user,const char* pwd, 
              const char* dbName, int connSize);
    void close_pool();

private:
    SqlConnPool() = default;
    ~SqlConnPool() {close_pool();}

    SqlConnPool(const SqlConnPool&) = delete;
    SqlConnPool& operator=(const SqlConnPool&) = delete;
    SqlConnPool(SqlConnPool&&) = delete;
    SqlConnPool& operator=(SqlConnPool&&) = delete;

    int MAX_CONN_ = 0;
    int use_count_ = 0;
    int free_count_ = 0;

    std::queue<MYSQL *> conn_que_;
    std::mutex mtx_;
    std::counting_semaphore<> sem_{0};
};
