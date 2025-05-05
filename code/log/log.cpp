#include "log.h"

using namespace std;

Log::Log() = default;

Log::~Log() {
    if(write_thread_ && write_thread_->joinable()) {
        while(!deque_->empty()) {
            deque_->flush();
        };
        deque_->close();
        write_thread_->join();
    }
    if(fp_) {
        lock_guard<mutex> locker(mtx_);
        flush();
        fclose(fp_);
    }
}

void Log::init(int level = 1, const string& path, const string& suffix,
    int max_queue_size) {
    is_open_ = true;
    level_ = level;
    
    if(max_queue_size > 0) {
        is_async_ = true;
        if(!deque_) {
            deque_ = make_unique<BlockDeque<string>>(max_queue_size);
            write_thread_ = make_unique<thread>([this]{async_write_();});
        }
    }
    else {
        is_async_ = false;
    }

    line_count_ = 0;
    time_t timer = time(nullptr);
    struct tm t = *localtime(&timer);
    path_ = path;
    suffix_ = suffix;

    char file_name[LOG_NAME_LEN] = {0};
    snprintf(file_name, LOG_NAME_LEN - 1,"%s/%04d_%02d_%02d%s", 
            path_.c_str(), t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, suffix_.c_str());
    today_ = t.tm_mday;
    {
        lock_guard<mutex> lck(mtx_);
        buffer_.retrieve_all();

        if(fp_) {
            flush();
            fclose(fp_);
        }

        fp_ = fopen(file_name, "a");
        if(fp_ == nullptr) {
            mkdir(path.c_str(), 0777);
            fp_ = fopen(file_name, "a");
        }
    }
}

void Log::write(int level, const char *format, ...) {
    struct timeval now = {0, 0};
    gettimeofday(&now, nullptr);
    time_t t_sec = now.tv_sec;
    struct tm t = *localtime(&t_sec);
    va_list args;

    if(today_ != t.tm_mday || (line_count_ && (line_count_ % MAX_LINES == 0))) {
        unique_lock<mutex> lck(mtx_);
        lck.unlock();

        char new_file[LOG_NAME_LEN];
        char tail[36] = {0};
        snprintf(tail, 36, "%04d_%02d_%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

        if(today_ != t.tm_mday) {
            snprintf(new_file, LOG_NAME_LEN - 72, "%s/%s%s", path_.c_str(), tail, suffix_.c_str());
            today_ = t.tm_mday;
            line_count_ = 0;
        }
        else {
            snprintf(new_file, LOG_NAME_LEN - 72, "%s/%s-%d%s", 
                    path_.c_str(), tail, (line_count_ / MAX_LINES), suffix_.c_str());
        }

        lck.lock();
        flush();
        fclose(fp_);
        fp_ = fopen(new_file, "a");
    }

    {
        unique_lock<mutex> lck(mtx_);
        line_count_++;

        char* buf = buffer_.begin_write();
        int n = snprintf(buf, 128, "%d-%02d-%02d %02d:%02d:%02d.%06ld ",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec, now.tv_usec);
        buffer_.has_written(n);

        append_title_(level);
        va_start(args, format);
        int m = vsnprintf(buffer_.begin_write(), buffer_.writable_bytes(), format, args);
        va_end(args);
        buffer_.has_written(m);
        buffer_.append("\n", 1);

        if(is_async_ && !deque_->full()) {
            deque_->push_back(buffer_.retrieve_allstring());
        }
        else {
            fputs(buffer_.readable_view().data(), fp_);
            buffer_.retrieve_all();
        }
    }
}

void Log::append_title_(int level) {
    switch(level) {
    case 0:
        buffer_.append("[debug]: ", 9);
        break;
    case 1:
        buffer_.append("[info] : ", 9);
        break;
    case 2:
        buffer_.append("[warn] : ", 9);
        break;
    case 3:
        buffer_.append("[error]: ", 9);
        break;
    default:
        buffer_.append("[info] : ", 9);
        break;
    }
}

void Log::flush() {
    if(is_async_) { 
        deque_->flush(); 
    }
    fflush(fp_);
}

void Log::async_write_() {
    string str = "";
    while(deque_->pop(str)) {
        lock_guard<mutex> locker(mtx_);
        fputs(str.c_str(), fp_);
    }
}