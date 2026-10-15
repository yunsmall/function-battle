#include "ai.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <vector>

#include "ai_curves.h"

namespace fg {
namespace {

// 调试开关：FG_AI_TRACE 环境变量开启后，往 stderr 打决策路径与候选统计
// （调 AI 时的分析手段，正常运行时零开销）
bool ai_trace_on() {
    static const bool on = [] {
#if defined(_MSC_VER)
        // MSVC 把 getenv 标记为不安全（警告噪声），用 _dupenv_s 等价实现
        char* buf = nullptr;
        size_t sz = 0;
        const bool has = (_dupenv_s(&buf, &sz, "FG_AI_TRACE") == 0 && buf != nullptr);
        std::free(buf);
        return has;
#else
        return std::getenv("FG_AI_TRACE") != nullptr;
#endif
    }();
    return on;
}
bool ai_trace_take() {
    static std::atomic<int> budget{5000};
    return budget.fetch_sub(1) > 0;
}

// ---------- 插值与路径评估 ----------

// 牛顿差商插值：给定互异的 xs 与对应 ys，返回标准升幂系数。
// 约定 xs[0] = 0、ys[0] = 0，于是 f(0)=0——按锚点规则曲线天然从玩家位置出发。
std::vector<double> interp_coeffs(const std::vector<double>& xs, const std::vector<double>& ys) {
    const size_t n = xs.size();
    std::vector<std::vector<double>> table(n, std::vector<double>(n, 0.0));
    for (size_t i = 0; i < n; ++i) table[i][0] = ys[i];
    for (size_t j = 1; j < n; ++j) {
        for (size_t i = 0; i + j < n; ++i) {
            table[i][j] = (table[i + 1][j - 1] - table[i][j - 1]) / (xs[i + j] - xs[i]);
        }
    }

    std::vector<double> result = {0.0};  // 升幂系数
    std::vector<double> basis = {1.0};   // 牛顿基 prod(x - xs[k])
    for (size_t j = 0; j < n; ++j) {
        result.resize(std::max(result.size(), basis.size()), 0.0);
        for (size_t i = 0; i < basis.size(); ++i) result[i] += table[0][j] * basis[i];
        if (j + 1 < n) {
            std::vector<double> next(basis.size() + 1, 0.0);
            for (size_t i = 0; i < basis.size(); ++i) {
                next[i + 1] += basis[i];
                next[i] += -xs[j] * basis[i];
            }
            basis = std::move(next);
        }
    }
    return result;
}

// 阶梯表达式：分段水平、在相邻两点的中点处跳变，精确经过每个插值点。
// 与多项式插值互补——点多时多项式会剧烈震荡甚至甩出地图，阶梯永远不发散；
// 水平段还让"目标位置有点误差"也落在扫射范围内。要求 xs 升序且含 x=0 的点
std::string stair_text(const std::vector<double>& xs, const std::vector<double>& ys,
                       const char* var) {
    std::string s = num_text(ys[0]);
    for (size_t i = 1; i < xs.size(); ++i) {
        const double jump = ys[i] - ys[i - 1];
        if (std::abs(jump) < 1e-9) continue;
        const double mid = (xs[i] + xs[i - 1]) * 0.5;
        s += (jump < 0.0 ? " - " : " + ") + num_text(std::abs(jump)) + "*step(" + var + " - " +
             num_text(mid) + ")";
    }
    return s;
}

// 表达式里的数字字面量逐个乘一个小扰动因子（0.9~1.1）：保留整体结构、只微调参数。
// 扫描"连续的数字与小数点"替换即可。**输入必须来自 num_text（%.4f 格式，不产生
// 科学计数法）**：形如 1e5 的写法会被拆成两段扰动，将来若要用于人类自由输入需先扩展词法。
// ^ 后面的数字是指数、保持原样：x^2 被扰动成 x^1.9 后在负底数处算出 NaN，
// 曲线会半途断开、白费一个候选
std::string jitter_numbers(const std::string& s, Rng& rng) {
    std::string out;
    size_t i = 0;
    char prev = '\0';  // 上一个非空白字符（用于判断数字是否紧跟 ^）
    while (i < s.size()) {
        const char c = s[i];
        const bool digit = std::isdigit(static_cast<unsigned char>(c)) != 0;
        const bool dot_number =
            (c == '.' && i + 1 < s.size() &&
             std::isdigit(static_cast<unsigned char>(s[i + 1])) != 0);
        if (digit || dot_number) {
            size_t j = i;
            while (j < s.size() &&
                   (std::isdigit(static_cast<unsigned char>(s[j])) != 0 || s[j] == '.')) {
                ++j;
            }
            if (prev == '^') {
                out.append(s, i, j - i);  // 指数原样保留
            } else {
                const double v = std::atof(s.substr(i, j - i).c_str());
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.4f", v * rng.range(0.9, 1.1));
                out += buf;
            }
            i = j;
        } else {
            out += c;
            if (c != ' ') prev = c;
            ++i;
        }
    }
    return out;
}

// 表达式里的数字参数序列（与 jitter_numbers 同一套词法：连续的数字/小数点算一个数）。
// 供 is_repeat_of 比较两条式子的"参数向量"——结构（数字个数）不同直接判不同
std::vector<double> number_params(const std::string& s) {
    std::vector<double> out;
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        const bool digit = std::isdigit(static_cast<unsigned char>(c)) != 0;
        const bool dot_number =
            (c == '.' && i + 1 < s.size() &&
             std::isdigit(static_cast<unsigned char>(s[i + 1])) != 0);
        if (digit || dot_number) {
            size_t j = i;
            while (j < s.size() &&
                   (std::isdigit(static_cast<unsigned char>(s[j])) != 0 || s[j] == '.')) {
                ++j;
            }
            out.push_back(std::atof(s.substr(i, j - i).c_str()));
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

// 表达式的"结构串"：数字字面量整段替换成 '#'、其余字符原样保留（与 number_params
// 同一套词法）。两条式子要谈"数字级近似"，结构必须一致——只比数字的话，
// sin(2*x) 与 2*sin(x)、+0.5*x 与 -0.5*x 这类数字相同、弹道完全不同的式子
// 会被误判成同一发，把本该保留的候选错误剔掉
std::string expr_shape(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        const bool digit = std::isdigit(static_cast<unsigned char>(c)) != 0;
        const bool dot_number =
            (c == '.' && i + 1 < s.size() &&
             std::isdigit(static_cast<unsigned char>(s[i + 1])) != 0);
        if (digit || dot_number) {
            while (i < s.size() &&
                   (std::isdigit(static_cast<unsigned char>(s[i])) != 0 || s[i] == '.')) {
                ++i;
            }
            out += '#';
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

// 两段的数字参数是否逐个对得上：结构串一致（数字之外逐字相同），且个数相同、
// 每个参数落在 12% 相对误差（加 0.02 绝对容差兜零附近的参数）内。12% 罩得住
// jitter_numbers 的 ±10% 扰动——AI 微调上一发的变体也算"重复"
bool expr_repeats(const std::string& a, const std::string& b) {
    if (a.empty() != b.empty()) return false;
    if (a.empty()) return true;  // 都空：这一半没有约束，视为相同
    if (expr_shape(a) != expr_shape(b)) return false;
    const auto va = number_params(a);
    const auto vb = number_params(b);
    if (va.size() != vb.size()) return false;
    for (size_t i = 0; i < va.size(); ++i) {
        if (std::abs(va[i] - vb[i]) >
            0.12 * std::max(std::abs(va[i]), std::abs(vb[i])) + 0.02) {
            return false;
        }
    }
    return true;
}

// （is_repeat_of 的定义在文件尾部：测试要直接验证判定边界，放匿名 namespace 之外）

// 出手多样性：同档候选里与上一发"数字级近似"的优先剔掉（同命中档的多条线本来就
// 等效，换一条只有观感收益）；剔完为空说明那条线是唯一解，原样保留——不为了躲
// 复读牺牲强度。局面停滞时（上发命中、对方只是掉血）AI 每回合会重算出一模一样的
// 线，连着出手看起来像卡带——这道过滤就是给那种局面准备的
template <typename T>
void drop_repeats_of_prev(std::vector<const T*>& cands, const CurveRequest& prev) {
    std::vector<const T*> fresh;
    for (const T* c : cands) {
        if (!is_repeat_of(c->req, prev)) fresh.push_back(c);
    }
    if (!fresh.empty()) cands = std::move(fresh);
}

// 统计路径扫过的士兵数：穿透机制下一条曲线会依次命中多个士兵，一发"扫射"
// 的收益成倍。命中判定与 World 同构（士兵是轴对齐方块，用连续段求交判定，不是
// 圆形——圆是方块的内切圆，会系统性漏算"擦过方块角点"的命中，让择优错过更好的
// 候选）；跨步抽样仍保留（统计不需子步级精度，段判定已消除采样相位漏检）；
// 发射者自己不算（炮口保护不会伤到他）
void scan_soldier_hits(const Path& path, const World& world, const Player& self,
                       double hit_radius, int& enemy_hits, int& ally_hits) {
    enemy_hits = 0;
    ally_hits = 0;
    if (path.points.size() < 2) return;
    const double avg_step = path.length() / static_cast<double>(path.points.size());
    const size_t stride =
        std::max<size_t>(1, static_cast<size_t>(0.2 / std::max(avg_step, 1e-6)));
    // 撞上第一个遮挡物之后的部分不算"实际扫过"（炮弹已炸毁）——与真实飞行同口径，
    // 不然"撞墙前其实谁也扫不到"的候选会被打分虚报成高命中。跨步内的墙也要算
    // （段求交），否则点判定会把"跨步中途撞的墙"漏到下一步
    const double crater = world.config().world.crater_radius;
    const double pr = world.config().world.projectile_radius;
    size_t usable = path.points.size();
    for (size_t i = 0; i + 1 < path.points.size(); i += stride) {
        const size_t j = std::min(i + stride, path.points.size() - 1);
        const bool wall = std::ranges::any_of(world.obstacles(), [&](const Obstacle& ob) {
            return segment_hits_obstacle(ob, path.points[i], path.points[j], crater, pr)
                .has_value();
        });
        if (wall) {
            usable = i;
            break;
        }
    }
    // 路径包围盒：士兵的命中框与它无交集时必不命中，直接跳过整条路径的抽样
    //（多数候选路径并不经过多数士兵，这一步把 O(路径点) 的扫描提前降为 O(1)）
    double min_x = path.points[0].x(), max_x = min_x;
    double min_y = path.points[0].y(), max_y = min_y;
    for (const auto& p : path.points) {
        min_x = std::min(min_x, p.x());
        max_x = std::max(max_x, p.x());
        min_y = std::min(min_y, p.y());
        max_y = std::max(max_y, p.y());
    }
    for (const auto& pl : world.players()) {
        for (size_t si = 0; si < pl.soldiers.size(); ++si) {
            const Soldier& s = pl.soldiers[si];
            if (!s.alive) continue;
            if (pl.id == self.id && static_cast<int>(si) == self.current_soldier) continue;
            if (s.pos.x() + hit_radius < min_x || s.pos.x() - hit_radius > max_x ||
                s.pos.y() + hit_radius < min_y || s.pos.y() - hit_radius > max_y) {
                continue;
            }
            bool hit = false;
            for (size_t i = 0; i + 1 < usable; i += stride) {
                const size_t j = std::min(i + stride, usable - 1);
                if (segment_hits_square(path.points[i], path.points[j], s.pos, hit_radius, pr)) {
                    hit = true;
                    break;
                }
            }
            if (!hit) continue;
            if (pl.team == self.team) {
                ++ally_hits;
            } else {
                ++enemy_hits;
            }
        }
    }
}

// 单次遍历产出两个量：撞墙前能实际命中的目标数 + 第一个挡路障碍的索引（-1 = 全程
// 未碰）。这两个量原先由本函数与 first_blocking_obstacle 各扫一遍完全相同的细分
// 路径（同 kAiFlightStep、同 prev 推进）——合并后段判定调用减近半（perf 里段判定
// 是 AI 决策的绝对大头）。判定顺序、阈值、prev 推进与两个原实现逐点一致：撞墙
// （t_wall <= 1.0）时两边的结果都已定型，直接返回；出界只停目标统计——
// first_blocking_obstacle 从不看出界，blocker 要继续找到
struct PathScan {
    int reach = 0;
    int blocker = -1;
};

PathScan scan_path(const Path& path, const std::vector<Vec2>& targets, const World& world) {
    PathScan out;
    std::vector<bool> hit(targets.size(), false);
    const GameConfig& cfg = world.config();
    const double crater = cfg.world.crater_radius;
    const double pr = cfg.world.projectile_radius;
    const double hit_r = cfg.world.player_half_size + pr;  // 命中半径含炮弹碰撞体
    // 远障碍先剔掉（见 obstacles_near_path）：省掉逐子段×远障碍的 AABB 拒绝开销
    const std::vector<size_t> near_obs = obstacles_near_path(path, world);
    bool reach_active = true;  // 出界宽限内为 true；超时销毁后不再统计命中
    // 连续出界的弧长累计（与 step_flight 的 out_mark / evaluate_candidate 的 out_arc
    // 同构）：炮弹短暂出界又绕回来时，回程航段照常结算命中；连续超时才销毁
    double out_arc = 0.0;
    Vec2 prev = path.points.front();
    for (size_t i = 0; i + 1 < path.points.size(); ++i) {
        const Vec2& a = path.points[i];
        const Vec2& b = path.points[i + 1];
        const double seg_len = (b - a).norm();
        const size_t n =
            std::max<size_t>(1, static_cast<size_t>(std::ceil(seg_len / kAiFlightStep)));
        const double sub_len = seg_len / static_cast<double>(n);
        for (size_t s = 1; s <= n; ++s) {
            const double t = static_cast<double>(s) / static_cast<double>(n);
            const Vec2 p(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t);
            // 与飞行同构：子段先对障碍连续求交，目标只在几何上先于撞墙时算命中；
            // 顺带按障碍表顺序记录第一个命中的——与 first_blocking_obstacle 的
            // "子段内顺序扫描、命中即返回"同序，blocker 即它原样会返回的索引
            double t_wall = 2.0;
            for (const size_t k : near_obs) {
                if (const auto tw =
                        segment_hits_obstacle(world.obstacles()[k], prev, p, crater, pr)) {
                    if (out.blocker < 0) out.blocker = static_cast<int>(k);
                    t_wall = std::min(t_wall, *tw);
                }
            }
            if (reach_active) {
                for (size_t k = 0; k < targets.size(); ++k) {
                    if (hit[k]) continue;
                    // 命中判定用"子段到目标的距离"：只取点会漏掉从目标身上掠过的命中；
                    // 阈值与评估/飞行统一用命中半径
                    if (point_segment_distance(targets[k], prev, p) <= hit_r &&
                        point_segment_t(targets[k], prev, p) <= t_wall) {
                        hit[k] = true;
                        ++out.reach;
                    }
                }
                if (t_wall <= 1.0) return out;  // 撞墙：两边的结果都已定型
                // 出界销毁按真实飞行的宽限来：连续出界的弧长累计超过 speed×宽限秒
                // 才算销毁（回界内重新计时），期间"出界兜回来"的航段照常结算——
                // 原先这里见出界就永久停算，把"绕出去再打回来"的命中整段漏掉
                const bool in_map = p.x() >= cfg.world.map_x_min &&
                                    p.x() <= cfg.world.map_x_max &&
                                    p.y() >= cfg.world.map_y_min && p.y() <= cfg.world.map_y_max;
                if (!in_map) {
                    out_arc += sub_len;
                    if (out_arc > cfg.world.projectile_speed * kOutGraceSec) {
                        reach_active = false;  // 出界超时销毁（不是撞墙）
                    }
                } else {
                    out_arc = 0.0;
                }
            }
            if (!reach_active && out.blocker >= 0) return out;  // 两个量都定了
            prev = p;
        }
    }
    return out;
}

// ---------- 多目标规划（困难 AI） ----------

// 多杀骨架类型：节点式（多项式/阶梯/折线链）按插值点构造；
// 两目标的参数化风格（折点/波浪/抛物波浪/螺线）由 try_pair_styles 构造。
// pick 按类型配平随机（而不是对候选平权）——同一种骨架会在多个目标子集里各出现
// 一次，候选平权时靠数量碾压其它形状，出手花样就没了
enum class PlanKind { None, Poly, Stair, Kink, Bend, Wave, PolyWave, Spiral, Wave3 };

struct Plan {
    bool valid = false;
    CurveRequest req;
    Path path;   // 评估时采样的路径；绕行修正直接复用它，不再对同一条 req 重采样
    int blocker = -1;  // 第一个挡路障碍的索引（-1 = 全程未碰）；clear 由它导出
    int reach = 0;  // "撞墙前"能命中的目标数
    int ally = 0;   // 路径扫过的队友数（穿透机制下有友伤，择优时尽量避开）
    bool clear = true;  // 全程未碰障碍实体（false = 要靠炸障碍开路，最后手段）
    double len = 1e18;
    PlanKind kind = PlanKind::None;
};

// 计划优劣：命中数 > 友伤 > 长度。"命中数优先于友伤"是刻意口径——宁可擦到队友
// （甚至曲线绕回扫到自家兵）也换多打一个敌人；实战里大神档确实会选中"扫过自己兵、
// 换掉对方两个兵"的极坐标曲线，这是设计而非缺陷
bool better_plan(const Plan& a, const Plan& b) {
    if (!b.valid) return true;
    if (!a.valid) return false;
    if (a.reach != b.reach) return a.reach > b.reach;
    if (a.ally != b.ally) return a.ally < b.ally;
    return a.len < b.len;
}

// 节点式骨架（多项式/阶梯/折线链）按插值点构造
enum class PlanStyle { Poly, Stair, Kink };

// 汇总目标点：投影到自变量/因变量轴，滤掉反向与过近的点，按自变量升序（含原点）
std::vector<std::pair<double, double>> plan_points(bool horizontal, FireDir dir,
                                                   const std::vector<Vec2>& rel_points) {
    std::vector<std::pair<double, double>> pts = {{0.0, 0.0}};
    for (const auto& p : rel_points) {
        const double t = horizontal ? p.x() : p.y();  // 自变量
        const double v = horizontal ? p.y() : p.x();  // 因变量
        // 单向炮弹只能打到所选一侧
        if ((dir == FireDir::Positive && t <= 0.0) || (dir == FireDir::Negative && t >= 0.0)) {
            continue;
        }
        // 自变量过近会让插值病态（曲线剧烈震荡）——但阈值过严会错杀相邻格子上的
        // 多杀组合（士兵命中框 1-2 单位量级），1.0 足够挡住真正的病态插值
        bool too_close = false;
        for (const auto& prev : pts) {
            if (std::abs(t - prev.first) < 1.0) too_close = true;
        }
        if (too_close) continue;
        pts.push_back({t, v});
    }
    if (pts.size() >= 2) std::sort(pts.begin(), pts.end());
    return pts;
}

// 给定已构造的曲线做采样评估（reach 与飞行同构、统计队友扫中数）
Plan evaluate_req(CurveRequest req, const World& world, const Vec2& origin,
                  const std::vector<Vec2>& abs_targets, const Player& self) {
    Plan plan;
    CurveResult sampled = sample_curve(req, origin, world.config());
    if (!sampled.ok()) return plan;
    plan.valid = true;
    plan.req = std::move(req);
    const PathScan scan = scan_path(sampled.path, abs_targets, world);
    plan.reach = scan.reach;
    plan.blocker = scan.blocker;
    plan.clear = (plan.blocker < 0);
    int enemy_hits = 0;
    scan_soldier_hits(sampled.path, world, self, world.config().world.player_half_size, enemy_hits,
                      plan.ally);
    plan.len = sampled.path.length();
    plan.path = std::move(sampled.path);  // 前面几项都看过路径了，最后移交给缓存
    return plan;
}

// 评估一组插值点上的节点式骨架（多项式/阶梯/折线链）
Plan evaluate_plan(const std::vector<std::pair<double, double>>& pts, bool horizontal,
                   PlanStyle style, const World& world, const Vec2& origin,
                   const std::vector<Vec2>& abs_targets, const Player& self) {
    Plan plan;
    if (pts.size() < 2) return plan;
    const double any_t = (pts.front().first != 0.0) ? pts.front().first : pts.back().first;
    const char* var = horizontal ? "x" : "y";
    std::vector<double> xs, ys;
    xs.reserve(pts.size());
    ys.reserve(pts.size());
    for (const auto& [t, v] : pts) {
        xs.push_back(t);
        ys.push_back(v);
    }

    CurveRequest req;
    req.mode = horizontal ? CurveMode::YofX : CurveMode::XofY;
    req.dir = (any_t >= 0.0) ? FireDir::Positive : FireDir::Negative;
    switch (style) {
        case PlanStyle::Poly:
            req.expr_a = poly_text(interp_coeffs(xs, ys), var);
            break;
        case PlanStyle::Stair:  // 目标多时插值多项式会剧烈震荡，阶梯永远不发散
            req.expr_a = stair_text(xs, ys, var);
            break;
        case PlanStyle::Kink: {
            // 折线链走 ai_curves 的生成器（同时供单测直接验证过点）。
            // 过滤原点而不能按"跳过 pts[0]"：plan_points 按自变量升序，负方向时
            // 原点排在末尾而不是开头——重复传给 make_kink_chain 会让节点表出现
            // 重复坐标、斜率算出 NaN 表达式，整个候选被静默作废（原点由
            // make_kink_chain 自己补，rel 不该含它）
            std::vector<Vec2> rel;
            rel.reserve(pts.size() - 1);
            for (size_t i = 0; i < pts.size(); ++i) {
                if (pts[i].first == 0.0 && pts[i].second == 0.0) continue;
                rel.push_back(horizontal ? Vec2(pts[i].first, pts[i].second)
                                         : Vec2(pts[i].second, pts[i].first));
            }
            req = make_kink_chain(horizontal, rel);
            break;
        }
    }
    Plan out = evaluate_req(std::move(req), world, origin, abs_targets, self);
    out.kind = style == PlanStyle::Poly    ? PlanKind::Poly
               : style == PlanStyle::Stair ? PlanKind::Stair
                                           : PlanKind::Kink;
    return out;
}

// 双约束点的参数化骨架：折点扫位（abs 折线）/ 波浪扫频率 / 抛物波浪扫正负 / 螺线。
// 约束点必须先过滤掉原点再取：负方向时 plan_points 排序会把原点排在末尾，"取最后
// 两个"会拿到 (近目标, 原点)，最远的目标被丢掉、wave/polywave 的行列式直接退化。
// 过滤后按"离原点的自变量距离"排远近（曲线先经过近点再经过远点；解本身对两点顺序
// 不敏感，但 make_kink_bend 的折点定义在两点之间、依赖先后）。至少要在两个真实目标
// 上做：单目标时一个约束点必是原点，参数化族数学退化，其形状由主线 Poly/Stair/Kink
// 承担。每个变体都精确过两点，评估后把可行的全部塞进 out 参与最终择优——注意不要在
// 这里只保留"子集最优"：折线（连点直线）的长度天然最短，会把慢曲线全压死、
// 最终池里就只剩它一种形状了
void try_pair_styles(const std::vector<std::pair<double, double>>& pts, bool horizontal,
                     const World& world, const Vec2& origin,
                     const std::vector<Vec2>& abs_targets, const Player& self,
                     std::vector<Plan>& out) {
    std::vector<std::pair<double, double>> targets;
    targets.reserve(pts.size());
    for (const auto& pt : pts) {
        if (pt.first == 0.0 && pt.second == 0.0) continue;
        targets.push_back(pt);
    }
    if (targets.size() < 2) return;  // 单目标：数学退化（见头部注释），不生成
    std::sort(targets.begin(), targets.end(), [](const auto& a, const auto& b) {
        return std::abs(a.first) < std::abs(b.first);
    });
    const double t1 = targets[0].first, v1 = targets[0].second;  // 近点（飞行先经过）
    const double t2 = targets[1].first, v2 = targets[1].second;  // 远点
    const double dx = horizontal ? t1 : v1;
    const double dy = horizontal ? v1 : t1;
    const double ox = horizontal ? t2 : v2;
    const double oy = horizontal ? v2 : t2;
    CurveRequest req;
    auto try_req = [&](PlanKind k) {
        Plan q = evaluate_req(req, world, origin, abs_targets, self);
        if (q.valid) {
            q.kind = k;
            out.push_back(q);
        }
    };
    // 每个变体都是一次 ExprTk 编译（~0.6ms）。多杀候选的直接来源，全部扫位：
    // 随机取样一旦撞上不可行的那个变体，这组多杀就整个丢了，而两两子集数量有限、
    // 扫得起（单目标子集已在上面提前返回，不参与）
    const double amp = std::max(3.0, 0.4 * std::max(std::abs(v1), std::abs(v2)));
    for (const double fp : {0.35, 0.5, 0.65}) {
        if (make_kink_bend(horizontal, dx, dy, ox, oy, t1 + (t2 - t1) * fp, req)) {
            try_req(PlanKind::Bend);
        }
    }
    for (const double waves : {0.5, 1.0}) {
        const double w = 2.0 * std::numbers::pi * waves / std::abs(t2 - t1);
        if (make_wave_through(horizontal, dx, dy, ox, oy, w, req)) try_req(PlanKind::Wave);
    }
    for (const double c : {amp, -amp}) {
        if (make_poly_wave_through(horizontal, dx, dy, ox, oy, c, req)) {
            try_req(PlanKind::PolyWave);
        }
    }
    if (make_spiral_through(dx, dy, ox, oy, req)) try_req(PlanKind::Spiral);
}

// 三目标专属骨架：三点版抛物+正弦合并与三点双频波浪（三点全中，形状与折线/阶梯
// 完全不同）。三个频率都扫、两种骨架都试：三点同侧的子集很稀有（每侧最多出现
// 一次），这点成本可以忽略，而"一炮三杀"的杀手锏主要靠它——之前跟着别的骨架
// 一起做变体取样，直接让三杀从"偶发"变成了"绝迹"（回归测试当场抓住）
void try_triple_styles(const std::vector<std::pair<double, double>>& pts, bool horizontal,
                       const World& world, const Vec2& origin,
                       const std::vector<Vec2>& abs_targets, const Player& self,
                       std::vector<Plan>& out) {
    // 过滤原点后再取三个目标：负方向时 plan_points 排序把原点排在末尾，直接按
    // 索引取会拿原点当第三个目标、丢掉最远的一个。span 用自变量 t 的跨度（pts 的
    // first 就是 t），不能借 rel3 的分量算——rel3 是世界系 (x, y)，纵向时它的 x
    // 分量是因变量 v，会把"同竖列的纵向三杀"（三点骨架的主场）当跨度≈0 整批丢掉
    std::vector<Vec2> rel3;
    rel3.reserve(pts.size());
    double t_lo = 1e18;
    double t_hi = -1e18;
    for (const auto& pt : pts) {
        if (pt.first == 0.0 && pt.second == 0.0) continue;
        rel3.push_back(horizontal ? Vec2(pt.first, pt.second) : Vec2(pt.second, pt.first));
        t_lo = std::min(t_lo, pt.first);
        t_hi = std::max(t_hi, pt.first);
    }
    if (rel3.size() != 3) return;  // 专属"三目标"
    const double span = t_hi - t_lo;
    if (span < 1e-9) return;
    CurveRequest req;
    for (const double waves : {0.5, 0.75, 1.5}) {
        const double w = 2.0 * std::numbers::pi * waves / span;
        auto try3 = [&](bool built) {
            if (!built) return;
            Plan q = evaluate_req(req, world, origin, abs_targets, self);
            if (q.valid) {
                q.kind = PlanKind::Wave3;
                out.push_back(q);
            }
        };
        try3(make_poly_wave3_through(horizontal, rel3[0], rel3[1], rel3[2], w, req));
        try3(make_wave3_through(horizontal, rel3[0], rel3[1], rel3[2], w, req));
    }
}

// 绕过障碍用的过渡点（相对坐标）：取障碍中心处的自变量，因变量取障碍边缘再往外一点
Vec2 detour_point(const Obstacle& ob, const Vec2& origin, bool horizontal, double side) {
    constexpr double kMargin = 4.0;
    if (horizontal) {
        const double ext = (ob.is_circle ? ob.radius : ob.half_h) + kMargin;
        return Vec2(ob.center.x() - origin.x(), (ob.center.y() + side * ext) - origin.y());
    }
    const double ext = (ob.is_circle ? ob.radius : ob.half_w) + kMargin;
    return Vec2((ob.center.x() + side * ext) - origin.x(), ob.center.y() - origin.y());
}

// 敌方士兵引用（多士兵规则下目标是"某个士兵"而不是"某个玩家"）
struct TargetRef {
    const Player* player = nullptr;
    int soldier = 0;
    Vec2 pos() const { return player->soldiers[static_cast<size_t>(soldier)].pos; }
    int hp() const { return player->soldiers[static_cast<size_t>(soldier)].hp; }
};

// 困难 AI 的规划主流程：枚举目标子集 → 各骨架构造/插值 → 评估 → 必要时加绕行点修正。
// max_targets：单次规划取用的目标数上限（困难档收在 4 划出与大神的一发多穿差距；
// 大神传敌人总数 = 不限——能一波全扫就全扫）
// prev：上一回合的出手（出手多样性过滤用，见 drop_repeats_of_prev；首回合为 null）
Plan plan_hard(const World& world, const Player& self, const std::vector<TargetRef>& enemies,
               const AiProfile& prof, size_t max_targets, Rng& rng,
               const CurveRequest* prev = nullptr) {
    const Vec2 self_pos = self.current().pos;
    // 带瞄准误差的目标相对坐标
    std::vector<Vec2> rels;
    for (const auto& e : enemies) {
        Vec2 aim = e.pos();
        const double dist = distance(self_pos, aim);
        const double slack = std::min(dist * prof.pos_error, prof.aim_slack_cap);
        aim.x() += rng.range(-1.0, 1.0) * slack;
        aim.y() += rng.range(-1.0, 1.0) * slack;
        rels.push_back(aim - self_pos);
    }
    std::vector<Vec2> abs_targets;
    for (const auto& r : rels) abs_targets.push_back(self_pos + r);

    // 收集所有可行计划（每个子集先决出最优骨架，最后统一择优）
    std::vector<Plan> plans;
    std::vector<Plan> singles;  // 单目标子集的计划：多杀限流触发时退而用它们
    // 主轴 × 方向四个组合各跑一整轮。主轴此前按"目标在哪个轴更分散"全局定一个，
    // 但那只是启发式：共享轴的敌人组合（同一列/同一行）在选错的主轴下会被
    // plan_points 的"自变量过近"整条丢掉，障碍布局也会让同一组合只剩另一个轴
    // 可打——只试一个主轴等于白白扔掉一半多杀候选
    for (int combo = 0; combo < 4; ++combo) {
        const bool horizontal = combo < 2;
        const FireDir dir = (combo % 2 == 0) ? FireDir::Positive : FireDir::Negative;
        std::vector<Vec2> group;
        for (const auto& r : rels) {
            const double t = horizontal ? r.x() : r.y();
            if ((dir == FireDir::Positive && t > 0.0) || (dir == FireDir::Negative && t < 0.0)) {
                group.push_back(r);
            }
        }
        if (group.empty()) continue;
        // 目标多于上限时只规划最近的若干个：子集枚举是 O(n²)，每个子集的每种骨架
        // 都要一次 ExprTk 编译（~0.6ms）——远处目标本来就难打中，截断对战术几乎无损
        //（group 里是相对坐标，模长即距离）。困难档上限 4（仍有 C(4,2)=6 组两杀
        // 配对）；大神由调用方传"敌人总数"= 不限
        if (group.size() > max_targets) {
            std::partial_sort(group.begin(), group.begin() + static_cast<ptrdiff_t>(max_targets),
                              group.end(), [](const Vec2& a, const Vec2& b) {
                                  return a.squaredNorm() < b.squaredNorm();
                              });
            group.resize(max_targets);
        }
        std::sort(group.begin(), group.end(), [horizontal](const Vec2& a, const Vec2& b) {
            return (horizontal ? a.x() : a.y()) < (horizontal ? b.x() : b.y());
        });

        // 候选子集：全部目标、两两组合、单个目标
        std::vector<std::vector<Vec2>> subsets;
        subsets.push_back(group);
        for (size_t i = 0; i < group.size(); ++i) {
            for (size_t j = i + 1; j < group.size(); ++j) {
                subsets.push_back({group[i], group[j]});
            }
        }
        for (const auto& g : group) subsets.push_back({g});

        for (const auto& sub : subsets) {
            const auto pts = plan_points(horizontal, dir, sub);
            if (pts.size() < 2) continue;
            // 各骨架都评估、可行的全留（见 try_pair_styles 的注释：子集最优会压死慢曲线）
            std::vector<Plan> subset_plans;
            for (const PlanStyle st : {PlanStyle::Poly, PlanStyle::Stair}) {
                subset_plans.push_back(
                    evaluate_plan(pts, horizontal, st, world, self_pos, abs_targets, self));
            }
            // 折线链只在多目标子集有意义（单目标时它退化成直线，与多项式重合）
            if (sub.size() >= 2) {
                subset_plans.push_back(evaluate_plan(pts, horizontal, PlanStyle::Kink, world,
                                                     self_pos, abs_targets, self));
            }
            if (sub.size() <= 2) {
                try_pair_styles(pts, horizontal, world, self_pos, abs_targets, self, subset_plans);
            }
            if (sub.size() == 3) {
                try_triple_styles(pts, horizontal, world, self_pos, abs_targets, self,
                                  subset_plans);
            }

            // 子集最优：绕行修正从它出发（撞了障碍——没能全打到或"打中了但靠炸障碍开路"）
            Plan p;
            for (const auto& q : subset_plans) {
                if (better_plan(q, p)) p = q;
            }
            // "没全打中"以 plan_points 实际接受的点数为准：过近/反向被丢的目标
            // 根本不在曲线上，拿原始子集大小比较会把这类子集误判成"没打全"、
            // 触发徒劳的绕行迭代（绕行救不回被丢的点）
            if (p.valid && (p.reach < static_cast<int>(pts.size()) - 1 || !p.clear)) {
                // 迭代绕行（三轮封顶）：每轮找当前曲线的第一个挡路障碍、借它的边缘加一个
                // 过渡点重规划；绕开一个又撞下一个时把新点累积起来再试。折线链与多项式
                // 都能过一串点——"找不到干净方程才炸障碍"的兜底要靠它尽量兜住
                std::vector<Vec2> route = sub;
                for (int round = 0; round < 3; ++round) {
                    // 挡路障碍用评估时顺带算好的缓存：同一条 req、同一个 origin，
                    // 重采样只会得到同一结果。进入本分支时 p 必然有效（上面判过
                    // valid），blocker 就是这条曲线的结果
                    const int ob_idx = p.blocker;
                    if (ob_idx < 0) break;  // 已经不撞墙，没什么可绕的
                    const Obstacle& ob = world.obstacles()[static_cast<size_t>(ob_idx)];
                    struct RPlan {
                        Plan plan;
                        Vec2 pt;
                    };
                    std::vector<RPlan> round_best;
                    for (double s : {1.0, -1.0}) {
                        const Vec2 dp = detour_point(ob, self_pos, horizontal, s);
                        std::vector<Vec2> with_detour = route;
                        with_detour.push_back(dp);
                        // 两种骨架都试：绕行点多了之后插值多项式会剧烈振荡，折线链更稳
                        for (const PlanStyle st : {PlanStyle::Poly, PlanStyle::Kink}) {
                            Plan q =
                                evaluate_plan(plan_points(horizontal, dir, with_detour), horizontal,
                                              st, world, self_pos, abs_targets, self);
                            if (q.valid) round_best.push_back({q, dp});
                        }
                    }
                    if (round_best.empty()) break;
                    // 本轮最优：干净优先 > 命中多 > 友伤少 > 短
                    const RPlan* win = &round_best[0];
                    for (const auto& rp : round_best) {
                        const Plan& a = rp.plan;
                        const Plan& b = win->plan;
                        const bool better = a.clear != b.clear   ? a.clear
                                            : a.reach != b.reach ? a.reach > b.reach
                                            : a.ally != b.ally   ? a.ally < b.ally
                                                                 : a.len < b.len;
                        if (better) win = &rp;
                    }
                    for (const auto& rp : round_best) subset_plans.push_back(rp.plan);
                    if (win->plan.clear && win->plan.reach >= static_cast<int>(pts.size()) - 1)
                        break;  // 全中口径与上面的触发条件一致（按有效点数）
                    if (round == 2) break;  // 三轮封顶：再绕下去评估成本翻倍，收益很小
                    route.push_back(win->pt);  // 带上这个绕行点，下一轮继续找下一个挡路的
                    p = win->plan;
                }
            }
            // 只有真能打到人的计划才配进池（valid 仅代表采样成功，reach 才是评估出的
            // 实际命中数；单目标退路还要求 reach ≥ 1，退到一条打了也空的线没意义）
            for (const auto& q : subset_plans) {
                if (!q.valid || q.reach < 1) continue;
                plans.push_back(q);
                if (sub.size() == 1) singles.push_back(q);
            }
        }
    }
    if (plans.empty()) return Plan{};

    // 择优：先命中数、再避开友伤，最后在同档且长度相近（≤最短 1.5 倍）的候选里随机挑。
    // 不硬挑"永远最短"：路径长短没有游戏性代价（回合制、无时间惩罚），同档内换着骨架
    // 打让弹道观感多样；随机数来自决策 rng，同种子结果仍然完全确定
    auto pick = [&](const std::vector<Plan>& pool) {
        // 好线优先：能命中且全程不碰障碍的方案是"好线"，有就不沾障碍；一条好线
        // 都没有（数学上绕不开）才允许炸障碍开路——炸是最后手段，不是多杀的代价。
        // 在好线内部，命中人数才是第一优先级（一发多杀是水平档位的载体）
        bool any_clear = false;
        for (const auto& pl : pool) {
            if (pl.clear) any_clear = true;
        }
        std::vector<const Plan*> usable;
        for (const auto& pl : pool) {
            if (!any_clear || pl.clear) usable.push_back(&pl);
        }
        int max_reach = -1;
        for (const auto* pl : usable) max_reach = std::max(max_reach, pl->reach);
        // 偶尔把"少打一个"的档也拉进选择池：可达/净收益仍优先，但给形状多样性一点
        // 空间——多杀骨架（多项式/阶梯/折线链）会垄断"打得多"的档，不让半步时
        // 击杀形状一直是它们的专场（评测里占七成）
        // 只在命中人数最高的那一档里挑。旧的"偶尔少打一个"降档已移除：它会让
        // reach=1 的无伤单杀线混进池子，随后的 ally 过滤（取最小友伤）进而把
        // reach=2 的多杀线整条剔掉——统计上专门压多杀，与 better_plan 的
        // reach 优先口径也矛盾。形状多样性由类型平权与同档长度偏好承担，
        // 不再牺牲命中数（多杀是水平档位的载体）
        int min_ally = std::numeric_limits<int>::max();
        for (const auto* pl : usable) {
            if (pl->reach >= max_reach) min_ally = std::min(min_ally, pl->ally);
        }
        std::vector<const Plan*> cands;
        for (const auto* pl : usable) {
            if (pl->reach >= max_reach && pl->ally == min_ally) cands.push_back(pl);
        }
        // 与上一发近似的同档线优先剔掉（见 drop_repeats_of_prev）：pick 有两处
        // 调用（多杀线集/单杀线集），过滤放这里对两者一体生效
        if (prev != nullptr) drop_repeats_of_prev(cands, *prev);
        // 每种可行骨架先平等翻牌（各 1/kinds 概率），再在类型里取最短档随机：
        // 跨类型的长度阈值会把"稍长一点的其它形状"整族排除，出手全挤在最短的
        // 一两种几何上（评测里困难/大神多项式占一半就是这么来的）；长度只做
        // 类型内的弱偏好（1.05 倍档），防止绕大圈
        std::vector<PlanKind> kinds;
        for (const auto* pl : cands) {
            if (std::find(kinds.begin(), kinds.end(), pl->kind) == kinds.end()) {
                kinds.push_back(pl->kind);
            }
        }
        const PlanKind pick_kind =
            kinds[static_cast<size_t>(rng.range_int(0, static_cast<int>(kinds.size()) - 1))];
        double kind_min = 1e18;
        for (const auto* pl : cands) {
            if (pl->kind == pick_kind) kind_min = std::min(kind_min, pl->len);
        }
        std::vector<const Plan*> same_kind;
        for (const auto* pl : cands) {
            if (pl->kind == pick_kind && pl->len <= kind_min * 1.05) same_kind.push_back(pl);
        }
        return *same_kind[static_cast<size_t>(
            rng.range_int(0, static_cast<int>(same_kind.size()) - 1))];
    };
    const Plan best = pick(plans);
    const Plan best_single = singles.empty() ? Plan{} : pick(singles);
    // 多杀限流（见 AiProfile 注释）：刻意扫多人的线按概率保留，否则退到最佳单杀线；
    // best_single 无效（单目标全被障碍挡死）时保底仍用多杀线，不至于这回合不会打人。
    // multi_kill_kept 的调用位置与短路顺序不要再动：掷骰时机必须只由局面决定，
    // 加提前 return 会改变 rng 消费时机、破坏同种子确定性

    bool degraded = false;
    if (best.valid && best.reach >= 2 && !multi_kill_kept(best.reach, prof, rng)) {
        degraded = best_single.valid;
    }
    if (ai_trace_on() && ai_trace_take()) {
        // multi_clean：池里"干净且能打中≥2人"的候选数——多杀机会诊断用；degraded：
        // 本回合的多杀线被限流掷骰否掉、退到了单杀
        size_t multi_clean = 0;
        for (const auto& pl : plans) {
            if (pl.clear && pl.reach >= 2) ++multi_clean;
        }
        std::fprintf(stderr,
                     "[trace] plan_hard best: kind=%d reach=%d ally=%d clear=%d len=%.1f "
                     "plans=%zu singles=%zu multi_clean=%zu degraded=%d\n",
                     static_cast<int>(best.kind), best.reach, best.ally, best.clear ? 1 : 0,
                     best.len, plans.size(), singles.size(), multi_clean, degraded ? 1 : 0);
    }
    if (degraded) return best_single;
    return best;
}

// 类型"出场配额"（balance 抽取用）：多项式族（直线/抛物线/插值过点）各种局面都能
// 凑合、又是兜底，常年占掉六成的出手；压它的配额给其它形状让位，各类型出场分布
// 更均衡（观感上"各种曲线都看得到"）。大神档不适用——大神只管赢，由它自己挑
double kind_quota(CurveKind k) { return (k == CurveKind::Poly) ? 0.6 : 1.0; }

// 候选池"挑形状类型、再类型内取最短档随机"：直接对候选平权时，同一类型在多个
// 生成变体里重复出现，会靠数量碾压其它形状——出手全一个样；长度只在类型内做弱偏好
//（绕大圈没必要，但也不许跨类型压掉"稍长"的整个形状族）。balance = true 时类型层
// 按 kind_quota 加权（简单/普通/困难求"出场均衡"），false 时均匀（大神）
const CurveCandidate* pick_by_kind(const std::vector<const CurveCandidate*>& v, Rng& rng,
                                   bool balance) {
    std::vector<CurveKind> kinds;
    for (const auto* c : v) {
        if (std::find(kinds.begin(), kinds.end(), c->kind) == kinds.end()) {
            kinds.push_back(c->kind);
        }
    }
    CurveKind k = kinds.back();
    double r = rng.range(0.0, [&] {
        double total = 0.0;
        for (const CurveKind kk : kinds) total += balance ? kind_quota(kk) : 1.0;
        return total;
    }());
    for (const CurveKind kk : kinds) {
        r -= balance ? kind_quota(kk) : 1.0;
        if (r <= 0.0) {
            k = kk;
            break;
        }
    }
    double kmin = 1e18;
    for (const auto* c : v) {
        if (c->kind == k) kmin = std::min(kmin, c->sampled.path.length());
    }
    std::vector<const CurveCandidate*> same;
    for (const auto* c : v) {
        if (c->kind == k && c->sampled.path.length() <= kmin * 1.05) same.push_back(c);
    }
    return same[static_cast<size_t>(rng.range_int(0, static_cast<int>(same.size()) - 1))];
}

}  // namespace

namespace {
// 评测调参用的档位覆盖（见 ai.h 的说明）：空 = 用默认档位表
std::optional<AiProfile> g_profile_override[kAiDifficultyCount];
}  // namespace

void set_ai_profile_override(int difficulty, std::optional<AiProfile> prof) {
    if (difficulty >= 0 && difficulty < kAiDifficultyCount) {
        g_profile_override[difficulty] = std::move(prof);
    }
}

bool multi_kill_kept(int reach, const AiProfile& prof, Rng& rng) {
    // 单目标不掷骰：限流只拦"刻意多杀"，保证 rng 消费时机只由局面决定的过程简单
    if (reach <= 1) return true;
    return rng.chance(reach >= 3 ? prof.multi_keep3 : prof.multi_keep2);
}

// 档位默认值 = 2026-10-15 自动调参（scripts/tune_ai.py，口径：相邻 70% 优先、
// fire0 锚 0.25）取到的最优参数；改物理（碰撞体/地图）或手改这些值后，用调参
// 脚本重扫并更新这里
AiProfile ai_profile(int difficulty) {
    if (difficulty >= 0 && difficulty < kAiDifficultyCount &&
        g_profile_override[difficulty].has_value()) {
        return *g_profile_override[difficulty];
    }
    switch (difficulty) {
        case 0:  // 简单：误差大、散布封顶也小（打远目标也是差一两格），只玩直线与抛物线
            return AiProfile{.pos_error = 0.15,
                             .angle_error = 0.0355957,
                             .think_min = 1.0,
                             .think_max = 2.5,
                             .aim_slack_cap = 1.44196};
        case 2:  // 困难：误差小，全模式可用，还做一发多穿的规划（带多杀限流）——
                 // 与大神是"会失手 vs 不失手"的关系：精度刻意留档差，不然两档对局五五开。
                 // 炮弹碰撞体变大后（命中更宽容）差档的失手不再致命，误差据此上调过，
                 // 维持两档的胜率差
            return AiProfile{.pos_error = 0.042,
                             .angle_error = 0.045,
                             .think_min = 2.5,
                             .think_max = 6.0,
                             // 多杀是高水平 AI 的主要强度载体，限流只留"偶尔的
                             // 杀手锏"观感（这是唯一还在主动丢多杀的机制，别再加）
                             .multi_keep2 = 0.85,
                             .multi_keep3 = 0.85};
        case 3:  // 大神：比困难更准，多杀不限流——能一炮扫完就一炮扫完
            return AiProfile{.pos_error = 0.002,
                             .angle_error = 0.00225,
                             .think_min = 2.5,
                             .think_max = 6.0,
                             .multi_keep2 = 1.0,
                             .multi_keep3 = 1.0};
        default:  // 普通：误差小，多模式可用
            return AiProfile{.pos_error = 0.05625,
                             .angle_error = 0.03375,
                             .think_min = 2.0,
                             .think_max = 4.5};
    }
}

AiDecision ai_decide(const World& world, int player_id, int difficulty, Rng& rng,
                     const CurveRequest* prev) {
    const AiProfile prof = ai_profile(difficulty);
    AiDecision out;

    const Player* self = nullptr;
    for (const auto& p : world.players()) {
        if (p.id == player_id) self = &p;
    }
    if (!self || !self->any_alive()) return out;
    // 出手的是"当前轮到的那个士兵"（多士兵规则下由世界游标决定）
    const Vec2 self_pos = self->current().pos;

    std::vector<TargetRef> enemies;
    for (const auto& p : world.players()) {
        if (p.team == self->team) continue;
        for (size_t si = 0; si < p.soldiers.size(); ++si) {
            if (p.soldiers[si].alive) {
                enemies.push_back(TargetRef{&p, static_cast<int>(si)});
            }
        }
    }
    if (enemies.empty()) return out;

    // 思考时间（不超过输入时限的 80%，避免自己把自己拖超时）
    const double max_think = std::max(0.1, world.config().match.input_seconds * 0.8);
    out.delay = std::min(rng.range(prof.think_min, prof.think_max), max_think);

    // 困难：多目标插值规划（一发尽量多穿人 + 绕障）。只有不碰障碍（clear）才直接定；
    // 脏线留作保底——先给单目标候选池一次机会（池里可能有更干净的线），实在没有
    // 干净方案才允许"炸障碍开路"：那是有代价的，会把双方共享的地形炸掉
    Plan plan_hard_result;
    if (difficulty >= 2) {
        // 一发多穿的目标数上限：困难 4，大神不限（详见 plan_hard 的说明）
        const size_t max_targets = (difficulty >= 3) ? enemies.size() : 4;
        plan_hard_result = plan_hard(world, *self, enemies, prof, max_targets, rng, prev);
        if (plan_hard_result.valid && plan_hard_result.reach >= 1 && plan_hard_result.clear) {
            if (ai_trace_on() && ai_trace_take()) {
                std::fprintf(stderr, "[trace] d=%d path=A kind=%d reach=%d ally=%d\n", difficulty,
                             static_cast<int>(plan_hard_result.kind), plan_hard_result.reach,
                             plan_hard_result.ally);
            }
            out.request = std::move(plan_hard_result.req);
            return out;
        }
        if (ai_trace_on() && ai_trace_take()) {
            std::fprintf(stderr, "[trace] d=%d plan_hard not used: valid=%d reach=%d clear=%d\n",
                         difficulty, plan_hard_result.valid ? 1 : 0, plan_hard_result.reach,
                         plan_hard_result.clear ? 1 : 0);
        }
    }

    // 简单/普通（以及困难规划的兜底）：单目标瞄准（困难优先打残血的士兵）
    const TargetRef* target =
        &enemies[static_cast<size_t>(rng.range_int(0, static_cast<int>(enemies.size()) - 1))];
    if (difficulty >= 2) {
        for (const auto& e : enemies) {
            if (e.hp() < target->hp()) target = &e;
        }
    }

    Vec2 aim = target->pos();
    const double dist = distance(self_pos, aim);
    // 瞄准误差按距离比例、但按档封顶（见 AiProfile::aim_slack_cap 的注释）
    const double slack = std::min(dist * prof.pos_error, prof.aim_slack_cap);
    aim.x() += rng.range(-1.0, 1.0) * slack;
    aim.y() += rng.range(-1.0, 1.0) * slack;
    if (prof.angle_error > 0.0) {
        const double ang = rng.range(-1.0, 1.0) * prof.angle_error;
        const Vec2 rel = aim - self_pos;
        const double c = std::cos(ang), s = std::sin(ang);
        aim = self_pos + Vec2(rel.x() * c - rel.y() * s, rel.x() * s + rel.y() * c);
    }

    const double dx = aim.x() - self_pos.x();
    const double dy = aim.y() - self_pos.y();
    if (std::hypot(dx, dy) < 1e-6) return out;  // 目标与自身重合（实际不会发生）：这回合跳过
    const bool horizontal = std::abs(dx) >= std::abs(dy);

    // 多模式候选池：各生成器保证"曲线经过瞄准点"，评估负责筛掉到不了/撞墙的形状
    std::vector<CurveCandidate> pool =
        generate_candidates(horizontal, dx, dy, difficulty, self_pos, aim, world, rng);
    // 拟人化"微调上一发"：一定概率把上一回合的表达式做数字级扰动后入池
    //（结构不变、参数微调——"就差一点，调调系数再来"）。评估照常，不行会被筛掉。
    // 大神必然扰动且多扰几轮：同样是择优输入，多份"差一点的上一次"变体让大神
    // 更容易捡回自己失手的那条线（与困难的差距主要由它拉开）。
    // 扰动量（±10%）落在 is_repeat_of 的 12% 判定内：这些变体与上一发同档时会被
    // 出手多样性过滤剔掉，只在"那条线是唯一解"时才真的微调重发
    if (prev != nullptr && (difficulty >= 3 || rng.chance(0.6))) {
        const int rounds = (difficulty >= 3) ? prof.jitter_rounds : 1;
        for (int k = 0; k < rounds; ++k) {
            CurveRequest mutated = *prev;
            mutated.expr_a = jitter_numbers(mutated.expr_a, rng);
            if (!mutated.expr_b.empty()) mutated.expr_b = jitter_numbers(mutated.expr_b, rng);
            pool.push_back(evaluate_candidate(mutated, self_pos, aim, world));
        }
    }
    // 扫射候选："瞄点 + 另一个敌人"的插值抛物线（纵横两个单值方向各试一条）。
    // 穿透机制下一发依次扫过两人、收益成倍；配对取离目标最近的几个——
    // 太远的配对形状会夸张到被评估筛掉，白花采样成本。
    // 多杀限流（见 AiProfile 注释）：困难档按概率才生成这类"刻意两杀"的配对；
    // 普通档不掷骰，维持原有"形状随机"的风格；简单档完全不生成——它的设定是
    // "一次只打一个士兵"，顺路蹭到别人是弹道自然发生的，不算刻意（rng 消费与
    // 旧版一致：只有困难/大神档才掷骰）
    const bool want_pair = (difficulty >= 2) ? multi_kill_kept(2, prof, rng) : (difficulty >= 1);
    if (want_pair) {
        std::vector<const TargetRef*> others;
        for (const auto& e : enemies) {
            if (e.player == target->player && e.soldier == target->soldier) continue;
            others.push_back(&e);
        }
        std::sort(others.begin(), others.end(), [&](const TargetRef* a, const TargetRef* b) {
            return distance(aim, a->pos()) < distance(aim, b->pos());
        });
        constexpr size_t kMaxPairs = 3;
        for (size_t i = 0; i < others.size() && i < kMaxPairs; ++i) {
            const Vec2 p = others[i]->pos();
            const double ox = p.x() - self_pos.x();
            const double oy = p.y() - self_pos.y();
            CurveRequest req;
            // 单值曲线只朝发射方向的一侧延伸：两点必须同侧且坐标不同
            if (std::abs(dx) > 1e-6 && std::abs(ox) > 1e-6 && (dx > 0) == (ox > 0) &&
                std::abs(dx - ox) > 1e-6) {
                pool.push_back(evaluate_candidate(make_parabola_through(true, dx, dy, ox, oy),
                                                  self_pos, aim, world));
                // 折点折线与波浪变体：同是"过两人"，形状不同、地形适应性不同
                if (make_kink_bend(true, dx, dy, ox, oy, (dx + ox) * 0.5, req)) {
                    pool.push_back(evaluate_candidate(req, self_pos, aim, world));
                }
                const double w = 2.0 * std::numbers::pi / std::abs(dx - ox);
                if (make_wave_through(true, dx, dy, ox, oy, w, req)) {
                    pool.push_back(evaluate_candidate(req, self_pos, aim, world));
                }
            }
            if (std::abs(dy) > 1e-6 && std::abs(oy) > 1e-6 && (dy > 0) == (oy > 0) &&
                std::abs(dy - oy) > 1e-6) {
                pool.push_back(evaluate_candidate(make_parabola_through(false, dx, dy, ox, oy),
                                                  self_pos, aim, world));
                if (make_kink_bend(false, dx, dy, ox, oy, (dy + oy) * 0.5, req)) {
                    pool.push_back(evaluate_candidate(req, self_pos, aim, world));
                }
                const double w = 2.0 * std::numbers::pi / std::abs(dy - oy);
                if (make_wave_through(false, dx, dy, ox, oy, w, req)) {
                    pool.push_back(evaluate_candidate(req, self_pos, aim, world));
                }
            }
        }
    }

    // 池里没有一条"到得了又不撞墙"的（障碍把常见形状都挡死）时，主动绕障：
    // 沿曲线找第一个挡路障碍，借它的边缘（外扩 4 单位）当过渡点，插一条
    // "先绕障碍、再落瞄准点"的抛物线。过渡点高度贴着障碍尺寸算——固定拱高
    // 的小抛物线绕不过高障碍，这里能自动适配。起拱后的新曲线若又被别的障碍
    // 挡住，就对它再找下一个挡路障碍、再起拱一轮（最多两轮）——普通档没有
    // 困难档的多目标规划器，靠这套迭代把"连挡两个"的常见局面兜住
    if (difficulty >= 1) {
        bool have_clean = false;
        for (const auto& c : pool) {
            if (c.reaches && c.clear) {
                have_clean = true;
                break;
            }
        }
        if (!have_clean) {
            const double u2 = horizontal ? dx : dy;  // 瞄准点自变量
            std::vector<CurveRequest> frontier = {make_parabola(horizontal, dx, dy, 0.0)};
            for (int round = 0; round < 2; ++round) {
                std::vector<CurveRequest> next;
                for (const CurveRequest& cur : frontier) {
                    const CurveResult sampled = sample_curve(cur, self_pos, world.config());
                    if (!sampled.ok()) continue;
                    const int ob_idx = first_blocking_obstacle(sampled.path, world);
                    if (ob_idx < 0) continue;
                    const Obstacle& ob = world.obstacles()[static_cast<size_t>(ob_idx)];
                    const Vec2 rel = ob.center - self_pos;
                    const double t_ob = horizontal ? rel.x() : rel.y();
                    const double hv = horizontal ? rel.y() : rel.x();
                    const double ext =
                        (ob.is_circle ? ob.radius : (horizontal ? ob.half_h : ob.half_w)) + 4.0;
                    // 单值曲线只朝一侧延伸：障碍与瞄准点同侧、离原点与彼此都够远
                    if (std::abs(t_ob) > 0.5 && (t_ob > 0.0) == (u2 > 0.0) &&
                        std::abs(t_ob - u2) > 1.5) {
                        // 按障碍高度精确起拱（顶/底两侧各一条）：比"过绕行点插值"稳——
                        // 障碍离原点近时过点插值的曲线会贴着障碍爬升、还是免不了撞
                        for (const double h : {hv + ext, hv - ext}) {
                            const CurveRequest arc =
                                make_obstacle_arc(horizontal, dx, dy, t_ob, h);
                            pool.push_back(evaluate_candidate(arc, self_pos, aim, world));
                            next.push_back(arc);
                        }
                    }
                }
                frontier = std::move(next);
            }
        }
    }

    // 按难度分层用池子：
    //   简单——不筛选也不择优，从能发射的里按类型均匀乱抽（弱得像在瞎打）
    //   普通——只要打得到就行，形状随机（能命中，但轨迹五花八门）
    //   困难——每次都选最优：命中精度同一档里取最短路径（又准又快）
    if (difficulty <= 0) {
        // 瞎打，但也不能一枪不中：有"经过瞄点"的候选时八成回合从它们里随机抽
        // （形状照样乱），两成完全随机保留"乱来"的观感。"经过瞄点"只保证曲线过
        // 带偏差的瞄点，命中真人仍靠散布运气——简单档该弱还弱，只是不再全程放空
        std::vector<const CurveCandidate*> any, on_aim;
        for (const auto& c : pool) {
            if (!c.sampled.ok()) continue;
            any.push_back(&c);
            if (c.reaches) on_aim.push_back(&c);
        }
        if (!on_aim.empty() && rng.chance(0.85)) {
            out.request = pick_by_kind(on_aim, rng, true)->req;
            return out;
        }
        if (!any.empty()) {
            out.request = pick_by_kind(any, rng, true)->req;
            return out;
        }
    } else {
        std::vector<const CurveCandidate*> clear_ok, reachable;
        for (const auto& c : pool) {
            if (!c.reaches) continue;
            if (c.clear) clear_ok.push_back(&c);
            reachable.push_back(&c);
        }
        // 干净曲线优先，普通档另留三成概率从"能命中但会擦到障碍"的线里挑：
        // 阶梯这类"竖直跳变段撞墙、但确实能打中人"的形状，只走干净池的话永远轮不到
        // （实测出手占比 0%）——普通档本来就是"打得花、偶尔糙"的档，正好收它们的多样性
        const bool prefer_clean = !clear_ok.empty() && (difficulty >= 2 || rng.chance(0.7));
        const std::vector<const CurveCandidate*>& usable = prefer_clean ? clear_ok : reachable;
        if (difficulty == 1 && !usable.empty()) {
            if (ai_trace_on() && ai_trace_take()) {
                std::fprintf(stderr, "[trace] d=1 path=B pool=%zu clear=%zu reach=%zu prefer=%s\n",
                             pool.size(), clear_ok.size(), reachable.size(),
                             prefer_clean ? "clear" : "reach");
            }
            out.request = pick_by_kind(usable, rng, true)->req;
            return out;
        }
        if (difficulty >= 2 && !usable.empty()) {
            // 择优次序：净扫中数（敌人 − 队友：穿透机制下一发多杀、收益成倍，代价是
            // 形状往往不瞄着单点）→ 命中精度（与最准的差不到半个命中半径算同一档）
            // → 形状类型平等、类型内取最短档（见 pick_by_kind）
            const double hit_r = world.config().world.player_half_size;
            struct Scored {
                const CurveCandidate* c;
                int net;  // 扫过的敌人 - 扫过的队友
                double len;
            };
            std::vector<Scored> scored;
            int best_net = std::numeric_limits<int>::min();
            for (const auto* c : usable) {
                int eh = 0, ah = 0;
                scan_soldier_hits(c->sampled.path, world, *self, hit_r, eh, ah);
                scored.push_back({c, eh - ah, c->sampled.path.length()});
                best_net = std::max(best_net, eh - ah);
            }
            std::vector<const CurveCandidate*> swept;
            double best_gap = 1e18;
            for (const auto& s : scored) {
                if (s.net != best_net) continue;
                swept.push_back(s.c);
                best_gap = std::min(best_gap, s.c->aim_gap);
            }
            std::vector<const CurveCandidate*> finals;
            for (const auto* c : swept) {
                // 命中精度在同一档即可；类型平等与类型内的长度弱偏好交给 pick_by_kind
                // （跨类型的长度阈值会整族压掉"稍长"的形状，出手花样就没了）
                if (c->aim_gap <= best_gap + 0.5) finals.push_back(c);
            }
            // 与上一发近似的线优先剔掉（同档等效，换一条只有观感收益，见 drop_repeats_of_prev）
            if (prev != nullptr) drop_repeats_of_prev(finals, *prev);
            if (ai_trace_on() && ai_trace_take()) {
                std::fprintf(stderr, "[trace] d=%d path=B finals=%zu net=%d best_gap=%.2f\n",
                             difficulty, finals.size(), best_net, best_gap);
            }
            // 困难档（含）以下按出场配额抽取（见 kind_quota），大神保持均匀
            out.request = pick_by_kind(finals, rng, difficulty <= 2)->req;
            return out;
        }
    }

    // 单目标池也挑不出打得到的线时，困难规划里"能打到但会炸障碍"的方案好过乱打
    if (plan_hard_result.valid && plan_hard_result.reach >= 1) {
        out.request = std::move(plan_hard_result.req);
        return out;
    }
    // 最后兜底发射直线：可能中途被遮挡物挡住（炮弹会炸坑开路），但一定朝瞄准点方向
    out.request = make_parabola(horizontal, dx, dy, 0.0);
    return out;
}

// 两条请求是否"同一发"：视角/方向/模式不同的式子即使参数一致也是完全不同的弹道，
// 先排除；再要求两段表达式都数字级近似（见 expr_repeats）
bool is_repeat_of(const CurveRequest& c, const CurveRequest& prev) {
    if (c.mode != prev.mode || c.dir != prev.dir || c.mirror != prev.mirror) return false;
    return expr_repeats(c.expr_a, prev.expr_a) && expr_repeats(c.expr_b, prev.expr_b);
}

}  // namespace fg
