#pragma once
#include "sqlconnpool.h"

class SqlConnRAII {
public:
    SqlConnRAII(MYSQL** sql, SqlConnPool *connpool) {
        assert(connpool);
        *sql = connpool->get_conn();
        sql_ = *sql;
        connpool_ = connpool;
    }
    
    ~SqlConnRAII() {
        if(sql_) { connpool_->free_conn(sql_); }
    }
    
private:
    MYSQL *sql_;
    SqlConnPool* connpool_;
};
