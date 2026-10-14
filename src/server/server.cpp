#include "server.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace fg {

Server::Server(asio::io_context& io, uint16_t port, uint16_t web_port, const std::string& static_dir,
               const MatchConfig& default_match, std::error_code& ec)
    : io_(io), tcp_(io, port, ec), default_match_(default_match), tick_timer_(io),
      last_tick_(std::chrono::steady_clock::now()) {
    tcp_.on_connect = [this](std::shared_ptr<Connection> conn) { on_connect(std::move(conn)); };
    tcp_.on_message = [this](std::shared_ptr<Connection> conn, const json& msg) {
        on_message(std::move(conn), msg);
    };
    tcp_.on_disconnect = [this](std::shared_ptr<Connection> conn) {
        on_disconnect(std::move(conn));
    };
    tcp_.start();

    // 网页端口：WebSocket 帧的收发归 websocketpp（复用同一个 io_context），
    // 回调挂法与 tcp_ 完全对称——会话/房间逻辑对两类连接零分叉。
    // 游戏端口本身都没绑上（ec 非空）时不再去起网页端口
    if (!ec) {
        std::error_code web_ec;
        web_ = std::make_unique<WebFront>(io_, web_port, static_dir,
                                          [this] { return tcp_.alloc_id(); }, web_ec);
        if (web_ec) {
            ec = web_ec;  // 具体原因（含端口号）已由 WebFront 打印
            return;
        }
        web_->on_open = [this](std::shared_ptr<Connection> conn) { on_connect(std::move(conn)); };
        web_->on_message = [this](std::shared_ptr<Connection> conn, const json& msg) {
            on_message(std::move(conn), msg);
        };
        web_->on_close = [this](std::shared_ptr<Connection> conn) {
            on_disconnect(std::move(conn));
        };
    }

    // 约 60Hz 的节拍驱动所有房间（实际步长按真实经过时间算，见 tick）
    tick_timer_.expires_after(std::chrono::milliseconds(16));
    tick_timer_.async_wait([this](const std::error_code&) { tick(); });
}

void Server::on_connect(std::shared_ptr<Connection> conn) {
    conns_[conn->id()] = conn;
    sessions_[conn->id()] = Session{};
    conn->send(msg::make_hello(conn->id()));
}

void Server::on_disconnect(std::shared_ptr<Connection> conn) {
    auto it = sessions_.find(conn->id());
    if (it != sessions_.end()) {
        const int room_id = it->second.room_id;
        const int member_id = it->second.member_id;
        sessions_.erase(it);
        if (room_id >= 0) {
            auto rit = rooms_.find(room_id);
            if (rit != rooms_.end()) {
                // 对局中断线不能直接踢人（阵容锁定），由 Room 记账、局末清理。
                // 断线本身不改成员表：对局中参战者只记入 disconnected_（消息里没有
                // "离线"标记可发）；观战者走 remove_member 的内部广播。两种分支都
                // 无需在这里补发状态（补发只会重复同一份内容）
                rit->second->mark_disconnected(member_id);
                erase_room_if_empty(room_id);
            }
        }
    }
    conns_.erase(conn->id());
}

void Server::on_message(std::shared_ptr<Connection> conn, const json& msg) {
    auto it = sessions_.find(conn->id());
    if (it == sessions_.end()) return;
    if (it->second.room_id < 0) {
        handle_lobby(std::move(conn), msg);
    } else {
        handle_in_room(std::move(conn), it->second, msg);
    }
}

void Server::handle_lobby(std::shared_ptr<Connection> conn, const json& msg) {
    const std::string type = json_str(msg, "type");
    if (type == "list_rooms") {
        conn->send(room_list_json());
    } else if (type == "create_room") {
        // 新房间：房间属性用默认值打底、客户端 room 对象覆盖（创建时唯一一次设置机会）；
        // 下一把配置以服务器默认模板打底、客户端 match 对象覆盖
        //（seed 给数值 = 固定种子；省略或 null = 每开一把随机换新图）
        RoomOptions room_opts;
        MatchConfig match_cfg = default_match_;
        if (msg.contains("room")) {
            const Error err = apply_room_config_json(msg["room"], room_opts);
            if (!err.ok()) {
                conn->send(msg::make_error({err.code, "创建房间失败：" + err.message}));
                return;
            }
        }
        if (msg.contains("match")) {
            const Error err = apply_match_config_json(msg["match"], match_cfg);
            if (!err.ok()) {
                conn->send(msg::make_error({err.code, "创建房间失败：" + err.message}));
                return;
            }
        }
        // 遮挡物密度对地图面积的交叉校验
        const Error derr = check_obstacle_density(kDefaultWorld, match_cfg);
        if (!derr.ok()) {
            conn->send(msg::make_error({derr.code, "创建房间失败：" + derr.message}));
            return;
        }
        auto room = std::make_unique<Room>(room_opts, match_cfg);
        // 房间密码：客户端已把明文算成 SHA-256 再上行（传输中不出现明文）；
        // 这里只校验格式，比对发生在加入时——哈希不下发给任何人
        const std::string pw = json_str(msg, "password_hash");
        if (!pw.empty()) {
            if (!is_sha256_hex(pw)) {
                conn->send(msg::make_error({"bad_password_hash", "创建房间失败：密码哈希格式不对"}));
                return;
            }
            room->set_password_hash(pw);
        }
        // AI 的思考放到线程池里算：满配规划可达数百毫秒，不能让单个房间的决策
        // 阻塞整条 io 线程（所有房间共用同一 tick）
        room->set_think_pool(&think_pool_);
        const int rid = next_room_id_++;
        rooms_.emplace(rid, std::move(room));
        join_room(std::move(conn), rid, json_str(msg, "name"), MemberRole::Player, "创建房间失败",
                  json_bool(msg, "trail", false));
    } else if (type == "join_room") {
        const int rid = json_int(msg, "room_id", -1);
        auto rit = rooms_.find(rid);
        if (rit == rooms_.end()) {
            conn->send(msg::make_error({"room_not_found", "房间号不存在"}));
            return;
        }
        // 有密码的房间先验哈希（客户端本地算好、传输中不出现明文）；观战同样要过
        if (!rit->second->check_password(json_str(msg, "password_hash"))) {
            conn->send(msg::make_error({"bad_room_password", "房间密码错误"}));
            return;
        }
        const MemberRole role =
            json_bool(msg, "spectate", false) ? MemberRole::Spectator : MemberRole::Player;
        join_room(std::move(conn), rid, json_str(msg, "name"), role, "加入房间失败",
                  json_bool(msg, "trail", false));
    } else {
        conn->send(msg::make_error({"must_join_room", "请先创建或加入房间（可用 list_rooms 查看房间列表）"}));
    }
}

void Server::join_room(std::shared_ptr<Connection> conn, int room_id, const std::string& name,
                       MemberRole role, const std::string& error_context, bool wants_trail) {
    auto rit = rooms_.find(room_id);
    if (rit == rooms_.end()) {
        conn->send(msg::make_error({"room_not_found", error_context + "：房间不存在"}));
        return;
    }
    const int mid = rit->second->add_member(name, false, 1, std::nullopt, role);
    if (mid < 0) {
        // 观战者不会被"对局进行中"拒绝（观战随时可进），失败只剩席位满一种原因；
        // 沿用玩家那句话会让人误以为"进行中不能观战"而放弃重试
        const std::string reason =
            (role == MemberRole::Spectator) ? "观战席位已满" : "房间已满或对局进行中";
        conn->send(msg::make_error({"join_failed", error_context + "：" + reason}));
        erase_room_if_empty(room_id);
        return;
    }
    sessions_[conn->id()] = Session{room_id, mid, wants_trail};
    conn->send(msg::make_joined(room_id, mid, rit->second->host_id()));
    send_room_state(room_id);
    // 观战者可能是在对局进行中进来的：给这一个连接补发对局快照，
    // 后续的广播（炮弹位置等）本来就覆盖房间内所有人，会自动接上
    if (role == MemberRole::Spectator) {
        for (const auto& m : rit->second->game_snapshot_msgs()) send_to_conn(conn->id(), m);
    }
}

void Server::handle_in_room(std::shared_ptr<Connection> conn, Session& sess, const json& msg) {
    auto rit = rooms_.find(sess.room_id);
    if (rit == rooms_.end()) {
        sess = Session{};  // 房间已销毁，退回大厅
        conn->send(msg::make_error({"room_closed", "房间已关闭"}));
        return;
    }
    Room& room = *rit->second;
    const std::string type = json_str(msg, "type");

    if (type == "leave_room") {
        // 观战者不占阵容，对局中也能离开；参战者对局中锁定（阵容不变）。
        // context="leave"：客户端只对"退出观战"的拒绝复位按钮状态
        if (room.in_game() && !room.is_spectator(sess.member_id)) {
            conn->send(msg::make_error({"in_game_leave", "对局进行中不能离开房间"}, "leave"));
            return;
        }
        const int old_room = sess.room_id;
        room.remove_member(sess.member_id);  // 内部会把新状态广播给房间内剩余成员
        sess = Session{};
        conn->send(msg::make_left_room());
        erase_room_if_empty(old_room);
        return;
    }
    if (type == "set_spectate") {
        // 成功时 Room 内部已广播新状态，这里不再重复发
        const Error err = room.set_spectate(sess.member_id, json_bool(msg, "spectate", true));
        if (!err.ok()) conn->send(msg::make_error(err));
        return;
    }
    if (type == "add_ai") {
        const int diff = json_int(msg, "difficulty", 1);
        std::optional<Team> team;
        const std::string t = json_str(msg, "team", "auto");
        if (t == "A") team = Team::A;
        if (t == "B") team = Team::B;
        // 房主校验与状态广播都在 Room::add_ai_member 内完成（与 set_team / kick 同一风格）
        const Error err = room.add_ai_member(sess.member_id, json_str(msg, "name"), diff, team);
        if (!err.ok()) conn->send(msg::make_error(err));
        return;
    }
    if (type == "set_team") {
        const int target = json_int(msg, "member_id", -1);
        const std::string t = json_str(msg, "team", "A");
        // 成功时 Room 内部已广播新状态，这里不再重复发（err 时状态没变、无需刷新）
        const Error err =
            room.set_member_team(sess.member_id, target, t == "B" ? Team::B : Team::A);
        if (!err.ok()) conn->send(msg::make_error(err));
        return;
    }
    if (type == "kick") {
        const int target = json_int(msg, "member_id", -1);
        // 先找到被踢者的连接：踢成功后由网络层直接通知并让他回到大厅。
        // 持 shared_ptr 而不是裸指针：若将来 send 变成"写失败同步触发回调/close()"，
        // 裸指针会在 kick_member 之后悬垂；shared_ptr 把连接保活到本函数结束
        std::shared_ptr<Connection> victim;
        for (auto& [cid, s] : sessions_) {
            if (s.room_id == sess.room_id && s.member_id == target) {
                auto cit = conns_.find(cid);
                if (cit != conns_.end()) victim = cit->second;
            }
        }
        // 成功时 Room 内部已广播新状态，这里不再重复发（err 时状态没变、无需刷新）
        const Error err = room.kick_member(sess.member_id, target);
        if (!err.ok()) {
            conn->send(msg::make_error(err));
        } else {
            if (victim) victim->send(msg::make_kicked());
            for (auto& [cid, s] : sessions_) {
                if (s.room_id == sess.room_id && s.member_id == target) s = Session{};
            }
        }
        return;
    }
    if (type == "start") {
        const Error err = room.start_game(sess.member_id);
        if (!err.ok()) conn->send(msg::make_error(err));
        return;
    }
    if (type == "set_match_config") {
        // Room::apply_config 内部已把新状态广播给所有人（含调用者）；补丁在 match 字段里
        const Error err = room.apply_config(
            sess.member_id, msg.contains("match") ? msg["match"] : json::object());
        if (!err.ok()) conn->send(msg::make_error(err));
        return;
    }
    if (type == "vote_skip") {
        room.vote_skip(sess.member_id, json_bool(msg, "vote", true));
        return;
    }
    if (type == "function_preview") {
        // 限频 100ms/连接（被丢的请求不更新时间戳，刷屏最多就是节流到 10 次/秒）
        const auto now = std::chrono::steady_clock::now();
        if (now - sess.last_preview < std::chrono::milliseconds(100)) return;
        sess.last_preview = now;
        // 超长文本直接丢弃而不是截断：按字节切断会破坏 UTF-8，json 序列化会失败
        const std::string preview = json_str(msg, "preview");
        if (preview.size() <= 400) room.broadcast_preview(sess.member_id, preview);
        return;
    }
    if (type == "preview_curve") {
        // 曲线预览：网页端不共享 core，函数采样只能由服务器代算。限频约 1 秒 1 次/连接，
        // 超出回 preview_too_frequent（明确报错便于客户端诊断，也符合"错误码是稳定契约"
        // 的约定）；被拒的请求不刷新时间戳，客户端下一拍就能重新拿上
        const auto now = std::chrono::steady_clock::now();
        if (now - sess.last_curve_preview < std::chrono::milliseconds(1000)) {
            conn->send(msg::make_error(
                {"preview_too_frequent", "预览请求过于频繁（每秒最多 1 次）"}, "preview"));
            return;
        }
        sess.last_curve_preview = now;
        // 预览链路的错误带 context="preview"：客户端据此只更新预览提示，
        // 不弹"提交失败"那类对话框（打字中途语法不完整是常态）
        if (!msg.contains("request")) {
            conn->send(msg::make_error({"missing_request", "预览缺少 request 字段"}, "preview"));
            return;
        }
        Error perr;
        auto req = curve_request_from_json(msg["request"], perr);
        if (!req) {
            conn->send(msg::make_error(perr, "preview"));
            return;
        }
        Path path;
        const Error err = room.preview_curve(sess.member_id, *req, path);
        if (!err.ok()) {
            conn->send(msg::make_error(err, "preview"));
        } else {
            conn->send(msg::make_curve_preview(path));
        }
        return;
    }
    if (type == "submit") {
        // submit 链路的错误统一带 context="submit"：客户端只据此复位本回合的
        // "已提交"状态（提交被拒必须能重提），不受对局中其它错误干扰
        if (!msg.contains("request")) {
            conn->send(msg::make_error({"missing_request", "提交缺少 request 字段"}, "submit"));
            return;
        }
        Error perr;
        auto req = curve_request_from_json(msg["request"], perr);
        if (!req) {
            conn->send(msg::make_error(perr, "submit"));
            return;
        }
        const Error err = room.submit(sess.member_id, *req);
        if (!err.ok()) conn->send(msg::make_error(err, "submit"));
        return;
    }
    conn->send(msg::make_error({"unknown_message_type", "未知的消息类型：" + type}));
}

void Server::send_room_state(int room_id) {
    if (room_id < 0) return;
    auto rit = rooms_.find(room_id);
    if (rit == rooms_.end()) return;
    const json state = rit->second->state_json();
    for (auto& [cid, s] : sessions_) {
        if (s.room_id != room_id) continue;
        if (Connection* c = find_connection(cid)) c->send(state);
    }
}

void Server::erase_room_if_empty(int room_id) {
    auto rit = rooms_.find(room_id);
    if (rit == rooms_.end()) return;
    // "空"指没有人类成员：人类全离开后只剩 AI 的僵尸房间没有归属者，同样回收
    if (!rit->second->has_human()) rooms_.erase(rit);
}

void Server::tick() {
    // 用真实经过时间推进模拟：定时器是 16ms 节拍（≈62.5Hz），若按固定 1/60 秒推进，
    // 模拟时间会比真实时间慢约 4%，服务器判定（输入超时等）与客户端倒计时逐渐漂移
    const auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - last_tick_).count();
    last_tick_ = now;
    // 防御：进程被挂起（调试断点、系统休眠）后的超大间隔不至于让炮弹瞬移一大截
    dt = std::clamp(dt, 0.0, 0.1);
    // 任何一步抛异常都不能让定时器链断掉：定时器不重挂时 io.run() 会立即返回、
    // 服务器静默停摆（比直接崩溃更难排查），所以重挂定时器无条件放 catch 之外。
    // 每个房间独立捕获：一个房间的异常不能饿死同帧的其它房间
    for (auto& [rid, room] : rooms_) {
        try {
            room->tick(dt);
        } catch (const std::exception& e) {
            std::printf("房间 %d tick 异常（忽略本帧）：%s\n", rid, e.what());
        } catch (...) {
            std::printf("房间 %d tick 未知异常（忽略本帧）\n", rid);
        }
    }
    try {
        dispatch_outgoing();
    } catch (const std::exception& e) {
        std::printf("tick 分发异常（忽略本帧并继续）：%s\n", e.what());
    } catch (...) {
        std::printf("tick 分发未知异常（忽略本帧并继续）\n");
    }
    try {
        // 统一清扫：对局结束、踢人、断线等路径都可能留下已无人类成员的房间。
        // 独立于分发的 try：分发异常不能把僵尸房间永远留在表里
        for (auto it = rooms_.begin(); it != rooms_.end();) {
            if (!it->second->has_human()) it = rooms_.erase(it);
            else ++it;
        }
    } catch (const std::exception& e) {
        std::printf("tick 清扫异常（忽略并继续）：%s\n", e.what());
    } catch (...) {
        std::printf("tick 清扫未知异常（忽略并继续）\n");
    }
    tick_timer_.expires_after(std::chrono::milliseconds(16));
    tick_timer_.async_wait([this](const std::error_code&) { tick(); });
}

void Server::dispatch_outgoing() {
    // 每 tick 重建一份 房间 → 连接 的局部索引：分发只扫本房间的连接，不再对每条消息
    // 遍历全体会话（多房间多人时是 消息数 × 全会话 的嵌套循环）。
    // 刻意"惰性重建"而不是长期维护：加入/离开/断线/回收各处的同步漏更新会丢消息，
    // 重建一遍是 O(会话数) 且天然一致
    std::map<int, std::vector<int>> by_room;
    for (const auto& [cid, s] : sessions_) {
        if (s.room_id >= 0) by_room[s.room_id].push_back(cid);
    }
    for (auto& [rid, room] : rooms_) {
        const auto rit = by_room.find(rid);
        std::vector<OutgoingMessage> msgs = room->take_outgoing();
        if (rit == by_room.end()) continue;  // 房间里没有在线连接：消息照 take 掉防堆积
        for (const auto& out : msgs) {
            if (out.to < 0) {
                for (int cid : rit->second) {
                    send_to_conn(cid, out.msg);
                }
            } else {
                // 定向发给某个成员
                for (int cid : rit->second) {
                    const auto sit = sessions_.find(cid);
                    if (sit != sessions_.end() && sit->second.member_id == out.to) {
                        send_to_conn(cid, out.msg);
                    }
                }
            }
        }
    }
}

// 按连接偏好投递：声明了 trail 的连接收原消息；其余连接（C++ 客户端）先把
// trail 字段去掉再发——它们的网络表示与没有这个字段之前完全一致，
// "本地重采样"与"服务器下发"两种模式因此可以共存
void Server::send_to_conn(int conn_id, const json& msg) {
    Connection* c = find_connection(conn_id);
    if (!c) return;
    const auto sit = sessions_.find(conn_id);
    const bool wants = sit != sessions_.end() && sit->second.wants_trail;
    if (wants || !msg.contains("trail")) {
        c->send(msg);
        return;
    }
    json stripped = msg;
    stripped.erase("trail");
    c->send(stripped);
}

Connection* Server::find_connection(int conn_id) {
    auto it = conns_.find(conn_id);
    return it == conns_.end() ? nullptr : it->second.get();
}

json Server::room_list_json() const {
    json list = json::array();
    for (const auto& [rid, room] : rooms_) {
        // 参战与观战分开报数：大厅列表里两者不是一回事
        list.push_back(json{{"id", rid},
                            {"players", room->player_count()},
                            {"spectators", room->spectator_count()},
                            {"in_game", room->in_game()},
                            {"has_password", room->has_password()}});
    }
    return msg::make_room_list(std::move(list));
}

}  // namespace fg
