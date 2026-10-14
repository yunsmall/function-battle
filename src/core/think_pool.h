#pragma once
// 供 AI 决策用的极简线程池（手写，不引 asio/不引第三方）。
// 用途：轮到 AI 时把"决策所需的纯数据快照"投进池里算，服务器 tick 与单机渲染帧
// 都不再被规划计算（满配可到二三百毫秒）阻塞；任务只碰自己的快照与结果槽，
// 不共享游戏状态，因此不需要任何游戏层的锁。
//
// 回合制下每个房间同一时刻至多一个 AI 在思考，池大小 2~4 足够多房间并行。

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace fg {

class ThinkPool {
public:
    // threads <= 0 时不起线程：submit 在调用线程里同步执行（单测与无池场景）
    explicit ThinkPool(int threads);
    ~ThinkPool();
    ThinkPool(const ThinkPool&) = delete;
    ThinkPool& operator=(const ThinkPool&) = delete;

    // 提交一个任务；池为空线程时直接同步执行。任务应自己兜住异常
    //（未捕获的异常会终止工作线程）
    void submit(std::function<void()> task);

private:
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
};

}  // namespace fg
