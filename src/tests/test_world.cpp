// 世界模块的单元测试：地图生成与种子复现、回合状态机、炮弹碰撞、胜负。
// 注：各用例把 turn_delay 置 0——默认的"命中后展示停顿"是给玩家看的节奏效果，
// 测试不需要为了等它而多跑几百个 tick
#include <gtest/gtest.h>

#include <cmath>
#include <iomanip>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

#include "world.h"

namespace {

std::vector<fg::PlayerSpec> make_specs(int per_team) {
    std::vector<fg::PlayerSpec> specs;
    for (int i = 1; i <= per_team; ++i) {
        specs.push_back({"A" + std::to_string(i), fg::Team::A, false, 0});
        specs.push_back({"B" + std::to_string(i), fg::Team::B, false, 0});
    }
    return specs;
}

// 构造从 from 指向 to 的直线（参数方程模式，t 递增即朝目标方向）
fg::CurveRequest aim_at(const fg::Vec2& from, const fg::Vec2& to, double overshoot = 1.5) {
    const double ang = std::atan2(to.y() - from.y(), to.x() - from.x());
    const double len = fg::distance(from, to);
    fg::CurveRequest req;
    req.mode = fg::CurveMode::Parametric;
    req.expr_a = "t*" + std::to_string(std::cos(ang));
    req.expr_b = "t*" + std::to_string(std::sin(ang));
    req.p_start = 0.0;
    req.p_end = len * overshoot;
    return req;
}

// 顶点式抛物线 f(x) = b1*x - b2*x*x 的表达式文本：出界宽限的测试要用"顶点高度
// 超上界固定值"的形状，双精度系数必须以足够有效位写进表达式，形状才不走样
std::string parabola_expr(double b1, double b2) {
    std::ostringstream os;
    os << std::setprecision(12) << b1 << "*x - " << b2 << "*x*x";
    return os.str();
}

// 推进直到飞行阶段结束（或到达安全上限，防止死循环）
void run_flight(fg::World& w, int max_ticks = 100000) {
    int i = 0;
    while (w.phase() == fg::Phase::Flight && i++ < max_ticks) {
        w.update(1.0 / 60.0);
    }
    EXPECT_LT(i, max_ticks) << "飞行阶段未能结束（疑似死循环）";
}

// 校验出生点的完整契约：每个士兵都不卡遮挡物、任意两个士兵不重叠
void check_spawn_points(const fg::World& w, const fg::GameConfig& cfg, uint64_t seed) {
    std::vector<fg::Vec2> pos;
    for (const auto& pl : w.players()) {
        for (const auto& s : pl.soldiers) {
            for (const auto& ob : w.obstacles()) {
                EXPECT_FALSE(fg::obstacle_blocks(ob, s.pos, cfg.world.crater_radius))
                    << "seed=" << seed;
            }
            pos.push_back(s.pos);
        }
    }
    const double h = cfg.world.player_half_size;
    for (size_t i = 0; i < pos.size(); ++i) {
        for (size_t j = i + 1; j < pos.size(); ++j) {
            const bool overlap = std::abs(pos[i].x() - pos[j].x()) < h * 2.0 &&
                                 std::abs(pos[i].y() - pos[j].y()) < h * 2.0;
            EXPECT_FALSE(overlap) << "seed=" << seed;
        }
    }
}

int last_turn_begin(const std::vector<fg::Event>& evs) {
    int id = -1;
    for (const auto& e : evs) {
        if (e.type == fg::EventType::TurnBegin) id = e.player_id;
    }
    return id;
}

bool has_event(const std::vector<fg::Event>& evs, fg::EventType t) {
    for (const auto& e : evs) {
        if (e.type == t) return true;
    }
    return false;
}

}  // namespace

// 炮口保护：原地绕小圈（离炮口始终很近）的曲线不会伤到发射者
TEST(World, MuzzleArmProtectsTinyLoop) {
    fg::GameConfig cfg;
    cfg.match.turn_delay = 0.0;
    cfg.match.seed = 21;
    cfg.match.obstacle_count = 0;
    cfg.match.hp = 3;
    cfg.match.first_team = 0;  // 固定 A 先手，保证先轮到我
    fg::World w;
    w.start(cfg, {{"A1", fg::Team::A}, {"B1", fg::Team::B}});

    ASSERT_EQ(last_turn_begin(w.take_events()), 0);
    fg::CurveRequest req;
    req.mode = fg::CurveMode::Parametric;
    // 半径 0.2、锚点平移后圆心在炮口旁 0.2 处：离炮口最远仅 0.4，
    // 弧长（约 1.26）虽够但距离条件永不满足——保护不应解除
    req.expr_a = "0.2*cos(t)";
    req.expr_b = "0.2*sin(t)";
    req.p_end = 2.0 * std::numbers::pi;
    ASSERT_TRUE(w.submit(0, req).ok());
    run_flight(w);

    EXPECT_EQ(w.players()[0].soldiers[0].hp, 3) << "原地小圈的曲线不应伤到发射者";
}

// 炮口保护解除后，曲线折回仍可伤到发射者（不是永久免疫）：
// 沿 x 轴往返一次（最远 1 单位，弧长 2 > 保护所需的 1.2），回程穿过自己
TEST(World, SelfHitAfterArming) {
    fg::GameConfig cfg;
    cfg.match.turn_delay = 0.0;
    cfg.match.seed = 22;
    cfg.match.obstacle_count = 0;
    cfg.match.hp = 3;
    cfg.match.first_team = 0;
    fg::World w;
    w.start(cfg, {{"A1", fg::Team::A}, {"B1", fg::Team::B}});

    ASSERT_EQ(last_turn_begin(w.take_events()), 0);
    fg::CurveRequest req;
    req.mode = fg::CurveMode::Parametric;
    req.expr_a = "sin(t)";
    req.expr_b = "0";
    req.p_end = std::numbers::pi;
    ASSERT_TRUE(w.submit(0, req).ok());
    run_flight(w);

    EXPECT_EQ(w.players()[0].soldiers[0].hp, 2) << "折回的曲线应在解除保护后伤到发射者";
}

TEST(World, SeedReproducible) {
    fg::GameConfig cfg;
    cfg.match.turn_delay = 0.0;
    cfg.match.seed = 999;
    auto specs = make_specs(3);

    fg::World w1, w2;
    w1.start(cfg, specs);
    w2.start(cfg, specs);

    ASSERT_EQ(w1.players().size(), w2.players().size());
    for (size_t i = 0; i < w1.players().size(); ++i) {
        const auto& s1 = w1.players()[i].soldiers;
        const auto& s2 = w2.players()[i].soldiers;
        ASSERT_EQ(s1.size(), s2.size());
        for (size_t k = 0; k < s1.size(); ++k) {
            EXPECT_DOUBLE_EQ(s1[k].pos.x(), s2[k].pos.x());
            EXPECT_DOUBLE_EQ(s1[k].pos.y(), s2[k].pos.y());
        }
    }
    ASSERT_EQ(w1.obstacles().size(), w2.obstacles().size());
    for (size_t i = 0; i < w1.obstacles().size(); ++i) {
        EXPECT_DOUBLE_EQ(w1.obstacles()[i].center.x(), w2.obstacles()[i].center.x());
        EXPECT_DOUBLE_EQ(w1.obstacles()[i].center.y(), w2.obstacles()[i].center.y());
        EXPECT_DOUBLE_EQ(w1.obstacles()[i].radius, w2.obstacles()[i].radius);
        EXPECT_EQ(w1.obstacles()[i].is_circle, w2.obstacles()[i].is_circle);
    }

    // 不同种子应产生不同地图
    fg::GameConfig cfg2 = cfg;
    cfg2.match.seed = 1000;
    fg::World w3;
    w3.start(cfg2, specs);
    bool any_diff = false;
    for (size_t i = 0; i < w3.obstacles().size(); ++i) {
        if (w3.obstacles()[i].center.x() != w1.obstacles()[i].center.x()) any_diff = true;
    }
    EXPECT_TRUE(any_diff);
}

TEST(World, SpawnPointsValid) {
    for (uint64_t seed : {1u, 2u, 3u, 42u, 999u}) {
        fg::GameConfig cfg;
        cfg.match.turn_delay = 0.0;
        cfg.match.seed = seed;
            fg::World w;
        w.start(cfg, make_specs(3));

        check_spawn_points(w, cfg, seed);
    }
}

TEST(World, MultiSoldierSpawnPointsValid) {
    // 每人 2~3 兵：同一玩家的士兵之间也要互不重叠（放置时玩家还没进 players_，
    // 互斥检查必须带上"已放下但尚未入库"的士兵）
    for (uint64_t seed : {1u, 2u, 9u, 42u, 777u}) {
        for (int per : {2, 3}) {
            fg::GameConfig cfg;
            cfg.match.turn_delay = 0.0;
            cfg.match.seed = seed;
                    cfg.room.soldiers_per_player = per;
            fg::World w;
            w.start(cfg, make_specs(3));
            check_spawn_points(w, cfg, seed);
        }
    }
}

TEST(World, SpawnPointsValidUnderExtremeObstacleDensity) {
    // 高密度（配置校验允许的范围内，48×30 地图的密度上限是 45）：随机采样可能失败、
    // 进网格扫描兜底，出生点在任何情况下都必须合法（不卡遮挡物、互不重叠）。
    // 超出该上限的配置已被 check_obstacle_density 用 obstacles_too_dense 拒绝
    for (uint64_t seed : {7u, 11u, 13u, 42u}) {
        fg::GameConfig cfg;
        cfg.match.turn_delay = 0.0;
        cfg.match.seed = seed;
            cfg.match.obstacle_count = 45;
        cfg.world.map_x_min = -24.0;
        cfg.world.map_x_max = 24.0;
        cfg.world.map_y_min = -15.0;
        cfg.world.map_y_max = 15.0;
        fg::World w;
        w.start(cfg, make_specs(3));

        check_spawn_points(w, cfg, seed);
    }
}

TEST(World, TurnOrderAlternatesAndSkipsDead) {
    fg::GameConfig cfg;
    cfg.match.turn_delay = 0.0;
    cfg.match.seed = 5;
    cfg.match.obstacle_count = 0;
    cfg.match.first_team = 0;       // 固定 A 先手以断言具体顺序
    cfg.match.input_seconds = 0.1;  // 短时限，方便用超时推进回合
    fg::World w;
    w.start(cfg, make_specs(3));  // 3v3：ids 0=A1 1=B1 2=A2 3=B2 4=A3 5=B3
    w.take_events();

    // 全部超时，每个参战者收集一次 TurnBegin，顺序应为 A1 B1 A2 B2 A3 B3
    std::vector<int> order;
    for (size_t i = 0; i < w.players().size(); ++i) {
        const int cur = w.current_player_id();
        ASSERT_GE(cur, 0);
        order.push_back(cur);
        w.update(0.2);  // 超过 input_seconds → 超时换人
        auto evs = w.take_events();
        EXPECT_TRUE(has_event(evs, fg::EventType::TurnSkipped));
    }
    const std::vector<int> expected = {0, 1, 2, 3, 4, 5};
    EXPECT_EQ(order, expected);
}

TEST(World, StraightShotKillsAndEndsGame) {
    fg::GameConfig cfg;
    cfg.match.turn_delay = 0.0;
    cfg.match.seed = 17;
    cfg.match.obstacle_count = 0;
    cfg.match.hp = 1;
    cfg.match.first_team = 0;  // 固定 A 先手
    cfg.room.soldiers_per_player = 1;  // 一炮定胜负的场景：每方一兵
    fg::World w;
    w.start(cfg, make_specs(1));  // 1v1
    w.take_events();

    const fg::Vec2 a_pos = w.players()[0].soldiers[0].pos;  // A1
    const fg::Vec2 b_pos = w.players()[1].soldiers[0].pos;  // B1
    ASSERT_EQ(w.current_player_id(), 0);

    const fg::Error err = w.submit(0, aim_at(a_pos, b_pos));
    EXPECT_TRUE(err.ok()) << err.message;
    EXPECT_EQ(w.phase(), fg::Phase::Flight);

    run_flight(w);

    EXPECT_FALSE(w.players()[1].soldiers[0].alive);
    EXPECT_EQ(w.players()[1].soldiers[0].hp, 0);
    EXPECT_EQ(w.phase(), fg::Phase::GameOver);
    EXPECT_EQ(w.winner(), 0);  // A 队胜

    auto evs = w.take_events();
    EXPECT_TRUE(has_event(evs, fg::EventType::PlayerHit));
    EXPECT_TRUE(has_event(evs, fg::EventType::PlayerDied));
    EXPECT_TRUE(has_event(evs, fg::EventType::GameOver));
}

TEST(World, FriendlyFireHitsTeammate) {
    fg::GameConfig cfg;
    cfg.match.turn_delay = 0.0;
    cfg.match.seed = 23;
    cfg.match.obstacle_count = 0;
    cfg.match.first_team = 0;  // 固定 A 先手（先手需要是 A1 才能打队友 A2）
    fg::World w;
    std::vector<fg::PlayerSpec> specs = {{"A1", fg::Team::A, false, 0},
                                         {"A2", fg::Team::A, false, 0},
                                         {"B1", fg::Team::B, false, 0}};
    w.start(cfg, specs);
    w.take_events();

    // 让 A1 打自己的队友 A2（友伤）
    const fg::Vec2 a1 = w.players()[0].soldiers[0].pos;
    const fg::Vec2 a2 = w.players()[1].soldiers[0].pos;
    ASSERT_EQ(w.current_player_id(), 0);
    EXPECT_TRUE(w.submit(0, aim_at(a1, a2)).ok());

    run_flight(w);

    EXPECT_EQ(w.players()[1].soldiers[0].hp, 0);      // 队友被自己人打中
    EXPECT_FALSE(w.players()[1].soldiers[0].alive);
    // A 队还有 A1 活着，游戏应当继续（不被误判为结束）
    EXPECT_TRUE(w.players()[0].any_alive());
    EXPECT_EQ(w.winner(), -1);
    // 发射者本人不受自己炮弹伤害
    EXPECT_EQ(w.players()[0].soldiers[0].hp, w.players()[0].soldiers[0].max_hp);
}

TEST(World, ProjectileMakesCraterAndDies) {
    fg::GameConfig cfg;
    cfg.match.turn_delay = 0.0;
    cfg.match.seed = 31;
    cfg.match.obstacle_count = 1;  // 只有一个障碍，保证炮弹一定撞它
    cfg.match.first_team = 0;      // 固定 A 先手
    fg::World w;
    w.start(cfg, make_specs(1));
    w.take_events();

    const fg::Vec2 a_pos = w.players()[0].soldiers[0].pos;
    const fg::Vec2 ob_center = w.obstacles()[0].center;

    // 朝障碍中心射击（若恰好撞到玩家也不影响：炮弹会穿透继续飞向障碍）
    ASSERT_EQ(w.current_player_id(), 0);
    EXPECT_TRUE(w.submit(0, aim_at(a_pos, ob_center)).ok());
    run_flight(w);

    auto evs = w.take_events();
    bool crater_on_0 = false;
    for (const auto& e : evs) {
        if (e.type == fg::EventType::CraterMade && e.obstacle_index == 0) crater_on_0 = true;
    }
    EXPECT_TRUE(crater_on_0);
    EXPECT_FALSE(w.obstacles()[0].craters.empty());
    EXPECT_FALSE(w.projectile().has_value());  // 炮弹已销毁
}

TEST(World, SubmitRejectedWhenNotYourTurn) {
    fg::GameConfig cfg;
    cfg.match.turn_delay = 0.0;
    cfg.match.seed = 3;
    cfg.match.obstacle_count = 0;
    cfg.match.first_team = 0;  // 固定 A 先手
    fg::World w;
    w.start(cfg, make_specs(1));
    w.take_events();

    ASSERT_EQ(w.current_player_id(), 0);
    // 不是自己的回合
    EXPECT_FALSE(
        w.submit(1, aim_at(w.players()[1].soldiers[0].pos, w.players()[0].soldiers[0].pos)).ok());
    // 非法表达式
    fg::CurveRequest bad;
    bad.mode = fg::CurveMode::YofX;
    bad.expr_a = "x +";
    EXPECT_FALSE(w.submit(0, bad).ok());
    // 失败后仍应处于输入阶段，允许重提
    EXPECT_EQ(w.phase(), fg::Phase::Input);
}

TEST(World, FirstTeamRandomAndConfigurable) {
    // 显式指定先手队必须生效
    for (int ft = 0; ft <= 1; ++ft) {
        fg::GameConfig cfg;
        cfg.match.turn_delay = 0.0;
        cfg.match.seed = 7;
        cfg.match.obstacle_count = 0;
        cfg.match.first_team = ft;
        fg::World w;
        w.start(cfg, make_specs(2));
        const int cur = w.current_player_id();
        ASSERT_GE(cur, 0);
        EXPECT_EQ(w.players()[static_cast<size_t>(cur)].team,
                  ft == 0 ? fg::Team::A : fg::Team::B);
    }

    // 默认（first_team = -1）：同种子复现，跨种子两队都可能先手
    int a_first = 0, b_first = 0;
    for (uint64_t seed = 0; seed < 24; ++seed) {
        fg::GameConfig cfg;
        cfg.match.turn_delay = 0.0;
        cfg.match.seed = seed;
        cfg.match.obstacle_count = 0;
        fg::World w1, w2;
        w1.start(cfg, make_specs(2));
        w2.start(cfg, make_specs(2));
        EXPECT_EQ(w1.current_player_id(), w2.current_player_id()) << "seed=" << seed;
        const int cur = w1.current_player_id();
        ASSERT_GE(cur, 0);
        if (w1.players()[static_cast<size_t>(cur)].team == fg::Team::A) ++a_first;
        else ++b_first;
    }
    EXPECT_GT(a_first, 0) << "随机先手在多种子下应能轮到 A 队";
    EXPECT_GT(b_first, 0) << "随机先手在多种子下应能轮到 B 队";
}

TEST(World, ObstacleBlocksGeometry) {
    // 圆形：内含坑
    fg::Obstacle circle;
    circle.is_circle = true;
    circle.center = fg::Vec2(5.0, 5.0);
    circle.radius = 3.0;
    EXPECT_TRUE(fg::obstacle_blocks(circle, fg::Vec2(5.0, 5.0), 2.0));
    EXPECT_TRUE(fg::obstacle_blocks(circle, fg::Vec2(7.0, 5.0), 2.0));
    EXPECT_FALSE(fg::obstacle_blocks(circle, fg::Vec2(9.0, 5.0), 2.0));
    circle.craters.push_back(fg::Vec2(5.0, 5.0));
    EXPECT_FALSE(fg::obstacle_blocks(circle, fg::Vec2(5.0, 5.0), 2.0));   // 坑内不阻挡
    EXPECT_FALSE(fg::obstacle_blocks(circle, fg::Vec2(6.5, 5.0), 2.0));   // 距坑 1.5 < 2
    EXPECT_TRUE(fg::obstacle_blocks(circle, fg::Vec2(7.5, 5.0), 2.0));    // 距坑 2.5 > 2，仍在圆内

    // 矩形
    fg::Obstacle rect;
    rect.is_circle = false;
    rect.center = fg::Vec2(0.0, 0.0);
    rect.half_w = 4.0;
    rect.half_h = 2.0;
    EXPECT_TRUE(fg::obstacle_blocks(rect, fg::Vec2(3.9, 1.9), 1.0));
    EXPECT_FALSE(fg::obstacle_blocks(rect, fg::Vec2(3.9, 2.5), 1.0));
    EXPECT_FALSE(fg::obstacle_blocks(rect, fg::Vec2(4.5, 0.0), 1.0));
}

// ---- 多士兵与展示停顿 ----

TEST(World, SoldiersAllSpawnedAndRotate) {
    // 每人 2 兵：开局全部出生；出手后轮换到下一个存活士兵，走到头回绕
    fg::GameConfig cfg;
    cfg.match.turn_delay = 0.0;
    cfg.match.seed = 9;
    cfg.match.obstacle_count = 0;
    cfg.match.hp = 5;  // 空放误中队友也不致死，不影响轮转断言
    cfg.match.first_team = 0;
    cfg.room.soldiers_per_player = 2;
    fg::World w;
    w.start(cfg, make_specs(1));  // 1v1，每人 2 兵
    w.take_events();

    ASSERT_EQ(w.players().size(), 2u);
    for (const auto& pl : w.players()) EXPECT_EQ(pl.soldiers.size(), 2u);
    ASSERT_EQ(w.current_player_id(), 0);
    EXPECT_EQ(w.players()[0].current_soldier, 0);

    // 朝正上方打（40 单位内出界）：不命中任何人，只推进回合轮转
    const fg::Vec2 a0 = w.players()[0].soldiers[0].pos;
    ASSERT_TRUE(w.submit(0, aim_at(a0, a0 + fg::Vec2(0.0, 40.0))).ok());
    run_flight(w);
    EXPECT_EQ(w.players()[0].current_soldier, 1) << "出手后应轮换到下一个士兵";

    ASSERT_EQ(w.current_player_id(), 1);
    const fg::Vec2 b0 = w.players()[1].soldiers[0].pos;
    ASSERT_TRUE(w.submit(1, aim_at(b0, b0 + fg::Vec2(0.0, 40.0))).ok());
    run_flight(w);
    EXPECT_EQ(w.players()[1].current_soldier, 1);

    // 回到 A：用 1 号士兵出手，出手后回绕到 0 号
    ASSERT_EQ(w.current_player_id(), 0);
    EXPECT_EQ(w.current_soldier_index(), 1);
    const fg::Vec2 a1 = w.players()[0].soldiers[1].pos;
    ASSERT_TRUE(w.submit(0, aim_at(a1, a1 + fg::Vec2(0.0, 40.0))).ok());
    run_flight(w);
    EXPECT_EQ(w.players()[0].current_soldier, 0);
    EXPECT_EQ(w.winner(), -1) << "空放不应结束对局";
}

TEST(World, TeamEliminatedOnlyWhenAllSoldiersDead) {
    // 打死一个士兵不等于打掉这个玩家：另一个还活着对局继续，全灭才结束
    fg::GameConfig cfg;
    cfg.match.turn_delay = 0.0;
    cfg.match.seed = 41;
    cfg.match.obstacle_count = 0;
    cfg.match.hp = 1;
    cfg.match.first_team = 0;
    cfg.room.soldiers_per_player = 2;
    fg::World w;
    w.start(cfg, make_specs(1));
    w.take_events();

    // A 的第一发打死 B 的 0 号（直线穿过瞄准点，命中必须携带受击士兵序号）
    ASSERT_EQ(w.current_player_id(), 0);
    ASSERT_TRUE(
        w.submit(0, aim_at(w.players()[0].soldiers[0].pos, w.players()[1].soldiers[0].pos)).ok());
    run_flight(w);
    bool hit_soldier_0 = false;
    for (const auto& e : w.take_events()) {
        if (e.type == fg::EventType::PlayerHit && e.target_id == 1 && e.target_soldier == 0) {
            hit_soldier_0 = true;
        }
    }
    EXPECT_TRUE(hit_soldier_0) << "命中事件应携带受击士兵序号";
    EXPECT_FALSE(w.players()[1].soldiers[0].alive);
    EXPECT_TRUE(w.players()[1].any_alive());
    EXPECT_EQ(w.winner(), -1) << "还有一个士兵活着，不该判负";

    // B 空放：向正下方打出界（两队出生区相隔很远，垂直向下不会伤到 A）
    ASSERT_EQ(w.current_player_id(), 1);
    const fg::Vec2 bp = w.players()[1].current().pos;  // 轮到 B 时游标应已指到存活的 1 号
    EXPECT_EQ(w.players()[1].current_soldier, 1);
    ASSERT_TRUE(w.submit(1, aim_at(bp, bp + fg::Vec2(0.0, -40.0))).ok());
    run_flight(w);
    ASSERT_EQ(w.winner(), -1);

    // 回到 A：打死 B 的 1 号 → B 全灭 → 结束
    ASSERT_EQ(w.current_player_id(), 0);
    ASSERT_TRUE(
        w.submit(0, aim_at(w.players()[0].current().pos, w.players()[1].soldiers[1].pos)).ok());
    run_flight(w);
    EXPECT_FALSE(w.players()[1].soldiers[1].alive);
    EXPECT_FALSE(w.players()[1].any_alive());
    EXPECT_EQ(w.phase(), fg::Phase::GameOver);
    EXPECT_EQ(w.winner(), 0);
}

TEST(World, TurnDelayHoldsNextTurn) {
    // 展示停顿：炮弹消失后不立刻换人，停顿期间仍是飞行阶段；停顿走完才轮到下一位
    fg::GameConfig cfg;
    cfg.match.turn_delay = 0.5;
    cfg.match.seed = 43;
    cfg.match.obstacle_count = 0;
    cfg.match.hp = 5;  // 命中扣血但不致死，走"命中后停顿"路径
    cfg.match.first_team = 0;
    fg::World w;
    w.start(cfg, make_specs(1));
    w.take_events();

    const fg::Vec2 a0 = w.players()[0].soldiers[0].pos;
    const fg::Vec2 b0 = w.players()[1].soldiers[0].pos;
    ASSERT_TRUE(w.submit(0, aim_at(a0, b0)).ok());

    int guard = 0;
    while (w.projectile().has_value() && guard++ < 100000) w.update(1.0 / 60.0);
    ASSERT_LT(guard, 100000);
    // 炮弹已消失：展示停顿期间阶段不变、回合不推进
    ASSERT_EQ(w.phase(), fg::Phase::Flight);
    EXPECT_EQ(w.current_player_id(), -1);
    w.update(0.3);
    EXPECT_EQ(w.phase(), fg::Phase::Flight);
    w.update(0.3);  // 累计 0.6 秒 > 设置的 0.5 秒
    EXPECT_EQ(w.phase(), fg::Phase::Input);
    EXPECT_EQ(w.current_player_id(), 1);
}

// ---- 出界宽限：炮弹飞出地图后有 3 秒（速度×3 的弧长）"绕回来"的机会 ----

// 共用形状：顶点全局高度固定为 18（超上界 15 三单位、在采样余量 8 内），
// 顶点参数位置 k=6——出界区弧长约 9~13 单位（随士兵所在高度略变）。y=f(x) 的
// 曲线起点由平移自动落在士兵身上，与士兵位置无关
fg::CurveRequest out_of_map_arc(const fg::Vec2& soldier, double k) {
    const double cp = 18.0 - soldier.y();  // 顶点的士兵局部高
    fg::CurveRequest req;
    req.mode = fg::CurveMode::YofX;
    req.expr_a = parabola_expr(2.0 * cp / k, cp / (k * k));
    req.dir = fg::FireDir::Positive;
    return req;
}

TEST(World, ProjectileSurvivesOutOfMapGrace) {
    // 高抛弹道飞出上边界一小段后回落到图内：炮弹必须带着"存活"状态出界再回来，
    // 旧行为"出界即销毁"会永远看不到这类弹道
    fg::GameConfig cfg;
    cfg.match.turn_delay = 0.0;
    cfg.match.seed = 7;
    cfg.match.obstacle_count = 0;
    cfg.match.first_team = 0;
    fg::World w;
    w.start(cfg, make_specs(1));
    w.take_events();
    ASSERT_EQ(w.current_player_id(), 0);
    ASSERT_TRUE(w.submit(0, out_of_map_arc(w.players()[0].soldiers[0].pos, 6.0)).ok());

    // 找出"弹头在地图外且炮弹仍存活"的瞬间：宽限生效时该状态持续约 0.8 秒
    //（数十个 tick），逐 tick 观察必然捕获；旧行为在出界的同一子步就销毁
    bool saw_alive_out = false;
    for (int i = 0; i < 240 && w.phase() == fg::Phase::Flight; ++i) {
        w.update(1.0 / 60.0);
        if (!w.projectile()) break;  // 回到图内之前消失 = 被误杀
        const fg::Projectile& pr = *w.projectile();
        if (pr.path.at(pr.travel).y() > cfg.world.map_y_max) {
            saw_alive_out = true;
            break;
        }
    }
    EXPECT_TRUE(saw_alive_out) << "炮弹应带着存活状态飞出地图（宽限生效）";
    run_flight(w);
    EXPECT_EQ(w.current_player_id(), 1) << "炮弹回到图内、飞完余程后正常轮到 B";
}

TEST(World, ProjectileDiesAfterOutOfMapGrace) {
    // 宽限的上限：调慢炮弹速度（宽限弧长 = 速度×3 秒），让同一条拱线的出界区
    // 远超宽限——炮弹必须在超时后销毁，且销毁点在地图外（区别于"走到路径尽头"
    // 的销毁点：那在图下方 8 单位的采样截断处）
    fg::GameConfig cfg;
    cfg.match.turn_delay = 0.0;
    cfg.match.seed = 7;
    cfg.match.obstacle_count = 0;
    cfg.match.first_team = 0;
    cfg.world.projectile_speed = 1.0;  // 宽限弧长 3，小于出界区弧长（≥9）
    fg::World w;
    w.start(cfg, make_specs(1));
    w.take_events();
    ASSERT_EQ(w.current_player_id(), 0);
    ASSERT_TRUE(w.submit(0, out_of_map_arc(w.players()[0].soldiers[0].pos, 6.0)).ok());

    fg::Vec2 last_pos;
    bool had_projectile = false;
    int ticks = 0;
    while (w.phase() == fg::Phase::Flight && ticks++ < 1800) {
        if (w.projectile()) {
            const fg::Projectile& pr = *w.projectile();
            last_pos = pr.path.at(pr.travel);
            had_projectile = true;
        }
        w.update(1.0 / 60.0);
    }
    ASSERT_EQ(w.phase(), fg::Phase::Input) << "出界超时的炮弹应被销毁并轮转";
    ASSERT_TRUE(had_projectile);
    EXPECT_GT(last_pos.y(), cfg.world.map_y_max)
        << "销毁应发生在图外的宽限超时处，而不是路径尽头的下方截断处";
    EXPECT_EQ(w.current_player_id(), 1);
}

TEST(World, ZeroSpeedProjectileDoesNotLockFlight) {
    // 非正速度（错误配置）下炮弹永远推不动：必须直接按失效收尾并轮转，
    // 不能把对局锁死在飞行阶段——step_flight 的速度兜底就是为此
    fg::GameConfig cfg;
    cfg.match.turn_delay = 0.0;
    cfg.match.seed = 7;
    cfg.match.obstacle_count = 0;
    cfg.match.first_team = 0;
    cfg.world.projectile_speed = 0.0;
    fg::World w;
    w.start(cfg, make_specs(1));
    w.take_events();
    ASSERT_EQ(w.current_player_id(), 0);
    ASSERT_TRUE(w.submit(0, aim_at(w.players()[0].soldiers[0].pos,
                                   w.players()[1].soldiers[0].pos)).ok());
    w.update(1.0 / 60.0);
    EXPECT_EQ(w.phase(), fg::Phase::Input) << "速度 0 的炮弹应立即销毁收尾，不锁死飞行阶段";
    EXPECT_EQ(w.current_player_id(), 1);
}

TEST(World, TinySpeedProjectileDoesNotLockFlight) {
    // 极小正速度（毒配置，如 1e-10）：炮弹永远飞不完，飞行时长硬上限（60 秒）
    // 必须兜底收尾——不能把对局无限期锁在飞行阶段
    fg::GameConfig cfg;
    cfg.match.turn_delay = 0.0;
    cfg.match.seed = 7;
    cfg.match.obstacle_count = 0;
    cfg.match.first_team = 0;
    cfg.world.projectile_speed = 1e-10;
    fg::World w;
    w.start(cfg, make_specs(1));
    w.take_events();
    ASSERT_EQ(w.current_player_id(), 0);
    ASSERT_TRUE(w.submit(0, aim_at(w.players()[0].soldiers[0].pos,
                                   w.players()[1].soldiers[0].pos)).ok());
    // 61 秒模拟内必须结束（上限 60 秒留 1 秒余量）
    int ticks = 0;
    while (w.phase() == fg::Phase::Flight && ticks++ < 61 * 60 + 15) w.update(1.0 / 60.0);
    EXPECT_EQ(w.phase(), fg::Phase::Input) << "极小速度的炮弹应由飞行时长上限兜底收尾";
    EXPECT_EQ(w.current_player_id(), 1);
}
