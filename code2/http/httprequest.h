#pragma once

#include <unordered_map>
#include <unordered_set>
#include <string>
#include <string_view>
#include <regex>
#include <mysql/mysql.h>

#include "../buffer/buffer.h"
#include "../log/log.h"
#include "../pool/sqlconnpool.h"
#include "../pool/sqlconnRAII.h"

class HttpRequest {
public:
    enum class ParseState {
        REQUEST_LINE,
        HEADERS,
        BODY,
        FINISH
    };

    enum class HttpCode {
        NO_REQUEST = 0,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    
    HttpRequest() { init(); }
    ~HttpRequest() = default;

    void init();
    
    bool parse(Buffer& buffer);

    std::string_view path() const { return path_; }
    std::string& path() { return path_; }
    std::string_view method() const { return method_; }
    std::string_view version() const { return version_; }
    
    std::string_view get_post(std::string_view key) const;
    
    bool is_keep_alive() const;

private:
    bool parse_request_line(std::string_view line);
    void parse_header(std::string_view line);
    void parse_body(std::string_view line);
    
    void process_path();
    void process_post();
    
    void parse_url_encoded();
    
    static bool verify_user(std::string_view name, std::string_view pwd, bool is_login);
    
    static int convert_hex(char ch);

    ParseState state_ = ParseState::REQUEST_LINE;
    
    std::string method_;
    std::string path_;
    std::string version_;
    std::string body_;
    
    std::unordered_map<std::string, std::string> header_;
    std::unordered_map<std::string, std::string> post_data_;

    static const std::unordered_set<std::string_view> DEFAULT_HTML;
    static const std::unordered_map<std::string_view, int> DEFAULT_HTML_TAG;
};
