#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <numbers>
#include <optional>

#include "ai.h"
#include "curve.h"
#include "render.h"
#include "ui_style.h"

namespace fg {

namespace {
// 参数范围输入解析：只有整串都是合法数字才认（"1e2"、"2.5" 可以）。
// 裸 "e"、"-"、"1.2.3" 这类残留输入若用 atof 会静默变成 0 或截断值，
// 被当成"玩家填的数"提交出去
std::optional<double> parse_range_text(const std::string& s) {
    if (s.empty()) return std::nullopt;
    const char* begin = s.c_str();
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end == begin || *end != '\0') return std::nullopt;
    // 溢出成 inf（如输入 1e999）不是合法范围值：传进 core 是脏输入，
    // 联机时还会在请求 JSON 序列化处直接翻车（inf 不能序列化）
    if (!std::isfinite(v)) return std::nullopt;
    return v;
}

// 发射角输入框解析（文本为"度"）：空或非法按 0 处理（0 = 沿轴出发），
// 并夹到 ±85（与 core 里"过于竖直"的校验同一档）
double parse_angle_deg(const std::string& s) {
    if (s.empty()) return 0.0;
    const char* begin = s.c_str();
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end == begin || *end != '\0' || !std::isfinite(v)) return 0.0;
    return std::clamp(v, -85.0, 85.0);
}
}  // namespace

// ---------------- 主循环 ----------------

int App::run() {
    InitWindow(kWinW, kWinH, "函数对战 Function Battle");
    SetTargetFPS(60);
    load_ui_font();
    while (!WindowShouldClose() && !quit_) {
        // 与服务器一致地钳制帧间隔：拖动/最小化窗口或系统挂起后 GetFrameTime()
        // 可能是数秒的大值，不钳制的话单机模拟会在一帧内瞬完 AI 思考、
        // 越过输入时限触发跳回合、炮弹整段瞬移
        frame_dt_ = std::min(GetFrameTime(), 0.1f);
        update();
        BeginDrawing();
        draw();
        EndDrawing();
    }
    CloseWindow();
    return 0;
}

void App::update() {
    // 意外断线（online_ 还亮着而连接已断）：清掉会话状态并退回大厅。不处理的话
    // 各界面会静默卡死——按钮点了消息发不出去、对局回合永不推进且没有提示。
    // 主动"断开连接"会先置 online_=false，不会走到这里
    if (online_ && !net_.connected()) {
        online_ = false;
        in_room_ = false;
        my_room_id_ = -1;
        my_member_id_ = -1;
        host_member_id_ = -1;
        battle_.active = false;
        battle_.submitted = false;
        battle_.leaving_spectate = false;
        // 断线时这两个标志必须一起清：create_cancelled_ 残留会让重连后新建的房间
        // 一收到 joined 就被"代发 leave"踢出去；silent_leave_ 残留会让下次真正
        // 离开房间时提示被吞
        create_cancelled_ = false;
        silent_leave_ = false;
        // 坏帧（超长等）导致的断线只把 connected 置了假，socket 还挂着：
        // 在主线程安全地走一次完整清理（stop → join → 关 socket），
        // 避免坏帧之后到下次连接前的一段 socket 未释放窗口
        net_.disconnect();
        set_status("与服务器断开连接");
        if (screen_ == Screen::Battle || screen_ == Screen::CreateRoom) {
            screen_ = Screen::Lobby;
        }
    }
    switch (screen_) {
        case Screen::Menu: update_menu(); break;
        case Screen::LocalSetup: update_local_setup(); break;
        case Screen::Lobby: update_lobby(); break;
        case Screen::CreateRoom: update_create_room(); break;
        case Screen::Battle: update_battle(); break;
    }
    // 弹头本地推进：服务器约 20Hz 降频广播位置（Room::tick），只靠消息到达时更新
    // 位置会一顿一顿（长弹道尤其明显）。目标 = 最新 travel 按流逝时间外推出的
    // "服务器此刻应在的位置"——消息到达时天然连续、不会瞬移；弹头按比值（dt×12）
    // 向目标收敛，抹掉 RTT 抖动（约几十毫秒的平滑滞后）。停滞或超过 0.25s 收不到
    // 广播（炮弹已销毁）即停止推进，不会出现"炸坑后还在继续飞"
    if (battle_.has_path && battle_.has_projectile && !battle_.projectile_stopped &&
        GetTime() - battle_.projectile_msg_time < 0.25) {
        const double target =
            battle_.projectile_net_travel +
            battle_.projectile_speed * (GetTime() - battle_.projectile_msg_time);
        const double k = std::min(1.0, static_cast<double>(frame_dt_) * 12.0);
        battle_.projectile_travel += (target - battle_.projectile_travel) * k;
        battle_.projectile = battle_.projectile_path.at(battle_.projectile_travel);
    }
    // 本帧新登记的字形在这里统一重建一次：紧随其后的 draw() 里名字不会缺字
    flush_glyph_reload();
}

void App::draw() {
    ui_click_consumed_ = false;
    // 背景渐变
    DrawRectangleGradientV(0, 0, kWinW, kWinH, kBgTop, kBgBottom);
    switch (screen_) {
        case Screen::Menu: draw_menu(); break;
        case Screen::LocalSetup: draw_local_setup(); break;
        case Screen::Lobby: draw_lobby(); break;
        case Screen::CreateRoom: draw_create_room(); break;
        case Screen::Battle: draw_battle(); break;
    }
    // 点击落在按钮/输入框之外（如战场空白）→ 取消全部输入焦点，
    // 免得玩家点完地图后按键还继续打进表达式框。
    // 帮助面板（模态）开着时外部点击由面板自己处理，不在这里清焦点。
    // 这个判断放在 draw()（而不是 update()）是有意的：ui_click_consumed_ 在 draw()
    // 开头重置、绘制控件时收集、本行消费，"当帧重置-收集-消费"闭环自洽；
    // 键盘则在下一帧的 update() 里被输入框读取，先后顺序天然正确。别把本段挪进
    // update()：那样会形成"同帧先被输入框吃键、后被这里清焦点"的坏序
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !ui_click_consumed_ && !help_modal_active()) {
        clear_text_focus();
    }
}

// ---------------- 网络消息 ----------------

void App::process_network() {
    if (!online_) return;
    for (const auto& m : net_.poll()) {
        // 主流路径不抛（handle_server_message 用 protocol.h 的 json_* 助手做类型检查取值）；
        // 这里的兜底防止未预期的异常把客户端整个拖崩：单帧出错只忽略该帧
        try {
            handle_server_message(m);
        } catch (const std::exception&) {
            // 忽略这一帧
        }
    }
}

void App::handle_server_message(const json& m) {
    const std::string type = json_str(m, "type");
    if (type == msg_type::kError) {
        set_status(json_str(m, "message", "未知错误"));
        // 任何错误都复位 silent_leave_：它只该活到"取消创建时代发的那次 leave_room
        // 的响应"为止。响应若是错误（房间已销毁、已不在房间等），kLeftRoom 不会再来，
        // 标志滞留会让下一次真正离开房间时"已离开房间"的提示被静默吞掉
        silent_leave_ = false;
        // "再来一局"被拒（如 already_started）时恢复按钮可点，否则会永久置灰
        battle_.restart_sent = false;
        // 只有"对提交的拒绝"（context=submit）才复位提交标记：被拒必须能重提，
        // 否则本回合卡到超时。不能写成"收到任何 error 都复位"——对局中未来会有
        // 别的错误路径，那时会把一个已被接受、炮弹在飞的提交恢复成可重提（假提交一闪）
        if (json_str(m, "context", "") == "submit" && battle_.submitted) battle_.submitted = false;
        // "退出观战"发出的 leave_room 被拒（context="leave"）时复位按钮状态（否则
        // 按钮永久消失、观战者卡在对局屏只能关窗口）；room_closed 是房间已销毁的
        // 组通用错误（无 context），观战者等待期间同样要据此恢复并退回大厅
        if (json_str(m, "context", "") == "leave" || json_str(m, "code", "") == "room_closed") {
            battle_.leaving_spectate = false;
        }
        // 房间已销毁：会话状态清干净并退回大厅（房间都没了，界面不该再留着）
        if (json_str(m, "code", "") == "room_closed") {
            in_room_ = false;
            my_room_id_ = -1;
            if (screen_ == Screen::Battle) screen_ = Screen::Lobby;
        }
    } else if (type == msg_type::kRoomList) {
        room_list_ = json_array(m, "rooms");
    } else if (type == msg_type::kJoined) {
        // 创建房间界面点了"返回"后 joined 才到（网络慢或手快）：本意是取消——
        // 照实记录会话 id 后立刻代发退出，不置 in_room_、不切屏，
        // 否则用户以为已取消的房间会自己在大厅里冒出来
        if (create_cancelled_) {
            create_cancelled_ = false;
            silent_leave_ = true;
            my_room_id_ = json_int(m, "room_id", -1);
            my_member_id_ = json_int(m, "member_id", -1);
            host_member_id_ = json_int(m, "host", -1);
            net_.send(json{{"type", msg_type::kLeaveRoom}});
            return;
        }
        my_room_id_ = json_int(m, "room_id", -1);
        my_member_id_ = json_int(m, "member_id", -1);
        host_member_id_ = json_int(m, "host", -1);
        in_room_ = true;
        // 创建成功后从"地图设置"界面进入房间准备界面
        if (screen_ == Screen::CreateRoom) screen_ = Screen::Lobby;
    } else if (type == msg_type::kRoomState) {
        room_members_ = json_array(m, "members");
        host_member_id_ = json_int(m, "host", host_member_id_);
        if (m.contains("room")) room_cfg_ = m["room"];
        if (m.contains("next_match")) {
            match_cfg_ = m["next_match"];
            // "固定种子"输入框跟随房间状态（房主改过就显示服务端的值）；
            // 框有焦点 = 用户正在编辑（典型是清空后准备重输），此时不回填覆盖。
            // next_match.seed 为 null = 每把随机，不显示固定值
            if (room_seed_fixed() && room_seed_box_.text.empty() && !room_seed_box_.focused) {
                room_seed_box_.text = std::to_string(json_uint(match_cfg_, "seed", 0ULL));
            }
        }
        for (const auto& mem : room_members_) ensure_glyphs(json_str(mem, "name"));
    } else if (type == msg_type::kLeftRoom || type == msg_type::kKicked) {
        // 代发退出（创建取消）不弹提示：用户根本没进过房间
        if (!silent_leave_) {
            set_status((type == msg_type::kKicked) ? "被移出房间" : "已离开房间");
        }
        silent_leave_ = false;
        in_room_ = false;
        my_room_id_ = -1;
        my_member_id_ = -1;  // 身份一并清掉：迟到的 room_state 等消息不会再按旧身份解析
        host_member_id_ = -1;
        // 观战者在对局进行中可以随时离开：把对局屏切回大厅，否则画面会冻结在
        // 不会再有新消息（已离开房间收不到广播）的对局上；
        // 从房间准备界面离开时屏幕本来就是大厅，无需切换
        if (screen_ == Screen::Battle) screen_ = Screen::Lobby;
        // 房间界面的状态只属于上一个房间：成员列表滚回顶部，种子框里的旧文本
        // 否则会被带进新房间、当成新房间的"固定种子"
        member_first_ = 0;
        room_seed_box_.text.clear();
    } else if (type == msg_type::kGameStart) {
        battle_.cfg = config_from_json(
            m.contains("room") ? m["room"] : json::object(),
            m.contains("world") ? m["world"] : json::object(),
            m.contains("match") ? m["match"] : json::object());
        battle_.online_players.clear();
        battle_.online_obstacles.clear();
        for (const auto& p : json_array(m, "players")) {
            battle_.online_players.push_back(player_from_json(p));
        }
        for (const auto& o : json_array(m, "obstacles")) {
            battle_.online_obstacles.push_back(obstacle_from_json(o));
        }
        battle_.players = &battle_.online_players;
        battle_.obstacles = &battle_.online_obstacles;
        for (const auto& p : battle_.online_players) ensure_glyphs(p.name);
        battle_.my_id = my_member_id_;
        battle_.active = true;
        battle_.winner = -1;
        battle_.has_projectile = false;
        battle_.has_path = false;
        // 中途观战快照的 game_start 带当前回合号（飞行中进入也能对齐；正常开局不带）
        battle_.round = json_int(m, "round", 0);
        battle_.current_player = -1;
        battle_.submitted = false;
        battle_.leaving_spectate = false;
        battle_.restart_sent = false;
        battle_.next_player = -1;
        battle_.next_soldier = 0;
        battle_.preview_text.clear();
        battle_.last_func.clear();
        battle_.skip_voted.clear();
        battle_.skip_total = 0;
        // 预览发送的节流状态归零：否则上一局末尾的发送时刻会拦掉新局第一条预览；
        // 观战者的本地镜像视角同样复位（新一局从常规视角起步，防止"没点过却是镜像"）
        sent_preview_text_.clear();
        last_preview_sent_ = -1e9;
        spectate_mirror_ = false;
        screen_ = Screen::Battle;
        status_.clear();
        clear_text_focus();  // 清掉大厅/房间界面残留的焦点，避免多个框同时吃键盘
        // 新对局从"没有帮助面板"开始：帮助开着时被断线/踢出再重新开局，
        // 残留的 help_open_ 会让面板一进对局就弹出来挡着
        help_open_ = false;
        // 给参战者表达式框焦点；观战者不在阵容里，不给——无意敲键会打进字符，
        // 虽然 can_submit 已保证观战者不会误提交
        if (!am_spectating()) expr_a_.focused = true;
    } else if (type == msg_type::kTurnBegin) {
        battle_.current_player = json_int(m, "player_id", -1);
        battle_.next_player = json_int(m, "next_player", -1);
        battle_.next_soldier = json_int(m, "next_soldier", 0);
        // 同步出手士兵游标：面板上"我的发射点"、轨迹起点等都要用当前士兵的位置。
        // 索引来自网络（客户端可连任意服务器），越界（含负数）时忽略
        {
            const int soldier = json_int(m, "soldier", 0);
            for (auto& p : battle_.online_players) {
                if (p.id == battle_.current_player && soldier >= 0 &&
                    soldier < static_cast<int>(p.soldiers.size())) {
                    p.current_soldier = soldier;
                }
            }
        }
        battle_.input_left = json_double(m, "input_seconds", 120.0);
        battle_.turn_start_left = battle_.input_left;
        battle_.turn_begin_time = GetTime();
        // 以服务器权威回合号为准（中途观战者据此对齐）；无该字段的旧实现按自增兜底
        const int r = json_int(m, "round", 0);
        battle_.round = (r > 0) ? r : (battle_.round + 1);
        battle_.submitted = false;
        // 新一轮开始：清掉上一轮的炮弹与弹道痕迹
        battle_.has_projectile = false;
        battle_.has_path = false;
        battle_.preview_text.clear();  // 上一轮的"正在写"不该残留到新回合
        sent_preview_text_.clear();    // 允许下一轮用相同文本时重新广播"正在写"
        preview_key_.used = false;  // 预览缓存失效（下一帧轮到我就重算）
        if (battle_.current_player == battle_.my_id) {
            // 先清掉上一回合残留的焦点（如参数范围框），否则 expr_a_ 与旧焦点框同时吃键盘
            clear_text_focus();
            expr_a_.focused = true;
        }
    } else if (type == msg_type::kTurnSkipped) {
        // 跳过与随后的 turn_begin 由服务器同批下发、客户端同帧处理完，界面无中间帧
    } else if (type == msg_type::kShot) {
        // 用 request + 发射者位置在本地重建轨迹（与服务器完全一致）
        Error err;
        auto req = curve_request_from_json(m.contains("request") ? m["request"] : json::object(), err);
        const int owner = json_int(m, "owner", -1);
        const int owner_soldier = json_int(m, "soldier", 0);
        Vec2 origin(0.0, 0.0);
        bool found = false;
        if (battle_.players) {
            for (const auto& p : *battle_.players) {
                if (p.id == owner) {
                    if (owner_soldier >= 0 && owner_soldier < static_cast<int>(p.soldiers.size())) {
                        origin = p.soldiers[static_cast<size_t>(owner_soldier)].pos;
                    }
                    found = true;
                }
            }
        }
        battle_.has_projectile = false;
        battle_.has_path = false;
        if (req) {
            // "最近用过的函数"（右侧面板每人一行小字）：只存表达式本身，超长截断。
            // 截断回退到 UTF-8 字符边界：客户端连的服务器不保证是本家的，
            // 切出孤立续字节会画成乱码
            std::string f = req->expr_a;
            if (f.size() > 16) {
                size_t cut = 16;
                while (cut > 0 && (static_cast<unsigned char>(f[cut]) & 0xC0) == 0x80) --cut;
                f = f.substr(0, cut) + "…";
            }
            battle_.last_func[owner] = std::move(f);
        }
        if (req && found) {
            CurveResult res = sample_curve(*req, origin, battle_.cfg);
            if (res.ok()) {
                battle_.projectile_path = std::move(res.path);
                battle_.has_path = true;
                battle_.projectile_speed =
                    json_double(m, "speed", battle_.cfg.world.projectile_speed);
                // 渲染裁头：炮弹先"藏"在士兵方块里（跳过半个士兵边长再显示），
                // 看起来从方块边缘冒出来；纯视觉，与命中判定无关
                battle_.projectile_travel =
                    std::min(battle_.cfg.world.player_half_size, battle_.projectile_path.length());
                battle_.projectile = battle_.projectile_path.at(battle_.projectile_travel);
                battle_.projectile_net_travel = 0.0;
                battle_.projectile_stopped = false;
                battle_.projectile_msg_time = GetTime();
                battle_.has_projectile = true;
            }
        }
    } else if (type == msg_type::kProjectiles) {
        const json list = json_array(m, "list");
        if (!list.empty() && list[0].is_object()) {
            // 位置优先按本地重建的轨迹插值（与服务器同一条曲线、同一弧长参数化）；
            // 本地轨迹缺失（重建失败）时才退回服务器坐标。list[0] 是 {id,pos,travel}
            // 对象而非坐标数组，绝不能整个丢给 vec2_from_json（那样恒得原点）
            if (battle_.has_path) {
                const double travel = json_double(list[0], "travel", 0.0);
                // 服务器是降频广播（约 20Hz，见 Room::tick：tick_phase_ % 3），弹头的
                // 逐帧位置由 App::update 连续平滑地追"外推目标"（见那里的说明）；
                // 这里只更新外推基准与停滞状态——travel 不再增长即炮弹已停（销毁）
                battle_.projectile_msg_time = GetTime();
                if (travel <= battle_.projectile_net_travel + 1e-9) {
                    battle_.projectile_stopped = true;
                } else {
                    battle_.projectile_stopped = false;
                    battle_.projectile_net_travel = travel;
                }
                battle_.has_projectile = true;
            } else if (list[0].contains("pos")) {
                battle_.projectile = vec2_from_json(list[0]["pos"]);
                battle_.has_projectile = true;
            }
        }
        // 炮弹销毁后服务器不再发位置更新；客户端上的炮弹/弹道由随后同批到达的
        // turn_begin 统一清掉（炮弹死亡与回合切换发生在服务器的同一次 tick）
    } else if (type == msg_type::kHit) {
        const int target = json_int(m, "target", -1);
        const int target_soldier = json_int(m, "soldier", 0);
        const int hp_left = json_int(m, "hp_left", 0);
        // 与 kDeath/kCrater 同风格：直接改联机数据存储（battle_.players 是它的只读视图，
        // 拿它做守卫却遍历另一个容器，容易在不相关容器上操作）
        for (auto& p : battle_.online_players) {
            if (p.id == target && target_soldier >= 0 &&
                target_soldier < static_cast<int>(p.soldiers.size())) {
                p.soldiers[static_cast<size_t>(target_soldier)].hp = hp_left;
            }
        }
    } else if (type == msg_type::kDeath) {
        const int target = json_int(m, "player_id", -1);
        const int target_soldier = json_int(m, "soldier", 0);
        for (auto& p : battle_.online_players) {
            if (p.id == target && target_soldier >= 0 &&
                target_soldier < static_cast<int>(p.soldiers.size())) {
                p.soldiers[static_cast<size_t>(target_soldier)].alive = false;
            }
        }
    } else if (type == msg_type::kCrater) {
        const int ob = json_int(m, "obstacle", -1);
        if (ob >= 0 && ob < static_cast<int>(battle_.online_obstacles.size()) && m.contains("pos")) {
            battle_.online_obstacles[static_cast<size_t>(ob)].craters.push_back(
                vec2_from_json(m["pos"]));
        }
    } else if (type == msg_type::kFunctionPreview) {
        const int from = json_int(m, "member_id", -1);
        // 只认当前回合玩家的预览：网络乱序后到的旧预览会被这里挡掉
        if (from == battle_.current_player) battle_.preview_text = json_str(m, "preview");
    } else if (type == msg_type::kSkipVote) {
        battle_.skip_voted.clear();
        for (const auto& v : json_array(m, "voted")) {
            if (v.is_number_integer()) battle_.skip_voted.push_back(v.get<int>());
        }
        battle_.skip_total = json_int(m, "total", 0);
    } else if (type == msg_type::kGameOver) {
        battle_.winner = json_int(m, "winner", 2);
        battle_.active = false;
        battle_.has_projectile = false;
        // has_path 有意保留：结算遮罩下仍能看到最后一发炮弹的完整弹道
        if (battle_.winner == 0) result_text_ = "A 队胜利";
        else if (battle_.winner == 1) result_text_ = "B 队胜利";
        else result_text_ = "平局";
    }
}

// ---------------- 单机对局 ----------------

void App::start_local_game() {
    RoomOptions room_opts;
    room_opts.soldiers_per_player = set_soldiers_;
    room_opts.coord_origin = set_coord_field_ ? CoordOrigin::Field : CoordOrigin::Soldier;
    MatchConfig mc;
    mc.hp = set_hp_;
    mc.obstacle_count = set_obstacles_;
    mc.input_seconds = static_cast<double>(set_input_seconds_);
    mc.first_team = set_first_team_;
    // 种子框：填了数字 = 固定种子；留空 = 每把随机（空 optional，开局时由房间现生成）。
    // 不能用"现取一个随机数写进模板"代替空值——那会把"每把随机"固化成固定种子，
    // 跳过地图重开还是同一张图（单机设置里就是这个观感）
    if (!seed_box_.text.empty()) {
        mc.seed = std::strtoull(seed_box_.text.c_str(), nullptr, 10);
    }

    ensure_glyphs(player_name_);
    result_text_.clear();
    // 本地对局里"我"就是房主：game_start 消息会以 my_member_id_ 认定自己的 player id
    my_member_id_ = -1;
    local_ = std::make_unique<Room>(room_opts, mc);
    // AI 的思考放到线程池里算：单机时渲染帧不会被满配的规划计算（可达数百毫秒）卡住
    local_->set_think_pool(&think_pool_);
    if (local_spectate_) {
        // 观战：我不进阵容，两队都是人机。把自己第一个加进去是为了让
        // host_id() 指向我（开局由我发起），阵容里则没有我
        local_->add_member(player_name_, false, 0, std::nullopt, MemberRole::Spectator);
        for (int i = 0; i < set_team_size_; ++i) {
            local_->add_member("AI-A" + std::to_string(i + 1), true, set_difficulty_, Team::A);
        }
    } else {
        local_->add_member(player_name_, false, 0, Team::A);  // 我
        for (int i = 1; i < set_team_size_; ++i) {
            local_->add_member("AI-A" + std::to_string(i + 1), true, set_difficulty_, Team::A);
        }
    }
    for (int i = 0; i < set_team_size_; ++i) {
        local_->add_member("AI-B" + std::to_string(i + 1), true, set_difficulty_, Team::B);
    }
    my_member_id_ = local_->host_id();
    const Error err = local_->start_game(my_member_id_);
    if (!err.ok()) {
        set_status(err.message);
        local_.reset();
        return;
    }
    online_ = false;
    battle_ = Battle{};
    battle_.my_id = my_member_id_;
    expr_a_.text.clear();
    expr_b_.text.clear();
    p_start_.text.clear();
    p_end_.text.clear();
    battle_.players = nullptr;  // 由 game_start 消息填充
    // 本地模式的"网络"就是 Room 的消息出口：喂给同一个消息处理器，保持两模式逻辑一致
    for (const auto& out : local_->take_outgoing()) handle_server_message(out.msg);
}

void App::tick_local() {
    if (!local_) return;
    local_->tick(frame_dt_);
    for (const auto& out : local_->take_outgoing()) {
        handle_server_message(out.msg);
    }
}

// ---------------- 提交与预览 ----------------

CurveRequest App::build_request_from_ui() const {
    CurveRequest req;
    req.mode = kModes[mode_index_];
    req.expr_a = expr_a_.text;
    req.expr_b = expr_b_.text;
    req.dir = (dir_index_ == 0) ? FireDir::Positive : FireDir::Negative;
    if (needs_range(req.mode)) {
        req.p_start = parse_range_text(p_start_.text);
        req.p_end = parse_range_text(p_end_.text);
    }
    // 镜像房间里的 B 队人类：所见即所写，提交时打上镜像标记（服务器在镜像空间里采样）
    req.mirror = am_i_mirrored();
    // Ypp 的发射角：输入框是"度"（相对行进方向的仰角），协议用弧度
    if (req.mode == CurveMode::Ypp) {
        req.angle = parse_angle_deg(angle_box_.text) * std::numbers::pi / 180.0;
    }
    return req;
}

bool App::can_submit() const {
    if (!battle_.active || battle_.winner >= 0 || battle_.submitted) return false;
    return battle_.current_player == battle_.my_id;
}

bool App::am_i_mirrored() const {
    if (!battle_.cfg.match.mirror_view || battle_.players == nullptr) return false;
    if (am_spectating()) return spectate_mirror_;  // 观战者不参战：视角是纯本地状态
    for (const auto& p : *battle_.players) {
        if (p.id == battle_.my_id) return p.team == Team::B;
    }
    return false;
}

Vec2 App::my_position() const {
    if (!battle_.players) return Vec2(0.0, 0.0);
    for (const auto& p : *battle_.players) {
        if (p.id == battle_.my_id) return p.current().pos;
    }
    return Vec2(0.0, 0.0);
}

bool App::am_spectating() const {
    if (!battle_.players) return false;
    for (const auto& p : *battle_.players) {
        if (p.id == battle_.my_id) return false;
    }
    return true;
}

void App::update_preview() {
    if (!can_submit() || expr_a_.text.empty()) {
        battle_.has_preview = false;
        preview_key_.used = false;  // 清空后再输入相同表达式时缓存必须失效，否则预览不重算
        return;
    }
    CurveRequest req = build_request_from_ui();
    // 输入没变化就沿用上次的预览（结果存在 battle_.preview_path 里）：
    // ExprTk 编译 + 曲线采样没必要每帧重做
    if (preview_key_.used && preview_key_.a == req.expr_a && preview_key_.b == req.expr_b &&
        preview_key_.mode == static_cast<int>(req.mode) &&
        preview_key_.dir == static_cast<int>(req.dir) && preview_key_.ps == req.p_start &&
        preview_key_.pe == req.p_end && preview_key_.mirror == req.mirror &&
        preview_key_.angle == angle_box_.text) {
        return;
    }
    preview_key_ = PreviewKey{req.expr_a,
                              req.expr_b,
                              static_cast<int>(req.mode),
                              static_cast<int>(req.dir),
                              req.p_start,
                              req.p_end,
                              req.mirror,
                              angle_box_.text,
                              true};
    battle_.has_preview = false;
    // 预览只画起点起 kPreviewTravel 一段（与服务器 preview_curve 的截断一致）：
    // 整条弹道全画出来等于提前剧透能否命中
    GameConfig preview_cfg = battle_.cfg;
    preview_cfg.world.max_travel = kPreviewTravel;
    CurveResult res = sample_curve(req, my_position(), preview_cfg);
    if (res.ok()) {
        battle_.preview_path = std::move(res.path);
        battle_.has_preview = true;
    }
}

void App::submit_current() {
    if (!can_submit()) return;
    CurveRequest req = build_request_from_ui();
    if (req.expr_a.empty()) {
        set_status("表达式不能为空");
        return;
    }
    if (online_) {
        // 本地先跑一遍同样的采样：明显会失败的请求不发往服务器，
        // 避免提交被拒后仍显示"已提交"而卡住整个回合
        CurveResult res = sample_curve(req, my_position(), battle_.cfg);
        if (!res.ok()) {
            set_status("提交失败：" + res.error.message);
            return;
        }
        // 未连接时消息会被丢弃：不能假装已提交（否则按钮卡在"已提交"、回合永不推进）
        if (!net_.send(json{{"type", msg_type::kSubmit}, {"request", to_json(req)}})) {
            set_status("与服务器断开连接");
            return;
        }
        battle_.submitted = true;
        status_.clear();
    } else if (local_) {
        const Error err = local_->submit(battle_.my_id, req);
        if (!err.ok()) {
            set_status("提交失败：" + err.message);
        } else {
            battle_.submitted = true;
            status_.clear();
        }
    }
}
}  // namespace fg
