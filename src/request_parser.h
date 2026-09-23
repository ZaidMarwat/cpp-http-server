#pragma once

// Incremental HTTP/1.1 request parser.
//
// Bytes are appended with feed() as they arrive from the socket, and parse()
// pulls complete requests out one at a time. Anything after a complete request
// (a pipelined follow-up) stays buffered for the next parse() call.
//
// The parser is deliberately strict about message framing, because framing
// disagreements between a proxy and a server are what request smuggling
// exploits: Content-Length must be plain digits, duplicate lengths must agree,
// Content-Length together with Transfer-Encoding is rejected, and header names
// may not contain whitespace.

#include <cstddef>
#include <stdexcept>
#include <string>

#include "http_types.h"

namespace httpd {

struct ParserLimits {
    size_t max_request_line = 8 * 1024;
    size_t max_header_bytes = 32 * 1024;
    size_t max_headers = 100;
    size_t max_body = 16 * 1024 * 1024;
};

// Thrown on malformed or over-limit input. `status` is the HTTP status the
// server should answer with before closing the connection.
class ParseError : public std::runtime_error {
public:
    ParseError(int status, const std::string& msg) : std::runtime_error(msg), status_(status) {}
    int status() const { return status_; }

private:
    int status_;
};

class RequestParser {
public:
    RequestParser() = default;
    explicit RequestParser(ParserLimits limits) : limits_(limits) {}

    void feed(const char* data, size_t n) { buf_.append(data, n); }

    // Returns true and fills `out` when a complete request is available.
    // Returns false when more bytes are needed. Throws ParseError.
    bool parse(Request& out);

    // True exactly once per request, after the headers of a request carrying
    // "Expect: 100-continue" have been parsed and its body has not yet fully
    // arrived. The server then sends an interim "100 Continue".
    bool take_continue();

    // True when some bytes of a not-yet-complete request are buffered.
    bool mid_request() const { return state_ != State::RequestLine || pos_ < buf_.size(); }

private:
    enum class State { RequestLine, Headers, FixedBody, ChunkSize, ChunkData, ChunkEnd, Trailers };

    ParserLimits limits_;
    std::string buf_;
    size_t pos_ = 0;  // start of unconsumed bytes in buf_

    State state_ = State::RequestLine;
    Request req_;
    size_t header_bytes_ = 0;
    size_t header_count_ = 0;
    size_t remaining_ = 0;  // body bytes left (fixed body or current chunk)
    bool expect_continue_ = false;

    bool take_line(std::string& line, size_t limit, int status, const char* what);
    void parse_request_line(const std::string& line);
    void parse_header_line(const std::string& line);
    void finish_headers();
    void parse_chunk_size(const std::string& line);
    void reset_message();
};

}  // namespace httpd
