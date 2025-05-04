#include "sqlconnpool.h"

SqlConnPool* SqlConnPool::instance() {
    static SqlConnPool connPool;
    return &connPool;
}

void SqlConnPool::init(const char* host, int port,
            const char* user,const char* pwd, const char* dbName,
            int connSize = 10) {
    assert(connSize > 0);
    for (int i = 0; i < connSize; i++) {
        MYSQL *sql = mysql_init(nullptr);
        if (!sql) {
            LOG_ERROR("MySql init error!");
            assert(sql);
        }
        sql = mysql_real_connect(sql, host,
                                 user, pwd,
                                 dbName, port, nullptr, 0);
        if (!sql) {
            LOG_ERROR("MySql Connect error!");
        }
        conn_que_.push(sql);
    }
    MAX_CONN_ = connSize;
    sem_.~counting_semaphore();
    new (&sem_) std::counting_semaphore<>(MAX_CONN_);
}

MYSQL* SqlConnPool::get_conn() {
    MYSQL *sql = nullptr;
    if(conn_que_.empty()){
        LOG_WARN("SqlConnPool busy!");
        return nullptr;
    }
    sem_.acquire();
    {
        lock_guard<mutex> locker(mtx_);
        sql = conn_que_.front();
        conn_que_.pop();
    }
    return sql;
}

void SqlConnPool::free_conn(MYSQL* sql) {
    assert(sql);
    lock_guard<mutex> locker(mtx_);
    conn_que_.push(sql);
    sem_.release();
}

void SqlConnPool::close_pool() {
    lock_guard<mutex> locker(mtx_);
    while(!conn_que_.empty()) {
        MYSQL* item = conn_que_.front();
        conn_que_.pop();
        mysql_close(item);
    }
    mysql_library_end();        
}

int SqlConnPool::get_free_count() {
    lock_guard<mutex> locker(mtx_);
    return conn_que_.size();
}

