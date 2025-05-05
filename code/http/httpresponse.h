#pragma once

#include <unordered_map>
#include <filesystem>
#include <fcntl.h>       
#include <unistd.h>      
#include <sys/stat.h>    
#include <sys/mman.h>    

#include "../buffer/buffer.h"
#include "../log/log.h"

class HttpResponse {
public:
    HttpResponse();
    ~HttpResponse();

    void init(const std::string& src_dir, std::string& path, bool is_keep_alive = false, int code = -1);
    void make_response(Buffer& buffer);
    void unmap_file();
    char* get_file();
    size_t get_file_len() const;
    void error_content(Buffer& buffer, std::string message);
    int code() const { return code_; }

private:
    void add_status_line_(Buffer &buff);
    void add_header_(Buffer &buff);
    void add_content_(Buffer &buff);

    void handle_error_page();
    std::string get_file_type_();

    int code_;
    bool is_keep_alive_;

    std::string path_;
    std::string src_dir_;
    
    char* mm_file_; 
    struct stat mm_file_stat_;

    static const std::unordered_map<std::string, std::string> SUFFIX_TYPE;
    static const std::unordered_map<int, std::string> CODE_STATUS;
    static const std::unordered_map<int, std::string> CODE_PATH;
};
