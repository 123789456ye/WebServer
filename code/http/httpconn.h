#pragma once

#include <sys/types.h>
#include <sys/uio.h>    
#include <arpa/inet.h>   
#include <atomic>
#include <string>
#include <string_view>

#include "../log/log.h"
#include "../buffer/buffer.h"
#include "httprequest.h"
#include "httpresponse.h"

class HttpConn {
public:
    HttpConn();
    ~HttpConn();

    HttpConn(const HttpConn&) = delete;
    HttpConn& operator=(const HttpConn&) = delete;

    void init(int sock_fd, const sockaddr_in& addr);

    ssize_t read(int* save_errno);
    ssize_t write(int* save_errno);

    void close();

    bool process();

    int get_fd() const { return fd_; }
    int get_port() const { return addr_.sin_port; }
    const char* get_ip() const { return inet_ntoa(addr_.sin_addr); }
    sockaddr_in get_addr() const { return addr_; }

    int get_write_bytes() const { 
        return iov_[0].iov_len + iov_[1].iov_len; 
    }

    bool is_keep_alive() const {
        return request_.is_keep_alive();
    }

    static bool is_et;                       
    static const char* src_dir;              
    static std::atomic<int> user_count;      

private:
    int fd_;                                 
    sockaddr_in addr_;                       

    bool is_closed_;                         
    
    int iov_count_;                         
    iovec iov_[2];                         
    
    Buffer read_buffer_;                    
    Buffer write_buffer_;                   

    HttpRequest request_;                 
    HttpResponse response_;                 
};
