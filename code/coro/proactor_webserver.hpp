#pragma once

#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <fstream>
#include <iostream>
#include <cstring>
#include <csignal>
#include <errno.h>
#include <tuple>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <exec/async_scope.hpp>
#include <exec/repeat_effect_until.hpp>
#include <stdexec/execution.hpp>
#include <uring_exec.hpp>
#include <format>
#include <ranges>
#include <string_view>
#include <chrono>
#include <charconv>


namespace async_webserver {

// Modern C++23 constants
namespace constants {
    constexpr size_t default_buffer_size = 8192;
    constexpr size_t max_chunk_size = 64 * 1024; // 64KB chunks
    constexpr size_t yield_threshold = 256 * 1024; // Yield every 256KB
    constexpr std::string_view http_200_ok = "HTTP/1.1 200 OK\r\n";

    // Content types
    constexpr std::string_view content_type_html = "text/html; charset=utf-8";
    constexpr std::string_view content_type_css = "text/css";
    constexpr std::string_view content_type_js = "application/javascript";
    constexpr std::string_view content_type_png = "image/png";
    constexpr std::string_view content_type_jpeg = "image/jpeg";
    constexpr std::string_view content_type_mp4 = "video/mp4";
    constexpr std::string_view content_type_webm = "video/webm";
}

class SignalHandler {
public:
    using StopCallback = std::function<void()>;

    static SignalHandler& instance() noexcept {
        static SignalHandler handler;
        return handler;
    }

    void setup_graceful_shutdown(StopCallback callback) {
        stop_callback_ = std::move(callback);

        // Install signal handlers
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        // Ignore SIGPIPE (common in network applications)
        std::signal(SIGPIPE, SIG_IGN);
    }

    bool should_stop() const noexcept {
        return should_stop_.load(std::memory_order_acquire);
    }

    void request_stop() noexcept {
        if (!should_stop_.exchange(true, std::memory_order_acq_rel)) {
            if (stop_callback_) {
                stop_callback_();
            }
        }
    }

private:
    SignalHandler() = default;

    static void signal_handler(int signal) noexcept {
        instance().request_stop();
    }

    std::atomic<bool> should_stop_{false};
    StopCallback stop_callback_;
};

class ProactorWebServer {
public:
    struct Config {
        int port = 2316;
        std::string src_dir = "../resources/";
        size_t uring_entries = 4096;
        size_t num_threads = std::thread::hardware_concurrency();
        size_t max_connections = 10000; 
        int listen_backlog = 16384;     
        bool opt_linger = false;
    };

    explicit ProactorWebServer(const Config& config);
    ~ProactorWebServer();

    ProactorWebServer(const ProactorWebServer&) = delete;

    template<typename Self>
    Self& operator=(this Self&& self, const ProactorWebServer&) = delete;

    void run();
    void stop();
    bool is_running() const noexcept { return !uring_->get_stop_token().stop_requested(); }

private:
    // Server initialization
    bool init_socket();
    static int set_fd_nonblock(int fd) noexcept;

    Config config_;
    int listen_fd_;

    // Single io_uring_exec instance
    std::unique_ptr<uring_exec::io_uring_exec> uring_;
    std::vector<std::jthread> worker_threads_;

    // Connection tracking
    std::atomic<size_t> active_connections_{0};
};

// Helper functions for HTTP processing

struct HttpRequest {
    std::string path;
    std::string range_header;
};

inline HttpRequest parse_http_request(std::string_view request) noexcept {
    HttpRequest result;
    result.path = "/";

    // Parse request line using modern C++23 ranges
    auto first_line_end = request.find("\r\n");
    if (first_line_end != std::string_view::npos) {
        auto first_line = request.substr(0, first_line_end);

        auto path_start = first_line.find(' ');
        auto path_end = first_line.find(' ', path_start + 1);

        if (path_start != std::string_view::npos && path_end != std::string_view::npos) {
            result.path = first_line.substr(path_start + 1, path_end - path_start - 1);
        }
    }

    // Parse headers for Range requests
    size_t header_start = first_line_end + 2;
    while (header_start < request.size()) {
        auto header_end = request.find("\r\n", header_start);
        if (header_end == std::string_view::npos) break;

        auto header_line = request.substr(header_start, header_end - header_start);
        if (header_line.empty()) break; // End of headers

        if (header_line.starts_with("Range: ") || header_line.starts_with("range: ")) {
            result.range_header = header_line.substr(7);
        }

        header_start = header_end + 2;
    }

    return result;
}

struct RangeRequest {
    size_t start{0};
    size_t end{0};
    bool is_valid{false};

    constexpr RangeRequest() noexcept = default;
    constexpr RangeRequest(size_t s, size_t e, bool valid) noexcept
        : start(s), end(e), is_valid(valid) {}
};

struct ResponseContext {
    std::string_view content_type;
    size_t file_size;
    size_t content_length;
    size_t range_start{0};
    size_t range_end{0};
    bool is_range_request{false};

    constexpr ResponseContext(std::string_view ct, size_t fs) noexcept
        : content_type(ct), file_size(fs), content_length(fs), range_end(fs - 1) {}

    constexpr void set_range(size_t start, size_t end) noexcept {
        range_start = start;
        range_end = end;
        content_length = end - start + 1;
        is_range_request = true;
    }
};

inline RangeRequest parse_range_header(std::string_view range_header, size_t file_size) noexcept {
    if (range_header.empty() || !range_header.starts_with("bytes=")) {
        return RangeRequest{0, file_size - 1, false};
    }

    auto range_spec = range_header.substr(6); // Remove "bytes="
    auto dash_pos = range_spec.find('-');

    if (dash_pos == std::string_view::npos) {
        return RangeRequest{0, file_size - 1, false};
    }

    auto start_str = range_spec.substr(0, dash_pos);
    auto end_str = range_spec.substr(dash_pos + 1);

    size_t start = 0;
    size_t end = file_size - 1;

    if (!start_str.empty()) {
        if (auto [ptr, ec] = std::from_chars(start_str.data(), start_str.data() + start_str.size(), start);
            ec != std::errc{}) {
            return RangeRequest{0, file_size - 1, false};
        }
    }

    if (!end_str.empty()) {
        if (auto [ptr, ec] = std::from_chars(end_str.data(), end_str.data() + end_str.size(), end);
            ec != std::errc{}) {
            return RangeRequest{0, file_size - 1, false};
        }
    }

    // Validate range
    bool is_valid = start < file_size && end < file_size && start <= end;
    return RangeRequest{start, end, is_valid};
}

inline std::string map_request_path(std::string_view path) {
    using namespace std::string_literals;

    if (path == "/") {
        return "welcome.html"s;
    } else if (path == "/index" || path == "/index.html") {
        return "index.html"s;
    } else {
        std::string filename{path.substr(1)};
        if (filename.find('.') == std::string::npos) {
            filename += ".html";
        }
        return filename;
    }
}

inline constexpr std::string_view get_content_type(std::string_view filename) noexcept {
    if (filename.ends_with(".css")) return constants::content_type_css;
    else if (filename.ends_with(".js")) return constants::content_type_js;
    else if (filename.ends_with(".png")) return constants::content_type_png;
    else if (filename.ends_with(".jpg") || filename.ends_with(".jpeg")) return constants::content_type_jpeg;
    else if (filename.ends_with(".mp4")) return constants::content_type_mp4;
    else if (filename.ends_with(".webm")) return constants::content_type_webm;
    return constants::content_type_html;
}

inline std::string build_response_headers(const ResponseContext& ctx) {
    if (ctx.is_range_request) {
        return std::format("HTTP/1.1 206 Partial Content\r\n"
                          "Content-Type: {}\r\n"
                          "Content-Length: {}\r\n"
                          "Content-Range: bytes {}-{}/{}\r\n"
                          "Accept-Ranges: bytes\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          ctx.content_type, ctx.content_length,
                          ctx.range_start, ctx.range_end, ctx.file_size);
    } else {
        return std::format("HTTP/1.1 200 OK\r\n"
                          "Content-Type: {}\r\n"
                          "Content-Length: {}\r\n"
                          "Accept-Ranges: bytes\r\n"
                          "Connection: close\r\n"
                          "\r\n",
                          ctx.content_type, ctx.content_length);
    }
}

inline int send_file_response(int client_fd, std::string_view headers, int file_fd,
                             const ResponseContext& ctx) noexcept {
    // Send headers first
    size_t total_sent = 0;
    const char* ptr = headers.data();
    size_t remaining = headers.size();

    while (remaining > 0) {
        ssize_t sent = send(client_fd, ptr + total_sent, remaining, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EPIPE || errno == ECONNRESET) break; // Client disconnected
            close(file_fd);
            return -1;
        }
        total_sent += sent;
        remaining -= sent;
    }

    // Then send file content
    total_sent = 0;
    off_t current_offset = ctx.range_start;

    while (total_sent < ctx.content_length) {
        size_t file_remaining = ctx.content_length - total_sent;
        size_t chunk_size = std::min(file_remaining, constants::max_chunk_size);

        ssize_t sent = sendfile(client_fd, file_fd, &current_offset, chunk_size);
        if (sent <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Would block - yield and continue later
                std::this_thread::sleep_for(std::chrono::microseconds{10});
                continue;
            }
            if (errno == EPIPE || errno == ECONNRESET) {
                // Client disconnected - clean shutdown
                break;
            }
            // Other error - abort
            close(file_fd);
            return -1;
        }

        total_sent += sent;

        // Cancellation point for very large transfers
        if (total_sent % constants::yield_threshold == 0) {
            // Small yield allows other operations and cancellation
            std::this_thread::sleep_for(std::chrono::microseconds{1});

            // Check if client is still connected by testing socket
            int error = 0;
            socklen_t len = sizeof(error);
            if (getsockopt(client_fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error != 0) {
                // Socket error - client likely disconnected
                break;
            }
        }
    }

    close(file_fd); // Clean up file descriptor
    return static_cast<int>(total_sent);
}

// Free function implementations

inline stdexec::sender auto process_http_connection(uring_exec::io_uring_exec::scheduler scheduler,
                                                    int client_fd, std::string_view src_dir) {
    // Use smart pointer for automatic cleanup
    auto buffer = std::make_unique<char[]>(constants::default_buffer_size);
    char* raw_buffer = buffer.get();

    return uring_exec::async_recv(scheduler, client_fd, raw_buffer, constants::default_buffer_size, 0)
         | stdexec::then([buffer = std::move(buffer), src_dir](int bytes_read) -> std::tuple<std::string, int, ResponseContext> {
               if (bytes_read <= 0) {
                   return std::make_tuple(std::string{}, 0, ResponseContext{constants::content_type_html, 0});
               }

               // Parse HTTP request
               std::string_view request{buffer.get(), static_cast<size_t>(bytes_read)};
               auto http_request = parse_http_request(request);

               // Map request path to filename
               std::string filename = map_request_path(http_request.path);

               // Open file and get size for sendfile (zero-copy)
               std::string filepath = std::format("{}{}", src_dir, filename);
               int file_fd = open(filepath.c_str(), O_RDONLY);
               struct stat st;
               fstat(file_fd, &st);
               size_t file_size = st.st_size;

               // Get content type and create response context
               std::string_view content_type = get_content_type(filename);
               ResponseContext ctx(content_type, file_size);

               // Parse range request if present
               auto range_request = parse_range_header(http_request.range_header, file_size);
               if (range_request.is_valid) {
                   ctx.set_range(range_request.start, range_request.end);
               }

               // Build headers using the context
               std::string headers = build_response_headers(ctx);

               // Return headers, file_fd, and context as tuple
               return std::make_tuple(std::move(headers), file_fd, std::move(ctx));
           })
         | stdexec::let_value([scheduler, client_fd](const std::tuple<std::string, int, ResponseContext>& response_data) {
               const auto& [headers, file_fd, ctx] = response_data;

               // Send response using helper function
               return stdexec::schedule(scheduler)
                    | stdexec::then([client_fd, headers, file_fd, ctx] -> int {
                          return send_file_response(client_fd, headers, file_fd, ctx);
                      });
           });
}

inline stdexec::sender auto handle_client(uring_exec::io_uring_exec::scheduler scheduler,
                                          int client_fd,
                                          std::string_view src_dir,
                                          std::atomic<size_t>& active_connections) {
    active_connections.fetch_add(1, std::memory_order_relaxed);

    return process_http_connection(scheduler, client_fd, src_dir)
         | stdexec::let_value([scheduler, client_fd, &active_connections](...) {
               active_connections.fetch_sub(1, std::memory_order_relaxed);
               return uring_exec::async_close(scheduler, client_fd);
           })
           | stdexec::then([](...) {});
}

inline stdexec::sender auto server_accept_loop(uring_exec::io_uring_exec::scheduler scheduler,
                                               int listen_fd,
                                               exec::async_scope& scope,
                                               std::string_view src_dir,
                                               std::atomic<size_t>& active_connections,
                                               size_t max_connections) {
    return uring_exec::async_accept(scheduler, listen_fd, nullptr, nullptr, 0)
         | stdexec::let_value([scheduler, &scope, src_dir, &active_connections, max_connections](int client_fd) {

               if (client_fd > 0) {
                   auto active = active_connections.load(std::memory_order_relaxed);

                   if (active < max_connections) {
                       scope.spawn(handle_client(scheduler, client_fd, src_dir, active_connections));
                   } else {
                       close(client_fd);
                   }
               }

               // The repeat_effect_until will naturally stop when the scheduler stops
               // due to stop token being triggered, no explicit running check needed
               return stdexec::just(false);  // Continue until stop token triggers
           })
         | exec::repeat_effect_until();
}

// ProactorWebServer implementation

inline ProactorWebServer::ProactorWebServer(const Config& config)
    : config_(config), listen_fd_(-1) {

    // Get resource directory (make path independent of execution directory)
    char* exe_path = realpath("/proc/self/exe", nullptr);
    if (exe_path) {
        std::string exe_dir = exe_path;
        free(exe_path);
        size_t last_slash = exe_dir.find_last_of('/');
        if (last_slash != std::string::npos) {
            exe_dir = exe_dir.substr(0, last_slash + 1);
        }
        config_.src_dir = exe_dir + "../resources/";
    } else {
        char* temp_dir = getcwd(nullptr, 256);
        if (temp_dir) {
            config_.src_dir = std::string(temp_dir) + "/resources/";
            free(temp_dir);
        }
    }

    uring_ = std::make_unique<uring_exec::io_uring_exec>(config_.uring_entries);
}

inline ProactorWebServer::~ProactorWebServer() {
    stop();
}

inline bool ProactorWebServer::init_socket() {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return false;
    }

    int opt = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(listen_fd_);
        return false;
    }

    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        // Not critical if this fails
    }

    if (config_.opt_linger) {
        struct linger opt_linger;
        opt_linger.l_onoff = 1;
        opt_linger.l_linger = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_LINGER, &opt_linger, sizeof(opt_linger));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(config_.port);

    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(listen_fd_);
        return false;
    }

    if (listen(listen_fd_, config_.listen_backlog) < 0) {
        close(listen_fd_);
        return false;
    }

    set_fd_nonblock(listen_fd_);
    return true;
}

inline int ProactorWebServer::set_fd_nonblock(int fd) noexcept {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

inline void ProactorWebServer::run() {
    // Check if already running using stop token
    if (!is_running()) {
        return; // Already stopped
    }

    if (!init_socket()) {
        throw std::runtime_error("Failed to initialize listen socket");
    }

    // Setup graceful shutdown
    SignalHandler::instance().setup_graceful_shutdown([this]() {
        stop();
    });

    // Get scheduler
    auto scheduler = uring_->get_scheduler();
    exec::async_scope scope;

    // Spawn the server accept loop using the proper pattern
    scope.spawn(
        stdexec::schedule(scheduler)
        | stdexec::let_value([this, scheduler, &scope] {
            return server_accept_loop(scheduler, listen_fd_, scope, config_.src_dir,
                                      active_connections_, config_.max_connections);
          })
    );

    // Start worker threads (each calling uring.run() with stop token support)
    for (size_t i = 0; i < config_.num_threads; ++i) {
        worker_threads_.emplace_back([this](std::stop_token stop_token) {
            uring_->run(stop_token);
        });
    }

    // Wait for all async operations to complete or be cancelled
    stdexec::sync_wait(scope.on_empty());

    // Wait for all threads to finish
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

inline void ProactorWebServer::stop() {
    // Use uring's stop token instead of running_ flag
    if (!is_running()) {
        return; // Already stopped
    }

    // Close listen socket first to stop accepting new connections
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    // Stop the io_uring event loop - this will trigger cancellation
    if (uring_) {
        uring_->request_stop();
    }

    // Give a brief moment for graceful shutdown of active connections
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    // Request stop for all worker threads
    for (auto& thread : worker_threads_) {
        if (thread.get_stop_source().stop_possible()) {
            thread.request_stop();
        }
    }

    // Wait for all worker threads with timeout
    auto start_time = std::chrono::steady_clock::now();
    constexpr auto shutdown_timeout = std::chrono::seconds{1};

    for (auto& thread : worker_threads_) {
        if (thread.joinable()) {
            auto elapsed = std::chrono::steady_clock::now() - start_time;
            if (elapsed < shutdown_timeout) {
                thread.join();
            } else {
                // Force shutdown if timeout exceeded
                std::cerr << "Warning: Force shutting down worker thread after timeout" << std::endl;
                thread.detach();
            }
        }
    }
    worker_threads_.clear();
}

} // namespace async_webserver