#pragma once

// Fixed-size worker pool. Jobs run in FIFO order; the destructor finishes
// queued jobs and joins the workers.

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace httpd {

class ThreadPool {
public:
    explicit ThreadPool(size_t threads) {
        if (threads == 0) threads = 1;
        for (size_t i = 0; i < threads; ++i) workers_.emplace_back([this] { loop(); });
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void submit(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;

    void loop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stopping_ || !jobs_.empty(); });
                if (jobs_.empty()) return;  // stopping and drained
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }
};

}  // namespace httpd
