// 战场快照（session/state.txt）：ASCII 地图 + 精确坐标 + 事件流 + 回合时限
#include "app.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace fbplay {

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
    // 回合时限：服务器的超时是硬性的（超时直接跳过这发），快照必须说清楚还剩多少。
    // 主循环每 10 秒重写本文件，"剩余秒数"最多旧 10 秒
    if (app.turn_has_deadline && app.turn_limit_s > 0.0) {
        const double elapsed = std::chrono::duration<double>(
                                   std::chrono::steady_clock::now() - app.turn_start)
                                   .count();
        const double left = app.turn_limit_s - elapsed;
        char buf[160];
        if (left > 0.0) {
            std::snprintf(buf, sizeof(buf),
                          "本回合输入时限 %.0f 秒，剩余约 %.0f 秒（快照每 10 秒刷新）",
                          app.turn_limit_s, left);
        } else {
            std::snprintf(buf, sizeof(buf),
                          "！！本回合已过时限（%.0f 秒）——这发会被服务器跳过", app.turn_limit_s);
        }
        L.push_back(buf);
    }
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
    // 先夹住计数再构造迭代器：事件不足 12 条时 rbegin() + 12 越过 rend()，
    // 构造本身按标准就是未定义行为（虽然连续存储下实践没出过事）
    const size_t ev_n = app.events.size() < 12 ? app.events.size() : 12;
    const auto ev_stop = app.events.rbegin() + static_cast<std::ptrdiff_t>(ev_n);
    for (auto it = app.events.rbegin(); it != ev_stop; ++it) {
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
        L.push_back("");
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
    L.push_back("只写式子本体（写 \"y=...\" 也会被自动剥掉前缀，但没必要）");

    std::ofstream f(app.state_path);
    for (const auto& line : L) f << line << "\n";
    f.flush();
}

}  // namespace fbplay
