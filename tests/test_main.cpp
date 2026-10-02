// Test suite: parser, response, router, and end-to-end over real loopback
// sockets (including the event loop's timeouts and idle-connection handling).

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "request_parser.h"
#include "router.h"
#include "server.h"

using namespace httpd;

// ---- tiny harness -----------------------------------------------------------

static int g_checks = 0, g_failures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++g_checks;                                                                   \
        if (!(cond)) {                                                                \
            ++g_failures;                                                             \
            std::printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                             \
    } while (0)

static void run(const char* name, const std::function<void()>& fn) {
    int before = g_failures;
    try {
        fn();
    } catch (const std::exception& e) {
        ++g_failures;
        std::printf("    FAIL unexpected exception: %s\n", e.what());
    }
    std::printf("%s  %s\n", g_failures == before ? "ok  " : "FAIL", name);
}

// Parses `wire` in one feed; returns the ParseError status, or 0 on success.
static int parse_status(const std::string& wire, Request* out = nullptr, ParserLimits limits = {}) {
    RequestParser p(limits);
    p.feed(wire.data(), wire.size());
    Request req;
    try {
        if (!p.parse(req)) return -1;  // incomplete
    } catch (const ParseError& e) {
        return e.status();
    }
    if (out) *out = req;
    return 0;
}

// ---- parser -----------------------------------------------------------------

static void parser_tests() {
    run("parses a simple GET with query and headers", [] {
        Request r;
        CHECK(parse_status("GET /hello?a=1&b=two+words&flag HTTP/1.1\r\nHost: x\r\nX-Thing:  v  \r\n\r\n", &r) == 0);
        CHECK(r.method == "GET" && r.path == "/hello" && r.version == "HTTP/1.1");
        CHECK(r.query["a"] == "1" && r.query["b"] == "two words" && r.query.count("flag"));
        CHECK(r.headers["host"] == "x" && r.headers["x-thing"] == "v");
    });

    run("handles byte-at-a-time feeds", [] {
        std::string wire = "POST /submit HTTP/1.1\r\nContent-Length: 11\r\n\r\nhello world";
        RequestParser p;
        Request r;
        bool done = false;
        for (char c : wire) {
            CHECK(!done);
            p.feed(&c, 1);
            done = p.parse(r);
        }
        CHECK(done && r.body == "hello world");
    });

    run("'+' is literal in paths, a space in queries", [] {
        Request r;
        CHECK(parse_status("GET /a+b%20c?q=a+b%2Bc HTTP/1.1\r\n\r\n", &r) == 0);
        CHECK(r.path == "/a+b c" && r.raw_path == "/a+b%20c");
        CHECK(r.query["q"] == "a b+c");
    });

    run("skips blank lines before the request line", [] {
        Request r;
        CHECK(parse_status("\r\n\r\nGET / HTTP/1.1\r\n\r\n", &r) == 0 && r.path == "/");
    });

    run("rejects malformed request lines with the right status", [] {
        CHECK(parse_status("NOTAREQUEST\r\n\r\n") == 400);
        CHECK(parse_status("GET  / HTTP/1.1\r\n\r\n") == 400);         // double space
        CHECK(parse_status("GET http://x/ HTTP/1.1\r\n\r\n") == 400);   // not origin-form
        CHECK(parse_status("G(T / HTTP/1.1\r\n\r\n") == 400);           // bad method token
        CHECK(parse_status("GET / HTTP/2.0\r\n\r\n") == 505);
        CHECK(parse_status("GET /a\nb HTTP/1.1\r\n\r\n") == 400);       // bare LF
    });

    run("enforces size limits (414, 431, 413)", [] {
        ParserLimits small;
        small.max_request_line = 32;
        small.max_header_bytes = 64;
        small.max_headers = 3;
        small.max_body = 10;
        CHECK(parse_status("GET /" + std::string(100, 'a') + " HTTP/1.1\r\n\r\n", nullptr, small) == 414);
        CHECK(parse_status("GET / HTTP/1.1\r\nX: " + std::string(100, 'a') + "\r\n\r\n", nullptr, small) == 431);
        CHECK(parse_status("GET / HTTP/1.1\r\nA: 1\r\nB: 2\r\nC: 3\r\nD: 4\r\n\r\n", nullptr, small) == 431);
        CHECK(parse_status("POST / HTTP/1.1\r\nContent-Length: 11\r\n\r\n", nullptr, small) == 413);
        CHECK(parse_status("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nb\r\n", nullptr, small) == 413);
        // Limits trip before the whole oversized line has even arrived.
        CHECK(parse_status("GET /" + std::string(100, 'a'), nullptr, small) == 414);
    });

    run("strict Content-Length framing (anti request smuggling)", [] {
        CHECK(parse_status("POST / HTTP/1.1\r\nContent-Length: 12abc\r\n\r\n") == 400);
        CHECK(parse_status("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n") == 400);
        CHECK(parse_status("POST / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 4\r\n\r\nabcd") == 400);
        CHECK(parse_status("POST / HTTP/1.1\r\nContent-Length: 3\r\nContent-Length: 3\r\n\r\nabc") == 0);
        CHECK(parse_status("POST / HTTP/1.1\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n") == 400);
        CHECK(parse_status("POST / HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n") == 501);
        CHECK(parse_status("GET / HTTP/1.1\r\nHost : x\r\n\r\n") == 400);    // space before colon
        CHECK(parse_status("GET / HTTP/1.1\r\nA: 1\r\n folded\r\n\r\n") == 400);  // obs-fold
    });

    run("repeated headers are joined with ', '", [] {
        Request r;
        CHECK(parse_status("GET / HTTP/1.1\r\nAccept: a\r\naccept: b\r\n\r\n", &r) == 0);
        CHECK(r.headers["accept"] == "a, b");
    });

    run("decodes chunked bodies with extensions and trailers", [] {
        Request r;
        std::string wire =
            "POST /up HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
            "5;name=value\r\nhello\r\n"
            "6\r\n world\r\n"
            "0\r\nX-Trailer: ignored\r\n\r\n";
        CHECK(parse_status(wire, &r) == 0);
        CHECK(r.body == "hello world");
    });

    run("chunked body survives byte-at-a-time feeds", [] {
        std::string wire = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\nA\r\n0123456789\r\n0\r\n\r\n";
        RequestParser p;
        Request r;
        bool done = false;
        for (char c : wire) {
            p.feed(&c, 1);
            done = p.parse(r);
        }
        CHECK(done && r.body == "abc0123456789");
    });

    run("rejects broken chunk framing", [] {
        CHECK(parse_status("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n") == 400);
        CHECK(parse_status("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcX\r\n") == 400);
    });

    run("pipelined requests come out one at a time", [] {
        RequestParser p;
        std::string wire = "GET /one HTTP/1.1\r\n\r\nPOST /two HTTP/1.1\r\nContent-Length: 2\r\n\r\nhiGET /th";
        p.feed(wire.data(), wire.size());
        Request a, b, c;
        CHECK(p.parse(a) && a.path == "/one");
        CHECK(p.parse(b) && b.path == "/two" && b.body == "hi");
        CHECK(!p.parse(c) && p.mid_request());
        std::string rest = "ree HTTP/1.1\r\n\r\n";
        p.feed(rest.data(), rest.size());
        CHECK(p.parse(c) && c.path == "/three" && !p.mid_request());
    });

    run("keep-alive defaults follow the HTTP version", [] {
        Request r;
        CHECK(parse_status("GET / HTTP/1.1\r\n\r\n", &r) == 0 && r.keep_alive);
        CHECK(parse_status("GET / HTTP/1.1\r\nConnection: Close\r\n\r\n", &r) == 0 && !r.keep_alive);
        CHECK(parse_status("GET / HTTP/1.0\r\n\r\n", &r) == 0 && !r.keep_alive);
        CHECK(parse_status("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", &r) == 0 && r.keep_alive);
    });

    run("Expect: 100-continue is reported once, only while the body is pending", [] {
        RequestParser p;
        std::string head = "POST / HTTP/1.1\r\nContent-Length: 3\r\nExpect: 100-continue\r\n\r\n";
        p.feed(head.data(), head.size());
        Request r;
        CHECK(!p.parse(r));
        CHECK(p.take_continue());
        CHECK(!p.take_continue());
        p.feed("abc", 3);
        CHECK(p.parse(r) && r.body == "abc");
        CHECK(parse_status("POST / HTTP/1.1\r\nContent-Length: 0\r\nExpect: something-else\r\n\r\n") == 417);
    });
}

// ---- response ---------------------------------------------------------------

static void response_tests() {
    run("serializes status line, length, date and connection", [] {
        std::string w = Response::text(200, "hi\n").serialize(true);
        CHECK(w.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
        CHECK(w.find("content-length: 3\r\n") != std::string::npos);
        CHECK(w.find("date: ") != std::string::npos && w.find(" GMT\r\n") != std::string::npos);
        CHECK(w.find("connection: keep-alive\r\n") != std::string::npos);
        CHECK(w.size() >= 7 && w.compare(w.size() - 7, 7, "\r\n\r\nhi\n") == 0);
    });

    run("HEAD keeps the length but drops the body; 204 has neither", [] {
        std::string head = Response::text(200, "hello").serialize(false, true);
        CHECK(head.find("content-length: 5\r\n") != std::string::npos);
        CHECK(head.compare(head.size() - 4, 4, "\r\n\r\n") == 0);
        Response nc;
        nc.status = 204;
        nc.body = "ignored";
        std::string w = nc.serialize(true);
        CHECK(w.find("content-length") == std::string::npos && w.compare(w.size() - 4, 4, "\r\n\r\n") == 0);
    });

    run("json_escape handles quotes, backslashes and control characters", [] {
        CHECK(json_escape("a\"b\\c\n\x01") == "a\\\"b\\\\c\\n\\u0001");
    });
}

// ---- router -----------------------------------------------------------------

static Request make_req(const std::string& method, const std::string& raw_path) {
    Request r;
    r.method = method;
    r.raw_path = raw_path;
    r.path = url_decode(raw_path, false);
    return r;
}

static void router_tests() {
    Router router;
    router.get("/users/{id}", [](const Request& r) { return Response::text(200, "user " + r.params.at("id")); });
    router.get("/users/me", [](const Request&) { return Response::text(200, "me"); });
    router.post("/users/{id}", [](const Request&) { return Response::text(201, "updated"); });
    router.get("/files/{name}", [](const Request& r) { return Response::text(200, r.params.at("name")); });
    router.get("/", [](const Request&) { return Response::text(200, "root"); });

    run("extracts path params", [&] {
        Request r = make_req("GET", "/users/42");
        Response res = router.dispatch(r);
        CHECK(res.status == 200 && res.body == "user 42" && r.params["id"] == "42");
    });

    run("literal segments beat params regardless of registration order", [&] {
        Request r = make_req("GET", "/users/me");
        CHECK(router.dispatch(r).body == "me");
    });

    run("an encoded '/' stays inside its segment", [&] {
        Request r = make_req("GET", "/files/a%2Fb");
        CHECK(router.dispatch(r).body == "a/b");
    });

    run("405 with Allow for a known path, 404 otherwise", [&] {
        Request r = make_req("DELETE", "/users/42");
        Response res = router.dispatch(r);
        CHECK(res.status == 405 && res.headers["allow"] == "GET, HEAD, POST");
        Request m = make_req("GET", "/nope");
        CHECK(router.dispatch(m).status == 404);
        Request e = make_req("GET", "/users/");  // empty param doesn't match
        CHECK(router.dispatch(e).status == 404);
    });

    run("HEAD falls back to the GET handler", [&] {
        Request r = make_req("HEAD", "/");
        CHECK(router.dispatch(r).body == "root");
    });
}

// ---- end to end -------------------------------------------------------------

namespace {

int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    timeval tv{3, 0};  // never let a broken test hang forever
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        throw std::runtime_error(std::string("connect failed: ") + std::strerror(errno));
    }
    return fd;
}

void send_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) throw std::runtime_error(std::string("send failed: ") + std::strerror(errno));
        off += static_cast<size_t>(n);
    }
}

// Reads until the peer closes (or the 3 s receive timeout hits).
std::string read_to_close(int fd) {
    std::string out;
    char buf[8192];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) out.append(buf, static_cast<size_t>(n));
    return out;
}

// Reads until `needle` has been seen `times` times.
std::string read_until(int fd, const std::string& needle, int times = 1) {
    std::string out;
    char buf[8192];
    auto count = [&] {
        int c = 0;
        for (size_t p = out.find(needle); p != std::string::npos; p = out.find(needle, p + 1)) ++c;
        return c;
    };
    while (count() < times) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

std::string exchange(int port, const std::string& wire) {
    int fd = connect_to(port);
    send_all(fd, wire);
    std::string out = read_to_close(fd);
    close(fd);
    return out;
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

struct TestServer {
    Server server;
    std::thread loop;
    TestServer(Router r, ServerOptions o) : server(std::move(r), o), loop([this] { server.run(); }) {}
    ~TestServer() {
        server.stop();
        loop.join();
    }
    int port() const { return server.port(); }
};

}  // namespace

static void end_to_end_tests() {
    Router router;
    router.get("/hello", [](const Request&) { return Response::text(200, "world\n"); });
    router.post("/echo", [](const Request& r) { return Response::text(200, r.body); });
    router.get("/boom", [](const Request&) -> Response { throw std::runtime_error("handler bug"); });
    router.get("/slow", [](const Request&) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return Response::text(200, "slow done\n");
    });

    ServerOptions opts;
    opts.port = 0;  // ephemeral
    opts.workers = 2;
    opts.idle_timeout_ms = 300;
    opts.request_timeout_ms = 300;
    TestServer ts(router, opts);
    const int port = ts.port();

    run("GET, POST and 404 over a real socket", [&] {
        std::string get = exchange(port, "GET /hello HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(contains(get, "200 OK") && contains(get, "\r\n\r\nworld\n"));
        std::string post = exchange(port, "POST /echo HTTP/1.1\r\nContent-Length: 5\r\nConnection: close\r\n\r\nabcde");
        CHECK(contains(post, "\r\n\r\nabcde"));
        CHECK(contains(exchange(port, "GET /missing HTTP/1.1\r\nConnection: close\r\n\r\n"), "404 Not Found"));
    });

    run("chunked upload over a real socket", [&] {
        std::string out = exchange(port,
            "POST /echo HTTP/1.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
            "4\r\nabcd\r\n3\r\nefg\r\n0\r\n\r\n");
        CHECK(contains(out, "content-length: 7\r\n") && contains(out, "\r\n\r\nabcdefg"));
    });

    run("malformed input gets the right status and a closed connection", [&] {
        CHECK(contains(exchange(port, "garbage\r\n\r\n"), "400 Bad Request"));
        CHECK(contains(exchange(port, "GET / HTTP/3.0\r\n\r\n"), "505 HTTP Version Not Supported"));
        CHECK(contains(exchange(port, "POST /echo HTTP/1.1\r\nContent-Length: 1x\r\n\r\n"), "400 Bad Request"));
    });

    run("a throwing handler returns 500 and the server keeps serving", [&] {
        CHECK(contains(exchange(port, "GET /boom HTTP/1.1\r\nConnection: close\r\n\r\n"), "500 Internal Server Error"));
        CHECK(contains(exchange(port, "GET /hello HTTP/1.1\r\nConnection: close\r\n\r\n"), "200 OK"));
    });

    run("keep-alive: sequential requests on one connection", [&] {
        int fd = connect_to(port);
        send_all(fd, "GET /hello HTTP/1.1\r\n\r\n");
        CHECK(contains(read_until(fd, "world\n"), "connection: keep-alive"));
        send_all(fd, "GET /hello HTTP/1.1\r\nConnection: close\r\n\r\n");
        std::string second = read_to_close(fd);
        CHECK(contains(second, "world\n") && contains(second, "connection: close"));
        close(fd);
    });

    run("pipelined requests are answered in order", [&] {
        std::string out = exchange(port,
            "GET /slow HTTP/1.1\r\n\r\n"
            "GET /hello HTTP/1.1\r\n\r\n"
            "POST /echo HTTP/1.1\r\nContent-Length: 3\r\nConnection: close\r\n\r\nend");
        size_t a = out.find("slow done"), b = out.find("world"), c = out.find("\r\n\r\nend");
        CHECK(a != std::string::npos && b != std::string::npos && c != std::string::npos);
        CHECK(a < b && b < c);
    });

    run("a request followed by a half-close is still answered", [&] {
        int fd = connect_to(port);
        send_all(fd, "GET /hello HTTP/1.1\r\n\r\n");
        shutdown(fd, SHUT_WR);
        CHECK(contains(read_to_close(fd), "world\n"));
        close(fd);
    });

    run("HEAD returns headers only", [&] {
        std::string out = exchange(port, "HEAD /hello HTTP/1.1\r\nConnection: close\r\n\r\n");
        CHECK(contains(out, "200 OK") && contains(out, "content-length: 6\r\n"));
        CHECK(!contains(out, "world"));
    });

    run("Expect: 100-continue gets an interim response before the body", [&] {
        int fd = connect_to(port);
        send_all(fd, "POST /echo HTTP/1.1\r\nContent-Length: 4\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n");
        CHECK(contains(read_until(fd, "\r\n\r\n"), "HTTP/1.1 100 Continue"));
        send_all(fd, "data");
        CHECK(contains(read_to_close(fd), "\r\n\r\ndata"));
        close(fd);
    });

    run("idle connections cost no workers (64 idle sockets, 2 workers)", [&] {
        std::vector<int> idle;
        for (int i = 0; i < 64; ++i) idle.push_back(connect_to(port));
        for (int i = 0; i < 8; ++i) {  // half-sent requests too
            int fd = connect_to(port);
            send_all(fd, "GET /hel");
            idle.push_back(fd);
        }
        auto t0 = std::chrono::steady_clock::now();
        std::string out = exchange(port, "GET /hello HTTP/1.1\r\nConnection: close\r\n\r\n");
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(contains(out, "world\n"));
        CHECK(ms < 250);  // well under the 300 ms timeouts: not waiting on idle clients
        for (int fd : idle) close(fd);
    });

    run("idle keep-alive connections are closed after the idle timeout", [&] {
        int fd = connect_to(port);
        auto t0 = std::chrono::steady_clock::now();
        std::string out = read_to_close(fd);  // returns when the server hangs up
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(out.empty());
        CHECK(ms >= 250 && ms < 2000);
        close(fd);
    });

    run("a request trickling in too slowly gets 408", [&] {
        int fd = connect_to(port);
        send_all(fd, "GET /hello HTTP/1.1\r\nHost: slowloris\r\n");
        std::string out = read_to_close(fd);
        CHECK(contains(out, "408 Request Timeout"));
        close(fd);
    });
}

static void concurrency_test() {
    run("200 concurrent keep-alive clients, 20 requests each", [] {
        Router router;
        router.post("/echo", [](const Request& r) { return Response::text(200, r.body); });
        ServerOptions opts;
        opts.port = 0;
        opts.workers = 4;
        TestServer ts(router, opts);

        std::atomic<int> good{0};
        std::mutex err_mu;
        std::string first_error;
        // Open all connections first so 200 are held open at once. (Connecting
        // in one burst from 200 threads would instead measure the kernel's
        // listen queue, which is only 128 on macOS.)
        std::vector<int> fds;
        for (int t = 0; t < 200; ++t) fds.push_back(connect_to(ts.port()));

        std::vector<std::thread> clients;
        for (int t = 0; t < 200; ++t) {
            clients.emplace_back([&, t] {
                int fd = fds[static_cast<size_t>(t)];
                try {
                    for (int i = 0; i < 20; ++i) {
                        std::string body = "client" + std::to_string(t) + "-req" + std::to_string(i);
                        send_all(fd, "POST /echo HTTP/1.1\r\nContent-Length: " + std::to_string(body.size()) +
                                         "\r\n\r\n" + body);
                        if (contains(read_until(fd, body), "\r\n\r\n" + body)) ++good;
                    }
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lk(err_mu);
                    if (first_error.empty()) first_error = e.what();
                }
                close(fd);
            });
        }
        for (auto& c : clients) c.join();
        if (!first_error.empty()) std::printf("    client error: %s\n", first_error.c_str());
        CHECK(first_error.empty());
        CHECK(good.load() == 200 * 20);
    });
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);  // the server may hang up on us mid-send

    std::printf("parser\n");
    parser_tests();
    std::printf("\nresponse\n");
    response_tests();
    std::printf("\nrouter\n");
    router_tests();
    std::printf("\nend to end\n");
    end_to_end_tests();
    concurrency_test();

    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
