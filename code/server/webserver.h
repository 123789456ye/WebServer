#pragma once

#include <unordered_map>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <memory>

#include "../event/eventloopthreadpool.h"      // 新增
#include "../log/log.h"
#include "../timer/heaptimer.h"
#include "../pool/sqlconnpool.h"
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
    
    void handle_listen();
    void handle_write(HttpConn* client);
    void handle_read(HttpConn* client);
    
    void send_error(int fd, const char* info);
    void extend_time(HttpConn* client);
    void close_conn(HttpConn* client);
    
    void on_connection(int fd);
    void on_read(HttpConn* client);
    void on_write(HttpConn* client);
    void on_process(HttpConn* client);

    void handle_cur();

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
    
    // 新增 - 主从Reactor相关
    std::unique_ptr<EventLoop> main_loop_;               // 主事件循环
    std::unique_ptr<EventLoopThreadPool> thread_pool_;   // 事件循环线程池
    std::unique_ptr<Channel> accept_channel_;            // 接受连接的通道
    
    // HTTP连接相关
    std::unique_ptr<HeapTimer> timer_;
    std::unordered_map<int, HttpConn> users_;            // 连接映射表
    std::unordered_map<int, Channel*> client_channels_;  // 客户端通道
    std::unordered_map<int, EventLoop*> client_loops_;
};
