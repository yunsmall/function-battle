// 文件信箱式对局客户端（脚本 / AI 助手用）的共享状态与接口。
// 连服务器打一局：我 = A 队 3 士兵 vs 人机 AI 3 士兵。轮到我出手时把战场快照写进
// session/state.txt；把一条 y=f(x) 表达式写进 session/shot.txt（一行）即自动提交。
// 不做图形界面、也不需要 stdin——专给不会开窗口的使用者。
// 运行文件（state/shot/log/done）放在程序所在目录的 session/ 下。
// 实现按职责拆分：net.cpp（帧收发）/ app.cpp（对局逻辑与信箱）/ render.cpp（快照）/ main.cpp
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <nlohmann/json.hpp>

namespace fbplay {

using nlohmann::json;

struct Soldier {
    double x = 0.0, y = 0.0;
    int hp = 0, max_hp = 0;
    bool alive = true;
};

struct PlayerState {
    std::string name;
    std::string team;
    bool is_ai = false;
    std::vector<Soldier> soldiers;
};

struct Obstacle {
    bool is_circle = true;
    double cx = 0.0, cy = 0.0;          // 中心
    double radius = 0.0;                // 圆形半径
    double half_w = 0.0, half_h = 0.0;  // 矩形半宽高
    std::vector<std::pair<double, double>> craters;
};

struct App {
    // 连接与收包缓冲（帧 = 4 字节小端长度 + JSON，与原生协议一致）
    asio::ip::tcp::socket sock;
    std::string recvbuf;

    // 对局状态
    std::map<int, PlayerState> players;
    std::vector<Obstacle> obstacles;
    json cfg;
    int my_id = -1;
    const std::string my_team = "A";
    int round_no = 0;
    json last_turn;                     // 最近一次 turn_begin 帧
    std::vector<std::string> events;    // 滚动事件（只给 state.txt 留最近一批）
    std::vector<std::string> my_history;
    std::vector<std::pair<std::string, std::string>> ai_shots;
    std::string pending_reject;
    int winner = -1;
    bool game_over = false;
    bool my_turn = false;               // 轮到我、等 shot.txt 写入
    bool disconnected = false;

    // 本回合时限（服务器在 turn_begin 里下发）：快照据此显示"剩余约 N 秒"，
    // 主循环每 10 秒重写快照，读取者拿到的数字最多旧 10 秒
    std::chrono::steady_clock::time_point turn_start;
    double turn_limit_s = 0.0;  // 0 = 服务器没告知，不显示时限行
    bool turn_has_deadline = false;

    // 运行参数
    int ai_diff = 2;
    std::string my_name = "Claude";

    // 文件信箱路径
    std::string state_path, shot_path, log_path, done_path;

    explicit App(asio::io_context& io) : sock(io) {}
};

// net.cpp：协议帧收发
void send_msg(App& app, const json& m);
void pump_socket(App& app);
void drain_frames(App& app);

// app.cpp：对局逻辑、事件处理、信箱提交、主循环
void log_line(App& app, const std::string& line);
void handle(App& app, const json& m);
void submit_if_file(App& app);
int run_game(App& app);
std::string name_of(const App& app, int pid);
std::string ai_name(const App& app);
const char* diff_name(int d);
void push_event(App& app, const std::string& text);
double vx(const json& v);
double vy(const json& v);

// render.cpp：战场快照
void render_state(const App& app, const std::string& reject_note);

}  // namespace fbplay
