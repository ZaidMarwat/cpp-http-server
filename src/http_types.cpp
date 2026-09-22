#include "http_types.h"

#include <cstdio>
#include <ctime>

namespace httpd {

const std::string* Request::header(const std::string& name) const {
    auto it = headers.find(name);
    return it == headers.end() ? nullptr : &it->second;
}

Response Response::text(int status, std::string body) {
    Response r;
    r.status = status;
    r.headers["content-type"] = "text/plain; charset=utf-8";
    r.body = std::move(body);
    return r;
}

Response Response::json(int status, std::string body) {
    Response r;
    r.status = status;
    r.headers["content-type"] = "application/json";
    r.body = std::move(body);
    return r;
}

std::string Response::serialize(bool keep_alive, bool omit_body) const {
    // 1xx, 204 and 304 never carry a body or a content-length.
    const bool bodyless = (status >= 100 && status < 200) || status == 204 || status == 304;

    std::string out;
    out.reserve(128 + headers.size() * 32 + (omit_body || bodyless ? 0 : body.size()));
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += status_reason(status);
    out += "\r\n";

    for (const auto& [name, value] : headers) {
        if (name == "content-length" || name == "connection" || name == "date") continue;  // ours
        out += name;
        out += ": ";
        out += value;
        out += "\r\n";
    }
    out += "date: " + http_date() + "\r\n";
    if (!bodyless) out += "content-length: " + std::to_string(body.size()) + "\r\n";
    out += keep_alive ? "connection: keep-alive\r\n" : "connection: close\r\n";
    out += "\r\n";
    if (!omit_body && !bodyless) out += body;
    return out;
}

std::string status_reason(int status) {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 414: return "URI Too Long";
        case 417: return "Expectation Failed";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default:  return "Unknown";
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(std::string_view s, bool plus_as_space) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '%' && i + 2 < s.size()) {
            int hi = hex_value(s[i + 1]), lo = hex_value(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>(hi * 16 + lo);
                i += 2;
                continue;
            }
        }
        out += (c == '+' && plus_as_space) ? ' ' : c;
    }
    return out;
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string http_date() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%a, %d %b %Y %H:%M:%S GMT", &tm);
    return buf;
}

}  // namespace httpd
