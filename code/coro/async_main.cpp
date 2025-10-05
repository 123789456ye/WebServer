#include <iostream>
#include <signal.h>
#include <stdexec/execution.hpp>
#include <thread>
#include <atomic>
#include "async_webserver.hpp"

using namespace async_webserver;

// Global server instance for signal handling
std::unique_ptr<AsyncWebServer> g_server;
std::atomic<bool> g_running{true};

// Signal handler for graceful shutdown
void signal_handler(int sig) {
    std::cout << "\n🛑 Received signal " << sig << ", shutting down server...\n";
    g_running = false;
    if (g_server) {
        g_server->stop();
    }
}

int main() {
    try {
        std::cout << "🚀 ASYNC WEBSERVER WITH COROUTINES + SENDER/RECEIVER\n";
        std::cout << std::string(60, '=') << "\n";

        // Setup signal handlers for graceful shutdown
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        // Create async webserver with coroutine-based I/O
        g_server = std::make_unique<AsyncWebServer>(
            2316,           // port
            60000,          // timeout_ms
            false,          // opt_linger
            3306,           // sql_port
            "root",         // sql_user
            "root",         // sql_pwd
            "webserver",    // db_name
            12,             // conn_pool_num
            1,              // thread_num
            false,          // open_log
            1,              // log_level
            1024            // log_que_size
        );

        std::cout << "🔧 Configuration:\n";
        std::cout << "   • Port: 2316\n";
        std::cout << "   • I/O: Async with io_uring (ThreadPoolScheduler)\n";
        std::cout << "   • Pattern: Sender/Receiver + Coroutines\n";
        std::cout << "   • Context: ThreadPoolScheduler with io_uring integration\n\n";

        std::cout << "🔥 Starting async webserver...\n";
        std::cout << "   Visit: http://localhost:2316\n";
        std::cout << "   Press Ctrl+C for graceful shutdown\n\n";

        // Execute the async server coroutine with sync_wait
        stdexec::sync_wait(g_server->start_accept_only());

        std::cout << "✅ Server stopped successfully!\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "💥 Server error: " << e.what() << std::endl;
        return 1;
    }
}