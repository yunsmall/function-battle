// 人机 AI 的单元测试：表达式合法性与难度对命中率的影响
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>
#include <vector>

#include "ai.h"
#include "ai_curves.h"
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

void run_flight(fg::World& w, int max_ticks = 200000) {
    int i = 0;
    while (w.phase() == fg::Phase::Flight && i++ < max_ticks) {
        w.update(1.0 / 60.0);
    }
    ASSERT_LT(i, max_ticks) << "飞行阶段未能结束";
}

// 断言曲线经过所有给定目标点（相对锚点坐标，锚点取原点）。
// tol 默认取直角模式采样步长的量级；极坐标模式顶点间距更大（0.01 rad × 半径），
// 调用方要按半径放宽——这里量的是"到采样顶点的距离"，不是到折线的距离
void expect_passes(const fg::CurveRequest& req, const std::vector<fg::Vec2>& pts,
                   double tol = 0.05) {
    fg::GameConfig cfg;
    const fg::CurveResult r = fg::sample_curve(req, fg::Vec2(0.0, 0.0), cfg);
    ASSERT_TRUE(r.ok()) << r.error.message << " 表达式: " << req.expr_a;
    for (size_t i = 0; i < pts.size(); ++i) {
        double best = 1e18;
        fg::Vec2 nearest(0.0, 0.0);
        for (const auto& q : r.path.points) {
            const double d = fg::distance(q, pts[i]);
            if (d < best) {
                best = d;
                nearest = q;
            }
        }
        const fg::Vec2& last = r.path.points.back();
        EXPECT_LT(best, tol)
            << "第 " << i << " 个目标没有被曲线穿过（最小距离 " << best << "）最近点 (" << nearest.x()
            << "," << nearest.y() << ") 路径末点 (" << last.x() << "," << last.y() << ") 点数 "
            << r.path.points.size() << " 表达式: " << req.expr_a;
    }
}

}  // namespace

TEST(Ai, ProducesValidSubmissionEveryDifficulty) {
    for (int diff = 0; diff < fg::kAiDifficultyCount; ++diff) {
        fg::GameConfig cfg;
        cfg.match.turn_delay = 0.0;  // 跳过命中后的展示停顿（测试不需要等它）
        cfg.match.seed = 77;
        cfg.match.obstacle_count = 6;
        fg::World w;
        w.start(cfg, make_specs(2));
        w.take_events();

        fg::Rng rng(static_cast<uint64_t>(1234 + diff * 17));
        const int cur = w.current_player_id();
        ASSERT_GE(cur, 0);
        const fg::AiDecision dec = fg::ai_decide(w, cur, diff, rng);

        EXPECT_GT(dec.delay, 0.0) << "难度 " << diff;
        EXPECT_LT(dec.delay, cfg.match.input_seconds) << "难度 " << diff;
        // AI 生成的表达式必须是合法提交（走和人类一样的校验）
        EXPECT_TRUE(w.submit(cur, dec.request).ok()) << "难度 " << diff;
        EXPECT_EQ(w.phase(), fg::Phase::Flight) << "难度 " << diff;
    }
}

TEST(Ai, HardAiCanHitMultipleTargets) {
    // 困难 AI 应当具备"一发穿多人"的能力（插值规划）
    constexpr int kTrials = 40;
    int multi = 0;
    for (int t = 0; t < kTrials; ++t) {
        fg::GameConfig cfg;
        cfg.match.turn_delay = 0.0;  // 跳过命中后的展示停顿（测试不需要等它）
        cfg.match.seed = static_cast<uint64_t>(2000 + t);
        cfg.match.obstacle_count = 0;  // 排除障碍干扰，只看多目标能力
        cfg.match.first_team = 0;      // 固定 A 先手，目标才固定是 B 队三人
        cfg.room.soldiers_per_player = 1;  // 四个玩家各一兵：一发"穿多人"才有意义
        fg::World w;
        std::vector<fg::PlayerSpec> specs = {
            {"A1", fg::Team::A, false, 0},
            {"B1", fg::Team::B, false, 0},
            {"B2", fg::Team::B, false, 0},
            {"B3", fg::Team::B, false, 0},
        };
        w.start(cfg, specs);
        w.take_events();

        const int cur = w.current_player_id();
        ASSERT_EQ(cur, 0);  // 1v3，先手是 A1
        fg::Rng rng(static_cast<uint64_t>(9000 + t));
        const fg::AiDecision dec = fg::ai_decide(w, cur, 2, rng);
        ASSERT_TRUE(w.submit(cur, dec.request).ok());
        run_flight(w);

        int dead = 0;
        for (int i = 1; i <= 3; ++i) {
            if (!w.players()[static_cast<size_t>(i)].any_alive()) ++dead;
        }
        if (dead >= 2) ++multi;
    }
    EXPECT_GT(multi, 0) << "困难 AI 在 " << kTrials << " 次里一次多杀都没有";
}

TEST(Ai, SlopeStepHitsLeftTarget) {
    // 回归：斜率阶跃候选在"目标在左边"时曾把抬升写成 A*|dx|/2（符号错），积分终点落到
    // dy - 2*offset。这里在瞄准列上直接量打点高度
    const fg::GameConfig cfg;
    for (const double dx : {-12.0, -7.5}) {
        const double dy = 4.0;
        const fg::CurveRequest req = fg::make_ode_slope_step(dx, dy, 0.0, 1.0);
        ASSERT_EQ(req.mode, fg::CurveMode::Ode1);
        ASSERT_EQ(req.dir, fg::FireDir::Negative);
        const fg::CurveResult r = fg::sample_curve(req, fg::Vec2(0.0, 0.0), cfg);
        ASSERT_TRUE(r.ok()) << r.error.message;
        // x 最接近瞄准列 dx 的点，就是"在瞄准列上打到哪里"
        const auto it = std::min_element(r.path.points.begin(), r.path.points.end(),
                                         [dx](const fg::Vec2& a, const fg::Vec2& b) {
                                             return std::abs(a.x() - dx) < std::abs(b.x() - dx);
                                         });
        EXPECT_LT(std::abs(it->y() - dy), cfg.world.player_half_size) << "dx = " << dx;
    }
}

TEST(Ai, MultiKillRateLimited) {
    // 限流判定：keep=0 一律不保留、keep=1 一律保留；reach<=1 恒保留（不掷骰）
    fg::Rng rng(2026);
    fg::AiProfile off;
    off.multi_keep2 = 0.0;
    off.multi_keep3 = 0.0;
    for (int i = 0; i < 200; ++i) {
        EXPECT_FALSE(fg::multi_kill_kept(2, off, rng));
        EXPECT_FALSE(fg::multi_kill_kept(3, off, rng));
    }
    EXPECT_TRUE(fg::multi_kill_kept(1, off, rng));
    fg::AiProfile on;
    on.multi_keep2 = 1.0;
    on.multi_keep3 = 1.0;
    for (int i = 0; i < 200; ++i) {
        EXPECT_TRUE(fg::multi_kill_kept(2, on, rng));
        EXPECT_TRUE(fg::multi_kill_kept(3, on, rng));
    }
    // 默认概率的统计（种子固定，结果每次一致）：两杀约一半、三杀约四分之一
    fg::AiProfile def;
    constexpr int kN = 4000;
    int kept2 = 0;
    int kept3 = 0;
    for (int i = 0; i < kN; ++i) {
        if (fg::multi_kill_kept(2, def, rng)) ++kept2;
        if (fg::multi_kill_kept(3, def, rng)) ++kept3;
    }
    EXPECT_GT(kept2, kN * 2 / 5);
    EXPECT_LT(kept2, kN * 3 / 5);
    EXPECT_GT(kept3, kN / 5);
    EXPECT_LT(kept3, kN / 3);
}

TEST(Ai, HardAiMultiKillIsOccasional) {
    // "一炮全干完"被限流后应当既保留（下界）又不再是常态（上界）。
    // 种子固定 → 每次运行结果完全一致，不是概率型 flaky 测试
    constexpr int kTrials = 40;
    int wipe = 0;  // 一炮扫中全部 3 人的次数
    for (int t = 0; t < kTrials; ++t) {
        fg::GameConfig cfg;
        cfg.match.turn_delay = 0.0;  // 跳过命中后的展示停顿（测试不需要等它）
        cfg.match.seed = static_cast<uint64_t>(2000 + t);
        cfg.match.obstacle_count = 0;  // 无遮挡开阔图："刻意三杀"最容易成立的极端场景
        cfg.match.first_team = 0;
        cfg.room.soldiers_per_player = 1;  // 四个玩家各一兵："一炮扫全 3 人"才有意义
        fg::World w;
        std::vector<fg::PlayerSpec> specs = {
            {"A1", fg::Team::A, false, 0},
            {"B1", fg::Team::B, false, 0},
            {"B2", fg::Team::B, false, 0},
            {"B3", fg::Team::B, false, 0},
        };
        w.start(cfg, specs);
        w.take_events();
        const int cur = w.current_player_id();
        ASSERT_EQ(cur, 0);
        fg::Rng rng(static_cast<uint64_t>(9000 + t));
        const fg::AiDecision dec = fg::ai_decide(w, cur, 2, rng);
        ASSERT_TRUE(w.submit(cur, dec.request).ok());
        run_flight(w);
        int dead = 0;
        for (int i = 1; i <= 3; ++i) {
            if (!w.players()[static_cast<size_t>(i)].any_alive()) ++dead;
        }
        if (dead >= 3) ++wipe;
    }
    EXPECT_GT(wipe, 0) << "限流把一炮三杀限没了（杀手锏应当保留）";
    EXPECT_LT(wipe, kTrials / 2) << "限流没生效：三杀仍是常态（" << wipe << "/" << kTrials << "）";
}

TEST(Ai, GodAiKeepsMultiKill) {
    // 大神档不限流：同样种子下"一炮三杀"次数必须严格多于困难档（单调关系，
    // 种子固定 → 结果完全确定）
    constexpr int kTrials = 40;
    int wipe[2] = {0, 0};  // [0]=困难 [1]=大神
    for (int t = 0; t < kTrials; ++t) {
        for (int god = 0; god < 2; ++god) {
            fg::GameConfig cfg;
            cfg.match.turn_delay = 0.0;
            cfg.match.seed = static_cast<uint64_t>(2000 + t);
            cfg.match.obstacle_count = 0;
            cfg.match.first_team = 0;
            cfg.room.soldiers_per_player = 1;  // 四个玩家各一兵："一炮三杀"才有意义
            fg::World w;
            std::vector<fg::PlayerSpec> specs = {
                {"A1", fg::Team::A, false, 0},
                {"B1", fg::Team::B, false, 0},
                {"B2", fg::Team::B, false, 0},
                {"B3", fg::Team::B, false, 0},
            };
            w.start(cfg, specs);
            w.take_events();
            const int cur = w.current_player_id();
            ASSERT_EQ(cur, 0);
            fg::Rng rng(static_cast<uint64_t>(9000 + t));
            const fg::AiDecision dec = fg::ai_decide(w, cur, god ? 3 : 2, rng);
            ASSERT_TRUE(w.submit(cur, dec.request).ok());
            run_flight(w);
            int dead = 0;
            for (int i = 1; i <= 3; ++i) {
                if (!w.players()[static_cast<size_t>(i)].any_alive()) ++dead;
            }
            if (dead >= 3) ++wipe[god];
        }
    }
    EXPECT_GT(wipe[1], wipe[0]) << "大神 " << wipe[1] << " 次 vs 困难 " << wipe[0]
                                << " 次：不限流应明显更多";
}

TEST(Ai, EvaluationMatchesFlightOnStepJumps) {
    // 回归：AI 的撞墙评估必须与真实飞行一致。曾出 bug：评估只检查采样顶点，
    // 瞄目标的"step 跳变段"（两个顶点相距 20+ 单位）横穿障碍时被漏检，
    // AI 连选 60/60 全打空的"假阳性最佳线"。这里用当时的真实局面复现：
    // 直接对比"评估口径的 blocked"与"飞行是否真的炸墙"——评估说干净的线，
    // 飞行绝不能撞墙（系统性漏检会成片违反；难度参数有意保留的瞄偏不算错）。
    // 已知数值边界：评估按 0.25 弧长等分取判定点、飞行按 speed×dt 推进（相位
    // 随服务器帧间抖动），穿透弦长小于点距的擦边穿越理论上偶有不同结论——
    // 那不是本测试守的系统性缺陷（那种会 100% 全空或成片漏检）
    fg::GameConfig cfg;
    cfg.match.seed = 11164153470160168473ULL;
    cfg.match.obstacle_count = 15;
    cfg.room.soldiers_per_player = 3;
    cfg.match.first_team = 0;
    cfg.match.turn_delay = 0.0;
    // A1 先手：提交当时真实用的那一发（保持世界演化一致，含真实弹坑）
    fg::CurveRequest mine;
    mine.mode = fg::CurveMode::YofX;
    mine.expr_a = "2.96/21.50*x";

    constexpr int kN = 60;
    for (const int diff : {2, 3}) {
        int mismatch = 0;
        for (int t = 0; t < kN; ++t) {
            fg::World w;
            w.start(cfg, {{"A1", fg::Team::A, false, 0}, {"B1", fg::Team::B, false, 0}});
            w.take_events();
            ASSERT_TRUE(w.submit(w.current_player_id(), mine).ok());
            run_flight(w);

            fg::Rng rng(static_cast<uint64_t>(100 + t));
            const int cur = w.current_player_id();
            const fg::AiDecision dec = fg::ai_decide(w, cur, diff, rng);
            const fg::Vec2 origin = w.players()[1].current().pos;
            // 评估重算：blocked 与 aim 无关（hit 不中断循环），aim 取 A1 一兵位置即可
            const fg::CurveCandidate ev = fg::evaluate_candidate(
                dec.request, origin, w.players()[0].soldiers[0].pos, w);
            ASSERT_TRUE(w.submit(cur, dec.request).ok());
            run_flight(w);
            // 飞行结果只看 B1 自己炸的墙：A1 第一发固定会撞一次（历史局面的设定）
            bool wall = false;
            for (const auto& e : w.take_events()) {
                if (e.type == fg::EventType::CraterMade && e.player_id == cur) wall = true;
            }
            if (!ev.blocked && wall) {
                ++mismatch;
                std::printf("[不一致] diff=%d t=%d expr=%.70s\n", diff, t,
                            dec.request.expr_a.c_str());
            }
        }
        EXPECT_EQ(mismatch, 0) << "难度 " << diff << "：评估说干净的线飞行撞墙了（" << mismatch
                               << "/" << kN << "）";
    }
}

TEST(Ai, DifficultyAffectsAccuracy) {
    constexpr int kTrials = 60;
    int hits[fg::kAiDifficultyCount] = {0};

    for (int diff = 0; diff < fg::kAiDifficultyCount; ++diff) {
        for (int t = 0; t < kTrials; ++t) {
            fg::GameConfig cfg;
            cfg.match.seed = static_cast<uint64_t>(1000 + t);
            cfg.match.obstacle_count = 0;  // 无遮挡的纯瞄准场合
            cfg.match.first_team = 0;      // 固定 A 先手，目标固定为 players[1]
            cfg.room.soldiers_per_player = 1;  // 1v1 各一兵：数的是"一发打死"
            fg::World w;
            w.start(cfg, make_specs(1));  // 1v1
            w.take_events();

            fg::Rng rng(static_cast<uint64_t>(5000 + t * 7 + diff * 101));
            const int cur = w.current_player_id();
            const fg::AiDecision dec = fg::ai_decide(w, cur, diff, rng);
            ASSERT_TRUE(w.submit(cur, dec.request).ok());
            run_flight(w);

            if (!w.players()[1].any_alive()) hits[diff]++;
        }
    }

    // 难度越高，命中率越高。注意口径：自动调参后三档的散布（pos/angle）落在同一带内
    // （档位梯度改由"候选形态/择优/绕障能力"承担），无遮挡的单发场合简单档与普通档
    // 打平是正常现象——这里守的是"不反超"和整体量级（实战 3v3 有障碍时差距仍然显著）
    EXPECT_LE(hits[0], hits[1]) << "简单:" << hits[0] << " 普通:" << hits[1];
    EXPECT_LT(hits[1], hits[2]) << "普通:" << hits[1] << " 困难:" << hits[2];
    EXPECT_GT(hits[2], kTrials * 3 / 5) << "困难 AI 命中率过低: " << hits[2];
    EXPECT_LT(hits[0], kTrials * 2 / 3) << "简单 AI 命中率过高: " << hits[0];
}

TEST(Ai, StairsReachAim) {
    // 阶梯/锯齿此前没有覆盖（评测发现普通档阶梯出手恒为 0%——先确认候选本身可不可达）
    fg::GameConfig cfg;
    cfg.match.seed = 42;
    cfg.match.obstacle_count = 0;  // 无障碍纯瞄准场合
    fg::World w;
    w.start(cfg, make_specs(1));
    struct Case {
        fg::CurveRequest req;
        fg::Vec2 aim;
        const char* name;
    };
    const std::vector<Case> cases = {
        {fg::make_stairs(true, 9.0, 4.0, 1.2, 2.25), {9.0, 4.0}, "stairs"},
        {fg::make_stairs(false, 4.0, -9.0, 1.2, 2.25), {4.0, -9.0}, "stairs-vertical"},
        {fg::make_stairs_wave(true, 9.0, 4.0, 1.0, 2.25, 1.5), {9.0, 4.0}, "stairs-wave"},
        {fg::make_sawtooth(true, 9.0, 4.0, 1.5, 3.0), {9.0, 4.0}, "sawtooth"},
    };
    for (const auto& [req, aim, name] : cases) {
        const fg::CurveCandidate c = fg::evaluate_candidate(req, fg::Vec2(0.0, 0.0), aim, w);
        EXPECT_TRUE(c.sampled.ok()) << name << " 采样失败：" << req.expr_a;
        EXPECT_TRUE(c.reaches) << name << " 到不了瞄点：aim_gap=" << c.aim_gap
                               << " expr=" << req.expr_a;
    }
}

TEST(Ai, KinkChainPassesThroughAllPoints) {
    // 折线链骨架：分段直线应精确穿过每个目标点（横向三点、竖向两点、朝左两点）。
    // 目标全部取在地图内（|x|≤22、|y|≤14）：采样到地图边缘外 2 格会停，超界的点测不到
    {
        const std::vector<fg::Vec2> rel = {{9, 4}, {17, -3}, {22, 6}};
        const fg::CurveRequest req = fg::make_kink_chain(true, rel);
        EXPECT_NE(req.expr_a.find("step("), std::string::npos) << "折线链应当是 step 复合形式";
        expect_passes(req, rel);
    }
    {
        const std::vector<fg::Vec2> rel = {{3, 11}, {-4, 14}};  // 竖向（y 为主轴）
        expect_passes(fg::make_kink_chain(false, rel), rel);
    }
    {
        const std::vector<fg::Vec2> rel = {{-8, 5}, {-20, -3}};  // 朝负方向
        expect_passes(fg::make_kink_chain(true, rel), rel);
    }
}

TEST(Ai, ParametricPairSkeletonsPassThroughPoints) {
    // 折点折线 / 波浪 / 抛物波浪 / 螺线 / 三点抛物波浪的所有参数变体都必须精确过点
    const fg::Vec2 a(14.0, 6.0);
    const fg::Vec2 b(22.0, -2.0);
    fg::CurveRequest req;
    // 折点位置：起点与第一目标之间一次、两目标之间两次（与 plan_hard 的扫位一致）
    for (double p : {7.0, 14.0 + 8.0 / 3.0, 14.0 + 16.0 / 3.0}) {
        ASSERT_TRUE(fg::make_kink_bend(true, a.x(), a.y(), b.x(), b.y(), p, req)) << "p=" << p;
        expect_passes(req, {a, b});
    }
    for (double waves : {0.75, 1.5}) {
        const double w = 2.0 * std::numbers::pi * waves / (b.x() - a.x());
        ASSERT_TRUE(fg::make_wave_through(true, a.x(), a.y(), b.x(), b.y(), w, req)) << "w=" << w;
        expect_passes(req, {a, b});
    }
    for (double c : {5.0, -5.0}) {
        ASSERT_TRUE(fg::make_poly_wave_through(true, a.x(), a.y(), b.x(), b.y(), c, req))
            << "c=" << c;
        expect_passes(req, {a, b});
    }
    // 三点抛物波浪：三个目标都要穿过（与 plan_hard 的频率扫位一致）。
    // 三点近似共线，解出的形状温和、不会中途拱出地图
    const fg::Vec2 t3(18.0, 2.5);
    for (double waves : {0.5, 0.75, 1.5}) {
        const double w = 2.0 * std::numbers::pi * waves / (b.x() - a.x());
        ASSERT_TRUE(fg::make_poly_wave3_through(true, a, b, t3, w, req)) << "w=" << w;
        expect_passes(req, {a, b, t3});
    }
    ASSERT_TRUE(fg::make_spiral_through(10.0, 3.0, 14.0, 8.0, req));
    expect_passes(req, {fg::Vec2(10.0, 3.0), fg::Vec2(14.0, 8.0)}, 0.2);
    // 竖向变体：折点与波浪（求解与方向无关，但竖向轴单独盯一遍）
    const fg::Vec2 c1(6.0, 5.0);
    const fg::Vec2 c2(0.0, 12.0);
    ASSERT_TRUE(fg::make_kink_bend(false, c1.x(), c1.y(), c2.x(), c2.y(),
                                   (c1.y() + c2.y()) * 0.5, req));
    expect_passes(req, {c1, c2});
    const double w2 = 2.0 * std::numbers::pi / (c2.y() - c1.y());
    ASSERT_TRUE(fg::make_wave_through(false, c1.x(), c1.y(), c2.x(), c2.y(), w2, req));
    expect_passes(req, {c1, c2});
    // 纵向三点专属骨架：横向三点在上面测过，纵向这一格曾因"跨度算错坐标轴"的 bug
    // 整批失效（同竖列排开的目标正是纵向三杀的主场），两个骨架都按 y 跨度给 w 再验。
    // 点位刻意不取整数：等距的 y 会在特定频率上让 sin 基取值退化成 0 列（生成器
    // 正确拒绝），那是数学巧合、不是这里要测的路径
    const fg::Vec2 u1(2.0, 4.3);
    const fg::Vec2 u2(3.0, 7.9);
    const fg::Vec2 u3(1.0, 12.2);
    for (double waves : {0.5, 0.75, 1.5}) {
        const double w3 = 2.0 * std::numbers::pi * waves / (u3.y() - u1.y());
        ASSERT_TRUE(fg::make_poly_wave3_through(false, u1, u2, u3, w3, req)) << "w=" << w3;
        expect_passes(req, {u1, u2, u3});
        ASSERT_TRUE(fg::make_wave3_through(false, u1, u2, u3, w3, req)) << "w=" << w3;
        expect_passes(req, {u1, u2, u3});
    }
}

TEST(Ai, MultiKillShapesAreVaried) {
    // 同种子下统计大神档的出手表达式并分三桶：sin 波浪族 / 纯多项式族
    //（无 step 无 sin）/ step 族（阶梯与折线链）。种子固定 → 结果完全确定，
    // 不是概率型 flaky 测试
    constexpr int kTrials = 40;
    int wave_shots = 0;   // 含 sin( （波浪、抛物+正弦）
    int poly_shots = 0;   // 无 step( 也无 sin( （插值抛物线族）
    int step_shots = 0;   // 含 step( （阶梯或折线链）
    for (int t = 0; t < kTrials; ++t) {
        fg::GameConfig cfg;
        cfg.match.turn_delay = 0.0;
        cfg.match.seed = static_cast<uint64_t>(2000 + t);
        cfg.match.obstacle_count = 0;  // 无障碍：多杀最容易成立的场合
        cfg.match.first_team = 0;
        fg::World w;
        std::vector<fg::PlayerSpec> specs = {
            {"A1", fg::Team::A, false, 0},
            {"B1", fg::Team::B, false, 0},
            {"B2", fg::Team::B, false, 0},
            {"B3", fg::Team::B, false, 0},
        };
        w.start(cfg, specs);
        w.take_events();
        const int cur = w.current_player_id();
        ASSERT_EQ(cur, 0);
        fg::Rng rng(static_cast<uint64_t>(9000 + t));
        const fg::AiDecision dec = fg::ai_decide(w, cur, 3, rng);
        const std::string& e = dec.request.expr_a;
        if (e.empty()) {
            continue;
        } else if (e.find("sin(") != std::string::npos) {
            ++wave_shots;
        } else if (e.find("step(") != std::string::npos) {
            ++step_shots;
        } else {
            ++poly_shots;
        }
    }
    EXPECT_GT(step_shots, 3) << "step 族一次都没出现（骨架池或选择有问题）";
    // 命中人数优先口径下，无障碍局的最大命中档本来就是"过全组点的折线链/阶梯"
    // 的主场（其它骨架要么只过两点、要么插值震荡）——旧口径要求"纯多项式族
    // 也有配额"，那靠的是"偶尔故意少打一个"的降档，已随多杀口径移除。现在只
    // 盯"非阶梯族完全绝迹"这一条：同命中数内形状仍按类型平权抽签，彻底只剩
    // 一族说明抽签通道坏了
    EXPECT_GT(poly_shots + wave_shots, 0) << "非阶梯族完全绝迹（类型平权失效）";
}

TEST(Ai, BlockedLineDoesNotCountAsReach) {
    // 直线路径上横着障碍时：炮弹先撞墙、到不了瞄准点，reaches 必须为 false。
    // 旧口径只按"几何经过瞄准点"判定，会把这种线当成可打的候选，AI 因此会
    // "明知有墙硬撞"（看着就像不会绕障碍）。
    // 场景在种子空间里现搜（不依赖某张固定地图的障碍分布）：直线被挡（用
    // obstacle_blocks 逐步独立判定）且拱线能打到的障碍才采用
    fg::GameConfig cfg;
    cfg.match.obstacle_count = 10;
    fg::World w;
    bool found = false;
    fg::Vec2 origin(0.0, 0.0), aim(0.0, 0.0);
    fg::CurveRequest line, arc;
    for (uint64_t seed = 1; seed <= 300 && !found; ++seed) {
        cfg.match.seed = seed;
        w.start(cfg, make_specs(1));
        for (const auto& ob : w.obstacles()) {
            const double half = ob.is_circle ? ob.radius : std::max(ob.half_w, ob.half_h);
            if (std::abs(ob.center.x()) > 8.0 || std::abs(ob.center.y()) > 1.0) continue;
            const fg::Vec2 o(ob.center.x() - (half + 10.0), ob.center.y());
            const fg::Vec2 a(ob.center.x() + (half + 10.0), ob.center.y());
            const double dxr = a.x() - o.x();
            const fg::CurveRequest l = fg::make_parabola(true, dxr, 0.0, 0.0);
            const fg::CurveResult s = fg::sample_curve(l, o, w.config());
            if (!s.ok()) continue;
            bool direct_blocked = false;
            for (const auto& p : s.path.points) {
                for (const auto& ob2 : w.obstacles()) {
                    if (fg::obstacle_blocks(ob2, p, w.config().world.crater_radius)) {
                        direct_blocked = true;
                        break;
                    }
                }
                if (direct_blocked) break;
            }
            if (!direct_blocked) continue;
            const fg::CurveRequest r = fg::make_parabola(true, dxr, 0.0, half * 2.0 + 8.0);
            const fg::CurveCandidate ca = fg::evaluate_candidate(r, o, a, w);
            if (!ca.reaches) continue;  // 这张地图上拱线过不去，换个场景
            found = true;
            origin = o;
            aim = a;
            line = l;
            arc = r;
            break;
        }
    }
    ASSERT_TRUE(found) << "300 个种子里没搜到可用场景（直线被挡且高拱能打到）";

    const fg::CurveCandidate blocked = fg::evaluate_candidate(line, origin, aim, w);
    ASSERT_TRUE(blocked.sampled.ok());
    EXPECT_FALSE(blocked.reaches) << "撞墙线被算成了打得到";
    EXPECT_FALSE(blocked.clear) << "撞墙线被算成了全程干净";

    const fg::CurveCandidate over = fg::evaluate_candidate(arc, origin, aim, w);
    ASSERT_TRUE(over.sampled.ok());
    EXPECT_TRUE(over.reaches) << "高拱线没能打到（表达式: " << arc.expr_a << "）";
}

TEST(Ai, GodAiPrefersCleanCurveOverBlastingObstacles) {
    // 大神级=不会犯错：直线被障碍挡死、但存在"到得了且全程不碰障碍"的曲线时，
    // 必须选干净的那条——炸障碍开路是最后手段（旧择优只看命中数与长度：直线短，
    // 总赢过绕行曲线，观感就是"AI 一直用直线炸对面、把障碍炸来炸去"）。
    // 场景在种子空间里现搜：真实对局局面（出手士兵 ↔ 目标）之间直线被挡、
    // 且至少一条常规拱高曲线既到得了又干净
    fg::GameConfig cfg;
    cfg.match.obstacle_count = 10;
    cfg.match.turn_delay = 0.0;
    cfg.match.first_team = 0;
    cfg.room.soldiers_per_player = 1;  // 1v1：目标唯一，"有干净解就必须找到"才严格
    fg::World w;
    uint64_t good_seed = 0;
    for (uint64_t seed = 1; seed <= 400 && good_seed == 0; ++seed) {
        cfg.match.seed = seed;
        w.start(cfg, make_specs(1));
        const fg::Vec2 o = w.players()[0].current().pos;
        const fg::Vec2 a = w.players()[1].soldiers[0].pos;
        const double dx = a.x() - o.x();
        const double dy = a.y() - o.y();
        if (std::abs(dx) < 4.0) continue;  // 单值曲线只朝一侧延伸，太近造不出形状
        const fg::CurveCandidate bl =
            fg::evaluate_candidate(fg::make_parabola(true, dx, dy, 0.0), o, a, w);
        // 场景条件：直线打不到人（撞障碍在前，reaches 必须为假）且确实撞了障碍
        if (!bl.sampled.ok() || bl.clear || bl.reaches) continue;
        bool clean_exists = false;
        for (double bulge : {3.0, 5.0, 8.0, 12.0, 16.0}) {
            for (double sign : {1.0, -1.0}) {
                const fg::CurveCandidate c = fg::evaluate_candidate(
                    fg::make_parabola(true, dx, dy, sign * bulge), o, a, w);
                if (c.sampled.ok() && c.reaches && c.clear) clean_exists = true;
            }
        }
        if (clean_exists) good_seed = seed;
    }
    ASSERT_NE(good_seed, 0u) << "400 个种子里没搜到场景（直线被挡 + 存在干净曲线）";

    cfg.match.seed = good_seed;
    w.start(cfg, make_specs(1));
    fg::Rng rng(20261015);
    const int cur = w.current_player_id();
    const fg::AiDecision dec = fg::ai_decide(w, cur, 3, rng);  // 大神档
    ASSERT_TRUE(w.submit(cur, dec.request).ok());

    const fg::CurveResult sr =
        fg::sample_curve(dec.request, w.players()[0].current().pos, w.config());
    ASSERT_TRUE(sr.ok()) << sr.error.message;
    bool dirty = false;
    for (const auto& p : sr.path.points) {
        for (const auto& ob : w.obstacles()) {
            if (fg::obstacle_blocks(ob, p, w.config().world.crater_radius)) {
                dirty = true;
                break;
            }
        }
        if (dirty) break;
    }
    EXPECT_FALSE(dirty) << "seed=" << good_seed << "：存在干净曲线，大神 AI 却选了炸障碍的线";
}
