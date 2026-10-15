// 对局逻辑：日志、收帧处理、信箱提交（shot.txt）、主循环
#include "app.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fbplay {

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
        // 服务器下发的输入时限：快照据此显示倒计时（超时=直接跳过这发，不可不知）
        app.turn_has_deadline = m.contains("input_seconds");
        app.turn_limit_s = m.value("input_seconds", 0.0);
        app.turn_start = std::chrono::steady_clock::now();
        const int pid = m.value("player_id", -1);
        const std::string who = (pid == app.my_id) ? "我" : name_of(app, pid);
        log_line(app, "第 " + std::to_string(app.round_no) + " 回合轮到 " + who + "（兵#" +
                          std::to_string(m.value("soldier", 0) + 1) + "）");
        if (pid == app.my_id) {
            app.my_turn = true;
            // 上次的拒绝信息在本回合首帧露出一次再清：被拒可能发生在回合翻页之后
            //（shot 回声先置假、error 后到），不露这一次快照读者就永远看不到原因
            const std::string note = app.pending_reject;
            app.pending_reject.clear();
            render_state(app, note);
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
        if (pid == app.my_id) {
            app.my_turn = false;
            // 超时那回合迟写进来的 shot.txt 已经作废：留着的话下个我的回合
            // 会把它当新出手提交（旧局面的表达式白丢一发、还污染出手历史）
            std::error_code ec;
            std::filesystem::remove(app.shot_path, ec);
        }
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

// ---------- 文件信箱取表达式 ----------

// 常见的整式写法（"y=..."、"f(x)=..."）会撞服务器的表达式字符白名单（'=' 不允许），
// 白丢一个回合——提交前剥掉左边只看得出是"赋值目标"的前缀，只发式子本体
std::optional<std::string> strip_lhs_prefix(const std::string& s) {
    const size_t eq = s.find('=');
    if (eq == std::string::npos) return std::nullopt;
    // 多个等号（y==x、y=x=x）：右值里还有 '='，剥了照样过不了服务器白名单，
    // 只会白记一条"已剥前缀"的日志——不剥，原样交给服务器裁决
    if (s.find('=', eq + 1) != std::string::npos) return std::nullopt;
    std::string lhs;
    for (const char c : s.substr(0, eq)) {
        if (c == ' ' || c == '\t') continue;
        lhs += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    static const char* kAssignTargets[] = {"y", "fx", "f(x)", "y(x)"};
    for (const char* target : kAssignTargets) {
        if (lhs == target) return s.substr(eq + 1);
    }
    return std::nullopt;  // 不认识的"="：原样发给服务器裁决
}

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
    if (const auto body = strip_lhs_prefix(expr)) {
        log_line(app, "[我] 已自动剥掉等号前缀：" + expr);
        expr = *body;
        if (expr.empty()) return;
    }

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
    auto last_state_write = std::chrono::steady_clock::now();
    while (!app.game_over && !app.disconnected) {
        pump_socket(app);
        drain_frames(app);
        if (app.my_turn) {
            submit_if_file(app);
            // 快照里的"剩余时间"每 10 秒刷一次：读取者只能靠它掌握何时必须提交
            //（提交被拒的重新提示走 handle 的即时渲染，不走这里）
            const auto now = std::chrono::steady_clock::now();
            if (now - last_state_write > std::chrono::seconds(10)) {
                render_state(app, app.pending_reject);
                last_state_write = now;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
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

}  // namespace fbplay
