#include "request_parser.h"

#include <cctype>
#include <limits>

namespace httpd {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim_ows(const std::string& s) {  // optional whitespace: SP / HTAB
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

// RFC 9110 "tchar": the characters allowed in a token (method, header name).
bool is_tchar(unsigned char c) {
    if (std::isalnum(c)) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
        case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

bool is_token(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!is_tchar(c)) return false;
    return true;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Strictly digits, no sign, no whitespace, no overflow.
bool parse_decimal(const std::string& s, size_t& out) {
    if (s.empty() || s.size() > 19) return false;
    size_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<size_t>(c - '0');
    }
    out = v;
    return true;
}

// True if the comma-separated header value contains `token` (case-insensitive).
bool has_token(const std::string& value, const std::string& token) {
    size_t pos = 0;
    while (pos <= value.size()) {
        size_t comma = value.find(',', pos);
        std::string item = lower(trim_ows(value.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos)));
        if (item == token) return true;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

void parse_query(const std::string& qs, std::map<std::string, std::string>& out) {
    size_t pos = 0;
    while (pos <= qs.size()) {
        size_t amp = qs.find('&', pos);
        std::string pair = qs.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            if (eq == std::string::npos) out[url_decode(pair, true)] = "";
            else out[url_decode(pair.substr(0, eq), true)] = url_decode(pair.substr(eq + 1), true);
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
}

}  // namespace

// Pulls one CRLF-terminated line out of the buffer. Bare CR, bare LF and NUL
// inside a line are rejected rather than guessed at.
bool RequestParser::take_line(std::string& line, size_t limit, int status, const char* what) {
    size_t eol = buf_.find("\r\n", pos_);
    if (eol == std::string::npos) {
        if (buf_.size() - pos_ > limit) throw ParseError(status, std::string(what) + " too long");
        return false;
    }
    if (eol - pos_ > limit) throw ParseError(status, std::string(what) + " too long");
    line.assign(buf_, pos_, eol - pos_);
    pos_ = eol + 2;
    for (char c : line)
        if (c == '\r' || c == '\n' || c == '\0') throw ParseError(400, "invalid character in " + std::string(what));
    return true;
}

void RequestParser::parse_request_line(const std::string& line) {
    // method SP request-target SP HTTP-version, single spaces only.
    size_t sp1 = line.find(' ');
    size_t sp2 = sp1 == std::string::npos ? std::string::npos : line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos || line.find(' ', sp2 + 1) != std::string::npos)
        throw ParseError(400, "malformed request line");

    req_.method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    req_.version = line.substr(sp2 + 1);

    if (!is_token(req_.method)) throw ParseError(400, "invalid method");
    if (req_.version != "HTTP/1.1" && req_.version != "HTTP/1.0") {
        if (req_.version.rfind("HTTP/", 0) == 0) throw ParseError(505, "unsupported HTTP version");
        throw ParseError(400, "malformed request line");
    }
    if (target.empty() || target[0] != '/') throw ParseError(400, "request target must be an absolute path");

    size_t mark = target.find('?');
    req_.raw_path = target.substr(0, mark);
    req_.path = url_decode(req_.raw_path, false);
    if (mark != std::string::npos) parse_query(target.substr(mark + 1), req_.query);
}

void RequestParser::parse_header_line(const std::string& line) {
    if (line[0] == ' ' || line[0] == '\t') throw ParseError(400, "obsolete header line folding");
    size_t colon = line.find(':');
    if (colon == std::string::npos) throw ParseError(400, "malformed header line");
    std::string name = line.substr(0, colon);
    if (!is_token(name)) throw ParseError(400, "invalid header name");  // also rejects "Name :"
    if (++header_count_ > limits_.max_headers) throw ParseError(431, "too many headers");

    name = lower(name);
    std::string value = trim_ows(line.substr(colon + 1));

    auto it = req_.headers.find(name);
    if (it == req_.headers.end()) {
        req_.headers.emplace(std::move(name), std::move(value));
    } else if (name == "content-length") {
        if (it->second != value) throw ParseError(400, "conflicting content-length headers");
    } else {
        it->second += ", " + value;
    }
}

void RequestParser::finish_headers() {
    const std::string* te = req_.header("transfer-encoding");
    const std::string* cl = req_.header("content-length");

    if (te) {
        if (cl) throw ParseError(400, "both content-length and transfer-encoding");
        if (req_.version == "HTTP/1.0") throw ParseError(400, "transfer-encoding in HTTP/1.0");
        if (lower(trim_ows(*te)) != "chunked") throw ParseError(501, "unsupported transfer-encoding");
        state_ = State::ChunkSize;
    } else if (cl) {
        size_t n = 0;
        if (!parse_decimal(*cl, n)) throw ParseError(400, "invalid content-length");
        if (n > limits_.max_body) throw ParseError(413, "body too large");
        remaining_ = n;
        state_ = State::FixedBody;
    } else {
        remaining_ = 0;
        state_ = State::FixedBody;
    }

    const std::string* conn = req_.header("connection");
    if (req_.version == "HTTP/1.1") req_.keep_alive = !(conn && has_token(*conn, "close"));
    else req_.keep_alive = conn && has_token(*conn, "keep-alive");

    if (const std::string* expect = req_.header("expect")) {
        if (lower(*expect) != "100-continue") throw ParseError(417, "unsupported expectation");
        bool body_expected = state_ == State::ChunkSize || remaining_ > 0;
        expect_continue_ = body_expected && req_.version == "HTTP/1.1";
    }
}

void RequestParser::parse_chunk_size(const std::string& line) {
    size_t semi = line.find(';');  // chunk extensions are allowed and ignored
    std::string hex = trim_ows(line.substr(0, semi));
    if (hex.empty() || hex.size() > 15) throw ParseError(400, "invalid chunk size");
    size_t n = 0;
    for (char c : hex) {
        int d = hex_digit(c);
        if (d < 0) throw ParseError(400, "invalid chunk size");
        n = n * 16 + static_cast<size_t>(d);
    }
    if (n > limits_.max_body || req_.body.size() + n > limits_.max_body) throw ParseError(413, "body too large");
    remaining_ = n;
    state_ = n == 0 ? State::Trailers : State::ChunkData;
}

bool RequestParser::take_continue() {
    bool v = expect_continue_;
    expect_continue_ = false;
    return v;
}

void RequestParser::reset_message() {
    req_ = Request{};
    state_ = State::RequestLine;
    header_bytes_ = 0;
    header_count_ = 0;
    remaining_ = 0;
    expect_continue_ = false;
}

bool RequestParser::parse(Request& out) {
    if (pos_ >= 64 * 1024) {  // drop consumed bytes during long uploads
        buf_.erase(0, pos_);
        pos_ = 0;
    }
    std::string line;
    for (;;) {
        switch (state_) {
            case State::RequestLine: {
                // RFC 9112 2.2: ignore empty lines before the request line.
                while (buf_.compare(pos_, 2, "\r\n") == 0) pos_ += 2;
                if (!take_line(line, limits_.max_request_line, 414, "request line")) return false;
                parse_request_line(line);
                state_ = State::Headers;
                break;
            }
            case State::Headers: {
                size_t budget = limits_.max_header_bytes - header_bytes_;
                if (!take_line(line, budget, 431, "header section")) return false;
                header_bytes_ += line.size() + 2;
                if (header_bytes_ > limits_.max_header_bytes) throw ParseError(431, "header section too long");
                if (line.empty()) finish_headers();
                else parse_header_line(line);
                break;
            }
            case State::FixedBody: {
                size_t avail = buf_.size() - pos_;
                if (avail < remaining_) return false;
                req_.body.assign(buf_, pos_, remaining_);
                pos_ += remaining_;
                goto complete;
            }
            case State::ChunkSize: {
                if (!take_line(line, 1024, 400, "chunk size line")) return false;
                parse_chunk_size(line);
                break;
            }
            case State::ChunkData: {
                size_t take = std::min(remaining_, buf_.size() - pos_);
                req_.body.append(buf_, pos_, take);
                pos_ += take;
                remaining_ -= take;
                if (remaining_ > 0) return false;
                state_ = State::ChunkEnd;
                break;
            }
            case State::ChunkEnd: {
                if (buf_.size() - pos_ < 2) return false;
                if (buf_.compare(pos_, 2, "\r\n") != 0) throw ParseError(400, "chunk not followed by CRLF");
                pos_ += 2;
                state_ = State::ChunkSize;
                break;
            }
            case State::Trailers: {
                size_t budget = limits_.max_header_bytes - std::min(header_bytes_, limits_.max_header_bytes);
                if (!take_line(line, budget, 431, "trailer section")) return false;
                header_bytes_ += line.size() + 2;
                if (line.empty()) goto complete;  // trailer fields are read and discarded
                break;
            }
        }
    }

complete:
    out = std::move(req_);
    reset_message();
    buf_.erase(0, pos_);  // keep only pipelined bytes
    pos_ = 0;
    return true;
}

}  // namespace httpd
