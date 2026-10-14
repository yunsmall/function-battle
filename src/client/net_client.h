#pragma once
// 客户端的 TCP 连接：后台线程跑 asio，收到/待发的消息通过队列与主线程（渲染线程）交换。
// 实现藏在 .cpp（pimpl）：asio 经 winsock2.h 会拉进 windows.h，与 raylib 同处一个编译单元
// 会撞名（Rectangle / CloseWindow / ShowCursor），所以这个头不暴露任何 asio 类型。
// 线程约定：所有公开方法只能从同一线程调用（内部不互斥；send 与 disconnect 并发会触碰
// 已被 reset 的 io）。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "protocol.h"

namespace fg {

class NetClient {
public:
    NetClient();
    ~NetClient();
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    // 连接服务器；返回错误信息（空 = 成功）
    std::string connect_to(const std::string& host, uint16_t port);
    void disconnect();
    bool connected() const;

    // 主线程调用：发送消息（内部转投到 io 线程执行）
    // 返回是否真正入队（false = 未连接，消息被丢弃）——提交等关键操作要检查
    bool send(const json& msg);
    // 主线程调用：取走已收到的消息
    std::vector<json> poll();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fg
