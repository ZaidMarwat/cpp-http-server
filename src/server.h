#pragma once

// HTTP/1.1 server: one event-loop thread does all socket I/O with poll(),
// and a worker pool runs the route handlers.
//
//   accept / read / parse / write          run handlers
//   ┌──────────────────────────┐  job     ┌──────────────┐
//   │ event loop (poll)        │ ───────► │ thread pool  │
//   │ non-blocking sockets     │ ◄─────── │              │
//   └──────────────────────────┘  wake    └──────────────┘
//                                 pipe
//
// Because a connection only occupies a worker while its handler is actually
// running, thousands of idle keep-alive connections cost no threads, and a
// slow or silent client can't starve the pool. Idle connections and requests
// that trickle in too slowly are closed by timeouts.
//
// Requests on one connection are handled strictly in order (pipelined bytes
// wait in the parser until the previous response has been written).

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "request_parser.h"
#include "router.h"
#include "thread_pool.h"

namespace httpd {

struct ServerOptions {
    std::string host = "127.0.0.1";
    int port = 8080;                 // 0 picks a free port; see Server::port()
    size_t workers = 4;
    int idle_timeout_ms = 5000;      // keep-alive connection with nothing to say
    int request_timeout_ms = 10000;  // to receive a full request / drain a response
    size_t max_connections = 10000;
    ParserLimits limits;
};

class Server {
public:
    Server(Router router, ServerOptions options = {});
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Runs the event loop on the calling thread until stop() is called.
    void run();

    // Asks run() to return. Safe to call from any thread and from a signal
    // handler (it only stores an atomic flag and writes one byte to a pipe).
    void stop();

    int port() const { return port_; }

private:
    using Clock = std::chrono::steady_clock;

    enum class Phase { Reading, Handling, Writing };

    struct Connection {
        int fd = -1;
        RequestParser parser;
        Phase phase = Phase::Reading;
        bool in_request = false;          // some bytes of the next request arrived
        bool close_after_write = false;
        bool peer_eof = false;            // client shut down its sending side
        bool dead = false;                // closed at the end of this loop turn
        std::string out;                  // bytes waiting to be written
        size_t out_off = 0;
        Clock::time_point deadline;
    };

    struct Completion {
        uint64_t conn_id;
        std::string wire;
        bool keep_alive;
    };

    Router router_;
    ServerOptions opts_;
    int listen_fd_ = -1;
    int wake_read_ = -1, wake_write_ = -1;
    int spare_fd_ = -1;  // reserved so we can still shed clients at the fd limit
    int port_ = 0;
    std::atomic<bool> stop_requested_{false};

    std::unordered_map<uint64_t, Connection> conns_;  // touched by the loop thread only
    uint64_t next_id_ = 1;

    std::mutex done_mu_;
    std::vector<Completion> done_;  // finished responses from workers

    std::unique_ptr<ThreadPool> pool_;  // last member: destroyed (joined) first

    void wake();
    void accept_clients();
    void on_readable(uint64_t id, Connection& c);
    void advance(uint64_t id, Connection& c);
    void flush(uint64_t id, Connection& c);
    void fail(Connection& c, int status, const std::string& message);
    void dispatch(uint64_t id, Request req);
    void collect_completions();
    void enforce_deadlines();
    void sweep_dead();
    int poll_timeout_ms() const;
};

}  // namespace httpd
