// 曲线模块的单元测试：定位平移规则、各模式、弧长参数化、镜像提交与 y''
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "curve.h"

namespace {

// 辅助：找路径上 x 最接近 xq 的点的 y 值
double y_at_x(const fg::Path& path, double xq) {
    double best_dx = 1e18;
    double best_y = 0.0;
    for (const auto& p : path.points) {
        const double dx = std::abs(p.x() - xq);
        if (dx < best_dx) {
            best_dx = dx;
            best_y = p.y();
        }
    }
    return best_y;
}

// 辅助：找路径上 y 最接近 yq 的点的 x 值
double x_at_y(const fg::Path& path, double yq) {
    double best_dy = 1e18;
    double best_x = 0.0;
    for (const auto& p : path.points) {
        const double dy = std::abs(p.y() - yq);
        if (dy < best_dy) {
            best_dy = dy;
            best_x = p.x();
        }
    }
    return best_x;
}

fg::CurveResult sample(const fg::CurveRequest& req, const fg::Vec2& origin, const fg::GameConfig& cfg = {}) {
    return fg::sample_curve(req, origin, cfg);
}

}  // namespace

TEST(Curve, YofXAnchorsAtPlayer) {
    // 规则用例：玩家 (2,10) 输入 cos(x) → 轨迹 y = cos(x-2) + 9
    fg::CurveRequest req;
    req.mode = fg::CurveMode::YofX;
    req.expr_a = "cos(x)";
    req.dir = fg::FireDir::Positive;
    const fg::Vec2 origin(2.0, 10.0);

    auto res = sample(req, origin);
    ASSERT_TRUE(res.ok()) << res.error.message;

    EXPECT_NEAR(res.path.points.front().x(), 2.0, 1e-9);
    EXPECT_NEAR(res.path.points.front().y(), 10.0, 1e-9);  // 起点即玩家位置
    EXPECT_NEAR(y_at_x(res.path, 2.0), 10.0, 1e-6);
    EXPECT_NEAR(y_at_x(res.path, 3.0), 9.0 + std::cos(1.0), 0.05);
}

TEST(Curve, ConstantTermHasNoEffect) {
    // 常数项只影响锚点高度，而锚点最终要被搬到玩家身上，因此轨迹完全相同
    fg::CurveRequest r1, r2;
    r1.mode = r2.mode = fg::CurveMode::YofX;
    r1.expr_a = "x*x + 3*x + 5";
    r2.expr_a = "x*x + 3*x";
    const fg::Vec2 origin(-3.0, 7.0);

    auto a = sample(r1, origin);
    auto b = sample(r2, origin);
    ASSERT_TRUE(a.ok()) << a.error.message;
    ASSERT_TRUE(b.ok()) << b.error.message;
    ASSERT_EQ(a.path.points.size(), b.path.points.size());
    for (size_t i = 0; i < a.path.points.size(); ++i) {
        EXPECT_NEAR(a.path.points[i].x(), b.path.points[i].x(), 1e-12);
        EXPECT_NEAR(a.path.points[i].y(), b.path.points[i].y(), 1e-12);
    }
}

TEST(Curve, NegativeDirection) {
    fg::CurveRequest req;
    req.mode = fg::CurveMode::YofX;
    req.expr_a = "x";
    req.dir = fg::FireDir::Negative;
    const fg::Vec2 origin(0.0, 0.0);

    auto res = sample(req, origin);
    ASSERT_TRUE(res.ok()) << res.error.message;
    ASSERT_GE(res.path.points.size(), 3u);
    EXPECT_LT(res.path.points[1].x(), res.path.points[0].x());
    EXPECT_LT(res.path.points[2].x(), res.path.points[1].x());
}

TEST(Curve, XofYMirrored) {
    // x = y*y：起点在玩家，全局满足 x - x0 = (y - y0)^2
    fg::CurveRequest req;
    req.mode = fg::CurveMode::XofY;
    req.expr_a = "y*y";
    const fg::Vec2 origin(1.0, -2.0);

    auto res = sample(req, origin);
    ASSERT_TRUE(res.ok()) << res.error.message;
    EXPECT_NEAR(res.path.points.front().x(), 1.0, 1e-9);
    EXPECT_NEAR(res.path.points.front().y(), -2.0, 1e-9);
    for (const auto& p : res.path.points) {
        const double dy = p.y() - origin.y();
        EXPECT_NEAR(p.x() - origin.x(), dy * dy, 0.05);
    }
}

TEST(Curve, PolarCircle) {
    // r = 5（默认 θ ∈ [0, 2π]）：原曲线是圆心在极点、从 (5,0) 出发的整圆；
    // 按统一规则锚点 (5,0) 被平移到玩家位置，所以平移后圆心在玩家的左侧 5 处。
    fg::CurveRequest req;
    req.mode = fg::CurveMode::Polar;
    req.expr_a = "5";
    const fg::Vec2 origin(0.0, 0.0);

    auto res = sample(req, origin);
    ASSERT_TRUE(res.ok()) << res.error.message;
    EXPECT_NEAR(res.path.points.front().x(), 0.0, 1e-9);
    EXPECT_NEAR(res.path.points.front().y(), 0.0, 1e-9);
    const fg::Vec2 center = origin + fg::Vec2(-5.0, 0.0);
    for (const auto& p : res.path.points) {
        EXPECT_NEAR(fg::distance(p, center), 5.0, 0.02);
    }
    // 扫满一圈后回到起点附近
    EXPECT_NEAR(fg::distance(res.path.points.back(), origin), 0.0, 0.05);
}

TEST(Curve, ParametricParabola) {
    // (t, t^2)，t ∈ [0, 2]：全局满足 y - y0 = (x - x0)^2
    fg::CurveRequest req;
    req.mode = fg::CurveMode::Parametric;
    req.expr_a = "t";
    req.expr_b = "t*t";
    req.p_start = 0.0;
    req.p_end = 2.0;
    const fg::Vec2 origin(1.0, -2.0);

    auto res = sample(req, origin);
    ASSERT_TRUE(res.ok()) << res.error.message;
    EXPECT_NEAR(res.path.points.front().x(), 1.0, 1e-9);
    EXPECT_NEAR(res.path.points.front().y(), -2.0, 1e-9);
    for (const auto& p : res.path.points) {
        const double dx = p.x() - origin.x();
        EXPECT_NEAR(p.y() - origin.y(), dx * dx, 1e-3);
    }
}

TEST(Curve, ParametricTravelBudget) {
    // 绕圈曲线 (10*cos t, 10*sin t)，t 给到 1000：参数方程的行程预算按地图算
    // （2 倍斜对角线，默认地图 48×30 → 约 113.2），不是全局 max_travel(300)——
    // t 没有几何含义，曲线能绕圈折返，预算得跟地图大小挂钩
    fg::CurveRequest req;
    req.mode = fg::CurveMode::Parametric;
    req.expr_a = "10*cos(t)";
    req.expr_b = "10*sin(t)";
    req.p_start = 0.0;
    req.p_end = 1000.0;

    auto res = sample(req, fg::Vec2(0.0, 0.0));
    ASSERT_TRUE(res.ok()) << res.error.message;
    const double budget = 2.0 * std::hypot(48.0, 30.0);
    // 截断时最后一步最多再超出一步的弧长（步长约 0.25，留点余量）
    EXPECT_LE(res.path.length(), budget + 0.5);
    // 确实绕了近两圈（周长 2π×10 ≈ 62.8），不是提前截断
    EXPECT_GT(res.path.length(), budget - 1.0);
}

TEST(Curve, ParametricDefaultRangeRunsToEdge) {
    // 不填参数范围时：t 默认扫 0→1000（而不是旧的 10）——(t, t) 这类曲线会一直
    // 画到出地图（约 21 单位）才停，不再被"t 用尽"卡在半路（旧默认 10 只到 14.1）
    fg::CurveRequest req;
    req.mode = fg::CurveMode::Parametric;
    req.expr_a = "t";
    req.expr_b = "t";

    auto res = sample(req, fg::Vec2(0.0, 0.0));
    ASSERT_TRUE(res.ok()) << res.error.message;
    EXPECT_GT(res.path.length(), 20.0) << "默认范围没有放开（还是被 t=10 卡住）";
}

TEST(Curve, Ode1Solution) {
    // dy/dx = x，从 (0,0) 出发 → y = x^2 / 2
    fg::CurveRequest req;
    req.mode = fg::CurveMode::Ode1;
    req.expr_a = "x";
    const fg::Vec2 origin(0.0, 0.0);

    auto res = sample(req, origin);
    ASSERT_TRUE(res.ok()) << res.error.message;
    EXPECT_NEAR(res.path.points.front().x(), 0.0, 1e-12);
    EXPECT_NEAR(res.path.points.front().y(), 0.0, 1e-12);
    for (const auto& p : res.path.points) {
        EXPECT_NEAR(p.y(), p.x() * p.x() / 2.0, 1e-3);
    }
}

TEST(Curve, Ode1DirectionNegative) {
    // dy/dx = 1 沿 -x 方向积分：x 递减、y 同步递减，从原点出发的轨迹是 y = x
    fg::CurveRequest req;
    req.mode = fg::CurveMode::Ode1;
    req.expr_a = "1";
    req.dir = fg::FireDir::Negative;
    const fg::Vec2 origin(0.0, 0.0);

    auto res = sample(req, origin);
    ASSERT_TRUE(res.ok()) << res.error.message;
    ASSERT_GE(res.path.points.size(), 3u);
    EXPECT_LT(res.path.points[1].x(), res.path.points[0].x());
    for (const auto& p : res.path.points) {
        EXPECT_NEAR(p.y(), p.x(), 1e-9);
    }
}

TEST(Curve, Ode2Line) {
    // (x', y') = (1, 1)：从任一点出发都是 y = x + c 的直线
    fg::CurveRequest req;
    req.mode = fg::CurveMode::Ode2;
    req.expr_a = "1";
    req.expr_b = "1";
    const fg::Vec2 origin(3.0, 3.0);

    auto res = sample(req, origin);
    ASSERT_TRUE(res.ok()) << res.error.message;
    for (const auto& p : res.path.points) {
        EXPECT_NEAR(p.y() - p.x(), 0.0, 1e-6);
    }
}

TEST(Curve, PathArcLengthInterpolation) {
    // y = 0：路径是水平直线，按弧长取点应精确
    fg::CurveRequest req;
    req.mode = fg::CurveMode::YofX;
    req.expr_a = "0";
    const fg::Vec2 origin(0.0, 0.0);

    auto res = sample(req, origin);
    ASSERT_TRUE(res.ok()) << res.error.message;
    EXPECT_NEAR(res.path.at(0.0).x(), 0.0, 1e-9);
    EXPECT_NEAR(res.path.at(10.0).x(), 10.0, 1e-6);
    EXPECT_NEAR(res.path.at(10.0).y(), 0.0, 1e-9);
    EXPECT_DOUBLE_EQ(res.path.at(-5.0).x(), 0.0);                        // 越界钳制到起点
    EXPECT_DOUBLE_EQ(res.path.at(1e9).x(), res.path.points.back().x());  // 越界钳制到终点
    EXPECT_NEAR(res.path.length(), res.path.points.back().x(), 1e-6);
}

TEST(Curve, ErrorsAreReported) {
    fg::GameConfig cfg;
    const fg::Vec2 origin(0.0, 0.0);

    fg::CurveRequest bad_expr;
    bad_expr.mode = fg::CurveMode::YofX;
    bad_expr.expr_a = "x +";
    EXPECT_FALSE(sample(bad_expr, origin, cfg).ok());

    fg::CurveRequest singular;
    singular.mode = fg::CurveMode::YofX;
    singular.expr_a = "log(x)";  // f(0) = -inf
    auto r = sample(singular, origin, cfg);
    EXPECT_FALSE(r.ok());
    EXPECT_FALSE(r.error.ok());

    fg::CurveRequest polar_empty;
    polar_empty.mode = fg::CurveMode::Polar;
    polar_empty.expr_a = "1";
    polar_empty.p_start = 1.0;
    polar_empty.p_end = 1.0;
    EXPECT_FALSE(sample(polar_empty, origin, cfg).ok());

    // 参数方程对 t 不设上限：1e6 的范围照收，截断交给长度预算与地图边界；
    // "参数范围过大"只对极坐标生效
    fg::CurveRequest huge_t;
    huge_t.mode = fg::CurveMode::Parametric;
    huge_t.expr_a = "cos(t)";
    huge_t.expr_b = "sin(t)";
    huge_t.p_start = 0.0;
    huge_t.p_end = 1e6;
    auto huge = sample(huge_t, origin, cfg);
    ASSERT_TRUE(huge.ok()) << huge.error.message;
    EXPECT_LE(huge.path.length(), 2.0 * std::hypot(48.0, 30.0) + 2.0);

    fg::CurveRequest polar_too_wide;
    polar_too_wide.mode = fg::CurveMode::Polar;
    polar_too_wide.expr_a = "1";
    polar_too_wide.p_end = 1e6;
    EXPECT_FALSE(sample(polar_too_wide, origin, cfg).ok());
}

// ---- 战场坐标系（房间属性 coord_origin = Field）----

TEST(Curve, FieldOriginYofX) {
    // field 参考系：y=f(x) 的自变量是地图横坐标，曲线竖直平移 y0-f(x0) 过士兵。
    // 玩家 (2,10) 写 x*x+3*x+5：f(2)=15，世界曲线 y = f(x) - 5 = x²+3x
    fg::GameConfig cfg;
    cfg.room.coord_origin = fg::CoordOrigin::Field;
    // 放宽地图边界：默认 ±24/±15 下 y=x²+3x 在 x≈3 就出界被截停，测不到后面的断言
    cfg.world.map_y_min = -100.0;
    cfg.world.map_y_max = 100.0;
    fg::CurveRequest req;
    req.mode = fg::CurveMode::YofX;
    req.expr_a = "x*x + 3*x + 5";
    req.dir = fg::FireDir::Positive;
    const fg::Vec2 origin(2.0, 10.0);

    auto res = sample(req, origin, cfg);
    ASSERT_TRUE(res.ok()) << res.error.message;
    EXPECT_NEAR(res.path.points.front().x(), 2.0, 1e-9);
    EXPECT_NEAR(res.path.points.front().y(), 10.0, 1e-9);  // 起点即士兵位置
    // x=3 处：f(3)+10-f(2) = 23-5 = 18（直接把地图坐标代进表达式）
    EXPECT_NEAR(y_at_x(res.path, 3.0), 18.0, 0.05);
    // x=4 处：f(4)=33，33-5 = 28
    EXPECT_NEAR(y_at_x(res.path, 4.0), 28.0, 0.1);
}

TEST(Curve, FieldOriginStepAimsAtOpponent) {
    // 验收例：对手 (5,6)、我 (-1,2)，写 4*step(x-5)（按地图坐标直接写对手位置）。
    // field 语义 = 曲线往上平移 y0-f(x0)：f(-1)=0 → 平移量 2（就是两人的高度差），
    // step 转折点顶端落在 (5,6)，正好压在对手身上
    fg::GameConfig cfg;
    cfg.room.coord_origin = fg::CoordOrigin::Field;
    fg::CurveRequest req;
    req.mode = fg::CurveMode::YofX;
    req.expr_a = "4*step(x-5)";
    req.dir = fg::FireDir::Positive;  // 对手在右侧，向右发射
    const fg::Vec2 origin(-1.0, 2.0);

    auto res = sample(req, origin, cfg);
    ASSERT_TRUE(res.ok()) << res.error.message;
    EXPECT_NEAR(res.path.points.front().x(), -1.0, 1e-9);
    EXPECT_NEAR(res.path.points.front().y(), 2.0, 1e-9);  // 起点即士兵位置
    EXPECT_NEAR(y_at_x(res.path, 4.9), 2.0, 1e-6);        // 转折前贴着我方高度
    EXPECT_NEAR(y_at_x(res.path, 5.0), 6.0, 1e-6);        // 转折点顶端 = 对手位置
}

TEST(Curve, FieldOriginConstantTermHasNoEffect) {
    // field 参考系下常数同样被"平移过士兵"抵消（起点函数值减法）
    fg::GameConfig cfg;
    cfg.room.coord_origin = fg::CoordOrigin::Field;
    fg::CurveRequest r1, r2;
    r1.mode = r2.mode = fg::CurveMode::YofX;
    r1.expr_a = "x*x + 3*x + 5";
    r2.expr_a = "x*x + 3*x";
    const fg::Vec2 origin(-3.0, 7.0);

    auto a = sample(r1, origin, cfg);
    auto b = sample(r2, origin, cfg);
    ASSERT_TRUE(a.ok()) << a.error.message;
    ASSERT_TRUE(b.ok()) << b.error.message;
    ASSERT_EQ(a.path.points.size(), b.path.points.size());
    for (size_t i = 0; i < a.path.points.size(); ++i) {
        EXPECT_NEAR(a.path.points[i].x(), b.path.points[i].x(), 1e-12);
        EXPECT_NEAR(a.path.points[i].y(), b.path.points[i].y(), 1e-12);
    }
}

TEST(Curve, FieldOriginXofY) {
    // x=f(y) 对称：y 是地图纵坐标，曲线水平平移 x0-f(y0)
    fg::GameConfig cfg;
    cfg.room.coord_origin = fg::CoordOrigin::Field;
    fg::CurveRequest req;
    req.mode = fg::CurveMode::XofY;
    req.expr_a = "y*y";
    req.dir = fg::FireDir::Positive;  // 向上
    const fg::Vec2 origin(2.0, 10.0);

    auto res = sample(req, origin, cfg);
    ASSERT_TRUE(res.ok()) << res.error.message;
    EXPECT_NEAR(res.path.points.front().x(), 2.0, 1e-9);
    EXPECT_NEAR(res.path.points.front().y(), 10.0, 1e-9);  // 起点即士兵位置
    // y=11 处：f(11)+2-f(10) = 121-98 = 23
    EXPECT_NEAR(x_at_y(res.path, 11.0), 23.0, 0.05);
}

TEST(Curve, FieldOriginMirrorUsesScreenCoords) {
    // 镜像房间：B 队玩家的自变量是他在屏幕上读到的坐标（世界 x 取负），
    // 曲线在他自己的坐标系里平移过士兵——所见即所写
    fg::GameConfig cfg;
    cfg.room.coord_origin = fg::CoordOrigin::Field;
    fg::CurveRequest req;
    req.mode = fg::CurveMode::YofX;
    req.expr_a = "x";  // 他写屏幕坐标下的 45° 斜线
    req.dir = fg::FireDir::Positive;
    req.mirror = true;
    const fg::Vec2 origin(-5.0, 3.0);  // B 队玩家站在世界左侧

    auto res = sample(req, origin, cfg);
    ASSERT_TRUE(res.ok()) << res.error.message;
    EXPECT_NEAR(res.path.points.front().x(), -5.0, 1e-9);
    EXPECT_NEAR(res.path.points.front().y(), 3.0, 1e-9);
    // 屏幕坐标 u=-x：世界曲线 y = u + (3-5) = -x - 2；x=-6 处 y=4
    EXPECT_NEAR(y_at_x(res.path, -6.0), 4.0, 0.05);
}

// ---- 镜像提交 ----

TEST(Curve, MirrorIsReflectionOfUnmirrored) {
    // 镜像提交：镜像轨迹的点集应等于普通轨迹关于"过锚点的竖轴"翻转。
    // 参数族在局部空间生成、最后统一 place_local，对任意表达式成立；
    // 微分方程族在"场不依赖 x"时成立（解只依赖初值与场，对称性地翻转）。
    // 这条性质是"所见即所写"的兜底：不成立的话 B 队玩家会看着曲线飞向另一侧
    const fg::Vec2 origin(5.0, 2.0);
    struct Case {
        fg::CurveMode mode;
        const char* a;
        const char* b;
    };
    const Case cases[] = {
        {fg::CurveMode::YofX, "0.2*x*x - 0.5*x", ""},
        {fg::CurveMode::XofY, "0.2*y*y - 0.5*y", ""},
        {fg::CurveMode::Polar, "2 + theta/4", ""},
        // 参数方程用绕圈曲线：直线类曲线扫 0→1000 会先被地图边界截断，
        // 而两侧出界点不同（锚点不在图正中），截断点对不上，测不出镜像性质；
        // 绕圈曲线会在界内被长度预算截断，两侧截断点一致
        {fg::CurveMode::Parametric, "cos(t)", "sin(t)"},
        {fg::CurveMode::Ode1, "0.2*y - 0.1", ""},
        {fg::CurveMode::Ode2, "1", "cos(t) - 0.2*y"},
        {fg::CurveMode::Ypp, "-0.25", ""},
    };
    fg::GameConfig cfg;
    cfg.world.max_travel = 5.0;  // 曲线都很短：两侧同样不触界，截断点一致
                                 // （参数方程不看 max_travel，截断由长度预算决定）

    for (const auto& c : cases) {
        fg::CurveRequest req;
        req.mode = c.mode;
        req.expr_a = c.a;
        req.expr_b = c.b;
        req.angle = 0.4;  // 只有 y'' 读取，其余模式忽略
        const fg::CurveResult plain = sample(req, origin, cfg);
        ASSERT_TRUE(plain.ok()) << "mode=" << static_cast<int>(c.mode) << ": "
                                << plain.error.message;
        req.mirror = true;
        const fg::CurveResult mir = sample(req, origin, cfg);
        ASSERT_TRUE(mir.ok()) << "mode=" << static_cast<int>(c.mode) << ": "
                              << mir.error.message;

        // 双向最近点距离：采样点错位在半个步长内，容差放宽到 0.25
        const auto worst = [&origin](const fg::Path& from, const fg::Path& to) {
            double w = 0.0;
            for (const auto& p : from.points) {
                double best = 1e18;
                for (const auto& q : to.points) {
                    const fg::Vec2 flipped(2.0 * origin.x() - q.x(), q.y());
                    best = std::min(best, fg::distance(p, flipped));
                }
                w = std::max(w, best);
            }
            return w;
        };
        EXPECT_LT(worst(mir.path, plain.path), 0.25) << "mode=" << static_cast<int>(c.mode);
        EXPECT_LT(worst(plain.path, mir.path), 0.25) << "mode=" << static_cast<int>(c.mode);
    }
}

// ---- y'' = f(x, y, dy) ----

TEST(Curve, YppConstantAccelIsParabola) {
    // y'' = -0.25（常值加速度）、发射角 25°：解析解 y = -0.125*x^2 + tan(25°)*x（相对锚点）。
    // 负方向发射时仰角相对行进方向，数学斜率取负——这是角度的语义锚点
    const double angle = 25.0 * std::numbers::pi / 180.0;
    for (const auto dir : {fg::FireDir::Positive, fg::FireDir::Negative}) {
        fg::CurveRequest req;
        req.mode = fg::CurveMode::Ypp;
        req.expr_a = "-0.25";
        req.dir = dir;
        req.angle = angle;
        const fg::Vec2 origin(0.0, 0.0);  // 起点在原点：全局坐标即相对坐标
        const fg::CurveResult res = sample(req, origin);
        ASSERT_TRUE(res.ok()) << res.error.message;
        ASSERT_GE(res.path.points.size(), 10u);
        const double k = std::tan(angle) * (dir == fg::FireDir::Positive ? 1.0 : -1.0);
        for (const auto& p : res.path.points) {
            const double expect = -0.125 * p.x() * p.x() + k * p.x();
            EXPECT_NEAR(p.y(), expect, 1e-3) << "x=" << p.x();
        }
    }
}

TEST(Curve, YppZeroAccelIsStraight) {
    // y'' = 0、无发射角：沿 +x 的直线
    fg::CurveRequest req;
    req.mode = fg::CurveMode::Ypp;
    req.expr_a = "0";
    req.dir = fg::FireDir::Positive;
    const fg::CurveResult res = sample(req, fg::Vec2(0.0, 0.0));
    ASSERT_TRUE(res.ok()) << res.error.message;
    ASSERT_GE(res.path.points.size(), 3u);
    for (const auto& p : res.path.points) {
        EXPECT_NEAR(p.y(), 0.0, 1e-9);
        EXPECT_GE(p.x(), 0.0);
    }
}

TEST(Curve, YppRejectsNearVerticalAngle) {
    fg::CurveRequest req;
    req.mode = fg::CurveMode::Ypp;
    req.expr_a = "0";
    req.angle = 1.6;  // 超过 1.5 弧度：tan 爆掉、x 不再是有效自变量
    const fg::CurveResult bad = sample(req, fg::Vec2(0.0, 0.0));
    EXPECT_FALSE(bad.ok());
    EXPECT_EQ(bad.error.code, "curve_bad_angle");
    req.angle = 1.4;  // 边界内可用
    EXPECT_TRUE(sample(req, fg::Vec2(0.0, 0.0)).ok());
}

TEST(Curve, YppDivergentFieldTerminatesSafely) {
    // 正反馈场 y'' = 2*dy：斜率指数爆炸。采样应安全结束（出界截断或没有有效轨迹），
    // 不允许死循环，也不允许把非有限点混进路径
    fg::CurveRequest req;
    req.mode = fg::CurveMode::Ypp;
    req.expr_a = "2*dy";
    const fg::CurveResult res = sample(req, fg::Vec2(0.0, 0.0));
    if (res.ok()) {
        for (const auto& p : res.path.points) {
            EXPECT_TRUE(std::isfinite(p.x()) && std::isfinite(p.y()));
        }
    } else {
        EXPECT_EQ(res.error.code, "curve_no_trail");
    }
}
