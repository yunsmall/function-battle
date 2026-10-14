#pragma once
// Web 前端：独立端口上的 WebSocket（网页端实时对战）+ 静态文件（网页本体）。
// 用 websocketpp（配置时自动下载，见 server/CMakeLists）：init_asio(&io) 复用服务器同一个
// io_context——所有回调都跑在 io 线程上，与"游戏逻辑不加锁"的约定一致。
// 消息层与原生 TCP 完全共用：一个 WS 文本帧 = 一条 JSON（见 PROTOCOL.md 传输层小节）。

#include <asio.hpp>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string>

#include "protocol.h"

namespace fg {

class Connection;

class WebFront {
public:
    using WsServer = websocketpp::server<websocketpp::config::asio>;
    using Hdl = websocketpp::connection_hdl;

    // 与 TcpServer 的三个回调完全对称（Server 挂了相同的处理，会话逻辑零分叉）
    std::function<void(std::shared_ptr<Connection>)> on_open;
    std::function<void(std::shared_ptr<Connection>, const json&)> on_message;
    std::function<void(std::shared_ptr<Connection>)> on_close;

    // 端口绑定失败（被占用等）通过 ec 返回，不抛异常（与 TcpServer 同款约定）；
    // 失败信息（含端口号）在构造里直接打印，ec 只带错误码给 main 报通用信息。
    // alloc_id：连接 id 由 TcpServer 统一发号（两类连接共用服务器一张连接表）
    WebFront(asio::io_context& io, uint16_t port, std::string static_dir,
             std::function<int()> alloc_id, std::error_code& ec);

    void send(Hdl hdl, const json& msg);
    void close(Hdl hdl);

private:
    bool handle_http(Hdl hdl);
    void handle_message(Hdl hdl, const std::string& payload);
    void handle_close(Hdl hdl);

    WsServer srv_;
    std::string static_dir_;
    std::function<int()> alloc_id_;
    // hdl → 连接（websocketpp 的句柄是 weak_ptr，须用 owner_less 做 map 比较）
    std::map<Hdl, std::shared_ptr<Connection>, std::owner_less<Hdl>> conns_;
};

}  // namespace fg
