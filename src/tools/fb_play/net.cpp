// 原生协议的帧收发（帧 = 4 字节小端长度 + JSON）
#include "app.h"

namespace fbplay {

void send_msg(App& app, const json& m) {
    const std::string body = m.dump();
    const uint32_t n = static_cast<uint32_t>(body.size());
    std::string frame;
    frame.push_back(static_cast<char>(n & 0xFF));
    frame.push_back(static_cast<char>((n >> 8) & 0xFF));
    frame.push_back(static_cast<char>((n >> 16) & 0xFF));
    frame.push_back(static_cast<char>((n >> 24) & 0xFF));
    frame += body;
    std::error_code ec;
    asio::write(app.sock, asio::buffer(frame), ec);
    if (ec) app.disconnected = true;
}

// 把内核缓冲里现有的字节读进来（非阻塞）；没有数据立刻返回
void pump_socket(App& app) {
    char buf[65536];
    for (;;) {
        std::error_code ec;
        const size_t n = app.sock.read_some(asio::buffer(buf), ec);
        if (!ec) {
            app.recvbuf.append(buf, n);
            continue;  // 可能还有
        }
        // 非阻塞下"暂无数据"的两种表现（Unix=try_again、Windows=would_block）
        if (ec != asio::error::would_block && ec != asio::error::try_again) {
            app.disconnected = true;
        }
        return;
    }
}

// 把缓冲里的完整帧全部解出来交给 handle（半包留在缓冲里等下次）
void drain_frames(App& app) {
    while (app.recvbuf.size() >= 4) {
        const uint32_t n = static_cast<uint8_t>(app.recvbuf[0]) |
                           (static_cast<uint32_t>(static_cast<uint8_t>(app.recvbuf[1])) << 8) |
                           (static_cast<uint32_t>(static_cast<uint8_t>(app.recvbuf[2])) << 16) |
                           (static_cast<uint32_t>(static_cast<uint8_t>(app.recvbuf[3])) << 24);
        if (app.recvbuf.size() < 4u + n) return;
        const json m = json::parse(app.recvbuf.data() + 4, app.recvbuf.data() + 4 + n,
                                   nullptr, false);
        app.recvbuf.erase(0, 4u + n);
        if (!m.is_discarded()) handle(app, m);
    }
}

}  // namespace fbplay
