#include "world.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "rng.h"

namespace fg {

namespace {
// 炮弹推进的子步长：碰撞判定本身是连续线段求交（见 segment_hits_obstacle），
// 子步只决定出界、炮口保护这些点判据的粒度——0.25 远小于任何目标与障碍的尺寸，
// 点判据不会漏
constexpr double kSubStep = 0.25;
// 炮口保护（借鉴参考实现）：发射者本人要等炮弹飞出足够弧长、且弹道起点已脱离
// 自己的碰撞方块，才可能被自己伤到。只看弧长，原地绕小圈的曲线瞬间自杀；
// 只看距离，飞出一点再折回的第二段会自杀
constexpr double kArmLength = 1.2;
// kOutGraceSec（出界宽限）见 world.h——AI 评估要与飞行同构地模拟它
// 炮弹全程飞行时间的硬上限（秒）：极小速度这类毒配置会把对局无限期锁死在飞行阶段，
// 超时一律判死兜底；正常炮弹远用不到（最长行程 300 弧长 ÷ 默认速度 12 ≈ 25 秒）
constexpr double kMaxFlightSec = 60.0;

// 点到轴对齐方块（中心 c、半边长 h）的最近距离
double dist_point_to_square(const Vec2& p, const Vec2& c, double h) {
    const double dx = std::max(std::abs(p.x() - c.x()) - h, 0.0);
    const double dy = std::max(std::abs(p.y() - c.y()) - h, 0.0);
    return std::hypot(dx, dy);
}

// 命中去重的 key：player_id 与士兵序号拼成一个整数（士兵序号最多 4，*16 安全）
long soldier_key(int player_id, int soldier_index) {
    return static_cast<long>(player_id) * 16 + static_cast<long>(soldier_index);
}

// 线段 a→b 落在圆 c（半径 r）内的参数区间（可能超出 [0,1]；无交或相切返回空）
std::optional<std::pair<double, double>> segment_circle_interval(const Vec2& a, const Vec2& b,
                                                                 const Vec2& c, double r) {
    const Vec2 d = b - a;
    const double aa = d.squaredNorm();
    const Vec2 f = a - c;
    if (aa < 1e-18) {  // 退化段（起讫重合）：等价于点判定
        return f.squaredNorm() <= r * r ? std::optional{std::pair{0.0, 1.0}} : std::nullopt;
    }
    // 二次方程 |a + t·d − c|² ≤ r² 的解区间
    const double bb = 2.0 * f.dot(d);
    const double cc = f.squaredNorm() - r * r;
    const double disc = bb * bb - 4.0 * aa * cc;
    if (disc <= 0.0) return std::nullopt;
    const double sq = std::sqrt(disc);
    return std::pair{(-bb - sq) / (2.0 * aa), (-bb + sq) / (2.0 * aa)};
}
}  // namespace

bool obstacle_blocks(const Obstacle& ob, const Vec2& p, double crater_radius) {
    const bool inside =
        ob.is_circle
            ? (distance(p, ob.center) <= ob.radius)
            : (std::abs(p.x() - ob.center.x()) <= ob.half_w &&
               std::abs(p.y() - ob.center.y()) <= ob.half_h);
    if (!inside) return false;
    for (const auto& c : ob.craters) {
        if (distance(p, c) <= crater_radius) return false;
    }
    return true;
}

bool obstacle_overlaps_circle(const Obstacle& ob, const Vec2& center, double radius) {
    if (ob.is_circle) return distance(ob.center, center) <= ob.radius + radius;
    // 矩形：把圆心钳到盒内取最近点，最近距离不超过半径即有交集
    const double nx = std::clamp(center.x(), ob.center.x() - ob.half_w, ob.center.x() + ob.half_w);
    const double ny = std::clamp(center.y(), ob.center.y() - ob.half_h, ob.center.y() + ob.half_h);
    return distance(Vec2(nx, ny), center) <= radius;
}

std::optional<double> segment_hits_obstacle(const Obstacle& ob, const Vec2& a, const Vec2& b,
                                            double crater_radius, double projectile_radius) {
    // 非有限端点按"不相交"拒绝：NaN 经 slab 除法会一路穿过比较（与数值比较恒 false）
    // 伪造出 t=0 的假接触。正常路径不会出现非有限点（sample_curve 的 in_bounds 用
    // >=/<= 天然拒绝 NaN），这里是防采样层未来漏洞的兜底
    if (!std::isfinite(a.x()) || !std::isfinite(a.y()) || !std::isfinite(b.x()) ||
        !std::isfinite(b.y())) {
        return std::nullopt;
    }
    // 按炮弹圆的 Minkowski 膨胀：判定形状比实体大一圈 projectile_radius
    const double ex = (ob.is_circle ? ob.radius : ob.half_w) + projectile_radius;
    const double ey = (ob.is_circle ? ob.radius : ob.half_h) + projectile_radius;
    // 包围盒预筛：段与障碍 AABB 无重叠直接拒——AI 评估每次要过几千个"子段×障碍"对，
    // 精确求交只应发生在近旁少数对上
    const double min_x = std::min(a.x(), b.x());
    const double max_x = std::max(a.x(), b.x());
    const double min_y = std::min(a.y(), b.y());
    const double max_y = std::max(a.y(), b.y());
    if (min_x > ob.center.x() + ex) return std::nullopt;
    if (max_x < ob.center.x() - ex) return std::nullopt;
    if (min_y > ob.center.y() + ey) return std::nullopt;
    if (max_y < ob.center.y() - ey) return std::nullopt;

    std::optional<std::pair<double, double>> cover;
    if (ob.is_circle) {
        cover = segment_circle_interval(a, b, ob.center, ob.radius + projectile_radius);
    } else {
        // 矩形：两个轴的 slab 求交（膨胀半宽，与 obstacle_blocks 一样含边界）
        const Vec2 d = b - a;
        double lo = 0.0, hi = 1.0;
        bool ok = true;
        for (int axis = 0; axis < 2 && ok; ++axis) {
            const double p0 = a[axis];
            const double half = (axis == 0) ? ex : ey;  // 复用膨胀半宽（矩形时与前式同值）
            const double mn = ob.center[axis] - half;
            const double mx = ob.center[axis] + half;
            if (std::abs(d[axis]) < 1e-18) {
                ok = p0 >= mn && p0 <= mx;  // 与该轴平行：留在带内才可能相交
                continue;
            }
            double t0 = (mn - p0) / d[axis];
            double t1 = (mx - p0) / d[axis];
            if (t0 > t1) std::swap(t0, t1);
            lo = std::max(lo, t0);
            hi = std::min(hi, t1);
            ok = lo <= hi;
        }
        if (ok) cover = std::pair{lo, hi};
    }
    if (!cover) return std::nullopt;
    auto [lo, hi] = *cover;
    lo = std::max(lo, 0.0);
    hi = std::min(hi, 1.0);
    if (lo > hi) return std::nullopt;

    // 坑豁免：从入口 lo 起，落在坑区间内就跳到该坑右端，重复到不在任何坑内。
    // 每次跳跃都严格向右跨过至少一个坑的右端（t 单调增、坑数有限），循环很快收敛，
    // 不需要维护区间集合——与 obstacle_blocks 的坑豁免在测度意义下一致
    double t = lo;
    if (ob.craters.empty()) return t;  // 无坑可豁免（原循环空转一轮也会落在同一返回）
    for (size_t guard = 0; guard <= ob.craters.size(); ++guard) {
        double next = -1.0;
        for (const Vec2& cc : ob.craters) {
            // 线段包围盒与坑圆不相交则跳过（被拒时下面的精确求交本来也无解）
            if (max_x < cc.x() - crater_radius || min_x > cc.x() + crater_radius ||
                max_y < cc.y() - crater_radius || min_y > cc.y() + crater_radius) {
                continue;
            }
            const auto hole = segment_circle_interval(a, b, cc, crater_radius);
            if (hole && hole->first <= t && t < hole->second) {
                next = std::max(next, hole->second);  // 多坑叠在一处时跳到并集右端
            }
        }
        if (next < 0.0) break;
        t = next;
        if (t > hi) return std::nullopt;  // 剩余区间全被坑覆盖
    }
    return t;
}

std::optional<double> segment_hits_square(const Vec2& a, const Vec2& b, const Vec2& center,
                                          double half, double projectile_radius) {
    // 非有限端点按"不相交"拒绝（理由见 segment_hits_obstacle 的同款兜底）
    if (!std::isfinite(a.x()) || !std::isfinite(a.y()) || !std::isfinite(b.x()) ||
        !std::isfinite(b.y())) {
        return std::nullopt;
    }
    // 士兵命中框：两个轴的 slab 求交（判定半宽含炮弹半径，方角处圆角膨胀被
    // 近似成直角）；lo==hi（擦着边角）也算命中，与方块命中的"含边界"语义一致。
    // 士兵没有坑豁免，无需再切区间
    const double h = half + projectile_radius;
    const Vec2 d = b - a;
    double lo = 0.0, hi = 1.0;
    for (int axis = 0; axis < 2; ++axis) {
        const double p0 = a[axis];
        const double mn = center[axis] - h;
        const double mx = center[axis] + h;
        if (std::abs(d[axis]) < 1e-18) {
            if (p0 < mn || p0 > mx) return std::nullopt;
            continue;
        }
        double t0 = (mn - p0) / d[axis];
        double t1 = (mx - p0) / d[axis];
        if (t0 > t1) std::swap(t0, t1);
        lo = std::max(lo, t0);
        hi = std::min(hi, t1);
        if (lo > hi) return std::nullopt;
    }
    return lo;  // lo 已钳在 [0,1]（初值 0 起、只与交集取大）
}

void World::start(const GameConfig& cfg, const std::vector<PlayerSpec>& specs) {
    cfg_ = cfg;
    players_.clear();
    obstacles_.clear();
    turn_order_.clear();
    events_.clear();
    projectile_.reset();
    winner_ = -1;
    next_projectile_id_ = 1;
    turn_delay_left_ = 0.0;

    Rng rng(cfg_.match.seed);

    // 遮挡物：随机圆或矩形，大小、位置随机（种子决定，可复现）
    for (int i = 0; i < cfg_.match.obstacle_count; ++i) {
        Obstacle ob;
        ob.is_circle = rng.chance(0.5);
        if (ob.is_circle) {
            ob.radius = rng.range(1.2, 3.6);
            ob.center = Vec2(rng.range(cfg_.world.map_x_min + ob.radius, cfg_.world.map_x_max - ob.radius),
                             rng.range(cfg_.world.map_y_min + ob.radius, cfg_.world.map_y_max - ob.radius));
        } else {
            ob.half_w = rng.range(0.8, 2.8);
            ob.half_h = rng.range(0.8, 2.8);
            ob.center = Vec2(rng.range(cfg_.world.map_x_min + ob.half_w, cfg_.world.map_x_max - ob.half_w),
                             rng.range(cfg_.world.map_y_min + ob.half_h, cfg_.world.map_y_max - ob.half_h));
        }
        obstacles_.push_back(ob);
    }

    // 玩家与他们的士兵：每人 soldiers_per_player 个，各自随机出生
    // （不与遮挡物重叠、互相不重叠）
    const int per = std::max(1, cfg_.room.soldiers_per_player);
    std::vector<int> team_a, team_b;
    // 已放置士兵的位置：同一玩家的士兵之间也要互斥（他放进 players_ 之前
    // 多士兵可以互相重叠出生）
    std::vector<Vec2> placed;
    int auto_id = 0;
    for (const auto& spec : specs) {
        Player p;
        p.id = (spec.id >= 0) ? spec.id : auto_id;
        auto_id = std::max(auto_id, p.id + 1);
        p.team = spec.team;
        p.name = spec.name;
        p.is_ai = spec.is_ai;
        p.ai_difficulty = spec.ai_difficulty;
        for (int k = 0; k < per; ++k) {
            Soldier s;
            s.max_hp = s.hp = std::max(1, cfg_.match.hp);
            s.pos = find_free_spot(rng, spec.team, placed);
            placed.push_back(s.pos);
            p.soldiers.push_back(s);
        }
        players_.push_back(p);
        (spec.team == Team::A ? team_a : team_b).push_back(p.id);
    }
    for (size_t i = 0; i < team_a.size(); ++i) find_player(team_a[i])->seat = static_cast<int>(i) + 1;
    for (size_t i = 0; i < team_b.size(); ++i) find_player(team_b[i])->seat = static_cast<int>(i) + 1;

    // 轮转顺序：两队交替（两队人数不等时跳过缺失的位置）。
    // 先手队默认由种子随机决定（同种子仍可复现），也可由配置显式指定。
    // 注意：这里的取随机数放在地图生成之后，插入随机流不会改动地图本身。
    const int first = (cfg_.match.first_team == 0 || cfg_.match.first_team == 1) ? cfg_.match.first_team
                                                                    : rng.range_int(0, 1);
    const std::vector<int>& first_team = (first == 0) ? team_a : team_b;
    const std::vector<int>& second_team = (first == 0) ? team_b : team_a;
    const size_t rounds = std::max(first_team.size(), second_team.size());
    for (size_t i = 0; i < rounds; ++i) {
        if (i < first_team.size()) turn_order_.push_back(first_team[i]);
        if (i < second_team.size()) turn_order_.push_back(second_team[i]);
    }

    if (turn_order_.empty()) {
        phase_ = Phase::GameOver;
        winner_ = 2;
        events_.push_back(Event{.type = EventType::GameOver, .winner = 2});
        return;
    }

    turn_index_ = 0;
    phase_ = Phase::Input;
    phase_time_ = cfg_.match.input_seconds;
    events_.push_back(Event{.type = EventType::TurnBegin,
                            .player_id = turn_order_[0],
                            .soldier = find_player(turn_order_[0])->current_soldier});
}

void World::update(double dt) {
    switch (phase_) {
        case Phase::Input:
            phase_time_ -= dt;
            if (phase_time_ <= 0.0) {
                events_.push_back(Event{.type = EventType::TurnSkipped,
                                        .player_id = current_player_id(),
                                        .soldier = current_soldier_index()});
                advance_turn();
            }
            break;
        case Phase::Flight:
            if (projectile_) {
                step_flight(dt);
            } else {
                // 炮弹已消失：展示停顿倒计时，归零后才轮到下一位（turn_delay = 0 时
                // step_flight 结束处就已直接推进，这里通常不会停留）
                turn_delay_left_ -= dt;
                if (turn_delay_left_ <= 0.0) advance_turn();
            }
            break;
        case Phase::GameOver:
            break;
    }
}

int World::current_player_id() const {
    if (phase_ != Phase::Input || turn_order_.empty()) return -1;
    return turn_order_[static_cast<size_t>(turn_index_)];
}

int World::current_soldier_index() const {
    if (phase_ != Phase::Input || turn_order_.empty()) return -1;
    const Player* pl = find_player(turn_order_[static_cast<size_t>(turn_index_)]);
    return pl ? pl->current_soldier : -1;
}

void World::peek_next(int& out_player_id, int& out_soldier) const {
    out_player_id = -1;
    out_soldier = -1;
    if (phase_ == Phase::GameOver || turn_order_.empty()) return;
    const int n = static_cast<int>(turn_order_.size());
    for (int i = 1; i <= n; ++i) {
        const int idx = (turn_index_ + i) % n;
        const Player* pl = find_player(turn_order_[static_cast<size_t>(idx)]);
        if (!pl || !pl->any_alive()) continue;
        int cs = pl->current_soldier;
        // 输入阶段绕回当前玩家本人（场上只剩他一人）：他出手后游标才会轮换，这里模拟
        const bool simulate_after_shot = (idx == turn_index_) && (phase_ == Phase::Input);
        if (simulate_after_shot || !pl->soldiers[static_cast<size_t>(cs)].alive) {
            for (size_t k = 1; k <= pl->soldiers.size(); ++k) {
                const size_t m = (static_cast<size_t>(cs) + k) % pl->soldiers.size();
                if (pl->soldiers[m].alive) {
                    cs = static_cast<int>(m);
                    break;
                }
            }
        }
        out_player_id = pl->id;
        out_soldier = cs;
        return;
    }
}

Error World::submit(int player_id, const CurveRequest& request) {
    if (phase_ != Phase::Input) return {"not_input_phase", "当前不是输入阶段"};
    if (player_id != current_player_id()) return {"not_your_turn", "还没轮到你"};
    Player* pl = find_player(player_id);
    if (!pl || !pl->any_alive()) return {"player_invalid", "玩家不存在或已阵亡"};

    const int si = pl->current_soldier;
    CurveResult res = sample_curve(request, pl->soldiers[static_cast<size_t>(si)].pos, cfg_);
    if (!res.ok()) return res.error;

    Projectile p;
    p.id = next_projectile_id_++;
    p.owner_id = player_id;
    p.owner_soldier = si;
    p.request = request;
    p.path = std::move(res.path);
    p.last_pos = p.path.points.front();  // 段判定的起点从炮口开始
    p.speed = cfg_.world.projectile_speed;
    // 发射者不在这里预标记：炮口保护由 step_flight 的 arm 判定负责，
    // 飞出足够远后曲线绕回来仍可能伤到自己（借鉴参考实现的机制）

    projectile_ = std::move(p);
    phase_ = Phase::Flight;
    events_.push_back(Event{.type = EventType::Shot, .player_id = player_id, .soldier = si});
    // 出手后轮换：下一次轮到本玩家时换下一个存活士兵出手（单士兵局原地不动）
    pl->advance_soldier();
    return {};
}

std::vector<Event> World::take_events() {
    std::vector<Event> out = std::move(events_);
    events_.clear();
    return out;
}

Player* World::find_player(int id) {
    for (auto& p : players_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const Player* World::find_player(int id) const {
    for (const auto& p : players_) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

bool World::in_map(const Vec2& p) const {
    return p.x() >= cfg_.world.map_x_min && p.x() <= cfg_.world.map_x_max && p.y() >= cfg_.world.map_y_min &&
           p.y() <= cfg_.world.map_y_max;
}

// 出生点合法性：士兵方块与遮挡物保持余量、与已放置士兵方块不重叠。
// placed 由调用方累积（含"当前玩家已放下、尚未进 players_"的士兵）
bool World::spot_clear(const Vec2& p, const std::vector<Vec2>& placed) const {
    const double h = cfg_.world.player_half_size;
    const double margin = 1.0;
    for (const auto& ob : obstacles_) {
        if (ob.is_circle) {
            if (dist_point_to_square(ob.center, p, h) <= ob.radius + margin) return false;
        } else {
            if (std::abs(p.x() - ob.center.x()) <= h + ob.half_w + margin &&
                std::abs(p.y() - ob.center.y()) <= h + ob.half_h + margin) {
                return false;
            }
        }
    }
    for (const auto& q : placed) {
        if (std::abs(q.x() - p.x()) <= h * 2.0 + 3.0 &&
            std::abs(q.y() - p.y()) <= h * 2.0 + 3.0) {
            return false;
        }
    }
    return true;
}

Vec2 World::find_free_spot(Rng& rng, Team team, const std::vector<Vec2>& placed) {
    const double pad = cfg_.world.player_half_size + 1.0;
    // 分边出生：A 队占左半场、B 队占右半场（对齐原版对阵格局，两队不互相穿插）
    const double mid = (cfg_.world.map_x_min + cfg_.world.map_x_max) / 2.0;
    const double x_lo = (team == Team::A) ? (cfg_.world.map_x_min + pad) : mid;
    const double x_hi = (team == Team::A) ? mid : (cfg_.world.map_x_max - pad);
    for (int attempt = 0; attempt < 500; ++attempt) {
        const Vec2 p(rng.range(x_lo, x_hi), rng.range(cfg_.world.map_y_min + pad, cfg_.world.map_y_max - pad));
        if (spot_clear(p, placed)) return p;
    }
    // 兜底：随机采样失败（遮挡物极端密集）时改用确定性网格扫描，找离半场中心最近的
    // 一个既不撞遮挡物、也不与已放置士兵重叠的点。扫描不消耗随机数：走不走兜底在同
    // 种子下是确定的，因此后续随机流与不扫描时完全一致
    const double y_lo = cfg_.world.map_y_min + pad, y_hi = cfg_.world.map_y_max - pad;
    const double cx = (x_lo + x_hi) / 2.0, cy = (y_lo + y_hi) / 2.0;
    Vec2 best(cx, cy);
    double best_d2 = -1.0;
    for (double x = x_lo; x <= x_hi; x += 2.0) {
        for (double y = y_lo; y <= y_hi; y += 2.0) {
            const Vec2 p(x, y);
            if (!spot_clear(p, placed)) continue;
            const double d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
            if (best_d2 < 0.0 || d2 < best_d2) {
                best_d2 = d2;
                best = p;
            }
        }
    }
    // 连网格都找不到：校验层（check_obstacle_density 的密度上限）已经把正常途径的配置
    // 限制在可承载范围，走到这里只剩"遮挡物分布极端不均"的种子——退回中心不再挣扎
    return best;
}

void World::advance_turn() {
    if (phase_ == Phase::GameOver) return;
    const int n = static_cast<int>(turn_order_.size());
    for (int i = 1; i <= n; ++i) {
        const int idx = (turn_index_ + i) % n;
        Player* pl = find_player(turn_order_[static_cast<size_t>(idx)]);
        if (pl && pl->any_alive()) {
            // 轮到时游标若已阵亡（等待期间被打死）：先换到存活的士兵
            if (!pl->current().alive) pl->advance_soldier();
            turn_index_ = idx;
            phase_ = Phase::Input;
            phase_time_ = cfg_.match.input_seconds;
            events_.push_back(Event{.type = EventType::TurnBegin,
                                    .player_id = pl->id,
                                    .soldier = pl->current_soldier});
            return;
        }
    }
    // 没有活着的玩家/士兵（胜负判定本应先拦住，这里兜底避免死循环）
    phase_ = Phase::GameOver;
    winner_ = 2;
    events_.push_back(Event{.type = EventType::GameOver, .winner = 2});
}

void World::step_flight(double dt) {
    if (!projectile_) return;  // 防御：炮弹消失后的回合推进由 update 的停顿分支驱动
    Projectile& p = *projectile_;
    // 速度非正（错误配置）：循环永远推不动炮弹，直接判死走统一收尾，避免锁死在飞行阶段
    if (!(p.speed > 0.0)) p.dead = true;
    // 飞行时长硬上限：极小正速度（毒配置）同样永远飞不完，超时判死兜底
    p.flight_time += dt;
    if (p.flight_time > kMaxFlightSec) p.dead = true;
    double remaining = p.speed * dt;
    // 子步的起点：碰撞以段 [prev, pos] 为单位做连续求交。起点取自炮弹上一步的
    // 位置（last_pos 跨帧持久），不能在本函数里初始化——step_flight 每帧调用，
    // 局部初值会让每帧的判定段从炮口重新拉起
    Vec2 prev = p.last_pos;

    // 剩余量判据用 > 0：推进量再小也让它照常累积（被阈值吞掉同样会永远飞不到头）
    while (remaining > 0.0 && !p.dead) {
        const double step = std::min(remaining, kSubStep);
        remaining -= step;
        p.travel += step;

        // 先判碰撞、再判走到尽头：终点那一格也要参与命中判定
        // （越过终点时 at() 返回最后一个采样点）
        const bool past_end = (p.travel >= p.path.length());
        const Vec2 pos = p.path.at(p.travel);

        // 炮口保护：距炮口够远且已飞出足够弧长才解除（一旦解除永久解除）。距离
        // 判据看本子步的起点：段判定下"这段还碰不碰得到发射者"由起点决定，起点
        // 脱离自己的膨胀方块（两轴任一越界即在外）后，只有"真的折回来撞上"才会
        // 命中——那本来就该算。用终点距离是不够的：终点在外时整段仍可能还在
        // 方块内（擦过角），会误伤发射者
        if (!p.armed) {
            const Vec2 muzzle = p.path.points.front();
            const double h = cfg_.world.player_half_size + cfg_.world.projectile_radius;
            const bool clear =
                std::abs(prev.x() - muzzle.x()) > h || std::abs(prev.y() - muzzle.y()) > h;
            if (p.travel >= kArmLength && clear) {
                p.armed = true;
            }
        }

        // 出界宽限：首次出界只记位置，超时才销毁；回到界内宽限重新计
        if (!in_map(pos)) {
            if (p.out_mark < 0.0) {
                p.out_mark = p.travel;
            } else if (p.travel - p.out_mark > p.speed * kOutGraceSec) {
                p.dead = true;
                break;
            }
        } else {
            p.out_mark = -1.0;
        }

        // 本子步的连续碰撞：a) 段 [prev, pos] 对所有障碍求交，取最小的撞墙参数
        // t_wall；b) 士兵命中（段与方块求交得 t_s）只在几何上先于撞墙时才结算。
        // 与旧的点判定相比：擦边不会漏检、接触点精确落在几何边界上（不再"先穿进
        // 障碍再被发现"），坑心与命中位置也都取在接触点
        double t_wall = 2.0;  // > 1 表示本段无撞墙
        for (const auto& ob : obstacles_) {
            if (const auto t = segment_hits_obstacle(ob, prev, pos, cfg_.world.crater_radius,
                                                     cfg_.world.projectile_radius)) {
                t_wall = std::min(t_wall, *t);
            }
        }
        // 命中士兵：扣血后穿透继续（同一炮弹对同一士兵只结算一次）
        for (auto& pl : players_) {
            for (size_t si = 0; si < pl.soldiers.size(); ++si) {
                Soldier& sd = pl.soldiers[si];
                if (!sd.alive) continue;
                // 炮口保护：保护解除前，发射者本人（这一手的士兵）不受自己的炮弹伤害
                if (pl.id == p.owner_id && static_cast<int>(si) == p.owner_soldier && !p.armed) {
                    continue;
                }
                const long key = soldier_key(pl.id, static_cast<int>(si));
                if (std::find(p.hit_soldiers.begin(), p.hit_soldiers.end(), key) !=
                    p.hit_soldiers.end()) {
                    continue;
                }
                const auto t_s = segment_hits_square(prev, pos, sd.pos,
                                                     cfg_.world.player_half_size,
                                                     cfg_.world.projectile_radius);
                if (!t_s || *t_s > t_wall) continue;  // 墙后的士兵打不到
                const Vec2 hit_pos = prev + (pos - prev) * (*t_s);
                p.hit_soldiers.push_back(key);
                sd.hp -= 1;
                events_.push_back(Event{.type = EventType::PlayerHit,
                                        .player_id = p.owner_id,
                                        .soldier = p.owner_soldier,
                                        .target_id = pl.id,
                                        .target_soldier = static_cast<int>(si),
                                        .pos = hit_pos,
                                        .hp_left = sd.hp});
                if (sd.hp <= 0) {
                    sd.alive = false;
                    events_.push_back(Event{.type = EventType::PlayerDied,
                                            .player_id = p.owner_id,
                                            .soldier = p.owner_soldier,
                                            .target_id = pl.id,
                                            .target_soldier = static_cast<int>(si),
                                            .pos = sd.pos});
                }
            }
        }

        // 命中遮挡物：爆炸把周围（坑圆覆盖到的）所有障碍都炸出坑并销毁炮弹——
        // 不只击中的那块，紧挨爆炸点的石头也被啃掉一角（坑内区域不再阻挡后续炮弹）
        if (t_wall <= 1.0) {
            const Vec2 hit_pos = prev + (pos - prev) * t_wall;
            for (size_t i = 0; i < obstacles_.size(); ++i) {
                if (!obstacle_overlaps_circle(obstacles_[i], hit_pos, cfg_.world.crater_radius)) {
                    continue;
                }
                obstacles_[i].craters.push_back(hit_pos);
                events_.push_back(Event{.type = EventType::CraterMade,
                                        .player_id = p.owner_id,
                                        .soldier = p.owner_soldier,
                                        .obstacle_index = static_cast<int>(i),
                                        .pos = hit_pos});
            }
            p.dead = true;
        }

        if (past_end) {
            p.dead = true;  // 轨迹走到尽头（终点位置的碰撞已判定完）
            break;
        }
        prev = p.last_pos = pos;  // 子步推进同时持久化起点（跨帧的段判定链）
    }

    if (p.dead) {
        projectile_.reset();
        if (!check_game_over()) {
            // 展示停顿：让玩家/观战者看清结果再轮到下一位（0 = 不停顿）
            turn_delay_left_ = cfg_.match.turn_delay;
            if (turn_delay_left_ <= 0.0) advance_turn();
        }
    }
}

bool World::check_game_over() {
    bool a_alive = false, b_alive = false;
    for (const auto& p : players_) {
        for (const auto& s : p.soldiers) {
            if (!s.alive) continue;
            if (p.team == Team::A) a_alive = true;
            if (p.team == Team::B) b_alive = true;
        }
    }
    if (a_alive && b_alive) return false;

    phase_ = Phase::GameOver;
    winner_ = (a_alive && !b_alive) ? 0 : ((b_alive && !a_alive) ? 1 : 2);
    events_.push_back(Event{.type = EventType::GameOver, .winner = winner_});
    return true;
}

}  // namespace fg
