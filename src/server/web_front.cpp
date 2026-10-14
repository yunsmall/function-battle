#include "web_front.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "net.h"
#include "web_static.h"

namespace fg {

WebFront::WebFront(asio::io_context& io, uint16_t port, std::string static_dir,
                   std::function<int()> alloc_id, std::error_code& ec)
    : static_dir_(std::move(static_dir)), alloc_id_(std::move(alloc_id)) {
    // websocketpp 默认把每条连接的收发都写日志，噪声大；这里全关，启动信息自己打印
    srv_.clear_access_channels(websocketpp::log::alevel::all);
    srv_.clear_error_channels(websocketpp::log::elevel::all);

    srv_.init_asio(&io);  // 复用服务器的 io_context：所有 WS 回调都在 io 线程
    srv_.set_reuse_addr(true);

    srv_.set_http_handler([this](Hdl hdl) { return handle_http(hdl); });
    srv_.set_open_handler([this](Hdl hdl) {
        auto conn = std::make_shared<Connection>(this, hdl, alloc_id_());
        conns_[hdl] = conn;
        if (on_open) on_open(conn);
    });
    srv_.set_message_handler([this](Hdl hdl, WsServer::message_ptr msg) {
        handle_message(hdl, msg->get_payload());
    });
    srv_.set_close_handler([this](Hdl hdl) { handle_close(hdl); });

    try {
        srv_.listen(port);
    } catch (const std::exception& e) {
        // 信息在源头打全（含端口号），main 只知道"服务器启动失败"的通用文案
        std::printf("[web] 网页端口 %u 绑定失败：%s\n", port, e.what());
        std::fflush(stdout);
        ec = std::make_error_code(std::errc::address_in_use);
        return;
    }
    srv_.start_accept();
}

void WebFront::send(Hdl hdl, const json& msg) {
    std::error_code ec;
    srv_.send(hdl, msg.dump(), websocketpp::frame::opcode::text, ec);
    // 发送失败（对端刚断开）：close_handler 马上会到、连接随之下线，这里静默即可
}

void WebFront::close(Hdl hdl) {
    std::error_code ec;
    srv_.close(hdl, websocketpp::close::status::normal, "", ec);  // 幂等，失败静默
}

void WebFront::handle_message(Hdl hdl, const std::string& payload) {
    auto it = conns_.find(hdl);
    if (it == conns_.end()) return;
    // 单条消息超过原生帧上限视为恶意/损坏：断开（两条传输层链路的上限保持一致）
    if (payload.size() > kMaxFrameSize) {
        it->second->close();
        return;
    }
    // 坏帧（discarded）忽略：与原生连接对坏 JSON 的健壮性处理一致
    const json parsed = json::parse(payload, nullptr, false);
    if (parsed.is_discarded()) return;
    if (!on_message) return;
    // 会话边界兜底：单个连接的处理器万一出现未预期异常，只回错误给该客户端；
    // 异常绝不能穿过 websocketpp 回调拖垮整个服务器（与原生连接的兜底同款）
    try {
        on_message(it->second, parsed);
    } catch (const std::exception& e) {
        it->second->send(
            msg::make_error({"server_error", std::string("服务器处理出错：") + e.what()}));
    }
}

void WebFront::handle_close(Hdl hdl) {
    auto it = conns_.find(hdl);
    if (it == conns_.end()) return;
    auto conn = it->second;
    conns_.erase(it);
    // 借 Connection::close 的统一出口通知下线（closed_ 幂等：主动关闭时这里
    // 直接返回，不会重复通知）
    conn->close();
}

bool WebFront::handle_http(Hdl hdl) {
    auto con = srv_.get_con_from_hdl(hdl);
    const std::string method = con->get_request().get_method();
    // HEAD 极少用，等同 GET 返回完整响应（语义小偏差可接受），只挡其它方法
    if (method != "GET" && method != "HEAD") {
        con->set_status(websocketpp::http::status_code::method_not_allowed);
        con->set_body("405 Method Not Allowed");
        con->append_header("Content-Type", "text/plain; charset=utf-8");
        return true;
    }
    std::string rel = resolve_static_path(con->get_request().get_uri());
    if (rel.empty()) {
        con->set_status(websocketpp::http::status_code::forbidden);
        con->set_body("403 Forbidden");
        con->append_header("Content-Type", "text/plain; charset=utf-8");
        return true;
    }
    std::filesystem::path target = std::filesystem::path(static_dir_) / rel;
    std::error_code fec;
    if (std::filesystem::is_directory(target, fec)) {
        rel += "/index.html";
        target /= "index.html";
    }
    std::ifstream in(target, std::ios::binary);
    if (!in) {
        con->set_status(websocketpp::http::status_code::not_found);
        con->set_body("404 Not Found");
        con->append_header("Content-Type", "text/plain; charset=utf-8");
        return true;
    }
    // 静态文件都是几十 KB 级、每连接至多一次 GET：在 io 线程同步读盘可以接受
    //（换来的是不引入异步文件 IO 的复杂度）
    const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    con->set_status(websocketpp::http::status_code::ok);
    con->append_header("Content-Type", mime_type_for(rel));
    // no-cache 是给开发留的：改完 JS 刷新即生效（与旧网关行为一致）
    con->append_header("Cache-Control", "no-cache");
    con->set_body(body);
    return true;
}

}  // namespace fg
