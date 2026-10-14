#include "think_pool.h"

namespace fg {

ThinkPool::ThinkPool(int threads) {
    for (int i = 0; i < threads; ++i) {
        workers_.emplace_back([this] { worker_loop(); });
    }
}

ThinkPool::~ThinkPool() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& w : workers_) w.join();
    // 析构返回时排队中的任务都已执行完：任务只写自己的结果槽，晚一秒收尾也无碍
}

void ThinkPool::submit(std::function<void()> task) {
    if (workers_.empty()) {
        task();  // 同步模式：调用线程里算完再返回
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

void ThinkPool::worker_loop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
            if (tasks_.empty()) return;  // stop_ 为真且没活了：收工
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

}  // namespace fg
