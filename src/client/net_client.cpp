#include "net_client.h"

#include <asio.hpp>

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace fg {

struct NetClient::Impl {
    void write_next();
    void do_read_header();
    void do_read_body(uint32_t n);

    // 每次连接重建 io_context 与 socket：断开时随旧 io 一起销毁，未执行的异步操作
    // （旧 socket 的读回调等）就此丢弃，不会在下一次连接运行时误改新连接的状态
    std::unique_ptr<asio::io_context> io;
    std::unique_ptr<asio::ip::tcp::socket> socket;
    std::thread thread;
    std::atomic<bool> connected{false};
    // 帧头固定 4 字节（vector 的花括号初始化是 initializer_list，只会得到 1 个元素）
    std::array<char, 4> header{};
    std::deque<std::string> outbox;  // 仅在 io 线程访问
    std::mutex mutex;
    std::deque<json> inbox;  // 主线程消费
};

void NetClient::Impl::write_next() {
    asio::async_write(*socket, asio::buffer(outbox.front()),
                      [this](const std::error_code& ec, size_t) {
                          if (ec) {
                              connected = false;
                              return;
                          }
                          outbox.pop_front();
                          if (!outbox.empty()) write_next();
                      });
}

void NetClient::Impl::do_read_header() {
    asio::async_read(*socket, asio::buffer(header),
                     [this](const std::error_code& ec, size_t) {
                         if (ec) {
                             connected = false;
                             return;
                         }
                         const auto* b = reinterpret_cast<const unsigned char*>(header.data());
                         const uint32_t n = static_cast<uint32_t>(b[0]) |
                                            (static_cast<uint32_t>(b[1]) << 8) |
                                            (static_cast<uint32_t>(b[2]) << 16) |
                                            (static_cast<uint32_t>(b[3]) << 24);
                         if (n > kMaxFrameSize) {
                             connected = false;
                             return;
                         }
                         do_read_body(n);
                     });
}

void NetClient::Impl::do_read_body(uint32_t n) {
    auto buf = std::make_shared<std::string>(n, '\0');
    asio::async_read(*socket, asio::buffer(*buf),
                     [this, buf](const std::error_code& ec, size_t) {
                         if (ec) {
                             connected = false;
                             return;
                         }
                         // 不抛版本解析：坏帧（discarded）直接忽略
                         json msg = json::parse(*buf, nullptr, false);
                         if (!msg.is_discarded()) {
                             std::lock_guard<std::mutex> lock(mutex);
                             inbox.push_back(std::move(msg));
                         }
                         do_read_header();
                     });
}

NetClient::NetClient() : impl_(std::make_unique<Impl>()) {}

NetClient::~NetClient() { disconnect(); }

std::string NetClient::connect_to(const std::string& host, uint16_t port) {
    Impl& self = *impl_;
    disconnect();
    self.io = std::make_unique<asio::io_context>();
    self.socket = std::make_unique<asio::ip::tcp::socket>(*self.io);
    std::error_code ec;
    asio::ip::tcp::resolver resolver(*self.io);
    const auto endpoints = resolver.resolve(host, std::to_string(port), ec);
    if (!ec) asio::connect(*self.socket, endpoints, ec);
    if (ec) {
        // 同样：socket 必须先于 io_context 销毁
        self.socket.reset();
        self.io.reset();
        return "连接失败：" + ec.message();
    }
    std::error_code opt_ec;
    self.socket->set_option(asio::ip::tcp::no_delay(true), opt_ec);
    self.connected = true;
    self.do_read_header();
    self.thread = std::thread([&self]() {
        // 线程边界兜底：异常逃出线程会直接 terminate 进程，捕获后一律按断线处理；
        // io.run() 正常返回（无更多工作）同样视为断线
        try {
            self.io->run();
        } catch (const std::exception&) {
            // 断线
        }
        self.connected = false;
    });
    return {};
}

void NetClient::disconnect() {
    Impl& self = *impl_;
    self.connected = false;
    // 让 io 线程尽快退出（不跨线程 close socket：那与运行中的异步操作是数据竞争，
    // socket 析构时自会关闭句柄）
    if (self.io) self.io->stop();
    if (self.thread.joinable()) self.thread.join();
    // asio 的 IO 对象（socket）必须先于 io_context 销毁：socket 的析构要用 io 的 service，
    // 先销毁 io_context 会留下悬空引用（实测在这里段错误过）。
    // 与 send 的锁配对：reset 之后 send 只会看到空指针
    std::lock_guard<std::mutex> lock(self.mutex);
    self.socket.reset();
    self.io.reset();
    // 残留的待发帧必须清掉：否则重连后 outbox 非空，后续发送会被"非空即忙"的判断卡死
    self.outbox.clear();
    self.inbox.clear();
}

bool NetClient::connected() const { return impl_->connected.load(); }

bool NetClient::send(const json& msg) {
    Impl& self = *impl_;
    auto frame = std::make_shared<std::string>(encode_frame(msg));
    // 锁内取 io：与 disconnect 的 reset 互斥，即便将来被跨线程误用也不会解引用已释放的 io
    std::lock_guard<std::mutex> lock(self.mutex);
    if (!self.connected.load() || !self.io) return false;
    asio::post(*self.io, [&self, frame]() {
        const bool idle = self.outbox.empty();
        self.outbox.push_back(std::move(*frame));
        if (idle) self.write_next();
    });
    return true;
}

std::vector<json> NetClient::poll() {
    Impl& self = *impl_;
    std::lock_guard<std::mutex> lock(self.mutex);
    std::vector<json> out(self.inbox.begin(), self.inbox.end());
    self.inbox.clear();
    return out;
}

}  // namespace fg
