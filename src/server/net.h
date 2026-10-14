#pragma once
// 基于 asio 的 TCP 服务器：4 字节长度前缀 + JSON 帧。
// 所有 IO 与游戏逻辑都跑在同一个 io_context 线程里，因此逻辑层无需加锁。
// 另一类连接是网页端的 WebSocket（WebFront 创建，见 web_front.h）：它不走本文件的
// 读循环（帧由 websocketpp 收好后回调进来），只是复用同一个会话载体 Connection——
// send/close 按后端分叉，服务器上层看到的两种连接没有区别。

#include <asio.hpp>
#include <websocketpp/common/connection_hdl.hpp>

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "protocol.h"

namespace fg {

class TcpServer;
class WebFront;

class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(asio::ip::tcp::socket socket, TcpServer* server, int id);
    // WebSocket 连接（WebFront 创建）：帧的收发归 websocketpp，这里只是会话载体
    Connection(WebFront* web, websocketpp::connection_hdl hdl, int id);
    ~Connection();

    void start();  // 仅原生连接调用（WS 的消息由 WebFront 回调推进）
    void send(const json& msg);  // 必须在 io 线程调用
    void close();
    int id() const { return id_; }

private:
    void do_read_header();
    void do_read_body(uint32_t n);
    void do_write();

    // 原生后端的 socket（WS 后端不用——新 asio 的 socket 没有无参构造，故装进 optional）
    std::optional<asio::ip::tcp::socket> socket_;
    TcpServer* server_ = nullptr;         // 原生后端（生命周期长于连接：进程级）
    WebFront* web_ = nullptr;             // WS 后端（与 server_ 二选一）
    websocketpp::connection_hdl ws_hdl_;  // 仅 WS 后端使用
    int id_ = 0;
    // 帧头固定 4 字节。注意不能写 std::vector<char> header_{4}——花括号是 initializer_list，
    // 那会得到"1 个值为 4 的元素"，asio::buffer 只读 1 字节，长度解析全部错位
    std::array<char, 4> header_{};
    std::deque<std::string> out_queue_;
    size_t out_bytes_ = 0;  // 待发送队列的累计字节数（慢客户端防线，超限即断开）
    bool closed_ = false;
};

class TcpServer {
public:
    // 绑定端口失败（被占用等）通过 ec 返回，不抛异常
    TcpServer(asio::io_context& io, uint16_t port, std::error_code& ec);
    void start();

    // 连接 id 统一在这里发号：原生连接与 WS 连接（WebFront 创建）共用服务器同一张
    // 连接表，两类 id 不能碰撞
    int alloc_id() { return next_conn_id_++; }

    std::function<void(std::shared_ptr<Connection>)> on_connect;
    std::function<void(std::shared_ptr<Connection>, const json&)> on_message;
    std::function<void(std::shared_ptr<Connection>)> on_disconnect;

private:
    void do_accept();

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    int next_conn_id_ = 1;
};

}  // namespace fg
