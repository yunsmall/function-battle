#include "ai.h"

#include <algorithm>
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

// 点到线段的距离（投影参数钳制到 [0,1]；退化段按点处理）
double point_segment_distance(const Vec2& p, const Vec2& a, const Vec2& b) {
    const Vec2 ab = b - a;
    const double len2 = ab.squaredNorm();
    if (len2 < 1e-18) return (p - a).norm();
    const double t = std::clamp((p - a).dot(ab) / len2, 0.0, 1.0);
    return (p - (a + t * ab)).norm();
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

// 统计路径扫过的士兵数：穿透机制下一条曲线会依次命中多个士兵，一发"扫射"
// 的收益成倍。命中判定与 World 一致（轴对齐方块 |dx| ≤ r 且 |dy| ≤ r，不是圆形——
// 圆是方块的内切圆，会系统性漏算"擦过方块角点"的命中，让择优错过更好的候选）；
// 抽样步长按平均点距自适应（保证不漏）；发射者自己不算（炮口保护不会伤到他）
void scan_soldier_hits(const Path& path, const World& world, const Player& self,
                       double hit_radius, int& enemy_hits, int& ally_hits) {
    enemy_hits = 0;
    ally_hits = 0;
    if (path.points.size() < 2) return;
    const double avg_step = path.length() / static_cast<double>(path.points.size());
    const size_t stride =
        std::max<size_t>(1, static_cast<size_t>(0.2 / std::max(avg_step, 1e-6)));
    // 撞上第一个遮挡物之后的部分不算"实际扫过"（炮弹已炸毁）——与真实飞行同口径，
    // 不然"撞墙前其实谁也扫不到"的候选会被打分虚报成高命中
    const double crater = world.config().world.crater_radius;
    size_t usable = path.points.size();
    for (size_t i = 0; i < path.points.size(); i += stride) {
        bool wall = false;
        for (const auto& ob : world.obstacles()) {
            if (obstacle_blocks(ob, path.points[i], crater)) {
                wall = true;
                break;
            }
        }
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
            for (size_t i = 0; i < usable; i += stride) {
                if (std::abs(path.points[i].x() - s.pos.x()) <= hit_radius &&
                    std::abs(path.points[i].y() - s.pos.y()) <= hit_radius) {
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

// 数一数路径在"撞上第一个障碍之前"能实际命中几个目标
int reachable_targets(const Path& path, const std::vector<Vec2>& targets, const World& world) {
    std::vector<bool> hit(targets.size(), false);
    int count = 0;
    const GameConfig& cfg = world.config();
    const double crater = cfg.world.crater_radius;
    Vec2 prev = path.points.front();
    for (size_t i = 0; i + 1 < path.points.size(); ++i) {
        const Vec2& a = path.points[i];
        const Vec2& b = path.points[i + 1];
        const size_t n = std::max<size_t>(
            1, static_cast<size_t>(std::ceil((b - a).norm() / kAiFlightStep)));
        for (size_t s = 1; s <= n; ++s) {
            const double t = static_cast<double>(s) / static_cast<double>(n);
            const Vec2 p(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t);
            // 命中判定用"子段到目标的距离"：只取点会漏掉从目标身上掠过的命中
            for (size_t k = 0; k < targets.size(); ++k) {
                if (!hit[k] && point_segment_distance(targets[k], prev, p) < 0.5) {
                    hit[k] = true;
                    ++count;
                }
            }
            // 与飞行同序：撞墙点也参与命中判定，之后都打不到了
            for (const auto& ob : world.obstacles()) {
                if (obstacle_blocks(ob, p, crater)) return count;
            }
            // 炮弹飞出地图即销毁：之后的点同样到不了
            if (p.x() < cfg.world.map_x_min || p.x() > cfg.world.map_x_max || p.y() < cfg.world.map_y_min ||
                p.y() > cfg.world.map_y_max) {
                return count;
            }
            prev = p;
        }
    }
    return count;
}

// ---------- 多目标规划（困难 AI） ----------

struct Plan {
    bool valid = false;
    CurveRequest req;
    int reach = 0;  // "撞墙前"能命中的目标数
    int ally = 0;   // 路径扫过的队友数（穿透机制下有友伤，择优时尽量避开）
    bool clear = true;  // 全程未碰障碍实体（false = 要靠炸障碍开路，最后手段）
    double len = 1e18;
};

bool better_plan(const Plan& a, const Plan& b) {
    if (!b.valid) return true;
    if (!a.valid) return false;
    if (a.reach != b.reach) return a.reach > b.reach;
    if (a.ally != b.ally) return a.ally < b.ally;
    return a.len < b.len;
}

// 多杀骨架类型：节点式（多项式/阶梯/折线链）按插值点构造；
// 两目标的参数化风格（折点/波浪/抛物波浪/螺线）由 try_pair_styles 构造
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
        // 自变量过近会让插值病态（曲线剧烈震荡），跳过这种组合
        bool too_close = false;
        for (const auto& prev : pts) {
            if (std::abs(t - prev.first) < 2.0) too_close = true;
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
    plan.reach = reachable_targets(sampled.path, abs_targets, world);
    plan.clear = (first_blocking_obstacle(sampled.path, world) < 0);
    int enemy_hits = 0;
    scan_soldier_hits(sampled.path, world, self, world.config().world.player_half_size, enemy_hits,
                      plan.ally);
    plan.len = sampled.path.length();
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
            // 折线链走 ai_curves 的生成器（同时供单测直接验证过点）
            std::vector<Vec2> rel;
            rel.reserve(pts.size() - 1);
            for (size_t i = 1; i < pts.size(); ++i) {
                rel.push_back(horizontal ? Vec2(pts[i].first, pts[i].second)
                                         : Vec2(pts[i].second, pts[i].first));
            }
            req = make_kink_chain(horizontal, rel);
            break;
        }
    }
    return evaluate_req(std::move(req), world, origin, abs_targets, self);
}

// 两目标的参数化骨架：折点扫位（abs 折线）/ 波浪扫频率 / 抛物波浪扫正负 / 螺线。
// 每个变体都精确过两点，评估后把可行的全部塞进 out 参与最终择优——注意不要在这里
// 只保留"子集最优"：折线（连点直线）的长度天然最短，会把慢曲线全压死、
// 最终池里就只剩它一种形状了
void try_pair_styles(const std::vector<std::pair<double, double>>& pts, bool horizontal,
                     const World& world, const Vec2& origin,
                     const std::vector<Vec2>& abs_targets, const Player& self,
                     std::vector<Plan>& out) {
    if (pts.size() != 3) return;  // 专属"两目标"：原点 + 2 个点
    const double t1 = pts[1].first, v1 = pts[1].second;
    const double t2 = pts[2].first, v2 = pts[2].second;
    const double dx = horizontal ? t1 : v1;
    const double dy = horizontal ? v1 : t1;
    const double ox = horizontal ? t2 : v2;
    const double oy = horizontal ? v2 : t2;
    CurveRequest req;
    auto try_req = [&]() {
        Plan q = evaluate_req(req, world, origin, abs_targets, self);
        if (q.valid) out.push_back(q);
    };
    // 折点位置：起点与第一目标之间一次、两目标之间两次
    for (double p : {t1 * 0.5, t1 + (t2 - t1) / 3.0, t1 + (t2 - t1) * 2.0 / 3.0}) {
        if (make_kink_bend(horizontal, dx, dy, ox, oy, p, req)) try_req();
    }
    // 波浪频率：两点跨度内 0.5 / 0.75 / 1.5 个波（不同相位余量解出不同幅度）
    for (double waves : {0.5, 0.75, 1.5}) {
        const double w = 2.0 * std::numbers::pi * waves / std::abs(t2 - t1);
        if (make_wave_through(horizontal, dx, dy, ox, oy, w, req)) try_req();
    }
    // 抛物+正弦：正弦幅值按目标高度取，正负两组（抬起/压下抛物线）
    const double amp = std::max(3.0, 0.4 * std::max(std::abs(v1), std::abs(v2)));
    for (double c : {amp, -amp}) {
        if (make_poly_wave_through(horizontal, dx, dy, ox, oy, c, req)) try_req();
    }
    // 螺线（极坐标；几何不合适时生成器自己拒绝）
    if (make_spiral_through(dx, dy, ox, oy, req)) try_req();
}

// 三目标专属骨架：三点版抛物+正弦合并（三点全中，形状与折线/阶梯完全不同）
void try_triple_styles(const std::vector<std::pair<double, double>>& pts, bool horizontal,
                       const World& world, const Vec2& origin,
                       const std::vector<Vec2>& abs_targets, const Player& self,
                       std::vector<Plan>& out) {
    if (pts.size() != 4) return;  // 专属"三目标"：原点 + 3 个点
    auto rel_of = [&](size_t i) {
        const double t = pts[i].first, v = pts[i].second;
        return horizontal ? Vec2(t, v) : Vec2(v, t);
    };
    const double span = std::abs(pts[3].first - pts[1].first);
    if (span < 1e-9) return;
    CurveRequest req;
    // 扫三个频率：频率不同时对三点约束的"余量"利用不同，解出的形状瘦瘦胖胖不一样
    for (double waves : {0.5, 0.75, 1.5}) {
        const double w = 2.0 * std::numbers::pi * waves / span;
        if (make_poly_wave3_through(horizontal, rel_of(1), rel_of(2), rel_of(3), w, req)) {
            Plan q = evaluate_req(req, world, origin, abs_targets, self);
            if (q.valid) out.push_back(q);
        }
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

// 困难 AI 的规划主流程：枚举目标子集 → 各骨架构造/插值 → 评估 → 必要时加绕行点修正
Plan plan_hard(const World& world, const Player& self, const std::vector<TargetRef>& enemies,
               const AiProfile& prof, Rng& rng) {
    const Vec2 self_pos = self.current().pos;
    // 带瞄准误差的目标相对坐标
    std::vector<Vec2> rels;
    for (const auto& e : enemies) {
        Vec2 aim = e.pos();
        const double dist = distance(self_pos, aim);
        aim.x() += rng.range(-1.0, 1.0) * dist * prof.pos_error;
        aim.y() += rng.range(-1.0, 1.0) * dist * prof.pos_error;
        rels.push_back(aim - self_pos);
    }
    std::vector<Vec2> abs_targets;
    for (const auto& r : rels) abs_targets.push_back(self_pos + r);

    // 主导方向：看目标在哪个轴上更分散
    double min_x = 1e18, max_x = -1e18, min_y = 1e18, max_y = -1e18;
    for (const auto& r : rels) {
        min_x = std::min(min_x, r.x());
        max_x = std::max(max_x, r.x());
        min_y = std::min(min_y, r.y());
        max_y = std::max(max_y, r.y());
    }
    const bool horizontal = (max_x - min_x) >= (max_y - min_y);

    // 收集所有可行计划（每个子集先决出最优骨架，最后统一择优）
    std::vector<Plan> plans;
    std::vector<Plan> singles;  // 单目标子集的计划：多杀限流触发时退而用它们
    for (int side = 0; side < 2; ++side) {
        const FireDir dir = (side == 0) ? FireDir::Positive : FireDir::Negative;
        std::vector<Vec2> group;
        for (const auto& r : rels) {
            const double t = horizontal ? r.x() : r.y();
            if ((dir == FireDir::Positive && t > 0.0) || (dir == FireDir::Negative && t < 0.0)) {
                group.push_back(r);
            }
        }
        if (group.empty()) continue;
        // 目标太多时只规划最近的若干个：子集枚举是 O(n²)，满员（每队 20 个士兵）时
        // 单次决策要编译几百条表达式、最坏可到二三百毫秒，会把服务器 tick 卡住；
        // 远处目标本来就难打中，截断对战术几乎无损（group 里是相对坐标，模长即距离）
        constexpr size_t kMaxPlanTargets = 6;
        if (group.size() > kMaxPlanTargets) {
            std::partial_sort(group.begin(), group.begin() + kMaxPlanTargets, group.end(),
                              [](const Vec2& a, const Vec2& b) {
                                  return a.squaredNorm() < b.squaredNorm();
                              });
            group.resize(kMaxPlanTargets);
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
            if (sub.size() == 2) {
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
            if (p.valid && (p.reach < static_cast<int>(sub.size()) || !p.clear)) {
                // 迭代绕行（两轮封顶）：每轮找当前曲线的第一个挡路障碍、借它的边缘加一个
                // 过渡点重规划；绕开一个又撞下一个时把新点累积起来再试。折线链与多项式
                // 都能过一串点——"找不到干净方程才炸障碍"的兜底要靠它尽量兜住
                std::vector<Vec2> route = sub;
                for (int round = 0; round < 2; ++round) {
                    CurveResult sampled = sample_curve(p.req, self_pos, world.config());
                    const int ob_idx =
                        sampled.ok() ? first_blocking_obstacle(sampled.path, world) : -1;
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
                    if (win->plan.clear && win->plan.reach >= static_cast<int>(sub.size())) break;
                    if (round == 1) break;  // 两轮封顶：再绕下去评估成本翻倍，收益很小
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
        // 不犯错：能命中且全程不碰障碍的方案优先——旧口径只看命中数与长度，直线
        // （穿过障碍炸开路）比绕行曲线短，总赢，看起来就是"AI 一直在炸障碍"。
        // 一条干净的都没有（数学上绕不开）才退到"炸障碍开路"的方案
        bool any_clear = false;
        for (const auto& pl : pool) {
            if (pl.clear) any_clear = true;
        }
        std::vector<const Plan*> usable;
        for (const auto& pl : pool) {
            if (!any_clear || pl.clear) usable.push_back(&pl);
        }
        int max_reach = -1;
        int min_ally = std::numeric_limits<int>::max();
        for (const auto* pl : usable) max_reach = std::max(max_reach, pl->reach);
        for (const auto* pl : usable) {
            if (pl->reach == max_reach) min_ally = std::min(min_ally, pl->ally);
        }
        double min_len = 1e18;
        for (const auto* pl : usable) {
            if (pl->reach == max_reach && pl->ally == min_ally) min_len = std::min(min_len, pl->len);
        }
        std::vector<const Plan*> cands;
        for (const auto* pl : usable) {
            if (pl->reach == max_reach && pl->ally == min_ally && pl->len <= min_len * 1.5) {
                cands.push_back(pl);
            }
        }
        return *cands[static_cast<size_t>(rng.range_int(0, static_cast<int>(cands.size()) - 1))];
    };
    const Plan best = pick(plans);
    const Plan best_single = singles.empty() ? Plan{} : pick(singles);
    // 多杀限流（见 AiProfile 注释）：刻意扫多人的线按概率保留，否则退到最佳单杀线；
    // best_single 无效（单目标全被障碍挡死）时保底仍用多杀线，不至于这回合不会打人
    if (best.valid && best.reach >= 2 && !multi_kill_kept(best.reach, prof, rng) &&
        best_single.valid) {
        return best_single;
    }
    return best;
}

}  // namespace

bool multi_kill_kept(int reach, const AiProfile& prof, Rng& rng) {
    // 单目标不掷骰：限流只拦"刻意多杀"，保证 rng 消费时机只由局面决定的过程简单
    if (reach <= 1) return true;
    return rng.chance(reach >= 3 ? prof.multi_keep3 : prof.multi_keep2);
}

AiProfile ai_profile(int difficulty) {
    switch (difficulty) {
        case 0:  // 简单：误差大，只玩直线与抛物线
            return AiProfile{0.15, 0.15, 1.0, 2.5};
        case 2:  // 困难：误差极小，全模式可用，还做一发多穿的规划（带多杀限流）
            return AiProfile{0.005, 0.008, 2.5, 6.0};
        case 3:  // 大神：比困难更准，多杀不限流——能一炮扫完就一炮扫完
            return AiProfile{0.002, 0.003, 2.5, 6.0, 1.0, 1.0};
        default:  // 普通：误差小，多模式可用
            return AiProfile{0.05, 0.05, 2.0, 4.5};
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
        plan_hard_result = plan_hard(world, *self, enemies, prof, rng);
        if (plan_hard_result.valid && plan_hard_result.reach >= 1 && plan_hard_result.clear) {
            out.request = std::move(plan_hard_result.req);
            return out;
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
    aim.x() += rng.range(-1.0, 1.0) * dist * prof.pos_error;
    aim.y() += rng.range(-1.0, 1.0) * dist * prof.pos_error;
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
    //（结构不变、参数微调——"就差一点，调调系数再来"）。评估照常，不行会被筛掉
    if (prev != nullptr && rng.chance(0.6)) {
        CurveRequest mutated = *prev;
        mutated.expr_a = jitter_numbers(mutated.expr_a, rng);
        if (!mutated.expr_b.empty()) mutated.expr_b = jitter_numbers(mutated.expr_b, rng);
        pool.push_back(evaluate_candidate(mutated, self_pos, aim, world));
    }
    // 扫射候选："瞄点 + 另一个敌人"的插值抛物线（纵横两个单值方向各试一条）。
    // 穿透机制下一发依次扫过两人、收益成倍；配对取离目标最近的几个——
    // 太远的配对形状会夸张到被评估筛掉，白花采样成本。
    // 多杀限流（见 AiProfile 注释）：困难档按概率才生成这类"刻意两杀"的配对；
    // 简单/普通档不掷骰，维持原有"形状随机"的风格
    if (difficulty < 2 || multi_kill_kept(2, prof, rng)) {
        std::vector<const TargetRef*> others;
        for (const auto& e : enemies) {
            if (e.player == target->player && e.soldier == target->soldier) continue;
            others.push_back(&e);
        }
        std::sort(others.begin(), others.end(), [&](const TargetRef* a, const TargetRef* b) {
            return distance(aim, a->pos()) < distance(aim, b->pos());
        });
        constexpr size_t kMaxPairs = 5;
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
    // 沿直线找第一个挡路障碍，借它的边缘（外扩 4 单位）当过渡点，插一条
    // "先绕障碍、再落瞄准点"的抛物线。过渡点高度贴着障碍尺寸算——固定拱高
    // 的小抛物线绕不过高障碍，这里能自动适配
    if (difficulty >= 1) {
        bool have_clean = false;
        for (const auto& c : pool) {
            if (c.reaches && c.clear) {
                have_clean = true;
                break;
            }
        }
        if (!have_clean) {
            const CurveResult direct =
                sample_curve(make_parabola(horizontal, dx, dy, 0.0), self_pos, world.config());
            const int ob_idx = direct.ok() ? first_blocking_obstacle(direct.path, world) : -1;
            if (ob_idx >= 0) {
                const Obstacle& ob = world.obstacles()[static_cast<size_t>(ob_idx)];
                const Vec2 rel = ob.center - self_pos;
                const double t_ob = horizontal ? rel.x() : rel.y();
                const double hv = horizontal ? rel.y() : rel.x();
                const double ext =
                    (ob.is_circle ? ob.radius : (horizontal ? ob.half_h : ob.half_w)) + 4.0;
                const double u2 = horizontal ? dx : dy;  // 瞄准点自变量
                // 单值曲线只朝一侧延伸：障碍与瞄准点同侧、离原点与彼此都够远
                if (std::abs(t_ob) > 0.5 && (t_ob > 0.0) == (u2 > 0.0) &&
                    std::abs(t_ob - u2) > 1.5) {
                    // 按障碍高度精确起拱（顶/底两侧各一条）：比"过绕行点插值"稳——
                    // 障碍离原点近时过点插值的曲线会贴着障碍爬升、还是免不了撞
                    pool.push_back(evaluate_candidate(make_obstacle_arc(horizontal, dx, dy, t_ob,
                                                                        hv + ext),
                                                      self_pos, aim, world));
                    pool.push_back(evaluate_candidate(make_obstacle_arc(horizontal, dx, dy, t_ob,
                                                                        hv - ext),
                                                      self_pos, aim, world));
                }
            }
        }
    }

    // 按难度分层用池子：
    //   简单——不筛选也不择优，随便挑一条能发射的（弱得像在瞎打）
    //   普通——只要打得到就行，形状随机（能命中，但轨迹五花八门）
    //   困难——每次都选最优：命中精度同一档里取最短路径（又准又快）
    auto pick_random = [&](const std::vector<const CurveCandidate*>& v) {
        const int idx = rng.range_int(0, static_cast<int>(v.size()) - 1);
        out.request = v[static_cast<size_t>(idx)]->req;
    };

    if (difficulty <= 0) {
        std::vector<const CurveCandidate*> any;
        for (const auto& c : pool) {
            if (c.sampled.ok()) any.push_back(&c);
        }
        if (!any.empty()) {
            pick_random(any);
            return out;
        }
    } else {
        std::vector<const CurveCandidate*> clear_ok, reachable;
        for (const auto& c : pool) {
            if (!c.reaches) continue;
            if (c.clear) clear_ok.push_back(&c);
            reachable.push_back(&c);
        }
        const std::vector<const CurveCandidate*>& usable = !clear_ok.empty() ? clear_ok : reachable;
        if (difficulty == 1 && !usable.empty()) {
            pick_random(usable);
            return out;
        }
        if (difficulty >= 2 && !usable.empty()) {
            // 择优次序：净扫中数（敌人 − 队友：穿透机制下一发多杀、收益成倍，代价是
            // 形状往往不瞄着单点）→ 命中精度（与最准的差不到半个命中半径算同一档）
            // → 路径最短（又准又快）
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
            double best_len = 1e18;
            for (const auto* c : swept) {
                if (c->aim_gap <= best_gap + 0.5) {
                    best_len = std::min(best_len, c->sampled.path.length());
                }
            }
            std::vector<const CurveCandidate*> finals;
            for (const auto* c : swept) {
                if (c->aim_gap <= best_gap + 0.5 && c->sampled.path.length() <= best_len * 1.05) {
                    finals.push_back(c);
                }
            }
            pick_random(finals);
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

}  // namespace fg
