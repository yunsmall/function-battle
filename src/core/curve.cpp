#include "curve.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "expr.h"

// 坐标系语义（全项目最容易写错的一处，这里一次说清）：
//
// 直角/极坐标/参数三种模式用"玩家局部坐标"：自变量（x、y、theta、t）从玩家的位置
// 量起、从 0 开始（发射方向由 FireDir 决定：Negative 时自变量取负递增），
// 函数值是相对玩家的偏移。采样出的局部点由 finalize 整体平移到玩家位置；
// 直角模式的竖直平移量是起点函数值 f(0)，所以表达式加任意常数不影响弹道形状
// （不减这一步，输入常数 H 就是一条任意高度的水平激光）。
//
// 房间属性 coord_origin = Field 时，y=f(x) / x=f(y) 两种直角模式改用"地图坐标"：
// 自变量就是地图上的真实坐标（镜像房间里是玩家屏幕读到的负值，所见即所写），
// 曲线按形状画出后整体平移使起点落在士兵身上（竖直/水平平移，起点函数值同样被
// 抵消——常数项依然不影响弹道）。极坐标/参数/微分方程三种模式与该属性无关。
//
// 微分方程三类模式直接用地图坐标：初值取玩家所在位置、朝 FireDir 方向积分，
// 因此 f(x,y) 里的 x、y 就是世界坐标（帮助面板中给玩家的说明与此对应）。

namespace fg {
namespace {

// 采样参数：步长决定折线对真实曲线的逼近精度
constexpr double kDxStep = 0.02;      // y=f(x) / x=f(y) 的自变量步长
constexpr double kParamStep = 0.01;   // 极坐标 theta 与参数方程 t 的步长
constexpr double kOdeStep = 0.05;     // dy/dx=f(x,y) 的 RK4 步长
constexpr double kOde2Step = 0.02;    // (x',y')=(f,g) 的 RK4 步长
constexpr size_t kMaxPoints = 40000;  // 采样点上限（防玩家写出发散曲线拖垮计算）
constexpr double kOutMargin = 2.0;    // 超出地图这么多就停止采样（炮弹在那里也会出界销毁）
constexpr double kMaxParamSpan = 1000.0;  // 极坐标的参数范围上限；参数方程只拿它当不填范围时的默认扫描终点

bool in_bounds(const Vec2& p, const GameConfig& cfg) {
    return p.x() >= cfg.world.map_x_min - kOutMargin && p.x() <= cfg.world.map_x_max + kOutMargin &&
           p.y() >= cfg.world.map_y_min - kOutMargin && p.y() <= cfg.world.map_y_max + kOutMargin;
}

Path build_path(std::vector<Vec2> pts) {
    Path path;
    path.points = std::move(pts);
    path.cum.resize(path.points.size(), 0.0);
    for (size_t i = 1; i < path.points.size(); ++i) {
        path.cum[i] = path.cum[i - 1] + (path.points[i] - path.points[i - 1]).norm();
    }
    return path;
}

// 局部点（锚点已在原点）→ 全局。镜像时先关于"过锚点的竖轴"翻转再平移：
// B 队玩家的屏幕也做了同样的镜像，所以对他来说曲线形状与方向完全没变
Vec2 place_local(const Vec2& p, const Vec2& origin, bool mirror) {
    return mirror ? Vec2(origin.x() - p.x(), origin.y() + p.y()) : p + origin;
}

// 把局部点（锚点已在原点）成路到全局
Path finalize(std::vector<Vec2> local, const Vec2& origin, bool mirror) {
    for (auto& p : local) p = place_local(p, origin, mirror);
    return build_path(std::move(local));
}

// 直角坐标 y = f(x)：自变量 t 从 0 出发（Positive 向 +x，Negative 向 -x）。
// Soldier 参考系：局部点 (t, f(t) - f(0))；
// Field 参考系：自变量取地图横坐标 u = base + t（base = 士兵横坐标，镜像取负），
// 局部点 (t, f(u) - f(base))，平移后世界曲线 = f(u) + y0 - f(x0)
CurveResult sample_y_of_x(const CurveRequest& req, const Vec2& origin, const GameConfig& cfg) {
    Error err;
    auto expr = compile_expression(req.expr_a, {"x"}, err);
    if (!expr) return {.error = err};

    const bool field = (cfg.room.coord_origin == CoordOrigin::Field);
    const double base = field ? (req.mirror ? -origin.x() : origin.x()) : 0.0;
    expr->set_var("x", base);
    double f0 = 0.0;
    if (!expr->eval_finite(f0))
        return {.error = {"curve_bad_f0", "曲线起点处的函数值不是有限数值，无法确定发射点"}};

    const double sign = (req.dir == FireDir::Positive) ? 1.0 : -1.0;
    std::vector<Vec2> pts;
    pts.reserve(4096);
    double travel = 0.0;
    for (size_t i = 0; i < kMaxPoints; ++i) {
        const double t = sign * static_cast<double>(i) * kDxStep;
        expr->set_var("x", field ? (base + t) : t);
        double v = 0.0;
        if (!expr->eval_finite(v)) break;  // 曲线在此断开（如 1/x 的奇点）
        const Vec2 p(t, v - f0);
        if (i > 0) travel += (p - pts.back()).norm();
        if (!in_bounds(place_local(p, origin, req.mirror), cfg)) break;  // 先判界：非有限/出界的点不进路径
        pts.push_back(p);
        if (travel > cfg.world.max_travel) break;
    }
    if (pts.size() < 2) return {.error = {"curve_no_trail", "曲线没有形成有效轨迹"}};
    return {.path = finalize(std::move(pts), origin, req.mirror)};
}

// 直角坐标 x = f(y)：局部点 (f(t) - f(0), t)。
// Field 参考系：自变量取地图纵坐标 v = base + t（base = 士兵纵坐标，y 轴不受镜像影响），
// 局部点 (f(v) - f(base), t)，平移后世界曲线 = f(v) + x0 - f(y0)
CurveResult sample_x_of_y(const CurveRequest& req, const Vec2& origin, const GameConfig& cfg) {
    Error err;
    auto expr = compile_expression(req.expr_a, {"y"}, err);
    if (!expr) return {.error = err};

    const bool field = (cfg.room.coord_origin == CoordOrigin::Field);
    const double base = field ? origin.y() : 0.0;
    expr->set_var("y", base);
    double f0 = 0.0;
    if (!expr->eval_finite(f0))
        return {.error = {"curve_bad_f0", "曲线起点处的函数值不是有限数值，无法确定发射点"}};

    const double sign = (req.dir == FireDir::Positive) ? 1.0 : -1.0;
    std::vector<Vec2> pts;
    pts.reserve(4096);
    double travel = 0.0;
    for (size_t i = 0; i < kMaxPoints; ++i) {
        const double t = sign * static_cast<double>(i) * kDxStep;
        expr->set_var("y", field ? (base + t) : t);
        double v = 0.0;
        if (!expr->eval_finite(v)) break;
        const Vec2 p(v - f0, t);
        if (i > 0) travel += (p - pts.back()).norm();
        if (!in_bounds(place_local(p, origin, req.mirror), cfg)) break;  // 先判界：非有限/出界的点不进路径
        pts.push_back(p);
        if (travel > cfg.world.max_travel) break;
    }
    if (pts.size() < 2) return {.error = {"curve_no_trail", "曲线没有形成有效轨迹"}};
    return {.path = finalize(std::move(pts), origin, req.mirror)};
}

// 极坐标 r = f(theta)：局部点 (r*cos, r*sin)，theta 从 p_start 扫到 p_end
// （默认 0 → 2*pi）。注意 r 为负是数学上合法的反向点，不做特殊处理。
CurveResult sample_polar(const CurveRequest& req, const Vec2& origin, const GameConfig& cfg) {
    Error err;
    auto expr = compile_expression(req.expr_a, {"theta"}, err);
    if (!expr) return {.error = err};

    const double p_start = req.p_start.value_or(0.0);
    const double p_end = req.p_end.value_or(2.0 * std::numbers::pi);
    const double span = p_end - p_start;
    if (std::abs(span) < 1e-9) return {.error = {"curve_param_empty", "参数范围为空"}};
    if (std::abs(span) > kMaxParamSpan)
        return {.error = {"curve_param_too_wide", "参数范围过大（最多 1000）"}};
    // 巨大的起点上 cos/sin 精度尽失，曲线会在采样中途悄悄失败成"没有轨迹"；
    // 这里直接给出可读的原因
    if (std::abs(p_start) > 1e6)
        return {.error = {"curve_param_start_too_large", "参数起点过大"}};

    size_t steps = static_cast<size_t>(std::abs(span) / kParamStep) + 1;
    steps = std::clamp<size_t>(steps, 2, kMaxPoints);

    // 锚点：theta = p_start 处的点
    expr->set_var("theta", p_start);
    double r0 = 0.0;
    if (!expr->eval_finite(r0)) return {.error = {"curve_bad_anchor_r", "起点处 r 不是有限数值"}};
    const Vec2 anchor(r0 * std::cos(p_start), r0 * std::sin(p_start));

    std::vector<Vec2> pts;
    pts.reserve(steps);
    double travel = 0.0;
    for (size_t i = 0; i < steps; ++i) {
        const double theta = p_start + span * static_cast<double>(i) / static_cast<double>(steps - 1);
        expr->set_var("theta", theta);
        double r = 0.0;
        if (!expr->eval_finite(r)) break;
        const Vec2 p(r * std::cos(theta) - anchor.x(), r * std::sin(theta) - anchor.y());
        if (i > 0) travel += (p - pts.back()).norm();
        if (!in_bounds(place_local(p, origin, req.mirror), cfg)) break;  // 先判界：非有限/出界的点不进路径
        pts.push_back(p);
        if (travel > cfg.world.max_travel) break;
    }
    if (pts.size() < 2) return {.error = {"curve_no_trail", "曲线没有形成有效轨迹"}};
    return {.path = finalize(std::move(pts), origin, req.mirror)};
}

// 参数方程 (x(t), y(t))：t 从 p_start 扫到 p_end（默认 0 → 1000，实际长度由行程预算截断）
CurveResult sample_parametric(const CurveRequest& req, const Vec2& origin, const GameConfig& cfg) {
    Error err;
    auto ex = compile_expression(req.expr_a, {"t"}, err);
    if (!ex) return {.error = {err.code, "x(t)：" + err.message}};
    auto ey = compile_expression(req.expr_b, {"t"}, err);
    if (!ey) return {.error = {err.code, "y(t)：" + err.message}};

    const double p_start = req.p_start.value_or(0.0);
    // 参数方程对 t 本身不设任何限制（t 多大都收）：唯一的长度限制是下方
    // "2 倍斜对角线"的行程预算。不填范围时默认终点给足（t 尺度≈1 的常见写法
    // 都能画到出图/预算用完为止），填了就用玩家的
    const double p_end = req.p_end.value_or(kMaxParamSpan);
    const double span = p_end - p_start;
    if (std::abs(span) < 1e-9) return {.error = {"curve_param_empty", "参数范围为空"}};

    size_t steps = static_cast<size_t>(std::abs(span) / kParamStep) + 1;
    steps = std::clamp<size_t>(steps, 2, kMaxPoints);

    // 行程预算按地图大小算（2 倍斜对角线，随地图边界自动变）：t 没有几何含义，
    // 曲线能绕圈折返，用全局 max_travel 当上限会凭 t 范围无限绕圈；
    // 这个预算下弯弯绕绕也足够覆盖全图，又不会无谓地绕
    const double travel_budget =
        2.0 * std::hypot(cfg.world.map_x_max - cfg.world.map_x_min,
                         cfg.world.map_y_max - cfg.world.map_y_min);

    ex->set_var("t", p_start);
    ey->set_var("t", p_start);
    double x0 = 0.0, y0 = 0.0;
    if (!ex->eval_finite(x0) || !ey->eval_finite(y0))
        return {.error = {"curve_bad_anchor_xy", "起点处坐标不是有限数值"}};

    std::vector<Vec2> pts;
    pts.reserve(steps);
    double travel = 0.0;
    for (size_t i = 0; i < steps; ++i) {
        const double t = p_start + span * static_cast<double>(i) / static_cast<double>(steps - 1);
        ex->set_var("t", t);
        ey->set_var("t", t);
        double xv = 0.0, yv = 0.0;
        if (!ex->eval_finite(xv) || !ey->eval_finite(yv)) break;
        const Vec2 p(xv - x0, yv - y0);
        if (i > 0) travel += (p - pts.back()).norm();
        if (!in_bounds(place_local(p, origin, req.mirror), cfg)) break;  // 先判界：非有限/出界的点不进路径
        pts.push_back(p);
        if (travel > travel_budget) break;
    }
    if (pts.size() < 2) return {.error = {"curve_no_trail", "曲线没有形成有效轨迹"}};
    return {.path = finalize(std::move(pts), origin, req.mirror)};
}

// 微分方程 dy/dx = f(x, y)：从玩家位置起 RK4 积分（初值即玩家位置，天然对齐）
CurveResult sample_ode1(const CurveRequest& req, const Vec2& origin, const GameConfig& cfg) {
    Error err;
    auto expr = compile_expression(req.expr_a, {"x", "y"}, err);
    if (!expr) return {.error = err};

    const double h = kOdeStep * ((req.dir == FireDir::Positive) ? 1.0 : -1.0);

    // 求 f(x, y)；ok 返回求值是否得到有限值
    auto slope = [&](double xx, double yy, bool& ok) -> double {
        expr->set_var("x", xx);
        expr->set_var("y", yy);
        double v = 0.0;
        ok = expr->eval_finite(v);
        return v;
    };

    // 镜像（B 队玩家）：在他的镜像坐标空间里积分（他的 x 正方向 = 全局 -x）——
    // 起点取 (-origin.x, origin.y)，表达式里的 x、y 直接就是他的空间坐标，
    // 逐点再映射回全局。他屏幕上看到的与写的完全一致
    const bool mir = req.mirror;
    double x = mir ? -origin.x() : origin.x();
    double y = origin.y();
    std::vector<Vec2> pts;
    pts.reserve(4096);
    pts.push_back(origin);
    double travel = 0.0;
    for (size_t i = 0; i < kMaxPoints; ++i) {
        bool ok1 = false, ok2 = false, ok3 = false, ok4 = false;
        const double k1 = slope(x, y, ok1);
        if (!ok1) break;
        const double k2 = slope(x + h * 0.5, y + h * 0.5 * k1, ok2);
        if (!ok2) break;
        const double k3 = slope(x + h * 0.5, y + h * 0.5 * k2, ok3);
        if (!ok3) break;
        const double k4 = slope(x + h, y + h * k3, ok4);
        if (!ok4) break;

        x += h;
        y += h * (k1 + 2.0 * k2 + 2.0 * k3 + k4) / 6.0;
        const Vec2 gp = mir ? Vec2(-x, y) : Vec2(x, y);  // 映射回全局
        travel += (gp - pts.back()).norm();
        if (!in_bounds(gp, cfg)) break;  // 先判界：非有限/出界的点不进路径
        pts.push_back(gp);
        if (travel > cfg.world.max_travel) break;
    }
    if (pts.size() < 2) return {.error = {"curve_no_trail", "曲线没有形成有效轨迹"}};
    return {.path = build_path(std::move(pts))};
}

// 微分方程 (x', y') = (f(t,x,y), g(t,x,y))：从玩家位置起 RK4 积分。
// 轨迹只决定形状，炮弹仍按弧长匀速运动（速度由房间配置统一决定，防止玩家用速度场作弊）。
CurveResult sample_ode2(const CurveRequest& req, const Vec2& origin, const GameConfig& cfg) {
    Error err;
    auto fx = compile_expression(req.expr_a, {"t", "x", "y"}, err);
    if (!fx) return {.error = {err.code, "x'(t,x,y)：" + err.message}};
    auto fy = compile_expression(req.expr_b, {"t", "x", "y"}, err);
    if (!fy) return {.error = {err.code, "y'(t,x,y)：" + err.message}};

    const double h = kOde2Step;

    auto eval2 = [&](double tt, double xx, double yy, double& dx, double& dy) -> bool {
        fx->set_var("t", tt);
        fx->set_var("x", xx);
        fx->set_var("y", yy);
        fy->set_var("t", tt);
        fy->set_var("x", xx);
        fy->set_var("y", yy);
        return fx->eval_finite(dx) && fy->eval_finite(dy);
    };

    // 镜像同 Ode1：在玩家的镜像坐标空间里积分（起点 (-ox, oy)），逐点映射回全局
    const bool mir = req.mirror;
    double t = 0.0;
    double x = mir ? -origin.x() : origin.x();
    double y = origin.y();
    std::vector<Vec2> pts;
    pts.reserve(4096);
    pts.push_back(origin);
    double travel = 0.0;
    for (size_t i = 0; i < kMaxPoints; ++i) {
        double k1x = 0, k1y = 0, k2x = 0, k2y = 0, k3x = 0, k3y = 0, k4x = 0, k4y = 0;
        if (!eval2(t, x, y, k1x, k1y)) break;
        if (!eval2(t + h * 0.5, x + h * 0.5 * k1x, y + h * 0.5 * k1y, k2x, k2y)) break;
        if (!eval2(t + h * 0.5, x + h * 0.5 * k2x, y + h * 0.5 * k2y, k3x, k3y)) break;
        if (!eval2(t + h, x + h * k3x, y + h * k3y, k4x, k4y)) break;

        t += h;
        x += h * (k1x + 2.0 * k2x + 2.0 * k3x + k4x) / 6.0;
        y += h * (k1y + 2.0 * k2y + 2.0 * k3y + k4y) / 6.0;
        const Vec2 gp = mir ? Vec2(-x, y) : Vec2(x, y);  // 映射回全局
        travel += (gp - pts.back()).norm();
        if (!in_bounds(gp, cfg)) break;  // 先判界：非有限/出界的点不进路径
        pts.push_back(gp);
        if (travel > cfg.world.max_travel) break;
    }
    if (pts.size() < 2) return {.error = {"curve_no_trail", "曲线没有形成有效轨迹"}};
    return {.path = build_path(std::move(pts))};
}

// 二阶微分方程 y'' = f(x, y, y')：状态 (y, v=y')，初值取玩家位置、初斜率由
// 发射角 angle 给出——angle 是相对行进方向的仰角（正数 = 朝发射方向的上方偏），
// 缺省 0 = 沿轴出发；以 x 为自变量、沿 FireDir 方向做二阶 RK4 积分
//（与 Ode1 同一套发散防御与出界处理）。x、y 与 Ode1 一样是地图坐标
CurveResult sample_ypp(const CurveRequest& req, const Vec2& origin, const GameConfig& cfg) {
    Error err;
    auto expr = compile_expression(req.expr_a, {"x", "y", "dy"}, err);
    if (!expr) return {.error = err};

    const double angle = req.angle.value_or(0.0);
    // tan 在 ±π/2 附近爆掉：给出可读原因，而不是让初斜率变成天文数字
    // 阈值与提示文案、客户端 clamp 三处对齐成精确的 85°（原先写 1.5 rad ≈ 85.94°，
    // 会出现"文案说合规、协议直连却被拒"的错位）
    constexpr double kMaxAngle = std::numbers::pi * 85.0 / 180.0;
    if (!std::isfinite(angle) || std::abs(angle) >= kMaxAngle) {
        return {.error = {"curve_bad_angle", "发射角过于竖直（需在 ±85 度内）"}};
    }
    // 仰角相对行进方向：向负方向发射时数学斜率随之取负
    const double v0 = std::tan(angle) * ((req.dir == FireDir::Positive) ? 1.0 : -1.0);

    const double h = kOdeStep * ((req.dir == FireDir::Positive) ? 1.0 : -1.0);

    auto accel = [&](double xx, double yy, double dy, bool& ok) -> double {
        expr->set_var("x", xx);
        expr->set_var("y", yy);
        expr->set_var("dy", dy);
        double v = 0.0;
        ok = expr->eval_finite(v);
        return v;
    };

    // 镜像同 Ode1：在玩家的镜像坐标空间里积分（起点 (-ox, oy)），逐点映射回全局
    const bool mir = req.mirror;
    double x = mir ? -origin.x() : origin.x();
    double y = origin.y();
    double v = v0;
    std::vector<Vec2> pts;
    pts.reserve(4096);
    pts.push_back(origin);
    double travel = 0.0;
    for (size_t i = 0; i < kMaxPoints; ++i) {
        bool ok1 = false, ok2 = false, ok3 = false, ok4 = false;
        const double a1 = accel(x, y, v, ok1);
        if (!ok1) break;
        const double a2 = accel(x + h * 0.5, y + h * 0.5 * v, v + h * 0.5 * a1, ok2);
        if (!ok2) break;
        const double a3 = accel(x + h * 0.5, y + h * 0.5 * (v + h * 0.5 * a1), v + h * 0.5 * a2, ok3);
        if (!ok3) break;
        const double a4 = accel(x + h, y + h * (v + h * 0.5 * a2), v + h * a3, ok4);
        if (!ok4) break;

        x += h;
        y += h * v + h * h * (a1 + a2 + a3) / 6.0;
        v += h * (a1 + 2.0 * a2 + 2.0 * a3 + a4) / 6.0;
        const Vec2 gp = mir ? Vec2(-x, y) : Vec2(x, y);  // 映射回全局
        travel += (gp - pts.back()).norm();
        if (!in_bounds(gp, cfg)) break;  // 先判界：非有限/出界的点不进路径
        pts.push_back(gp);
        if (travel > cfg.world.max_travel) break;
    }
    if (pts.size() < 2) return {.error = {"curve_no_trail", "曲线没有形成有效轨迹"}};
    return {.path = build_path(std::move(pts))};
}

}  // namespace

Vec2 Path::at(double s) const {
    // cum 与 points 由 build_path 保证等长；对外部手拼的 Path 也兜底：
    // 空容器直接返回零；起点用 cum.front() 而不是 0 比较——手拼的 Path 若 cum[0] != 0，
    // s ∈ (0, cum[0]) 时 upper_bound 会返回 begin()、i-1 越界，这里一并挡住
    if (points.empty() || cum.empty()) return Vec2::Zero();
    if (s <= cum.front()) return points.front();
    if (s >= cum.back()) return points.back();
    // 找到 cum[i] > s 的第一个 i（s 在区间 [i-1, i] 内）
    const auto it = std::upper_bound(cum.begin(), cum.end(), s);
    const size_t i = static_cast<size_t>(it - cum.begin());
    const double s0 = cum[i - 1];
    const double s1 = cum[i];
    const double w = (s1 > s0) ? (s - s0) / (s1 - s0) : 0.0;
    return points[i - 1] + (points[i] - points[i - 1]) * w;
}

CurveResult sample_curve(const CurveRequest& req, const Vec2& origin, const GameConfig& cfg) {
    switch (req.mode) {
        case CurveMode::YofX:
            return sample_y_of_x(req, origin, cfg);
        case CurveMode::XofY:
            return sample_x_of_y(req, origin, cfg);
        case CurveMode::Polar:
            return sample_polar(req, origin, cfg);
        case CurveMode::Parametric:
            return sample_parametric(req, origin, cfg);
        case CurveMode::Ode1:
            return sample_ode1(req, origin, cfg);
        case CurveMode::Ode2:
            return sample_ode2(req, origin, cfg);
        case CurveMode::Ypp:
            return sample_ypp(req, origin, cfg);
        case CurveMode::Count:
            break;  // 哨兵值不是真实模式，落到下面的错误返回
    }
    return {.error = {"unknown_curve_mode", "未知的曲线模式"}};
}

}  // namespace fg
