#include <iostream>
#include <thread>

#include "proactor_webserver.hpp"

using namespace async_webserver;

int main() {
    try {
        // Configure server
        ProactorWebServer::Config config{
            .port = 2316,
            .src_dir = "../resources/",
            .uring_entries = 16384,
            .num_threads = std::thread::hardware_concurrency(),
            .max_connections = 20000,
            .listen_backlog = 16384,
            .opt_linger = false,
            .keep_alive_timeout = std::chrono::seconds{5}
        };

        std::cout << "Configuration:\n";
        std::cout << "  Port: " << config.port << "\n";
        std::cout << "  Threads: " << config.num_threads << "\n";
        std::cout << "  io_uring entries: " << config.uring_entries << "\n";
        std::cout << "  Max connections: " << config.max_connections << "\n";
        std::cout << "  Resource dir: " << config.src_dir << "\n";
        std::cout << "  Keep-alive timeout: " << config.keep_alive_timeout << "\n";
        std::cout << std::string(60, '-') << "\n";

        // Create proactor server
        ProactorWebServer server(config);

        std::cout << "Server starting on port " << config.port << "...\n";
        std::cout << "Press Ctrl+C to stop gracefully\n";
        std::cout << std::string(60, '=') << "\n";

        // Run server (blocking) - this will spawn threads and call uring.run()
        server.run();

        std::cout << "Server stopped successfully!\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << std::endl;
        return 1;
    }
}