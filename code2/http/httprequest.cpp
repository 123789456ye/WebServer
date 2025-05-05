#include "httprequest.h"

using std::unordered_map;
using std::unordered_set;
using std::string;
using std::string_view;
using std::regex;
using std::cmatch;
using std::regex_match;

const unordered_set<string_view>HttpRequest::DEFAULT_HTML{
            "/index", "/register", "/login",
             "/welcome", "/video", "/picture", };

const unordered_map<string_view, int> HttpRequest::DEFAULT_HTML_TAG {
            {"/register.html", 0}, {"/login.html", 1},  };

void HttpRequest::init() {
    method_.clear();
    path_.clear();
    version_.clear();
    body_.clear();
    state_ = ParseState::REQUEST_LINE;
    header_.clear();
    post_data_.clear();
}

bool HttpRequest::is_keep_alive() const {
    auto it = header_.find("Connection");
    if(it != header_.end()) {
        return it->second == "keep-alive" && version_ == "1.1";
    }
    return false;
}

bool HttpRequest::parse(Buffer& buffer) {
    static constexpr string_view CRLF = "\r\n";

    if(buffer.readable_bytes() == 0) return false;
    
    while(buffer.readable_bytes() && state_ != ParseState::FINISH) {
        const char* line_end = std::search(
        buffer.readable_view().data(), 
        buffer.readable_view().data() + buffer.readable_bytes(),
        CRLF.data(), 
        CRLF.data() + 2);
        string_view line(buffer.readable_view().data(), line_end - buffer.readable_view().data());

        switch(state_) {
            case ParseState::REQUEST_LINE:
                if(!parse_request_line(line)) return false;
                process_path();
                break;
            case ParseState::HEADERS:
                parse_header(line);
                if(buffer.readable_bytes() <= 2) state_ = ParseState::FINISH;
                break;
            case ParseState::BODY:
                parse_body(line);
                break;
            default:
                break;
        }

        if(line_end == buffer.readable_view().data() + buffer.readable_bytes()) break;

        buffer.retrieve(line.size() + 2);
    }
    LOG_DEBUG("[%s], [%s], [%s]", method_.c_str(), path_.c_str(), version_.c_str());
    return true;
}

void HttpRequest::process_path() {
    if(path_ == "/") {
        path_ = "/index.html"; 
    }
    else {
        for(auto &item: DEFAULT_HTML) {
            if(item == path_) {
                path_ += ".html";
                break;
            }
        }
    }
}

bool HttpRequest::parse_request_line(string_view line) {
    regex patten("^([^ ]*) ([^ ]*) HTTP/([^ ]*)$");
    cmatch match;
    if(regex_match(line.begin(), line.end(), match, patten)) {   
        method_ = match[1].str();
        path_ = match[2].str();
        version_ = match[3].str();
        state_ = ParseState::HEADERS;
        return true;
    }
    LOG_ERROR("RequestLine Error");
    return false;
}

void HttpRequest::parse_header(string_view line) {
    regex patten("^([^:]*): ?(.*)$");
    cmatch match;
    if(regex_match(line.begin(), line.end(), match, patten)) {
        header_[match[1].str()] = match[2].str();
    }
    else {
        state_ = ParseState::BODY;
    }
}

void HttpRequest::parse_body(string_view line) {
    body_ = line;
    process_post();
    state_ = ParseState::FINISH;
    LOG_DEBUG("Body:%s, len:%d", body_.c_str(), body_.size());
}

int HttpRequest::convert_hex(char ch) {
    if(ch >= 'A' && ch <= 'F') return ch -'A' + 10;
    if(ch >= 'a' && ch <= 'f') return ch -'a' + 10;
    return ch;
}

void HttpRequest::process_post() {
    if(method_ == "POST" && header_["Content-Type"] == "application/x-www-form-urlencoded") {
        parse_url_encoded();
        auto it = DEFAULT_HTML_TAG.find(path_);
        if(it != DEFAULT_HTML_TAG.end()) {
            int tag = it->second;
            LOG_DEBUG("Tag:%d", tag);
            if(tag == 0 || tag == 1) {
                bool isLogin = (tag == 1);
                if(verify_user(post_data_["username"], post_data_["password"], isLogin)) {
                    path_ = "/welcome.html";
                } 
                else {
                    path_ = "/error.html";
                }
            }
        }
    }   
}

void HttpRequest::parse_url_encoded() {
    if(body_.empty()) return;

    string key, value;
    int num = 0;
    size_t n = body_.size();
    size_t i = 0, j = 0;

    for(; i < n; i++) {
        char ch = body_[i];
        switch (ch) {
        case '=':
            key = body_.substr(j, i - j);
            j = i + 1;
            break;
        case '+':
            body_[i] = ' ';
            break;
        case '%':
            num = convert_hex(body_[i + 1]) * 16 + convert_hex(body_[i + 2]);
            body_[i + 2] = num % 10 + '0';
            body_[i + 1] = num / 10 + '0';
            i += 2;
            break;
        case '&':
            value = body_.substr(j, i - j);
            j = i + 1;
            post_data_[key] = value;
            LOG_DEBUG("%s = %s", key.c_str(), value.c_str());
            break;
        default:
            break;
        }
    }
    assert(j <= i);
    if(post_data_.count(key) == 0 && j < i) {
        value = body_.substr(j, i - j);
        post_data_[key] = value;
    }
}

bool HttpRequest::verify_user(string_view name, string_view pwd, bool isLogin) {
    if(name == "" || pwd == "") { return false; }
    LOG_INFO("Verify name:%s pwd:%s", string(name).c_str(), string(pwd).c_str());
    MYSQL* sql;
    SqlConnRAII(&sql,  SqlConnPool::instance());
    assert(sql);
    
    bool flag = false;
    unsigned int j = 0;
    char order[256] = { 0 };
    MYSQL_FIELD *fields = nullptr;
    MYSQL_RES *res = nullptr;
    
    if(!isLogin) { flag = true; }

    snprintf(order, 256, "SELECT username, password FROM user WHERE username='%s' LIMIT 1", string(name).c_str());
    LOG_DEBUG("%s", order);

    if(mysql_query(sql, order)) { 
        mysql_free_result(res);
        return false; 
    }
    res = mysql_store_result(sql);
    j = mysql_num_fields(res);
    fields = mysql_fetch_fields(res);

    while(MYSQL_ROW row = mysql_fetch_row(res)) {
        LOG_DEBUG("MYSQL ROW: %s %s", row[0], row[1]);
        string password(row[1]);
        if(isLogin) {
            if(pwd == password) { flag = true; }
            else {
                flag = false;
                LOG_DEBUG("pwd error!");
            }
        } 
        else { 
            flag = false; 
            LOG_DEBUG("user used!");
        }
    }
    mysql_free_result(res);

    if(!isLogin && flag == true) {
        LOG_DEBUG("register!");
        bzero(order, 256);
        snprintf(order, 256,"INSERT INTO user(username, password) VALUES('%s','%s')", string(name).c_str(), string(pwd).c_str());
        LOG_DEBUG( "%s", order);
        if(mysql_query(sql, order)) { 
            LOG_DEBUG( "Insert error!");
            flag = false; 
        }
        flag = true;
    }
    SqlConnPool::instance()->free_conn(sql);
    LOG_DEBUG( "UserVerify %s", flag ? "succeded" : "failed");
    return flag;
}

string_view HttpRequest::get_post(string_view key) const {
    assert(key != "");
    auto it = post_data_.find(string(key));
    if(it != post_data_.end()) {
        return it->second;
    }
    return "";
}