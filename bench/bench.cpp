// Closed-loop load generator: C keep-alive connections, each sending one
// request, waiting for the full response, and repeating for D seconds.
// Reports throughput plus latency percentiles, and counts errors instead of
// silently stopping a connection.
//
// usage: bench <port> <path> <connections> <seconds>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Reads exactly one response (headers + content-length body). Any bytes past
// it are kept in `buf` for the next call. Returns false on error/close.
static bool read_response(int fd, std::string& buf) {
    size_t need = 0;
    for (;;) {
        if (need == 0) {
            size_t eoh = buf.find("\r\n\r\n");
            if (eoh != std::string::npos) {
                std::string head = buf.substr(0, eoh);
                for (char& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                size_t cl = head.find("\r\ncontent-length:");
                size_t len = cl == std::string::npos ? 0 : std::strtoul(head.c_str() + cl + 17, nullptr, 10);
                need = eoh + 4 + len;
            }
        }
        if (need && buf.size() >= need) {
            buf.erase(0, need);
            return true;
        }
        char tmp[16384];
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<size_t>(n));
    }
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: bench <port> <path> <connections> <seconds>\n");
        return 2;
    }
    std::signal(SIGPIPE, SIG_IGN);
    const int port = std::atoi(argv[1]);
    const std::string path = argv[2];
    const int conns = std::max(1, std::atoi(argv[3]));
    const int seconds = std::max(1, std::atoi(argv[4]));
    const std::string wire = "GET " + path + " HTTP/1.1\r\nHost: bench\r\n\r\n";

    std::atomic<bool> stop{false};
    std::atomic<long> errors{0};
    std::mutex mu;
    std::vector<uint32_t> all_us;  // per-request latency in microseconds

    std::vector<std::thread> pool;
    for (int i = 0; i < conns; ++i) {
        pool.emplace_back([&] {
            std::vector<uint32_t> mine;
            mine.reserve(1 << 16);
            int fd = connect_to(port);
            std::string buf;
            while (!stop.load(std::memory_order_relaxed)) {
                if (fd < 0) {
                    ++errors;
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    fd = connect_to(port);
                    continue;
                }
                auto t0 = Clock::now();
                if (send(fd, wire.data(), wire.size(), 0) != static_cast<ssize_t>(wire.size()) ||
                    !read_response(fd, buf)) {
                    ++errors;
                    close(fd);
                    fd = connect_to(port);
                    buf.clear();
                    continue;
                }
                auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
                mine.push_back(static_cast<uint32_t>(us));
            }
            if (fd >= 0) close(fd);
            std::lock_guard<std::mutex> lk(mu);
            all_us.insert(all_us.end(), mine.begin(), mine.end());
        });
    }

    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop = true;
    for (auto& t : pool) t.join();

    if (all_us.empty()) {
        std::printf("no successful requests (%ld errors) - is the server running on port %d?\n", errors.load(), port);
        return 1;
    }
    std::sort(all_us.begin(), all_us.end());
    auto pct = [&](double p) {
        size_t idx = static_cast<size_t>(p / 100.0 * static_cast<double>(all_us.size() - 1));
        return all_us[idx] / 1000.0;
    };
    std::printf("%zu requests in %ds over %d keep-alive connections: %.0f req/s, %ld errors\n", all_us.size(), seconds,
                conns, static_cast<double>(all_us.size()) / seconds, errors.load());
    std::printf("latency ms: p50 %.3f  p90 %.3f  p99 %.3f  p99.9 %.3f  max %.3f\n", pct(50), pct(90), pct(99), pct(99.9),
                all_us.back() / 1000.0);
    return 0;
}
