#include "httpconn.h"

const char* HttpConn::src_dir = nullptr;
std::atomic<int> HttpConn::user_count{0};
bool HttpConn::is_et = false;

HttpConn::HttpConn() { 
    fd_ = -1;
    addr_ = { 0 };
    iov_count_ = 0;
    is_closed_ = true;
};

HttpConn::~HttpConn() { 
    close(); 
};

void HttpConn::init(int fd, const sockaddr_in& addr) {
    assert(fd > 0);
    user_count++;
    addr_ = addr;
    fd_ = fd;
    write_buffer_.retrieve_all();
    read_buffer_.retrieve_all();
    is_closed_ = false;
    LOG_INFO("Client[%d](%s:%d) in, user_count:%d", fd_, get_ip(), get_port(), (int)user_count);
}

void HttpConn::close() {
    response_.unmap_file();
    if(is_closed_ == false){
        is_closed_ = true; 
        user_count--;
        ::close(fd_);
        LOG_INFO("Client[%d](%s:%d) quit, user_count:%d", fd_, get_ip(), get_port(), (int)user_count);
    }
}

ssize_t HttpConn::read(int* saveErrno) {
    ssize_t len = -1;
    do {
        len = read_buffer_.read_fd(fd_, saveErrno);
        if (len <= 0) {
            break;
        }
    } while (is_et);
    return len;
}

ssize_t HttpConn::write(int* saveErrno) {
    ssize_t len = -1;
    do {
        len = writev(fd_, iov_, iov_count_);
        if(len <= 0) {
            *saveErrno = errno;
            break;
        }
        if(iov_[0].iov_len + iov_[1].iov_len == 0) break; 
        else if(static_cast<size_t>(len) > iov_[0].iov_len) {
            iov_[1].iov_base = (uint8_t*) iov_[1].iov_base + (len - iov_[0].iov_len);
            iov_[1].iov_len -= (len - iov_[0].iov_len);
            if(iov_[0].iov_len) {
                write_buffer_.retrieve_all();
                iov_[0].iov_len = 0;
            }
        }
        else {
            iov_[0].iov_base = (uint8_t*)iov_[0].iov_base + len; 
            iov_[0].iov_len -= len; 
            write_buffer_.retrieve(len);
        }
    } while(is_et || get_write_bytes() > 10240);
    return len;
}

bool HttpConn::process() {
    request_.init();
    if(read_buffer_.readable_bytes() <= 0) {
        return false;
    }
    else if(request_.parse(read_buffer_)) {
        LOG_DEBUG("%s", request_.path().c_str());
        response_.init(src_dir, request_.path(), request_.is_keep_alive(), 200);
    } else {
        response_.init(src_dir, request_.path(), false, 400);
    }

    response_.make_response(write_buffer_);
    iov_[0].iov_base = const_cast<char*>(write_buffer_.readable_view().data());
    iov_[0].iov_len = write_buffer_.readable_bytes();
    iov_count_ = 1;

    if(response_.get_file_len() > 0  && response_.get_file()) {
        iov_[1].iov_base = response_.get_file();
        iov_[1].iov_len = response_.get_file_len();
        iov_count_ = 2;
    }
    LOG_DEBUG("filesize:%d, %d  to %d", response_.get_file_len() , iov_count_, get_write_bytes());
    return true;
}
