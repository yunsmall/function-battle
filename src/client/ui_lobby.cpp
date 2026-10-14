#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>

#include "ai.h"
#include "curve.h"
#include "render.h"
#include "sha256.h"
#include "ui_style.h"

namespace fg {

// ---------------- 联机大厅 ----------------

void App::connect_server() {
    // 解析 "host:port" 或纯 host
    std::string host = server_host_;
    int port = server_port_;
    const size_t colon = host.find(':');
    if (colon != std::string::npos) {
        // 端口要全串数字且在 1~65535 内：atoi 对 "abc"/"99999"/"-1" 会给出 0 或越界值，
        // 再截成 uint16_t 会静默回绕到别的端口、连去非预期服务（如 99999 → 34463）
        const std::string tail = host.substr(colon + 1);
        char* end = nullptr;
        const long v = std::strtol(tail.c_str(), &end, 10);
        if (tail.empty() || *end != '\0' || v < 1 || v > 65535) {
            set_status("端口无效（应为 1~65535）");
            return;
        }
        port = static_cast<int>(v);
        host = host.substr(0, colon);
    }
    if (host.empty()) host = "127.0.0.1";
    const std::string err = net_.connect_to(host, static_cast<uint16_t>(port));
    if (!err.empty()) {
        set_status(err);
        return;
    }
    online_ = true;
    set_status("已连接");
    in_room_ = false;
    my_room_id_ = -1;
    my_member_id_ = -1;
    net_.send(json{{"type", msg_type::kListRooms}});
}

void App::update_lobby() {
    // 房间准备界面只驱动房间内可见的输入框：大厅的地址/名字框此时不可见，
    // 继续驱动会把键盘输入吃进看不见的框；房间里的种子框曾漏驱动导致无法输入
    if (in_room_) {
        room_seed_box_.handle_input();
    } else {
        host_box_.handle_input();
        name_box_.handle_input();
        if (name_box_.text.empty()) player_name_ = "玩家";
        else player_name_ = name_box_.text;
    }
    process_network();
}

void App::send_room_config(const json& patch) {
    // set_match_config 是合并补丁：只发改动键，服务器在"下一把配置"模板上合并、校验、广播。
    // 房间属性/世界常量没有修改入口：补丁里出现它们的字段会被服务器拒绝（cannot_change_room）
    net_.send(json{{"type", msg_type::kSetMatchConfig}, {"match", patch}});
}

void App::draw_lobby() {
    const Rectangle left{40.0f, 80.0f, 700.0f, 660.0f};
    const Rectangle right{770.0f, 80.0f, 470.0f, 660.0f};
    DrawRectangleRounded(left, 0.04f, 10, kPanel);
    DrawRectangleRoundedLines(left, 0.04f, 10, kPanelBorder);
    DrawRectangleRounded(right, 0.04f, 10, kPanel);
    DrawRectangleRoundedLines(right, 0.04f, 10, kPanelBorder);

    if (ui_button(Rectangle{40.0f, 20.0f, 150.0f, 40.0f}, "返回主菜单")) {
        net_.disconnect();
        online_ = false;
        in_room_ = false;
        screen_ = Screen::Menu;
    }
    if (in_room_) {
        // 已在房间：准备界面（成员 + 下一把配置），打完一把也会回到这里
        draw_room_area(left, right);
        if (!status_.empty()) {
            text(status_, left.x + 24.0f, left.y + left.height - 34.0f, 17.0f, kDanger);
        }
        return;
    }

    // ---- 连接栏 ----
    text("服务器", left.x + 24.0f, left.y + 20.0f, 24.0f, kTextMain);
    ui_textbox(host_box_, Rectangle{left.x + 24.0f, left.y + 58.0f, 380.0f, 38.0f},
               "127.0.0.1:9527");
    if (host_box_.text.empty()) server_host_ = "127.0.0.1";
    else server_host_ = host_box_.text;
    const bool conn = net_.connected();
    if (conn) {
        if (ui_button(Rectangle{left.x + 420.0f, left.y + 58.0f, 120.0f, 38.0f}, "断开连接")) {
            net_.disconnect();
            online_ = false;
            in_room_ = false;
            set_status("未连接");
        }
        text("已连接", left.x + 556.0f, left.y + 66.0f, 18.0f, kSuccess);
    } else {
        if (ui_button(Rectangle{left.x + 420.0f, left.y + 58.0f, 120.0f, 38.0f}, "连接服务器")) {
            connect_server();
        }
        text("未连接", left.x + 556.0f, left.y + 66.0f, 18.0f, kTextFaint);
    }
    // 玩家名
    ui_textbox(name_box_, Rectangle{left.x + 24.0f, left.y + 108.0f, 200.0f, 36.0f}, "玩家名");

    // ---- 房间列表 ----
    text("房间列表", left.x + 24.0f, left.y + 172.0f, 24.0f, kTextMain);
    if (ui_button(Rectangle{left.x + 210.0f, left.y + 168.0f, 96.0f, 34.0f}, "刷新", false,
                  conn)) {
        net_.send(json{{"type", msg_type::kListRooms}});
    }
    // 创建房间：先进地图设置界面
    if (ui_button(Rectangle{left.x + 318.0f, left.y + 168.0f, 150.0f, 34.0f}, "创建房间", false,
                  conn)) {
        screen_ = Screen::CreateRoom;
        status_.clear();
    }

    float ry = left.y + 224.0f;
    if (room_list_.empty()) {
        text("暂无房间，点\"创建房间\"开一局", left.x + 24.0f, ry, 18.0f, kTextFaint);
    }
    for (const auto& r : room_list_) {
        // 底部留给"加入密码"输入框（有密码的房间在列表里标了"有密码"）
        if (ry > left.y + left.height - 118.0f) break;
        const Rectangle row{left.x + 24.0f, ry, left.width - 48.0f, 52.0f};
        DrawRectangleRounded(row, 0.3f, 6, kPanelSoft);
        DrawRectangleRoundedLines(row, 0.3f, 6, kPanelBorder);
        char buf[160];
        const bool in_game = json_bool(r, "in_game", false);
        std::snprintf(buf, sizeof(buf), "房间 %d ｜ 参战 %d ｜ 观战 %d ｜ %s%s", json_int(r, "id", 0),
                      json_int(r, "players", 0), json_int(r, "spectators", 0),
                      in_game ? "进行中" : "等待中",
                      json_bool(r, "has_password", false) ? " ｜ 有密码" : "");
        text(buf, row.x + 16.0f, row.y + 15.0f, 19.0f, in_game ? kTextDim : kTextMain);
        if (!in_room_ && conn) {
            // 观战任何房间都能进（进行中的局也能进去看）；加入只限等待中的房间。
            // 有密码的房间必须带上本地算好的哈希（在列表下方的"加入密码"框里填）
            auto join_msg = [&](bool spectate) {
                json j{{"type", msg_type::kJoinRoom},
                       {"room_id", json_int(r, "id", -1)},
                       {"name", player_name_}};
                if (spectate) j["spectate"] = true;
                if (!join_password_box_.text.empty()) {
                    j["password_hash"] = sha256_hex(join_password_box_.text);
                }
                net_.send(j);
            };
            if (ui_button(Rectangle{row.x + row.width - 196.0f, row.y + 9.0f, 76.0f, 34.0f},
                          "观战")) {
                create_cancelled_ = false;  // 明确的加入意图：撤销"创建已取消"的待定状态
                join_msg(true);
            }
            if (!in_game &&
                ui_button(Rectangle{row.x + row.width - 116.0f, row.y + 9.0f, 100.0f, 34.0f},
                          "加入")) {
                create_cancelled_ = false;  // 同上
                join_msg(false);
            }
        }
        ry += 62.0f;
    }
    // 加入密码：有密码的房间在列表里标了"有密码"，在这里填一次、加入/观战共用；
    // 明文输入、本地哈希后随 join_room 上行（传输中不出现明文）
    ui_textbox(join_password_box_,
               Rectangle{left.x + 24.0f, left.y + left.height - 92.0f, 300.0f, 36.0f},
               "加入密码（房间有密码时填）");

    // ---- 右侧：进入房间后的说明 ----
    text("进入房间后可以：", right.x + 28.0f, right.y + 40.0f, 20.0f, kTextDim);
    text("· 调整队伍与阵容，添加人机", right.x + 28.0f, right.y + 84.0f, 18.0f, kTextFaint);
    text("· 设置下一把的规则（先手、血量、时限）", right.x + 28.0f, right.y + 114.0f, 18.0f,
         kTextFaint);
    text("· 点\"开始游戏\"进入对局", right.x + 28.0f, right.y + 144.0f, 18.0f, kTextFaint);
    text("· 打完一把回到这里，改完配置再开一局", right.x + 28.0f, right.y + 174.0f, 18.0f,
         kTextFaint);
}

// 房间准备界面：左侧成员与人机管理，右侧"下一把"的规则配置（房主可改，其他人只读）。
// 对局结束后服务器会推 room_state，客户端回到本界面配置下一把。
void App::draw_room_area(const Rectangle& left, const Rectangle& right) {
    const bool host = is_host();

    // ---- 左：成员列表 ----
    text("房间 " + std::to_string(my_room_id_), left.x + 24.0f, left.y + 18.0f, 24.0f, kTextMain);
    // 自己的身份（标题提示与"转为观战/参战"按钮共用）：观战者不在对局阵容里
    bool self_spectating = false;
    for (const auto& m : room_members_) {
        if (json_int(m, "id", -1) == my_member_id_) {
            self_spectating = json_str(m, "role", "player") == "spectator";
        }
    }
    std::string self_label = host ? "你是房主" : "你是队员";
    if (self_spectating) self_label += "（观战中）";
    text(self_label, left.x + 190.0f, left.y + 24.0f, 17.0f, kTextFaint);

    float my = left.y + 62.0f;
    // 行高随人数压缩，但不低于行内按钮需要的高度（38）；装不下的成员用滚轮滚动
    // 查看（房主必须能操作到列表末尾的成员）——强行塞下 30 人会让行高只剩几像素、
    // 按钮跨行重叠、点击命中错乱
    const float list_h = left.y + left.height - 200.0f - my;
    const size_t total = room_members_.size();
    const size_t visible =
        std::max<size_t>(1, std::min<size_t>(total, static_cast<size_t>(list_h / 38.0f)));
    const float row_h = std::min(52.0f, list_h / static_cast<float>(visible));
    if (total > visible &&
        CheckCollisionPointRec(GetMousePosition(), Rectangle{left.x, my, left.width, list_h})) {
        member_first_ -= static_cast<int>(GetMouseWheelMove() * 3.0f);
    }
    // 人数变化后把越界的滚动位置拉回来（max_first 先判后减，避免 size_t 下溢）
    const int max_first = (total > visible) ? static_cast<int>(total - visible) : 0;
    member_first_ = std::clamp(member_first_, 0, max_first);
    for (size_t i = static_cast<size_t>(member_first_);
         i < std::min<size_t>(total, member_first_ + visible); ++i) {
        const auto& m = room_members_[i];
        const int mid = json_int(m, "id", -1);
        const std::string team = json_str(m, "team", "A");
        const bool is_ai = json_bool(m, "is_ai", false);
        const bool spectating = json_str(m, "role", "player") == "spectator";
        const Rectangle row{left.x + 20.0f, my, left.width - 40.0f, row_h - 6.0f};
        DrawRectangleRounded(row, 0.35f, 6, kPanelSoft);
        // 队色条：观战者用灰色，与两队颜色明确区分（它不是"第三种队"）
        DrawRectangleRounded(Rectangle{row.x + 6.0f, row.y + 8.0f, 5.0f, row.height - 16.0f}, 0.6f,
                             4, spectating ? Color{116, 120, 136, 255}
                                           : (team == "A" ? kTeamA : kTeamB));
        std::string label = json_str(m, "name", "?");
        if (spectating) {
            label += "（观战者）";
        } else if (is_ai) {
            const char* d[] = {"简单", "普通", "困难", "大神"};
            label += std::string("（人机·") +
                     d[std::clamp(json_int(m, "difficulty", 1), 0,
                                  static_cast<int>(std::size(d)) - 1)] +
                     "）";
        }
        if (mid == my_member_id_) label += "（你）";
        if (mid == host_member_id_) label += "（房主）";
        text(label, row.x + 22.0f, row.y + (row.height - 18.0f) * 0.5f, 18.0f, kTextMain);
        if (host && mid != my_member_id_) {
            // 按钮要放得下"换到X队"（4 个汉字）与"踢出"（2 个汉字）
            const float btn_y = row.y + (row.height - 30.0f) * 0.5f;
            const float bx = row.x + row.width - 120.0f;
            // 观战者没有队伍：不给"换队"（服务器也会拒绝），只留"踢出"
            if (!spectating) {
                const std::string other = (team == "A") ? "B" : "A";
                if (ui_button(Rectangle{bx, btn_y, 72.0f, 30.0f}, "换到" + other + "队")) {
                    net_.send(json{{"type", msg_type::kSetTeam}, {"member_id", mid}, {"team", other}});
                }
            }
            if (ui_button(Rectangle{bx + 76.0f, btn_y, 44.0f, 30.0f}, "踢出")) {
                net_.send(json{{"type", msg_type::kKick}, {"member_id", mid}});
            }
        } else if (mid == my_member_id_) {
            // 自己的行：参战 <-> 观战切换（任何成员都能对自己用；房主也不例外）
            const float btn_y = row.y + (row.height - 30.0f) * 0.5f;
            if (ui_button(Rectangle{row.x + row.width - 130.0f, btn_y, 114.0f, 30.0f},
                          spectating ? "转为参战" : "转为观战")) {
                net_.send(json{{"type", msg_type::kSetSpectate}, {"spectate", !spectating}});
            }
        }
        my += row_h;
    }
    if (total > visible) {
        text("共 " + std::to_string(total) + " 人，滚轮查看", left.x + 22.0f, my + 6.0f, 16.0f,
             kTextFaint);
    }

    // 添加人机（仅房主）
    if (host) {
        const float by = left.y + left.height - 186.0f;
        text("添加人机", left.x + 24.0f, by, 20.0f, kTextDim);
        const char* diffs[] = {"简单", "普通", "困难", "大神"};
        for (int i = 0; i < static_cast<int>(std::size(diffs)); ++i) {
            if (ui_button(Rectangle{left.x + 24.0f + i * 92.0f, by + 28.0f, 84.0f, 32.0f}, diffs[i],
                          ai_diff_ == i)) {
                ai_diff_ = i;
            }
        }
        if (ui_button(Rectangle{left.x + 24.0f, by + 68.0f, 180.0f, 36.0f}, "加入 A 队")) {
            net_.send(json{{"type", msg_type::kAddAi}, {"difficulty", ai_diff_}, {"team", "A"}});
        }
        if (ui_button(Rectangle{left.x + 214.0f, by + 68.0f, 180.0f, 36.0f}, "加入 B 队")) {
            net_.send(json{{"type", msg_type::kAddAi}, {"difficulty", ai_diff_}, {"team", "B"}});
        }
    }

    // ---- 右：下一把配置 ----
    text("下一把配置", right.x + 24.0f, right.y + 18.0f, 24.0f, kTextMain);
    text(host ? "改完点\"开始游戏\"，对下一把生效" : "只有房主可以修改（现在只读）",
         right.x + 24.0f, right.y + 52.0f, 16.0f, kTextFaint);
    // 士兵数与坐标系是"房间的属性"：创建时定死（服务器会拒绝中途修改），这里只展示
    text("每人士兵数 " + std::to_string(json_int(room_cfg_, "soldiers_per_player", 3)) +
             "（创建时定，不可更改）",
         right.x + 24.0f, right.y + 72.0f, 15.0f, kTextFaint);
    text("坐标系 " +
             std::string(json_str(room_cfg_, "coord_origin", "soldier") == "field" ? "地图坐标"
                                                                                    : "以你为原点") +
             "（创建时定，不可更改）",
         right.x + 24.0f, right.y + 92.0f, 15.0f, kTextFaint);

    const float lx = right.x + 24.0f;
    const float cx = right.x + 148.0f;
    float y = right.y + 112.0f;

    // 先手队伍
    text("先手队伍", lx, y + 8.0f, 19.0f, kTextDim);
    {
        const char* names[] = {"随机", "A 队", "B 队"};
        const int cur = json_int(match_cfg_, "first_team", -1);
        for (int i = 0; i < static_cast<int>(std::size(names)); ++i) {
            if (ui_button(Rectangle{cx + i * 76.0f, y, 68.0f, 36.0f}, names[i], cur == i - 1,
                          host)) {
                send_room_config(json{{"first_team", i - 1}});
            }
        }
    }
    y += 54.0f;

    // 每人血量
    text("每人血量", lx, y + 8.0f, 19.0f, kTextDim);
    {
        const int options[] = {1, 2, 3, 5};
        const int cur = json_int(match_cfg_, "hp", 1);
        for (int i = 0; i < static_cast<int>(std::size(options)); ++i) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", options[i]);
            if (ui_button(Rectangle{cx + i * 56.0f, y, 48.0f, 36.0f}, buf, cur == options[i],
                          host)) {
                send_room_config(json{{"hp", options[i]}});
            }
        }
    }
    y += 54.0f;

    // 每回合输入时限
    text("输入时限", lx, y + 8.0f, 19.0f, kTextDim);
    {
        const double options[] = {15.0, 30.0, 60.0, 120.0};
        const double cur = json_double(match_cfg_, "input_seconds", 120.0);
        for (int i = 0; i < static_cast<int>(std::size(options)); ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f 秒", options[i]);
            if (ui_button(Rectangle{cx + i * 76.0f, y, 68.0f, 36.0f}, buf,
                          std::abs(cur - options[i]) < 0.5, host)) {
                send_room_config(json{{"input_seconds", options[i]}});
            }
        }
    }
    y += 54.0f;

    // 遮挡物数量
    text("遮挡物数量", lx, y + 8.0f, 19.0f, kTextDim);
    {
        const int options[] = {0, 5, 10, 15, 20, 30};
        const int cur = json_int(match_cfg_, "obstacle_count", 15);
        for (int i = 0; i < static_cast<int>(std::size(options)); ++i) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", options[i]);
            // 六档按钮要比其他行更收：52 步进下整行仍留出右缘余量
            if (ui_button(Rectangle{cx + i * 52.0f, y, 48.0f, 36.0f}, buf, cur == options[i],
                          host)) {
                send_room_config(json{{"obstacle_count", options[i]}});
            }
        }
    }
    y += 54.0f;

    // 命中后的展示停顿（炮弹消失后等这么久再轮到下一位，让命中画面停一会儿）
    text("展示停顿", lx, y + 8.0f, 19.0f, kTextDim);
    {
        const double options[] = {0.0, 1.0, 3.0, 5.0};
        const double cur = json_double(match_cfg_, "turn_delay", 3.0);
        for (int i = 0; i < static_cast<int>(std::size(options)); ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0f 秒", options[i]);
            if (ui_button(Rectangle{cx + i * 76.0f, y, 68.0f, 36.0f}, buf,
                          std::abs(cur - options[i]) < 0.25, host)) {
                send_room_config(json{{"turn_delay", options[i]}});
            }
        }
    }
    y += 62.0f;

    // 地图种子：每把随机 / 固定（非房主只读：按钮禁用而不消费点击，免得误清输入焦点）
    text("地图种子", lx, y + 8.0f, 19.0f, kTextDim);
    if (ui_button(Rectangle{cx, y, 110.0f, 36.0f}, "每把随机", !room_seed_fixed(), host)) {
        room_seed_box_.text.clear();
        send_room_config(json{{"seed", nullptr}});
    }
    if (ui_button(Rectangle{cx + 118.0f, y, 110.0f, 36.0f}, "固定种子", room_seed_fixed(),
                  host)) {
        uint64_t v = 12345;
        if (!room_seed_box_.text.empty()) {
            v = std::strtoull(room_seed_box_.text.c_str(), nullptr, 10);
        }
        send_room_config(json{{"seed", v}});
    }
    y += 46.0f;
    // 种子是整数：只放行数字字符，字母/中文会被 strtoull 静默吃成 0（玩家会以为用了别的种子）
    room_seed_box_.digits_only = true;
    room_seed_box_.allow_e = false;
    ui_textbox(room_seed_box_, Rectangle{cx, y, 170.0f, 34.0f}, "固定种子填这里", host);
    if (ui_button(Rectangle{cx + 180.0f, y, 76.0f, 34.0f}, "应用", false, host) &&
        !room_seed_box_.text.empty()) {
        send_room_config(json{{"seed", std::strtoull(room_seed_box_.text.c_str(), nullptr, 10)}});
    }
    y += 44.0f;
    text(room_seed_fixed() ? "固定种子：每把同一张地图" : "每开一把自动换新地图", lx, y, 16.0f,
         kTextFaint);

    // 底部：开始 / 离开
    if (host) {
        if (ui_button(Rectangle{right.x + 24.0f, right.y + right.height - 130.0f,
                                right.width - 48.0f, 52.0f},
                      "开始游戏")) {
            net_.send(json{{"type", msg_type::kStart}});
        }
    } else {
        text("等待房主开始对局…", right.x + 24.0f, right.y + right.height - 112.0f, 18.0f,
             kTextFaint);
    }
    if (ui_button(Rectangle{right.x + 24.0f, right.y + right.height - 68.0f, right.width - 48.0f,
                            44.0f},
                  "离开房间")) {
        net_.send(json{{"type", msg_type::kLeaveRoom}});
    }
}

void App::update_create_room() {
    create_seed_box_.handle_input();
    name_box_.handle_input();
    if (name_box_.text.empty()) player_name_ = "玩家";
    else player_name_ = name_box_.text;
    process_network();
}

void App::draw_create_room() {
    if (ui_button(Rectangle{40.0f, 20.0f, 150.0f, 40.0f}, "返回")) {
        // 记下"取消"意图：create_room 可能已在路上，joined 迟到时自动退出，
        // 不把用户以为已取消的房间悄悄带进大厅
        create_cancelled_ = true;
        screen_ = Screen::Lobby;
        return;
    }
    const Rectangle panel{340.0f, 80.0f, 600.0f, 640.0f};
    DrawRectangleRounded(panel, 0.04f, 10, kPanel);
    DrawRectangleRoundedLines(panel, 0.04f, 10, kPanelBorder);
    text("创建房间 · 地图设置", panel.x + 28.0f, panel.y + 20.0f, 26.0f, kTextMain);

    float y = panel.y + 88.0f;
    const float lx = panel.x + 32.0f;
    const float cx = panel.x + 200.0f;

    text("地图种子", lx, y + 8.0f, 20.0f, kTextDim);
    // 种子是整数：只放行数字字符，字母/中文会被 strtoull 静默吃成 0（玩家会以为用了别的种子）
    create_seed_box_.digits_only = true;
    create_seed_box_.allow_e = false;
    ui_textbox(create_seed_box_, Rectangle{cx, y, 220.0f, 36.0f}, "留空 = 每把随机");
    y += 54.0f;

    text("遮挡物数量", lx, y + 8.0f, 20.0f, kTextDim);
    {
        const int options[] = {0, 5, 10, 15, 20, 30};
        for (int i = 0; i < static_cast<int>(std::size(options)); ++i) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", options[i]);
            if (ui_button(Rectangle{cx + i * 66.0f, y, 56.0f, 34.0f}, buf,
                          create_obstacles_ == options[i])) {
                create_obstacles_ = options[i];
            }
        }
    }
    y += 54.0f;

    text("先手队伍", lx, y + 8.0f, 20.0f, kTextDim);
    {
        const char* names[] = {"随机", "A 队先手", "B 队先手"};
        for (int i = 0; i < static_cast<int>(std::size(names)); ++i) {
            if (ui_button(Rectangle{cx + i * 116.0f, y, 108.0f, 34.0f}, names[i],
                          create_first_team_ == i - 1)) {
                create_first_team_ = i - 1;
            }
        }
    }
    y += 54.0f;

    text("每人士兵数", lx, y + 8.0f, 20.0f, kTextDim);
    {
        const int options[] = {1, 2, 3, 4};
        for (int i = 0; i < static_cast<int>(std::size(options)); ++i) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", options[i]);
            if (ui_button(Rectangle{cx + i * 66.0f, y, 56.0f, 34.0f}, buf,
                          create_soldiers_ == options[i])) {
                create_soldiers_ = options[i];
            }
        }
        text("创建后不可更改", cx + 4 * 66.0f + 8.0f, y + 9.0f, 15.0f, kTextFaint);
    }
    y += 54.0f;

    // 曲线参考系（房间属性，创建后不可更改）：只影响 y=f(x) 与 x=f(y) 的自变量语义。
    // 两种模式的差别就是"函数写在哪套坐标里"，按钮下方一行小字随选择解释给玩家看
    text("坐标系", lx, y + 8.0f, 20.0f, kTextDim);
    {
        const char* names[] = {"以你为原点", "地图坐标"};
        for (int i = 0; i < static_cast<int>(std::size(names)); ++i) {
            if (ui_button(Rectangle{cx + i * 116.0f, y, 108.0f, 34.0f}, names[i],
                          create_coord_field_ == i)) {
                create_coord_field_ = i;
            }
        }
        text("创建后不可更改", cx + 2 * 116.0f + 8.0f, y + 9.0f, 15.0f, kTextFaint);
        text(create_coord_field_ ? "按地图坐标写：曲线整体平移到你身上（y=f(x) 上下、x=f(y) 左右）"
                                 : "以你为原点：函数从你写起，0 就在你身上",
             lx, y + 44.0f, 15.0f, kTextFaint);
    }
    y += 84.0f;  // 行距含按钮下方的一行解释

    text("你的名字", lx, y + 8.0f, 20.0f, kTextDim);
    ui_textbox(name_box_, Rectangle{cx, y, 220.0f, 36.0f}, "玩家");
    y += 60.0f;

    text("房间密码（可选）", lx, y + 8.0f, 20.0f, kTextDim);
    ui_textbox(create_password_box_, Rectangle{cx, y, 220.0f, 36.0f}, "留空 = 无密码");
    y += 54.0f;

    text("进入房间后还可以调整队伍与每一把的规则", lx, y, 17.0f, kTextFaint);

    const float by = panel.y + panel.height - 92.0f;
    if (ui_button(Rectangle{panel.x + 32.0f, by, 220.0f, 56.0f}, "创建房间")) {
        if (!net_.connected()) {
            set_status("未连接服务器");
        } else {
            // 房间属性与下一把配置分成两个对象发：room = 唯一一次设置房间属性的机会
            //（士兵数/坐标系），match = 下一把配置的初值
            json room = json::object();
            room["soldiers_per_player"] = create_soldiers_;
            room["coord_origin"] = create_coord_field_ ? "field" : "soldier";
            json match = json::object();
            match["obstacle_count"] = create_obstacles_;
            match["first_team"] = create_first_team_;
            // 地图尺寸统一用服务器默认（48x30），不提供选择
            if (create_seed_box_.text.empty()) {
                match["seed"] = nullptr;  // 每把随机
            } else {
                match["seed"] = std::strtoull(create_seed_box_.text.c_str(), nullptr, 10);
            }
            create_cancelled_ = false;  // 新的创建意图，清掉上一次可能遗留的取消标记
            json msg{{"type", msg_type::kCreateRoom},
                     {"name", player_name_},
                     {"room", room},
                     {"match", match}};
            // 房间密码：本地算 SHA-256 再上行（传输中不出现明文）；哈希=口令，
            // 不放进 room 对象（那个是房间属性的集合，另有字段校验）
            if (!create_password_box_.text.empty()) {
                msg["password_hash"] = sha256_hex(create_password_box_.text);
            }
            net_.send(msg);
        }
    }
    if (!status_.empty()) {
        text(status_, lx, by - 36.0f, 17.0f, kDanger);
    }
}
}  // namespace fg
