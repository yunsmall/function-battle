#pragma once
// 多房间服务器：每个房间拥有独立的 World、配置与玩家 id 空间，彼此互不干扰。
// 连接在大厅里创建/加入房间（用房间号），房主可以配置人员（加人机、换队、踢人）与开局。

#include <asio.hpp>
#include <chrono>
#include <map>
#include <memory>
#include <string>

#include "net.h"
#include "protocol.h"
#include "room.h"
#include "web_front.h"

namespace fg {

class Server {
public:
    // 绑定端口失败（被占用等）通过 ec 返回，不抛异常。
    // default_match 是新建房间的"下一把配置"默认模板（客户端 match 对象可覆盖）；
    // web_port 是网页端口（WebSocket + 静态页，见 web_front.h），static_dir 是其静态根
    Server(asio::io_context& io, uint16_t port, uint16_t web_port, const std::string& static_dir,
           const MatchConfig& default_match, std::error_code& ec);

private:
    struct Session {
        int room_id = -1;   // -1 = 在大厅
        int member_id = -1;
        // 本连接是否要 shot 携带轨迹折线：不共享 core 的客户端（网页端）在
        // create_room/join_room 里声明 trail=true；C++ 客户端保持默认 false，
        // 收发的网络表示与没有这个字段之前完全一致（弹道继续本地重采样）
        bool wants_trail = false;
        std::chrono::steady_clock::time_point last_preview{};  // 上次转发预览的时刻（限频）
        // 上次曲线预览采样的时刻：预览限频与 function_preview 同风格（约 1 秒 1 次）
        std::chrono::steady_clock::time_point last_curve_preview{};
    };

    void on_connect(std::shared_ptr<Connection> conn);
    void on_message(std::shared_ptr<Connection> conn, const json& msg);
    void on_disconnect(std::shared_ptr<Connection> conn);

    void handle_lobby(std::shared_ptr<Connection> conn, const json& msg);
    void handle_in_room(std::shared_ptr<Connection> conn, Session& sess, const json& msg);
    void join_room(std::shared_ptr<Connection> conn, int room_id, const std::string& name,
                   MemberRole role, const std::string& error_context, bool wants_trail);

    void tick();
    void dispatch_outgoing();
    void send_room_state(int room_id);
    void erase_room_if_empty(int room_id);
    Connection* find_connection(int conn_id);
    // 按连接偏好投递（trail 字段的过滤在这里统一做，见实现处注释）
    void send_to_conn(int conn_id, const json& msg);
    json room_list_json() const;

    asio::io_context& io_;
    TcpServer tcp_;
    // 网页端口（WS+静态页）；unique_ptr 便于"游戏端口都没绑上时不去起网页端口"的短路
    std::unique_ptr<WebFront> web_;
    MatchConfig default_match_;
    std::map<int, Session> sessions_;  // conn_id → 会话
    std::map<int, std::shared_ptr<Connection>> conns_;
    std::map<int, std::unique_ptr<Room>> rooms_;  // room_id → 房间
    int next_room_id_ = 1;
    asio::steady_timer tick_timer_;
    std::chrono::steady_clock::time_point last_tick_;  // 上一帧时刻（模拟按真实经过时间推进）
    // AI 思考线程池：多房间共享（回合制下每房间至多一个活跃思考，2 线程足够）
    ThinkPool think_pool_{2};
};

}  // namespace fg
