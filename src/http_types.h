#pragma once

// Core HTTP/1.1 message types shared by the parser, router and server.

#include <map>
#include <string>
#include <string_view>

namespace httpd {

// Header names are stored lower-cased. Repeated headers are joined with ", "
// as RFC 9110 section 5.3 allows.
using Headers = std::map<std::string, std::string>;

struct Request {
    std::string method;       // "GET", "POST", ...
    std::string version;      // "HTTP/1.1" or "HTTP/1.0"
    std::string raw_path;     // still percent-encoded, no query string
    std::string path;         // percent-decoded raw_path (for display/logging)
    std::map<std::string, std::string> query;   // decoded, '+' means space
    std::map<std::string, std::string> params;  // filled in by the Router
    Headers headers;
    std::string body;         // de-chunked if it arrived chunked
    bool keep_alive = true;   // from version + Connection header

    // Returns nullptr when absent. `name` must be lower-case.
    const std::string* header(const std::string& name) const;
};

struct Response {
    int status = 200;
    Headers headers;
    std::string body;

    static Response text(int status, std::string body);
    static Response json(int status, std::string body);

    // Wire bytes for an HTTP/1.1 response. `omit_body` is for HEAD: the
    // headers (including content-length) describe the body that a GET would
    // have returned, but the body itself is not sent.
    std::string serialize(bool keep_alive, bool omit_body = false) const;
};

std::string status_reason(int status);

// Percent-decodes `s`. Invalid escapes are kept literally. '+' becomes a
// space only when `plus_as_space` is set (query strings, not paths).
std::string url_decode(std::string_view s, bool plus_as_space);

// Escapes a string for inclusion inside a JSON string literal.
std::string json_escape(std::string_view s);

// Current time as an IMF-fixdate, e.g. "Sun, 06 Nov 1994 08:49:37 GMT".
std::string http_date();

}  // namespace httpd
