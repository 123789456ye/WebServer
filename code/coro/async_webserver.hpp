#pragma once

#include <memory>
#include <unordered_map>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <chrono>
#include <cstring>
#include <errno.h>
#include <algorithm>
#include <sys/socket.h>
#include <fstream>

#include "thread_pool_scheduler.hpp"
#include "../http/httprequest.h"
#include "../http/httpresponse.h"
#include "../buffer/buffer.h"
#include "../timer/heaptimer.h"
#include "../pool/sqlconnpool.h"
#include "../log/log.h"
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

namespace async_webserver {

using namespace coro;
using namespace stdexec;

// Forward declarations
class AsyncWebServer;

// Async HTTP connection using coroutines and sender/receiver
class AsyncHttpConn {
public:
    AsyncHttpConn(int fd, sockaddr_in addr, ThreadPoolScheduler& scheduler, const std::string& src_dir);
    ~AsyncHttpConn();

    // Main coroutine that handles the entire HTTP request/response lifecycle
    exec::task<void> handle_connection();

    // Async I/O operations using sender/receiver pattern
    exec::task<ssize_t> async_read_request();
    exec::task<ssize_t> async_write_response();
    exec::task<std::string> async_read_file(const std::string& filepath);

    int get_fd() const { return fd_; }
    bool is_keep_alive() const;
    void close();

private:
    // Connection state
    int fd_;
    sockaddr_in addr_;
    bool is_closed_;
    ThreadPoolScheduler& scheduler_;
    std::string src_dir_;

    // HTTP processing components
    Buffer read_buffer_;
    Buffer write_buffer_;
    HttpRequest request_;
    HttpResponse response_;

    // Async file operations
    exec::task<void> process_request();
    exec::task<void> generate_response();
};

// Async WebServer using thread pool scheduler and coroutines
class AsyncWebServer {
public:
    AsyncWebServer(
        int port, int timeout_ms, bool opt_linger,
        int sql_port, const char* sql_user, const char* sql_pwd,
        const char* db_name, int conn_pool_num, int thread_num,
        bool open_log, int log_level, int log_que_size);

    ~AsyncWebServer();

    // Start the async server
    exec::task<void> start();

    // Stop the server
    void stop();

    // Simple socket initialization for debugging
    bool init_socket_simple();

    // Start just the accept loop without timer for testing
    exec::task<void> start_accept_only();

private:
    // Core async server loop
    exec::task<void> accept_loop();

    // Handle new client connections
    exec::task<void> handle_new_connection(int client_fd, sockaddr_in client_addr);

    // Synchronous connection handler for thread pool
    void handle_connection_sync(int client_fd, sockaddr_in client_addr);

    // Cleanup expired connections
    exec::task<void> timer_cleanup_loop();

    // Initialize server socket
    bool init_socket();

    // Server configuration
    int port_;
    bool open_linger_;
    int timeout_ms_;
    bool is_running_;
    int listen_fd_;
    std::string src_dir_;

    // Async I/O infrastructure
    std::unique_ptr<ThreadPoolScheduler> scheduler_;

    // Connection management
    std::unique_ptr<HeapTimer> timer_;
    std::unordered_map<int, std::unique_ptr<AsyncHttpConn>> connections_;
    std::mutex connections_mutex_;

    static constexpr int MAX_FD = 65536;
    static int set_fd_nonblock(int fd);
};

// Implementation

inline AsyncHttpConn::AsyncHttpConn(int fd, sockaddr_in addr, ThreadPoolScheduler& scheduler, const std::string& src_dir)
    : fd_(fd), addr_(addr), is_closed_(false), scheduler_(scheduler), src_dir_(src_dir) {
    request_.init();
}

inline AsyncHttpConn::~AsyncHttpConn() {
    close();
}

inline void AsyncHttpConn::close() {
    if (!is_closed_) {
        is_closed_ = true;
        ::close(fd_);
        LOG_INFO("Client[%d] closed connection", fd_);
    }
}

inline bool AsyncHttpConn::is_keep_alive() const {
    return request_.is_keep_alive();
}

// Main connection handling coroutine
inline exec::task<void> AsyncHttpConn::handle_connection() {
    try {
        LOG_INFO("AsyncHttpConn[%d] started handling connection", fd_);

        while (!is_closed_) {
            // 1. Async read HTTP request
            ssize_t read_bytes = co_await async_read_request();
            if (read_bytes <= 0) {
                LOG_DEBUG("Client[%d] read failed or disconnected", fd_);
                break;
            }

            // 2. Process request (HTTP parsing)
            co_await process_request();

            // 3. Generate response (may involve async file reads)
            co_await generate_response();

            // 4. Async write HTTP response
            ssize_t write_bytes = co_await async_write_response();
            if (write_bytes <= 0) {
                LOG_DEBUG("Client[%d] write failed", fd_);
                break;
            }

            // 5. Check if connection should be kept alive
            if (!is_keep_alive()) {
                LOG_DEBUG("Client[%d] connection not keep-alive, closing", fd_);
                break;
            }

            // Clear buffers for next request
            read_buffer_.retrieve_all();
            write_buffer_.retrieve_all();
            request_.init();
        }

    } catch (const std::exception& e) {
        LOG_ERROR("AsyncHttpConn[%d] error: %s", fd_, e.what());
    }

    close();
    co_return;
}

// Async read using io_uring sender/receiver pattern
inline exec::task<ssize_t> AsyncHttpConn::async_read_request() {
    try {
        // Temporary blocking I/O - will replace with proper io_uring later
        char buffer[4096];
        ssize_t bytes_read = recv(fd_, buffer, sizeof(buffer)-1, 0);

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            read_buffer_.append(buffer, bytes_read);
        } else if (bytes_read < 0) {
        }

        co_return bytes_read;

    } catch (const std::exception& e) {
        LOG_ERROR("AsyncHttpConn[%d] read error: %s", fd_, e.what());
        co_return -1;
    }
}

// Async write using io_uring sender/receiver pattern
inline exec::task<ssize_t> AsyncHttpConn::async_write_response() {
    try {
        size_t readable = write_buffer_.readable_bytes();
        if (readable == 0) {
            co_return 0;
        }

        // Temporary blocking I/O - will replace with proper io_uring later
        ssize_t bytes_written = send(fd_, write_buffer_.begin_read(), readable, 0);

        if (bytes_written > 0) {
            write_buffer_.retrieve(bytes_written);
        } else if (bytes_written < 0) {
        }

        co_return bytes_written;

    } catch (const std::exception& e) {
        LOG_ERROR("AsyncHttpConn[%d] write error: %s", fd_, e.what());
        co_return -1;
    }
}

// Process HTTP request (parsing and routing)
inline exec::task<void> AsyncHttpConn::process_request() {
    try {
        // Parse HTTP request from read buffer
        bool parse_success = request_.parse(read_buffer_);

        if (!parse_success) {
            LOG_WARN("Failed to parse HTTP request from client[%d]", fd_);
            // For bad requests, we'll create a simple error response
            co_return;
        }

        LOG_DEBUG("Parsed HTTP request: %s %s",
                 std::string(request_.method()).c_str(),
                 std::string(request_.path()).c_str());

    } catch (const std::exception& e) {
        LOG_ERROR("Process request error: %s", e.what());
    }

    co_return;
}

// Generate HTTP response (may include async file reading)
inline exec::task<void> AsyncHttpConn::generate_response() {
    try {
        // Use the existing HttpResponse to generate response
        std::string path_str = std::string(request_.path());
        response_.init(src_dir_.c_str(), path_str, request_.is_keep_alive());

        // Generate response into write buffer
        response_.make_response(write_buffer_);

        LOG_DEBUG("Generated response for client[%d]: %zu bytes", fd_, write_buffer_.readable_bytes());

    } catch (const std::exception& e) {
        LOG_ERROR("Generate response error: %s", e.what());
    }

    co_return;
}

// AsyncWebServer implementation

inline AsyncWebServer::AsyncWebServer(
    int port, int timeout_ms, bool opt_linger,
    int sql_port, const char* sql_user, const char* sql_pwd,
    const char* db_name, int conn_pool_num, int thread_num,
    bool open_log, int log_level, int log_que_size)
    : port_(port), open_linger_(opt_linger), timeout_ms_(timeout_ms),
      is_running_(false), listen_fd_(-1) {

    // Get resource directory - based on executable location not current working directory
    char* exe_path = realpath("/proc/self/exe", nullptr);
    if (exe_path) {
        std::string exe_dir = exe_path;
        free(exe_path);

        // Remove executable name to get directory
        size_t last_slash = exe_dir.find_last_of('/');
        if (last_slash != std::string::npos) {
            exe_dir = exe_dir.substr(0, last_slash + 1);  // Keep the trailing '/'
        }

        // Add "../resources/" (since executable is in build/ directory)
        src_dir_ = exe_dir + "../resources/";
    } else {
        // Fallback to current directory if realpath fails
        char* temp_dir = getcwd(nullptr, 256);
        if (temp_dir) {
            src_dir_ = std::string(temp_dir) + "/resources/";
            free(temp_dir);
        }
    }


    // Initialize thread pool scheduler for async I/O with io_uring
    scheduler_ = std::make_unique<ThreadPoolScheduler>(thread_num, 512);

    // Start the thread pool immediately
    scheduler_->start();

    // Initialize database connection pool
    // TODO: Re-enable after MySQL is running
    // try {
    //     SqlConnPool::instance()->init("localhost", sql_port, sql_user, sql_pwd, db_name, conn_pool_num);
    // } catch (const std::exception& e) {
    // }

    // Initialize timer
    timer_ = std::make_unique<HeapTimer>();

    // Initialize logging
    if (open_log) {
        Log::instance()->init(log_level, "./log", ".log", log_que_size);
        LOG_INFO("========== Async Server Init ==========");
        LOG_INFO("Port:%d, Threads:%d", port_, thread_num);
    }
}

inline AsyncWebServer::~AsyncWebServer() {
    stop();
}

inline bool AsyncWebServer::init_socket() {

    // Create socket
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        LOG_ERROR("Create socket error!");
        return false;
    }

    // Set socket options
    int opt = 1;
    int ret = setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (ret < 0) {
        close(listen_fd_);
        return false;
    }

    if (open_linger_) {
        struct linger opt_linger;
        opt_linger.l_onoff = 1;
        opt_linger.l_linger = 1;
        ret = setsockopt(listen_fd_, SOL_SOCKET, SO_LINGER, &opt_linger, sizeof(opt_linger));
        if (ret < 0) {
            close(listen_fd_);
            return false;
        }
    }

    // Bind and listen
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    ret = bind(listen_fd_, (sockaddr*)&addr, sizeof(addr));
    if (ret < 0) {
        LOG_ERROR("Bind Port:%d error!", port_);
        close(listen_fd_);
        return false;
    }


    if (listen(listen_fd_, 1024) < 0) {
        LOG_ERROR("Listen port:%d error!", port_);
        close(listen_fd_);
        return false;
    }


    // Don't set non-blocking for now - keep socket blocking to avoid busy loop in accept()
    // set_fd_nonblock(listen_fd_);

    LOG_INFO("Server listening on port:%d", port_);


    return true;
}

inline int AsyncWebServer::set_fd_nonblock(int fd) {
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFD, 0) | O_NONBLOCK);
}

// Main server coroutine
inline exec::task<void> AsyncWebServer::start() {

    if (!init_socket()) {
        LOG_ERROR("Failed to initialize socket");
        co_return;
    }

    is_running_ = true;

    // ThreadPoolScheduler already started in constructor

    try {
        // Launch concurrent server tasks
        auto accept_task = accept_loop();
        auto timer_task = timer_cleanup_loop();

        // Wait for both tasks (they run until server stops)
        co_await stdexec::when_all(std::move(accept_task), std::move(timer_task));

    } catch (const std::exception& e) {
        LOG_ERROR("Server error: %s", e.what());
    }

    LOG_INFO("Async server stopped");
    co_return;
}

inline void AsyncWebServer::stop() {
    if (!is_running_) return;

    is_running_ = false;

    // Close listen socket
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    // Close all connections
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto& [fd, conn] : connections_) {
            conn->close();
        }
        connections_.clear();
    }

    // Stop scheduler
    if (scheduler_) {
        scheduler_->stop();
    }

    LOG_INFO("Async server stopped gracefully");
}

// Accept loop coroutine - accepts new connections using io_uring
inline exec::task<void> AsyncWebServer::accept_loop() {


    while (is_running_) {
        try {

            sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);

            // Blocking accept - will wait for incoming connections
            int client_fd = accept(listen_fd_, (sockaddr*)&client_addr, &addr_len);

            if (client_fd > 0) {

                // Handle connection synchronously for now - will make async later
                try {
                    handle_connection_sync(client_fd, client_addr);
                } catch (const std::exception& e) {
                    close(client_fd);
                }
            } else {

                // For blocking sockets, this should not happen unless there's an error
                if (errno == EINTR) {
                    continue;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                }
            }

        } catch (const std::exception& e) {
            if (is_running_) {
                // Brief pause before retrying
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    co_return;
}

// Synchronous connection handler (called from thread pool)
inline void AsyncWebServer::handle_connection_sync(int client_fd, sockaddr_in client_addr) {

    // Read HTTP request
    char buffer[4096];
    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer)-1, 0);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        std::string request(buffer);

        // Parse request path
        std::string path = "/";
        auto first_line_end = request.find("\r\n");
        if (first_line_end != std::string::npos) {
            auto first_line = request.substr(0, first_line_end);
            auto path_start = first_line.find(' ');
            auto path_end = first_line.find(' ', path_start + 1);
            if (path_start != std::string::npos && path_end != std::string::npos) {
                path = first_line.substr(path_start + 1, path_end - path_start - 1);
            }
        }

        // Map paths to appropriate files
        std::string filename;
        if (path == "/") {
            filename = "welcome.html";
        } else if (path == "/index" || path == "/index.html") {
            filename = "index.html";
        } else {
            filename = path.substr(1);
            if (filename.find('.') == std::string::npos) {
                filename += ".html";
            }
        }

        // Read and serve file
        std::string filepath = src_dir_ + filename;
        std::ifstream file(filepath, std::ios::binary);
        if (file.is_open()) {
            std::string content((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
            file.close();

            // Determine content type
            std::string content_type = "text/html; charset=utf-8";
            if (filename.ends_with(".css")) {
                content_type = "text/css";
            } else if (filename.ends_with(".js")) {
                content_type = "application/javascript";
            } else if (filename.ends_with(".jpg") || filename.ends_with(".jpeg")) {
                content_type = "image/jpeg";
            } else if (filename.ends_with(".png")) {
                content_type = "image/png";
            } else if (filename.ends_with(".ico")) {
                content_type = "image/x-icon";
            }

            // Send response
            std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: " + content_type + "\r\n"
                "Content-Length: " + std::to_string(content.length()) + "\r\n"
                "Connection: close\r\n"
                "\r\n" +
                content;

            send(client_fd, response.c_str(), response.length(), 0);
        } else {
            // Send 404
            const char* not_found =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: 47\r\n"
                "Connection: close\r\n"
                "\r\n"
                "<html><body><h1>404 Not Found</h1></body></html>";
            send(client_fd, not_found, strlen(not_found), 0);
        }
    }

    close(client_fd);
}

// Timer cleanup loop - handles connection timeouts (simplified)
inline exec::task<void> AsyncWebServer::timer_cleanup_loop() {

    while (is_running_) {
        try {
            // Process timer events
            timer_->tick();

            // Simple delay using sleep instead of complex scheduler operations
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // Allow other coroutines to run
            co_await stdexec::just();

        } catch (const std::exception& e) {
        }
    }

    co_return;
}

// Simple socket initialization method for debugging
inline bool AsyncWebServer::init_socket_simple() {

    bool result = init_socket();
    if (result) {
        is_running_ = true;
    } else {
    }

    return result;
}

// Start just the accept loop for testing
inline exec::task<void> AsyncWebServer::start_accept_only() {


    if (!init_socket()) {
        co_return;
    }


    is_running_ = true;

    // Start only the accept loop, no timer

    co_await accept_loop();

    co_return;
}

} // namespace async_webserver