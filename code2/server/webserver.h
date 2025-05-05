#pragma once

#include <unordered_map>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <functional>
#include <memory>
#include <string>

#include "epoller.h"
#include "../log/log.h"
#include "../timer/heaptimer.h"
#include "../pool/sqlconnpool.h"
#include "../pool/threadpool.h"
#include "../http/httpconn.h"

class WebServer {
public:
    WebServer(
        int port, int trig_mode, int timeout_ms, bool opt_linger, 
        int sql_port, const char* sql_user, const char* sql_pwd, 
        const char* db_name, int conn_pool_num, int thread_num,
        bool open_log, int log_level, int log_que_size);

    ~WebServer();
    void start();

private:
    bool init_socket(); 
    void init_event_mode(int trig_mode);
    void add_client(int fd, sockaddr_in addr);
  
    void deal_listen();
    void deal_write(HttpConn* client);
    void deal_read(HttpConn* client);

    void send_error(int fd, const char* info);
    void extend_time(HttpConn* client);
    void close_conn(HttpConn* client);

    void on_read(HttpConn* client);
    void on_write(HttpConn* client);
    void on_process(HttpConn* client);

    static const int MAX_FD = 65536;
    static int set_fd_nonblock(int fd);

    int port_;
    bool open_linger_;
    int timeout_ms_;
    bool is_close_;
    int listen_fd_;
    char* src_dir_;
    
    uint32_t listen_event_;
    uint32_t conn_event_;
   
    std::unique_ptr<HeapTimer> timer_;
    std::unique_ptr<ThreadPool> threadpool_;
    std::unique_ptr<Epoller> epoller_;
    std::unordered_map<int, HttpConn> users_;
};
