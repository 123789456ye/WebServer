#ifndef LOG_H
#define LOG_H
#endif
#pragma once

#include <mutex>
#include <string>
#include <thread>
#include <sys/time.h>
#include <string.h>
#include <stdarg.h>           
#include <assert.h>
#include <sys/stat.h>         
#include "blockqueue.h"
#include "../buffer/buffer.h"

class Log {
public:
    void init(int level, const std::string& path = "./log", 
                const std::string& suffix =".log",
                int max_queue_size = 1024);

    static Log* instance() {
        static Log inst;
        return &inst;
    }
    void write(int level, const char *format,...);
    void flush();

    int get_level() {
        std::lock_guard<std::mutex> lck(mtx_);
        return level_;
    }
    void set_level(int level) {
        std::lock_guard<std::mutex> lck(mtx_);
        level_ = level;
    }
    bool is_open() { return is_open_; }
    
private:
    Log();
    ~Log();
    void append_title_(int level);
    void async_write_();

private:
    static constexpr int LOG_PATH_LEN = 256;
    static constexpr int LOG_NAME_LEN = 256;
    static constexpr int MAX_LINES = 50000;

    std::string path_;
    std::string suffix_;

    int line_count_ = 0;
    int today_ = 0;

    bool is_open_ = false;
 
    Buffer buffer_;
    int level_ = 1;
    bool is_async_ = false;

    FILE* fp_ = nullptr;
    std::unique_ptr<BlockDeque<std::string>> deque_; 
    std::unique_ptr<std::thread> write_thread_;
    std::mutex mtx_;
};

#define LOG_BASE(level, format, ...) \
    do {\
        Log* log = Log::instance();\
        if (log->is_open() && log->get_l evel() <= level) {\
            log->write(level, format, ##__VA_ARGS__); \
            log->flush();\
        }\
    } while(0);

#define LOG_DEBUG(format, ...) do {LOG_BASE(0, format, ##__VA_ARGS__)} while(0);
#define LOG_INFO(format, ...) do {LOG_BASE(1, format, ##__VA_ARGS__)} while(0);
#define LOG_WARN(format, ...) do {LOG_BASE(2, format, ##__VA_ARGS__)} while(0);
#define LOG_ERROR(format, ...) do {LOG_BASE(3, format, ##__VA_ARGS__)} while(0);
