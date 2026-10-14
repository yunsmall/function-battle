// 函数对战 —— 文件信箱式对局客户端（脚本 / AI 助手用）。
// 连服务器打一局：我 = A 队 3 士兵 vs 人机 AI 3 士兵。轮到我出手时把战场快照写进
// session/state.txt；把一条 y=f(x) 表达式写进 session/shot.txt（一行）即自动提交。
// 不做图形界面、也不需要 stdin——专给不会开窗口的使用者。
// 运行文件（state/shot/log/done）放在程序所在目录的 session/ 下。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <cxxopts.hpp>
#include <nlohmann/json.hpp>

#include "exe_path.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  // SetConsoleOutputCP：让中文日志在 Windows 终端正常显示
#endif

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

// ---------- 对局状态 ----------

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

    // 运行参数
    int ai_diff = 2;
    std::string my_name = "Claude";

    // 文件信箱路径
    std::string state_path, shot_path, log_path, done_path;

    explicit App(asio::io_context& io) : sock(io) {}
};

// ---------- 日志 ----------

std::string now_stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

void log_line(App& app, const std::string& line) {
    const std::string text = "[" + now_stamp() + "] " + line;
    std::printf("%s\n", text.c_str());
    std::fflush(stdout);
    std::ofstream f(app.log_path, std::ios::app);
    f << text << "\n";
}

// ---------- 帧收发 ----------

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

void handle(App& app, const json& m);
void render_state(const App& app, const std::string& reject_note);

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

// ---------- 小工具 ----------

// Vec2 在协议里是 [x, y] 数组；兼容 {x, y} 对象形式
double vx(const json& v) {
    if (v.is_array() && !v.empty()) return v[0].get<double>();
    return v.value("x", 0.0);
}
double vy(const json& v) {
    if (v.is_array() && v.size() > 1) return v[1].get<double>();
    return v.value("y", 0.0);
}

std::string name_of(const App& app, int pid) {
    const auto it = app.players.find(pid);
    return it == app.players.end() ? "#" + std::to_string(pid) : it->second.name;
}

std::string ai_name(const App& app) {
    for (const auto& [pid, p] : app.players) {
        if (p.is_ai) return p.name;
    }
    return "AI";
}

const char* diff_name(int d) {
    static const char* names[] = {"简单", "普通", "困难", "大神"};
    return (d >= 0 && d < 4) ? names[d] : "?";
}

void push_event(App& app, const std::string& text) {
    app.events.push_back(text);
    if (app.events.size() > 18) app.events.erase(app.events.begin());
}

// ---------- 收帧处理 ----------

void handle(App& app, const json& m) {
    const std::string t = m.value("type", "");
    if (t == "hello") {
        log_line(app, "已连接服务器（hello）");
    } else if (t == "joined") {
        app.my_id = m.value("member_id", -1);
        log_line(app, "已建房：member_id=" + std::to_string(app.my_id) +
                          " room_id=" + std::to_string(m.value("room_id", 0)));
    } else if (t == "game_start") {
        app.players.clear();
        for (const auto& p : m.value("players", json::array())) {
            PlayerState ps;
            ps.name = p.value("name", "");
            ps.team = p.value("team", "");
            ps.is_ai = p.value("is_ai", false);
            for (const auto& s : p.value("soldiers", json::array())) {
                Soldier sd;
                sd.x = vx(s.value("pos", json::array()));
                sd.y = vy(s.value("pos", json::array()));
                sd.hp = s.value("hp", 0);
                sd.max_hp = s.value("max_hp", 0);
                sd.alive = s.value("alive", true);
                ps.soldiers.push_back(sd);
            }
            app.players[p.value("id", -1)] = std::move(ps);
        }
        app.obstacles.clear();
        for (const auto& ob : m.value("obstacles", json::array())) {
            Obstacle o;
            o.is_circle = ob.value("is_circle", true);
            o.cx = vx(ob.value("center", json::array()));
            o.cy = vy(ob.value("center", json::array()));
            o.radius = ob.value("radius", 0.0);
            o.half_w = ob.value("half_w", 0.0);
            o.half_h = ob.value("half_h", 0.0);
            for (const auto& c : ob.value("craters", json::array())) {
                o.craters.emplace_back(vx(c), vy(c));
            }
            app.obstacles.push_back(std::move(o));
        }
        app.cfg = m.value("config", json::object());
        const std::string seed_txt = app.cfg.contains("seed") ? app.cfg["seed"].dump() : "?";
        log_line(app, "开局：种子 " + seed_txt + "，地图 [" +
                          std::to_string(app.cfg.value("map_x_min", 0.0)) + "," +
                          std::to_string(app.cfg.value("map_x_max", 0.0)) + "]x[" +
                          std::to_string(app.cfg.value("map_y_min", 0.0)) + "," +
                          std::to_string(app.cfg.value("map_y_max", 0.0)) + "]，障碍 " +
                          std::to_string(app.obstacles.size()));
        for (const auto& [pid, p] : app.players) {
            const char* tag = (pid == app.my_id) ? "我" : "AI";
            std::string coords;
            for (const auto& s : p.soldiers) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), " (%.2f,%.2f)", s.x, s.y);
                coords += buf;
            }
            log_line(app, std::string("  [") + tag + "] " + p.name + "(" + p.team +
                              "队) 士兵:" + coords);
        }
    } else if (t == "turn_begin") {
        app.round_no = m.value("round", app.round_no);
        app.last_turn = m;
        const int pid = m.value("player_id", -1);
        const std::string who = (pid == app.my_id) ? "我" : name_of(app, pid);
        log_line(app, "第 " + std::to_string(app.round_no) + " 回合轮到 " + who + "（兵#" +
                          std::to_string(m.value("soldier", 0) + 1) + "）");
        if (pid == app.my_id) {
            app.my_turn = true;
            app.pending_reject.clear();
            render_state(app, "");
        } else {
            app.my_turn = false;  // 兜底：回合已翻页
        }
    } else if (t == "shot") {
        const json req = m.value("request", json::object());
        const std::string expr = req.value("expr_a", "");
        const int owner = m.value("owner", -1);
        if (owner != app.my_id) {
            app.ai_shots.emplace_back(name_of(app, owner), expr);
            if (app.ai_shots.size() > 3) app.ai_shots.erase(app.ai_shots.begin());
            push_event(app, name_of(app, owner) + " 出手：" + expr);
        } else {
            app.my_turn = false;  // 自己的回声：本回合已提交完成
        }
        log_line(app, "出手：" + name_of(app, owner) + " 兵#" +
                          std::to_string(m.value("soldier", 0) + 1) + "  " + expr);
    } else if (t == "hit") {
        const int tgt_id = m.value("target", -1);
        const int si = m.value("soldier", 0);
        const auto it = app.players.find(tgt_id);
        if (it != app.players.end() && si >= 0 && si < static_cast<int>(it->second.soldiers.size())) {
            it->second.soldiers[si].hp = m.value("hp_left", 0);
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "@(%.1f,%.1f)", vx(m.value("pos", json::array())),
                      vy(m.value("pos", json::array())));
        push_event(app, name_of(app, m.value("by", -1)) + " 命中 " + name_of(app, tgt_id) +
                           "兵#" + std::to_string(si + 1) + "（剩 " +
                           std::to_string(m.value("hp_left", 0)) + " 血）" + buf);
        log_line(app, "命中：" + name_of(app, m.value("by", -1)) + " → " + name_of(app, tgt_id) +
                          " 兵#" + std::to_string(si + 1) + "（剩 " +
                          std::to_string(m.value("hp_left", 0)) + " 血）");
    } else if (t == "death") {
        const int pid = m.value("player_id", -1);
        const int si = m.value("soldier", 0);
        const auto it = app.players.find(pid);
        if (it != app.players.end() && si >= 0 && si < static_cast<int>(it->second.soldiers.size())) {
            it->second.soldiers[si].alive = false;
            it->second.soldiers[si].hp = 0;
        }
        push_event(app, name_of(app, pid) + " 士兵#" + std::to_string(si + 1) + " 阵亡");
        log_line(app, "阵亡：" + name_of(app, pid) + " 兵#" + std::to_string(si + 1));
    } else if (t == "crater") {
        const int idx = m.value("obstacle", -1);
        if (idx >= 0 && idx < static_cast<int>(app.obstacles.size())) {
            app.obstacles[idx].craters.emplace_back(vx(m.value("pos", json::array())),
                                                    vy(m.value("pos", json::array())));
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "障碍#%d 被打出坑 @(%.1f,%.1f)", idx,
                      vx(m.value("pos", json::array())), vy(m.value("pos", json::array())));
        push_event(app, buf);
    } else if (t == "turn_skipped") {
        const int pid = m.value("player_id", -1);
        const std::string who = (pid == app.my_id) ? "我" : name_of(app, pid);
        if (pid == app.my_id) app.my_turn = false;
        push_event(app, who + " 超时被跳过");
        log_line(app, "超时跳过：" + who);
    } else if (t == "game_over") {
        app.winner = m.value("winner", -1);
        app.game_over = true;
        app.my_turn = false;
        push_event(app, "对局结束 winner=" + std::to_string(app.winner));
        log_line(app, "对局结束：winner=" + std::to_string(app.winner) +
                          "（0=我(A)胜，1=AI 胜，2=平局）");
    } else if (t == "projectiles" || t == "function_preview" || t == "room_state") {
        // 炮弹位置广播、AI 输入预览、房间准备态刷新：对文件信箱模式没有意义，
        // 且预览是高频帧——静默忽略（py 版会把这些打进日志刷屏）
    } else if (t == "error") {
        const std::string code = m.value("code", "");
        const std::string msg = m.value("message", "");
        app.pending_reject = "上次提交被拒：[" + code + "] " + msg;
        log_line(app, "服务器错误：[" + code + "] " + msg);
        if (app.my_turn) render_state(app, app.pending_reject);  // 被拒后重新提示
    } else {
        log_line(app, "未处理消息：" + t);
    }
}

// ---------- 战场渲染（state.txt 的 ASCII 地图） ----------

std::vector<std::string> render_map(const App& app) {
    const double x0 = app.cfg.value("map_x_min", -24.0);
    const double x1 = app.cfg.value("map_x_max", 24.0);
    const double y0 = app.cfg.value("map_y_min", -15.0);
    const double y1 = app.cfg.value("map_y_max", 15.0);
    const int cols = static_cast<int>((x1 - x0) * 2.0);  // 每单位 2 列
    const int rows = static_cast<int>(y1 - y0);          // 每单位 1 行
    if (cols <= 0 || rows <= 0) return {};

    std::vector<std::string> grid(rows, std::string(cols, '.'));
    const double crater_r = app.cfg.value("crater_radius", 1.2);

    for (const auto& ob : app.obstacles) {
        for (int r = 0; r < rows; ++r) {
            const double y = y1 - (r + 0.5);
            for (int c = 0; c < cols; ++c) {
                if (grid[r][c] != '.') continue;
                const double x = x0 + (c + 0.5) / 2.0;
                const bool inside =
                    ob.is_circle ? ((x - ob.cx) * (x - ob.cx) + (y - ob.cy) * (y - ob.cy) <=
                                    ob.radius * ob.radius)
                                 : (std::abs(x - ob.cx) <= ob.half_w &&
                                    std::abs(y - ob.cy) <= ob.half_h);
                if (!inside) continue;
                bool in_crater = false;
                for (const auto& ct : ob.craters) {
                    const double dx = x - ct.first, dy = y - ct.second;
                    if (dx * dx + dy * dy <= crater_r * crater_r) {
                        in_crater = true;
                        break;
                    }
                }
                grid[r][c] = in_crater ? 'o' : '#';
            }
        }
    }

    for (const auto& [pid, p] : app.players) {
        const bool mine = (p.team == app.my_team);
        for (size_t si = 0; si < p.soldiers.size(); ++si) {
            const auto& s = p.soldiers[si];
            const int c = static_cast<int>((s.x - x0) * 2.0);
            const int r = static_cast<int>(y1 - s.y);
            if (c < 0 || c >= cols || r < 0 || r >= rows) continue;
            if (!s.alive) {
                grid[r][c] = 'x';
            } else {
                grid[r][c] = mine ? static_cast<char>('1' + si)
                                  : static_cast<char>('a' + si);
            }
        }
    }

    std::vector<std::string> lines;
    for (int r = 0; r < rows; ++r) {
        char head[8];
        std::snprintf(head, sizeof(head), "%4d ", static_cast<int>(y1 - r));
        lines.push_back(head + grid[r]);
    }
    // x 轴标尺（每 5 单位一个刻度）
    std::string ruler(static_cast<size_t>(cols), ' ');
    for (double xv = std::ceil(x0 / 5.0) * 5.0; xv <= x1; xv += 5.0) {
        const int p2 = static_cast<int>((xv - x0) * 2.0);
        const std::string label = std::to_string(static_cast<int>(xv));
        for (size_t i = 0; i < label.size(); ++i) {
            const int pos = p2 + static_cast<int>(i);
            if (pos >= 0 && pos < cols) ruler[pos] = label[i];
        }
    }
    lines.push_back("     " + ruler);
    return lines;
}

void render_state(const App& app, const std::string& reject_note) {
    std::vector<std::string> L;
    L.push_back("===== 函数对战：我（A 队 " + app.my_name + "） vs " + ai_name(app) +
                "（B 队，" + diff_name(app.ai_diff) + "） =====");
    L.push_back("第 " + std::to_string(app.round_no) + " 回合 | 轮到：我 | 当前出手士兵：#" +
                std::to_string(app.last_turn.value("soldier", 0) + 1));
    L.push_back("图例：1/2/3=我的兵  a/b/c=敌方兵  #=遮挡物  o=被打穿的坑  x=阵亡残骸");
    for (auto& line : render_map(app)) L.push_back(line);
    L.push_back("");
    L.push_back("--- 士兵坐标（精确）---");
    for (const auto& [pid, p] : app.players) {
        const std::string tag = (pid == app.my_id)
                                    ? "（我）"
                                    : std::string("（") + diff_name(app.ai_diff) + "AI）";
        L.push_back(p.name + " " + p.team + " 队" + tag + "：");
        for (size_t si = 0; si < p.soldiers.size(); ++si) {
            const auto& s = p.soldiers[si];
            const char mark = (p.team == app.my_team) ? static_cast<char>('1' + si)
                                                      : static_cast<char>('a' + si);
            const bool star = (pid == app.my_id &&
                               static_cast<int>(si) == app.last_turn.value("soldier", -1));
            char buf[128];
            std::snprintf(buf, sizeof(buf), "  [%c] (%7.2f, %7.2f)  hp%d/%d%s%s", mark, s.x,
                          s.y, s.hp, s.max_hp, s.alive ? "" : " 已阵亡",
                          star ? "  ★ 本回合出手" : "");
            L.push_back(buf);
        }
    }
    L.push_back("");
    L.push_back("--- 遮挡物 ---");
    for (size_t i = 0; i < app.obstacles.size(); ++i) {
        const auto& ob = app.obstacles[i];
        char buf[160];
        if (ob.is_circle) {
            std::snprintf(buf, sizeof(buf), "  #%zu 圆形 中心(%.2f,%.2f) 半径%.2f", i, ob.cx,
                          ob.cy, ob.radius);
        } else {
            std::snprintf(buf, sizeof(buf), "  #%zu 矩形 中心(%.2f,%.2f) 半宽%.2f 半高%.2f", i,
                          ob.cx, ob.cy, ob.half_w, ob.half_h);
        }
        std::string line = buf;
        if (!ob.craters.empty()) line += "  坑×" + std::to_string(ob.craters.size());
        L.push_back(line);
    }
    L.push_back("");
    L.push_back("--- 最近事件（新→旧）---");
    for (auto it = app.events.rbegin(); it != app.events.rend() && it != app.events.rbegin() + 12;
         ++it) {
        L.push_back("  " + *it);
    }
    if (!app.my_history.empty()) {
        L.push_back("");
        L.push_back("--- 我已出手 ---");
        for (size_t i = 0; i < app.my_history.size(); ++i) {
            L.push_back("  第" + std::to_string(i + 1) + "发: " + app.my_history[i]);
        }
    }
    if (!app.ai_shots.empty()) {
        L.push_back("--- AI 最近出手 ---");
        for (const auto& [nm, ex] : app.ai_shots) {
            L.push_back("  " + nm + ": " + ex);
        }
    }
    L.push_back("");
    if (!reject_note.empty()) L.push_back("！！ " + reject_note);
    L.push_back("================= 等待你的表达式 =================");
    L.push_back("把一条 y=f(x) 表达式写入 " + app.shot_path + "（一行，写入后自动提交）");
    L.push_back("坐标语义：以你当前出手士兵为原点，x 向右为正；常数对弹道无影响（f(0) 归零）");

    std::ofstream f(app.state_path);
    for (const auto& line : L) f << line << "\n";
    f.flush();
}

// ---------- 文件信箱取表达式 ----------

// shot.txt 出现即取第一条非空且非 # 注释的行提交；无论内容是否有效都先删文件
void submit_if_file(App& app) {
    std::error_code ec;
    if (!std::filesystem::exists(app.shot_path, ec)) return;
    std::string expr;
    {
        std::ifstream in(app.shot_path);
        std::string raw;
        while (std::getline(in, raw)) {
            const auto b = raw.find_first_not_of(" \t\r");
            if (b == std::string::npos) continue;
            const auto e2 = raw.find_last_not_of(" \t\r");
            const std::string ln = raw.substr(b, e2 - b + 1);
            if (!ln.empty() && ln[0] != '#') {
                expr = ln;
                break;
            }
        }
    }
    std::filesystem::remove(app.shot_path, ec);
    if (expr.empty()) return;  // 空文件：删掉重来

    json req;
    req["mode"] = "y_of_x";
    req["expr_a"] = expr;
    req["dir"] = "pos";
    json msg;
    msg["type"] = "submit";
    msg["request"] = req;
    send_msg(app, msg);
    app.my_history.push_back(expr);
    app.pending_reject.clear();
    log_line(app, "[我] 提交：" + expr);
    // 不在这里结束回合：等自己的 shot 回声（或回合翻页）确认
}

// ---------- 主循环 ----------

int run_game(App& app) {
    while (!app.game_over && !app.disconnected) {
        pump_socket(app);
        drain_frames(app);
        if (app.my_turn) submit_if_file(app);
        std::this_thread::sleep_for(20ms);
    }

    std::error_code ec;
    std::filesystem::remove(app.shot_path, ec);
    if (app.disconnected && !app.game_over) {
        log_line(app, "连接断开，对局中止");
        return 1;
    }
    const std::string verdict =
        app.winner == 0 ? "我（A 队）胜"
                        : (app.winner == 1 ? ai_name(app) + "（B 队）胜"
                                           : (app.winner == 2 ? "平局" : "未知"));
    {
        std::ofstream f(app.done_path);
        f << "对局结束：" << verdict << "\n我的出手：\n";
        for (size_t i = 0; i < app.my_history.size(); ++i) {
            f << "  第" << i + 1 << "发: " << app.my_history[i] << "\n";
        }
    }
    log_line(app, "对局结束：" + verdict + "；我一共出手 " +
                      std::to_string(app.my_history.size()) + " 次");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    cxxopts::Options options("fg_play",
                             "文件信箱式对局客户端：连服务器打一局（我 = A 队 vs 人机 AI）");
    options.add_options()
        ("seed", "固定地图种子（整数；'-'/'random' 或省略 = 每把随机）",
         cxxopts::value<std::string>()->default_value(""))
        ("difficulty", "AI 难度：0 简单 / 1 普通 / 2 困难 / 3 大神（默认 2）",
         cxxopts::value<int>()->default_value("2"))
        ("host", "服务器地址", cxxopts::value<std::string>()->default_value("127.0.0.1"))
        ("port", "游戏端口（原生协议）", cxxopts::value<uint16_t>()->default_value("9527"))
        ("name", "我的名字", cxxopts::value<std::string>()->default_value("Claude"))
        ("h,help", "显示帮助");
    options.parse_positional({"seed", "difficulty"});

    std::string seed_arg;
    try {
        const auto result = options.parse(argc, argv);
        if (result.count("help")) {
            std::printf("%s\n", options.help().c_str());
            return 0;
        }
        seed_arg = result.count("seed") ? result["seed"].as<std::string>() : "";
        // 名字/难度等直接读；难度的范围单独校验
        std::optional<uint64_t> seed;
        if (!seed_arg.empty() && seed_arg != "-" && seed_arg != "random" && seed_arg != "r") {
            try {
                seed = std::stoull(seed_arg);
            } catch (const std::exception&) {
                std::printf("种子必须是整数或 '-'（随机），收到：%s\n", seed_arg.c_str());
                return 1;
            }
        }

        asio::io_context io;
        App app(io);
        app.ai_diff = result["difficulty"].as<int>();
        if (app.ai_diff < 0 || app.ai_diff > 3) {
            std::printf("难度必须是 0-3\n");
            return 1;
        }
        app.my_name = result["name"].as<std::string>();
        const std::string host = result["host"].as<std::string>();
        const uint16_t port = result["port"].as<uint16_t>();

        // 文件信箱目录：程序所在目录旁的 session/（打包解压后即包内，从哪运行都固定）
        std::string base = fg::exe_dir();
        if (base.empty()) base = ".";
        const std::string dir = base + "/session";
        app.state_path = dir + "/state.txt";
        app.shot_path = dir + "/shot.txt";
        app.log_path = dir + "/log.txt";
        app.done_path = dir + "/done.txt";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::remove(app.shot_path, ec);
        std::filesystem::remove(app.done_path, ec);
        {
            std::ofstream f(app.log_path, std::ios::app);
            f << "---- 新对局 ----\n";
        }

        // 连接（先阻塞 connect，随后切非阻塞轮询）
        app.sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address(host), port), ec);
        if (ec) {
            log_line(app, "连不上服务器 " + host + ":" + std::to_string(port) + "：" +
                              ec.message());
            return 1;
        }
        app.sock.non_blocking(true, ec);

        // 等 hello（3 秒）
        {
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (app.recvbuf.empty() && !app.disconnected &&
                   std::chrono::steady_clock::now() < deadline) {
                pump_socket(app);
                std::this_thread::sleep_for(20ms);
            }
            drain_frames(app);
        }

        // 建房（配置与 py 版一致）；随后加 AI、开局
        json cfg_obj;
        cfg_obj["soldiers_per_player"] = 3;
        cfg_obj["obstacles"] = 15;
        cfg_obj["hp"] = 1;
        cfg_obj["first_team"] = 0;
        cfg_obj["input_seconds"] = 600.0;
        cfg_obj["turn_delay"] = 1.0;
        if (seed) cfg_obj["seed"] = *seed;
        {
            json msg;
            msg["type"] = "create_room";
            msg["name"] = app.my_name;
            msg["config"] = cfg_obj;
            send_msg(app, msg);
        }
        {
            // 等 joined（10 秒）
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (app.my_id < 0 && !app.disconnected &&
                   std::chrono::steady_clock::now() < deadline) {
                pump_socket(app);
                drain_frames(app);
                std::this_thread::sleep_for(20ms);
            }
        }
        if (app.my_id < 0) {
            log_line(app, "建房失败（未收到 joined）");
            return 1;
        }
        {
            json msg;
            msg["type"] = "add_ai";
            msg["difficulty"] = app.ai_diff;
            msg["team"] = "B";
            send_msg(app, msg);
        }
        std::this_thread::sleep_for(300ms);
        send_msg(app, json{{"type", "start"}});
        log_line(app, "已发 add_ai(难度 " + std::to_string(app.ai_diff) + ") 与 start");

        return run_game(app);
    } catch (const cxxopts::exceptions::exception& e) {
        std::printf("参数错误：%s\n\n%s\n", e.what(), options.help().c_str());
        return 1;
    }
}
