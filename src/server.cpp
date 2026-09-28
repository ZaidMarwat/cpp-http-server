#include "server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // macOS: SO_NOSIGPIPE is set per socket instead
#endif

namespace httpd {

namespace {

void set_nonblocking_cloexec(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
}

[[noreturn]] void fail_errno(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

}  // namespace

Server::Server(Router router, ServerOptions options) : router_(std::move(router)), opts_(std::move(options)) {
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) fail_errno("socket");
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(opts_.port));
    if (inet_pton(AF_INET, opts_.host.c_str(), &addr.sin_addr) != 1) {
        close(listen_fd_);
        throw std::invalid_argument("not an IPv4 address: " + opts_.host);
    }
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int saved = errno;
        close(listen_fd_);
        errno = saved;
        fail_errno("bind " + opts_.host + ":" + std::to_string(opts_.port));
    }
    if (listen(listen_fd_, 4096) < 0)  // the kernel caps this at its own limit
        fail_errno("listen");
    set_nonblocking_cloexec(listen_fd_);

    socklen_t len = sizeof(addr);
    getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);

    int fds[2];
    if (pipe(fds) < 0) fail_errno("pipe");
    wake_read_ = fds[0];
    wake_write_ = fds[1];
    set_nonblocking_cloexec(wake_read_);
    set_nonblocking_cloexec(wake_write_);

    spare_fd_ = open("/dev/null", O_RDONLY | O_CLOEXEC);
    pool_ = std::make_unique<ThreadPool>(opts_.workers);
}

Server::~Server() {
    stop();
    pool_.reset();  // finish in-flight handlers while the wake pipe still exists
    for (auto& [id, c] : conns_) close(c.fd);
    if (listen_fd_ >= 0) close(listen_fd_);
    if (wake_read_ >= 0) close(wake_read_);
    if (wake_write_ >= 0) close(wake_write_);
    if (spare_fd_ >= 0) close(spare_fd_);
}

void Server::stop() {
    stop_requested_.store(true);
    wake();
}

void Server::wake() {
    char byte = 1;
    // A full pipe already guarantees a pending wake-up, so EAGAIN is fine.
    [[maybe_unused]] ssize_t n = write(wake_write_, &byte, 1);
}

void Server::run() {
    std::vector<pollfd> fds;
    std::vector<uint64_t> ids;  // ids[i] belongs to fds[i + 2]

    while (!stop_requested_.load()) {
        fds.clear();
        ids.clear();
        fds.push_back({wake_read_, POLLIN, 0});
        fds.push_back({listen_fd_, static_cast<short>(conns_.size() < opts_.max_connections ? POLLIN : 0), 0});
        for (auto& [id, c] : conns_) {
            short events = 0;
            if (c.phase == Phase::Reading) events |= POLLIN;
            if (c.out_off < c.out.size()) events |= POLLOUT;
            // A connection whose handler is running has nothing to wait for;
            // a negative fd makes poll() skip it (so a hang-up can't spin us).
            fds.push_back({events ? c.fd : -1, events, 0});
            ids.push_back(id);
        }

        int ready = poll(fds.data(), static_cast<nfds_t>(fds.size()), poll_timeout_ms());
        if (ready < 0 && errno != EINTR) fail_errno("poll");

        if (ready > 0) {
            if (fds[0].revents & POLLIN) {
                char drain[256];
                while (read(wake_read_, drain, sizeof(drain)) > 0) {
                }
                collect_completions();
            }
            if (fds[1].revents & POLLIN) accept_clients();

            for (size_t i = 0; i < ids.size(); ++i) {
                short rev = fds[i + 2].revents;
                if (!rev) continue;
                auto it = conns_.find(ids[i]);
                if (it == conns_.end() || it->second.dead) continue;
                Connection& c = it->second;
                if (rev & POLLNVAL) {
                    c.dead = true;
                    continue;
                }
                if (c.phase == Phase::Reading && (rev & (POLLIN | POLLHUP | POLLERR))) on_readable(ids[i], c);
                if (!c.dead && (rev & (POLLOUT | POLLHUP | POLLERR)) && c.out_off < c.out.size()) flush(ids[i], c);
            }
        }

        enforce_deadlines();
        sweep_dead();
    }
}

void Server::accept_clients() {
    while (conns_.size() < opts_.max_connections) {
        int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if ((errno == EMFILE || errno == ENFILE) && spare_fd_ >= 0) {
                // Out of descriptors: the client would sit in the backlog and
                // keep the listen socket readable, spinning this loop. Free
                // the spare, accept and immediately drop the client, re-reserve.
                close(spare_fd_);
                int victim = accept(listen_fd_, nullptr, nullptr);
                if (victim >= 0) close(victim);
                spare_fd_ = open("/dev/null", O_RDONLY | O_CLOEXEC);
            }
            return;  // EAGAIN, or nothing more we can do this turn
        }
        set_nonblocking_cloexec(fd);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        Connection& c = conns_.emplace(next_id_++, Connection{}).first->second;
        c.fd = fd;
        c.parser = RequestParser(opts_.limits);
        c.deadline = Clock::now() + std::chrono::milliseconds(opts_.idle_timeout_ms);
    }
}

void Server::on_readable(uint64_t id, Connection& c) {
    char buf[64 * 1024];
    size_t total = 0;
    while (total < 1024 * 1024) {  // cap per turn so one client can't hog the loop
        ssize_t n = recv(c.fd, buf, sizeof(buf), 0);
        if (n > 0) {
            if (!c.in_request) {
                c.in_request = true;
                c.deadline = Clock::now() + std::chrono::milliseconds(opts_.request_timeout_ms);
            }
            c.parser.feed(buf, static_cast<size_t>(n));
            total += static_cast<size_t>(n);
            continue;
        }
        if (n == 0) {  // client finished sending; answer what it already sent
            c.peer_eof = true;
            break;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        c.dead = true;  // connection reset or another real error
        return;
    }
    advance(id, c);
    if (c.peer_eof && c.phase == Phase::Reading) c.dead = true;  // nothing more will arrive
}

// Tries to pull the next complete request out of the parser and hand it to
// the worker pool. Only called while the connection is in the Reading phase.
void Server::advance(uint64_t id, Connection& c) {
    if (c.phase != Phase::Reading || c.dead) return;
    try {
        Request req;
        if (c.parser.parse(req)) {
            c.phase = Phase::Handling;
            c.in_request = false;
            dispatch(id, std::move(req));
        } else if (c.parser.take_continue()) {
            c.out += "HTTP/1.1 100 Continue\r\n\r\n";
            flush(id, c);
        }
    } catch (const ParseError& e) {
        fail(c, e.status(), e.what());
        flush(id, c);
    }
}

void Server::fail(Connection& c, int status, const std::string& message) {
    c.out += Response::text(status, std::string(status_reason(status)) + ": " + message + "\n").serialize(false);
    c.close_after_write = true;
    c.phase = Phase::Writing;
    c.deadline = Clock::now() + std::chrono::milliseconds(opts_.request_timeout_ms);
}

void Server::dispatch(uint64_t id, Request req) {
    pool_->submit([this, id, req = std::move(req)]() mutable {
        Response res;
        try {
            res = router_.dispatch(req);
        } catch (const std::exception&) {
            res = Response::text(500, "internal server error\n");
        } catch (...) {
            res = Response::text(500, "internal server error\n");
        }
        bool keep_alive = req.keep_alive && !stop_requested_.load();
        if (const auto it = res.headers.find("connection"); it != res.headers.end() && it->second == "close")
            keep_alive = false;
        std::string wire = res.serialize(keep_alive, req.method == "HEAD");
        {
            std::lock_guard<std::mutex> lk(done_mu_);
            done_.push_back({id, std::move(wire), keep_alive});
        }
        wake();
    });
}

void Server::collect_completions() {
    std::vector<Completion> batch;
    {
        std::lock_guard<std::mutex> lk(done_mu_);
        batch.swap(done_);
    }
    for (Completion& done : batch) {
        auto it = conns_.find(done.conn_id);
        if (it == conns_.end() || it->second.dead) continue;  // client went away
        Connection& c = it->second;
        c.out += done.wire;
        c.close_after_write = !done.keep_alive;
        c.phase = Phase::Writing;
        c.deadline = Clock::now() + std::chrono::milliseconds(opts_.request_timeout_ms);
        flush(done.conn_id, c);
    }
}

void Server::flush(uint64_t id, Connection& c) {
    while (c.out_off < c.out.size()) {
        ssize_t n = send(c.fd, c.out.data() + c.out_off, c.out.size() - c.out_off, MSG_NOSIGNAL);
        if (n > 0) {
            c.out_off += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;  // wait for POLLOUT
        } else {
            c.dead = true;
            return;
        }
    }
    c.out.clear();
    c.out_off = 0;

    if (c.phase != Phase::Writing) return;  // e.g. an interim 100 Continue
    if (c.close_after_write) {
        c.dead = true;
        return;
    }
    // Response done: go back to reading. A pipelined request may already be
    // sitting in the parser, so try it right away.
    c.phase = Phase::Reading;
    c.in_request = c.parser.mid_request();
    c.deadline = Clock::now() + std::chrono::milliseconds(c.in_request ? opts_.request_timeout_ms : opts_.idle_timeout_ms);
    advance(id, c);
    if (c.peer_eof && c.phase == Phase::Reading) c.dead = true;
}

void Server::enforce_deadlines() {
    const auto now = Clock::now();
    for (auto& [id, c] : conns_) {
        if (c.dead || c.phase == Phase::Handling || now < c.deadline) continue;
        if (c.phase == Phase::Reading && c.in_request) {
            // Best effort: tell a slow client why, then hang up.
            fail(c, 408, "request not received in time");
            c.close_after_write = true;
            send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
        }
        c.dead = true;
    }
}

void Server::sweep_dead() {
    for (auto it = conns_.begin(); it != conns_.end();) {
        if (it->second.dead) {
            close(it->second.fd);
            it = conns_.erase(it);
        } else {
            ++it;
        }
    }
}

int Server::poll_timeout_ms() const {
    auto next = Clock::time_point::max();
    for (const auto& [id, c] : conns_)
        if (!c.dead && c.phase != Phase::Handling) next = std::min(next, c.deadline);
    if (next == Clock::time_point::max()) return 1000;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now()).count() + 1;
    return static_cast<int>(std::clamp<long long>(ms, 0, 1000));
}

}  // namespace httpd
