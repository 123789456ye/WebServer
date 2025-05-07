// webserver.cpp
#include "webserver.h"
#include <cassert>
#include <cstring>

WebServer::WebServer(
        int port, int trig_mode, int timeout_ms, bool opt_linger,
        int sql_port, const char* sql_user, const char* sql_pwd,
        const char* db_name, int conn_pool_num, int thread_num,
        bool open_log, int log_level, int log_que_size)
    : port_(port), open_linger_(opt_linger), timeout_ms_(timeout_ms), is_close_(false),
      listen_fd_(-1), main_loop_(new EventLoop()), timer_(new HeapTimer()) {
    
    // 获取资源目录
    src_dir_ = getcwd(nullptr, 256);
    assert(src_dir_);
    strncat(src_dir_, "/resources/", 16);
    
    // 初始化HTTP静态成员
    HttpConn::user_count = 0;
    HttpConn::src_dir = src_dir_;
    
    // 初始化数据库连接池
    SqlConnPool::instance()->init("localhost", sql_port, sql_user, sql_pwd, db_name, conn_pool_num);
    
    // 初始化事件模式
    init_event_mode(trig_mode);

    // 初始化Socket
    if(!init_socket()) { 
        is_close_ = true; 
        return;
    }
    
    // 初始化主从Reactor模式的线程池
    thread_pool_.reset(new EventLoopThreadPool(main_loop_.get(), thread_num));
    thread_pool_->start();
    
    // 初始化日志
    if(open_log) {
        Log::instance()->init(log_level, "./log", ".log", log_que_size);
        if(is_close_) { 
            LOG_ERROR("========== Server init error!=========="); 
        }
        else {
            LOG_INFO("========== Server init ==========");
            LOG_INFO("Port:%d, OpenLinger: %s", port_, opt_linger ? "true":"false");
            LOG_INFO("Listen Mode: %s, Connection Mode: %s",
                        (listen_event_ & EPOLLET ? "ET": "LT"),
                        (conn_event_ & EPOLLET ? "ET": "LT"));
            LOG_INFO("LogSys level: %d", log_level);
            LOG_INFO("srcDir: %s", HttpConn::src_dir);
            LOG_INFO("SqlConnPool num: %d, ThreadPool num: %d", conn_pool_num, thread_num);
        }
    }
}

WebServer::~WebServer() {
    close(listen_fd_);
    is_close_ = true;
    free(src_dir_);
    SqlConnPool::instance()->close_pool();
}

void WebServer::init_event_mode(int trig_mode) {
    listen_event_ = EPOLLRDHUP;
    conn_event_ = EPOLLONESHOT | EPOLLRDHUP;
    if(trig_mode & 1) conn_event_ |= EPOLLET;
    if(trig_mode & 2) listen_event_ |= EPOLLET;

    HttpConn::is_et = (conn_event_ & EPOLLET);
}

void WebServer::start() {
    if(is_close_) { 
        return; 
    }
    
    LOG_INFO("========== Server start ==========");
    main_loop_->loop(timer_.get(), timeout_ms_);
}

bool WebServer::init_socket() {
    int ret;
    struct sockaddr_in addr;
    
    if(port_ > 65535 || port_ < 1024) {
        LOG_ERROR("Port:%d error!", port_);
        return false;
    }
    
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);
    
    struct linger opt_linger = {0};
    if(open_linger_) {
        opt_linger.l_linger = 1;
        opt_linger.l_onoff = 1;
    }

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if(listen_fd_ < 0) {
        LOG_ERROR("Create socket error!");
        return false;
    }

    ret = setsockopt(listen_fd_, SOL_SOCKET, SO_LINGER, &opt_linger, sizeof(opt_linger));
    if(ret < 0) {
        close(listen_fd_);
        LOG_ERROR("Init linger error!");
        return false;
    }

    int optval = 1;
    ret = setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, (const void*)&optval, sizeof(int));
    if(ret == -1) {
        LOG_ERROR("set socket setsockopt error!");
        close(listen_fd_);
        return false;
    }

    ret = bind(listen_fd_, (struct sockaddr *)&addr, sizeof(addr));
    if(ret < 0) {
        LOG_ERROR("Bind Port:%d error!", port_);
        close(listen_fd_);
        return false;
    }

    ret = listen(listen_fd_, 6);
    if(ret < 0) {
        LOG_ERROR("Listen port:%d error!", port_);
        close(listen_fd_);
        return false;
    }

    // 创建接受连接的Channel并设置回调函数
    accept_channel_.reset(new Channel(main_loop_.get(), listen_fd_));
    accept_channel_->set_read_callback(
        std::bind(&WebServer::handle_listen, this));

    accept_channel_->set_update_callback(std::bind(&WebServer::handle_cur, this));

    main_loop_->run_in_loop([this]() {
        accept_channel_->enable_reading();
    });

    set_fd_nonblock(listen_fd_);
    LOG_INFO("Server port:%d", port_);
    return true;
}

void WebServer::handle_listen() {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    
    do {
        int fd = accept(listen_fd_, (struct sockaddr *)&addr, &len);
        if(fd <= 0) { return; }
        
        if(HttpConn::user_count >= MAX_FD) {
            send_error(fd, "Server busy!");
            LOG_WARN("Clients is full!");
            return;
        }
        
        add_client(fd, addr);
    } while(listen_event_ & EPOLLET);
}

void WebServer::add_client(int fd, sockaddr_in addr) {
    assert(fd > 0);
    
    // 选择一个IO线程
    EventLoop* io_loop = thread_pool_->get_next_loop();
    client_loops_[fd] = io_loop;
    
    // 初始化HTTP连接
    users_[fd].init(fd, addr);
    
    // 设置定时器
    if(timeout_ms_ > 0) {
        timer_->add(fd, timeout_ms_, std::bind(&WebServer::close_conn, this, &users_[fd]));
    }
    
    // 创建客户端Channel并设置到IO线程
    Channel* client_channel = new Channel(io_loop, fd);
    client_channels_[fd] = client_channel;
    
    client_channel->set_read_callback([this, fd]() {
        handle_read(&users_[fd]);
    });
    
    client_channel->set_write_callback([this, fd]() {
        handle_write(&users_[fd]);
    });
    
    client_channel->set_close_callback([this, fd]() {
        close_conn(&users_[fd]);
    });
    
    client_channel->set_error_callback([this, fd]() {
        close_conn(&users_[fd]);
    });
    
    // 注册到IO线程
    io_loop->run_in_loop([client_channel, this]() {
        client_channel->enable_reading();
    });
    
    set_fd_nonblock(fd);
    LOG_INFO("Client[%d] in!", users_[fd].get_fd());
}

void WebServer::handle_read(HttpConn* client) {
    assert(client);
    extend_time(client);
    
    // 将读取任务放入IO线程执行
    EventLoop* io_loop = nullptr;
    auto it = client_loops_.find(client->get_fd());
    if(it != client_loops_.end()) io_loop = it->second;
    if(io_loop) io_loop->run_in_loop(std::bind(&WebServer::on_read, this, client));
    else close_conn(client);
}

void WebServer::handle_write(HttpConn* client) {
    assert(client);
    extend_time(client);
    
    // 将写入任务放入IO线程执行
    EventLoop* io_loop = nullptr;
    auto it = client_loops_.find(client->get_fd());
    if(it != client_loops_.end()) io_loop = it->second;
    if(io_loop) io_loop->run_in_loop(std::bind(&WebServer::on_write, this, client));
    else close_conn(client);
}

void WebServer::on_read(HttpConn* client) {
    int read_errno = 0;
    ssize_t ret = client->read(&read_errno);
    
    if (ret <= 0 && read_errno != EAGAIN) {
        close_conn(client);
        return;
    }
    
    on_process(client);
}

void WebServer::on_process(HttpConn* client) {
    int fd = client->get_fd();

    Channel* channel = nullptr;
    auto it = client_channels_.find(fd);
    if(it == client_channels_.end()) return;
    channel = it->second;
    if (client->process()) channel->enable_writing();
    else channel->enable_reading();
    
}

void WebServer::on_write(HttpConn* client) {
    int fd = client->get_fd();
    int write_errno = 0;
    ssize_t ret = client->write(&write_errno);
    
    if (client->get_write_bytes() == 0) {
        // 传输完成
        if (client->is_keep_alive()) {
            on_process(client);
            return;
        }
    } else if (ret < 0) {
        if (write_errno == EAGAIN) {
            // 继续传输
            Channel* channel = nullptr;
            auto it = client_channels_.find(fd);
            if(it == client_channels_.end()) return;
            channel = it->second;
            channel->enable_writing();
            return;
        }
    }
    
    close_conn(client);
}

void WebServer::extend_time(HttpConn* client) {
    assert(client);
    if (timeout_ms_ > 0) {
        timer_->adjust(client->get_fd(), timeout_ms_);
    }
}

void WebServer::close_conn(HttpConn* client) {
    assert(client);
    int fd = client->get_fd();
    LOG_INFO("Client[%d] quit!", fd);

    auto loop_it = client_loops_.find(fd);
    EventLoop* loop = nullptr;
    if(loop_it != client_loops_.end()) {
        loop = loop_it->second;
        client_loops_.erase(loop_it);
    }
    
    // 释放Channel
    auto it = client_channels_.find(fd);
    if (it != client_channels_.end()) {
        if(loop) {
            loop->run_in_loop([channel = it->second]() {
                channel->disable_all();
                channel->remove();
                delete channel;
            });
        }
        
        client_channels_.erase(it);
    }
    
    // 关闭连接
    client->close();
}

void WebServer::send_error(int fd, const char* info) {
    assert(fd > 0);
    send(fd, info, strlen(info), 0);
    close(fd);
}

int WebServer::set_fd_nonblock(int fd) {
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) | O_NONBLOCK);
}

void WebServer::handle_cur() {
    main_loop_->modify_channel(accept_channel_.get());
}