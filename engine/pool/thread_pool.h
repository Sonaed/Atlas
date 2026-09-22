#pragma once
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
namespace creative::engine {
class ThreadPool {
public:
    explicit ThreadPool(std::size_t n = std::thread::hardware_concurrency()) : stopping_(false) {
        n = n ? n : 1; for (std::size_t i = 0; i < n; ++i) threads_.emplace_back([this]{ run(); });
    }
    ~ThreadPool() { stop(); }
    ThreadPool(const ThreadPool&) = delete;
    void enqueue(std::function<void()> task) { { std::lock_guard lock(mutex_); if (stopping_) return; tasks_.push(std::move(task)); } ready_.notify_one(); }
    void wait_idle() { std::unique_lock lock(mutex_); idle_.wait(lock, [this]{ return tasks_.empty() && active_ == 0; }); }
    void stop() { { std::lock_guard lock(mutex_); if (stopping_) return; stopping_ = true; } ready_.notify_all(); for (auto& t : threads_) if (t.joinable()) t.join(); threads_.clear(); }
    std::size_t size() const noexcept { return threads_.size(); }
private:
    void run() { for (;;) { std::function<void()> task; { std::unique_lock lock(mutex_); ready_.wait(lock,[this]{return stopping_ || !tasks_.empty();}); if(stopping_ && tasks_.empty()) return; task=std::move(tasks_.front()); tasks_.pop(); ++active_; } try { task(); } catch (...) {} { std::lock_guard lock(mutex_); --active_; } idle_.notify_all(); } }
    mutable std::mutex mutex_; std::condition_variable ready_, idle_; std::queue<std::function<void()>> tasks_; std::vector<std::thread> threads_; std::size_t active_=0; bool stopping_;
};
}
