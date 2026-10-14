#include "net.h"

#include <chrono>
#include <cstdio>

#include "web_front.h"

namespace fg {

namespace {
// 待发送队列字节上限：对端不读 socket 时队列会一直增长（对局中炮弹广播约 20Hz），
// 超过即认为该客户端已失联，断开而不是无限缓冲。正常速率下队列是几 KB 级
constexpr size_t kMaxOutQueueBytes = 1u << 20;  // 1 MB
}  // namespace

Connection::Connection(asio::ip::tcp::socket socket, TcpServer* server, int id)
    : socket_(std::move(socket)), server_(server), id_(id) {}

Connection::Connection(WebFront* web, websocketpp::connection_hdl hdl, int id)
    : web_(web), ws_hdl_(std::move(hdl)), id_(id) {}

Connection::~Connection() = default;

void Connection::start() {
    // 禁用 Nagle：这类回合制小消息希望尽快送达
    std::error_code ec;
    socket_->set_option(asio::ip::tcp::no_delay(true), ec);
    do_read_header();
}

void Connection::close() {
    if (closed_) return;
    closed_ = true;
    if (web_ != nullptr) {
        // WS：请 websocketpp 发起关闭；它稍后的 close_handler 会再次走到这里，
        // closed_ 已置位、直接返回（不会重复通知，也不会递归关闭）
        web_->close(ws_hdl_);
    } else {
        std::error_code ec;
        socket_->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_->close(ec);
    }
    // 必须在持有 shared_ptr 的语境调用（所有调用点都是自带保活的异步回调）：
    // 未来若从裸指针或析构语境调用，shared_from_this 会抛 bad_weak_ptr，
    // 该异常穿过 asio handler 可能让整个服务器退出
    if (web_ != nullptr) {
        if (web_->on_close) web_->on_close(shared_from_this());
    } else if (server_ != nullptr && server_->on_disconnect) {
        server_->on_disconnect(shared_from_this());
    }
}

void Connection::send(const json& msg) {
    if (closed_) return;
    if (web_ != nullptr) {
        // WS：交 websocketpp 发送（内部自排队），不参与下方原生队列的字节预算——
        // 慢客户端的背压由它的缓冲上限管；本函数仍在 io 线程被调用
        web_->send(ws_hdl_, msg);
        return;
    }
    // 队列已超限（且没能及时写出）：断开该连接，消息不再入队。
    // 关闭必须 post 到当前调用栈之后：send 的调用方（dispatch_outgoing、
    // send_room_state 等）常正遍历 sessions_/rooms_，而 close 会同步回调
    // on_disconnect 去 erase 这些容器——在遍历中途同步 erase 是迭代器失效 UB。
    // close 幂等，超限期间重复 post 无害
    if (out_bytes_ > kMaxOutQueueBytes) {
        auto self = shared_from_this();
        asio::post(socket_->get_executor(), [self] { self->close(); });
        return;
    }
    const bool idle = out_queue_.empty();
    std::string frame = encode_frame(msg);
    out_bytes_ += frame.size();
    out_queue_.push_back(std::move(frame));
    if (idle) do_write();
}

void Connection::do_write() {
    auto self = shared_from_this();
    asio::async_write(*socket_, asio::buffer(out_queue_.front()),
                      [this, self](const std::error_code& ec, size_t) {
                          if (ec) {
                              close();
                              return;
                          }
                          out_bytes_ -= out_queue_.front().size();
                          out_queue_.pop_front();
                          if (!out_queue_.empty()) do_write();
                      });
}

void Connection::do_read_header() {
    auto self = shared_from_this();
    asio::async_read(*socket_, asio::buffer(header_),
                     [this, self](const std::error_code& ec, size_t) {
                         if (ec) {
                             close();
                             return;
                         }
                         const auto* b = reinterpret_cast<const unsigned char*>(header_.data());
                         const uint32_t n = static_cast<uint32_t>(b[0]) |
                                            (static_cast<uint32_t>(b[1]) << 8) |
                                            (static_cast<uint32_t>(b[2]) << 16) |
                                            (static_cast<uint32_t>(b[3]) << 24);
                         if (n > kMaxFrameSize) {
                             close();  // 超大帧视为恶意/损坏连接
                             return;
                         }
                         do_read_body(n);
                     });
}

void Connection::do_read_body(uint32_t n) {
    auto self = shared_from_this();
    auto buf = std::make_shared<std::string>(n, '\0');
    asio::async_read(*socket_, asio::buffer(*buf),
                     [this, self, buf](const std::error_code& ec, size_t) {
                         if (ec) {
                             close();
                             return;
                         }
                         // 不抛版本解析：坏帧（discarded）忽略这一帧，连接继续（保持健壮）
                         const json msg = json::parse(*buf, nullptr, false);
                         if (!msg.is_discarded() && server_ && server_->on_message) {
                             // 会话边界兜底：单个连接的处理器万一出现未预期异常，只回错误给
                             // 该客户端；异常绝不能穿过 io_context 拖垮整个服务器进程
                             try {
                                 server_->on_message(self, msg);
                             } catch (const std::exception& e) {
                                 send(msg::make_error(
                                     {"server_error", std::string("服务器处理出错：") + e.what()}));
                             }
                         }
                         do_read_header();
                     });
}

TcpServer::TcpServer(asio::io_context& io, uint16_t port, std::error_code& ec)
    : io_(io), acceptor_(io) {
    // asio 没有"acceptor(io, endpoint, ec)"这个不抛异常的重载，只能分步 open/bind/listen
    const asio::ip::tcp::endpoint ep(asio::ip::tcp::v4(), port);
    acceptor_.open(ep.protocol(), ec);
    if (!ec) acceptor_.bind(ep, ec);
    if (!ec) acceptor_.listen(asio::socket_base::max_listen_connections, ec);
}

void TcpServer::start() { do_accept(); }

void TcpServer::do_accept() {
    acceptor_.async_accept([this](const std::error_code& ec, asio::ip::tcp::socket socket) {
        if (ec) {
            // acceptor 关闭时的取消不算错误；继续重挂会在已关闭的 acceptor 上操作
            if (ec == asio::error::operation_aborted) return;
            // 持久性错误（句柄耗尽等）下立即重挂会形成忙循环：async_accept 会
            // 立刻再失败，io 线程被空转的回调占满、tick 与收包全部饿死。
            // 延迟重试给系统（如 fd 释放）恢复的窗口
            auto timer = std::make_shared<asio::steady_timer>(io_);
            timer->expires_after(std::chrono::milliseconds(100));
            timer->async_wait([this, timer](const std::error_code&) { do_accept(); });
            return;
        }
        auto conn = std::make_shared<Connection>(std::move(socket), this, alloc_id());
        conn->start();
        if (on_connect) on_connect(conn);
        do_accept();
    });
}

}  // namespace fg
