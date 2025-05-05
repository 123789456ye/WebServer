/*
 * @Author       : mark
 * @Date         : 2020-06-27
 * @copyleft Apache 2.0
 */ 
#include "httpresponse.h"

using std::unordered_map;
using std::string;
using std::string_view;


const unordered_map<string, string> HttpResponse::SUFFIX_TYPE = {
    { ".html",  "text/html" },
    { ".xml",   "text/xml" },
    { ".xhtml", "application/xhtml+xml" },
    { ".txt",   "text/plain" },
    { ".rtf",   "application/rtf" },
    { ".pdf",   "application/pdf" },
    { ".word",  "application/nsword" },
    { ".png",   "image/png" },
    { ".gif",   "image/gif" },
    { ".jpg",   "image/jpeg" },
    { ".jpeg",  "image/jpeg" },
    { ".au",    "audio/basic" },
    { ".mpeg",  "video/mpeg" },
    { ".mpg",   "video/mpeg" },
    { ".avi",   "video/x-msvideo" },
    { ".gz",    "application/x-gzip" },
    { ".tar",   "application/x-tar" },
    { ".css",   "text/css "},
    { ".js",    "text/javascript "},
};

const unordered_map<int, string> HttpResponse::CODE_STATUS = {
    { 200, "OK" },
    { 400, "Bad Request" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
};

const unordered_map<int, string> HttpResponse::CODE_PATH = {
    { 400, "/400.html" },
    { 403, "/403.html" },
    { 404, "/404.html" },
};

HttpResponse::HttpResponse() {
    code_ = -1;
    path_ = src_dir_ = "";
    is_keep_alive_ = false;
    mm_file_ = nullptr; 
    mm_file_stat_ = { 0 };
};

HttpResponse::~HttpResponse() {
    unmap_file();
}

void HttpResponse::init(const string& src_dir, string& path, bool is_keep_alive, int code){
    assert(src_dir != "");
    unmap_file(); 
    code_ = code;
    is_keep_alive_ = is_keep_alive;
    path_ = path;
    src_dir_ = src_dir;
    mm_file_ = nullptr; 
    mm_file_stat_ = { 0 };
}

void HttpResponse::make_response(Buffer& buffer) {
    if(stat((src_dir_ + path_).data(), &mm_file_stat_) < 0 || S_ISDIR(mm_file_stat_.st_mode)) {
        code_ = 404;
    }
    else if(!(mm_file_stat_.st_mode & S_IROTH)) {
        code_ = 403;
    }
    else if(code_ == -1) { 
        code_ = 200; 
    }
    handle_error_page();
    add_status_line_(buffer);
    add_header_(buffer);
    add_content_(buffer);
}

char* HttpResponse::get_file() {
    return mm_file_;
}

size_t HttpResponse::get_file_len() const {
    return mm_file_stat_.st_size;
}

void HttpResponse::handle_error_page() {
    if(CODE_PATH.count(code_)) {
        path_ = CODE_PATH.at(code_);
        stat((src_dir_ + path_).c_str(), &mm_file_stat_);
    }
}

void HttpResponse::add_status_line_(Buffer& buffer) {
    string status;
    if(!CODE_STATUS.count(code_)) {
        code_ = 400;
    }
    status = CODE_STATUS.at(code_);
    buffer.append("HTTP/1.1 " + std::to_string(code_) + " " + status + "\r\n");
}

void HttpResponse::add_header_(Buffer& buffer) {
    buffer.append("Connection: ");
    if(is_keep_alive_) {
        buffer.append("keep-alive\r\n");
        buffer.append("keep-alive: max=6, timeout=120\r\n");
    } else{
        buffer.append("close\r\n");
    }
    buffer.append("Content-type: " + get_file_type_() + "\r\n");
}

void HttpResponse::add_content_(Buffer& buffer) {
    string file_path = src_dir_ + path_;
    int srcFd = open(file_path.c_str(), O_RDONLY);
    if(srcFd < 0) { 
        error_content(buffer, "File NotFound!");
        return; 
    }

    LOG_DEBUG("file path %s", file_path.c_str());
    void* mmRet = mmap(0, mm_file_stat_.st_size, PROT_READ, MAP_PRIVATE, srcFd, 0);
    close(srcFd);
    if(mmRet == MAP_FAILED) {
        error_content(buffer, "File NotFound!");
        return; 
    }
    mm_file_ = static_cast<char*>(mmRet);
    buffer.append("Content-length: " + std::to_string(mm_file_stat_.st_size) + "\r\n\r\n");
}

void HttpResponse::unmap_file() {
    if(mm_file_) {
        munmap(mm_file_, mm_file_stat_.st_size);
        mm_file_ = nullptr;
    }
}

string HttpResponse::get_file_type_() {
    std::filesystem::path file_path(path_);
    std::string ext = file_path.extension().string();
    if(ext.empty()) {
        return "text/plain";
    }
    if(SUFFIX_TYPE.count(ext)) {
        return SUFFIX_TYPE.at(ext);
    }
    return "text/plain";
}

void HttpResponse::error_content(Buffer& buffer, string message) 
{
    string body;
    string status;
    body += "<html><title>Error</title>";
    body += "<body bgcolor=\"ffffff\">";
    if(CODE_STATUS.count(code_)) {
        status = CODE_STATUS.at(code_);
    } else {
        status = "Bad Request";
    }
    body += std::to_string(code_) + " : " + status  + "\n";
    body += "<p>" + message + "</p>";
    body += "<hr><em>TinyWebServer</em></body></html>";

    buffer.append("Content-length: " + std::to_string(body.size()) + "\r\n\r\n");
    buffer.append(body);
}
