#include "ai_curves.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <string>

namespace fg {
namespace {

// dy/dx = k + A*sin(w*(x-x0))：积分得 y(x) = y0 + k*Δx + A/w*(1 - cos(w*Δx))。
// A 按"波浪高度 swing"反推（A = swing*w/2）：直接给斜率幅值的话，摆动高度 A/w 会被
// 频率放大几十倍，曲线在到达目标前就冲出地图。Ode1 的表达式变量是世界坐标 (x, y)，
// 所以自变量偏移（减去发射点 x0）要写进表达式里
CurveRequest make_ode_wave(double dx, double dy, double x0, double swing, double waves) {
    CurveRequest req;
    req.mode = CurveMode::Ode1;
    req.dir = (dx >= 0.0) ? FireDir::Positive : FireDir::Negative;
    const double w = 2.0 * std::numbers::pi * waves / std::abs(dx);
    const double a = swing * w * 0.5;
    const double k = (dy - a / w * (1.0 - std::cos(w * dx))) / dx;
    req.expr_a = std::format("{}+{}*sin({}*(x-{}))", num_text(k), num_text(a), num_text(w),
                             num_text(x0));
    return req;
}

// (x', y') = (cos(w*t+p), sin(w*t+p))：速度方向随时间匀速旋转 → 圆弧。
// 圆心 = 起点 + R*(-sin p, cos p)，令 |瞄准点 - 圆心| = |R| 解出半径 R，
// 这样从发射位置 RK4 积分出来的圆弧正好扫过瞄准点
bool make_ode_arc(double dx, double dy, double dev, CurveRequest& out) {
    const double d = std::hypot(dx, dy);
    const double th = std::atan2(dy, dx);
    const double phi = th + dev;
    const double denom = 2.0 * (dx * std::sin(phi) - dy * std::cos(phi));
    if (std::abs(denom) < 1e-9) return false;  // dev ≈ 0：退化成直线，交给别的策略
    const double r = -(d * d) / denom;
    if (std::abs(r) < 8.0 || std::abs(r) > 2000.0) return false;  // 曲率太极端或近平直，不划算
    const double w = 1.0 / r;  // 速度大小恒为 1，角速度即 1/R
    out = CurveRequest{};
    out.mode = CurveMode::Ode2;
    out.expr_a = std::format("cos({}*t+{})", num_text(w), num_text(phi));
    out.expr_b = std::format("sin({}*t+{})", num_text(w), num_text(phi));
    // Ode2 采样从 t = 0 连续积分到出界或行程上限，参数范围用不上
    return true;
}

}  // namespace

// dy/dx = k + A*heaviside(x - 折点)：斜率阶跃，积分曲线就是折线。
// A 按"折点相对直线的偏移量"反推（A = 2*offset/|dx|）；折点取在相对主向的中点，
// 积分到瞄准点时 H=1 段长度恒为 |dx|/2，但抬升的符号跟着积分方向走：
// 实际抬升是 A*dx/2，写成 A*|dx|/2 会在向左发射时反号（落到 dy - 2*offset，永远打不中）。
// 定义放公开区（头文件已声明）：单测需要直接盯住这个符号
CurveRequest make_ode_slope_step(double dx, double dy, double x0, double offset) {
    CurveRequest req;
    req.mode = CurveMode::Ode1;
    req.dir = (dx >= 0.0) ? FireDir::Positive : FireDir::Negative;
    const double a = 2.0 * offset / std::abs(dx);
    const double k = (dy - a * dx * 0.5) / dx;
    req.expr_a =
        std::format("{}+{}*heaviside(x-{})", num_text(k), num_text(a), num_text(x0 + dx * 0.5));
    return req;
}

void add_ode_candidates(std::vector<CurveCandidate>& out, bool horizontal, double dx, double dy,
                        const Vec2& origin, const Vec2& aim, const World& world, Rng& rng) {
    (void)rng;  // 微分方程类的参数都是解析算出来的，不需要随机数
    const double v = horizontal ? dy : dx;
    // 与 ai_curves.cpp 同源的小地图参数（原 6.0 是 120x80 地图时期的取值）
    const double amp = std::max(2.4, 0.4 * std::abs(v));
    auto add = [&](const CurveRequest& req) {
        out.push_back(evaluate_candidate(req, origin, aim, world));
    };

    // dy/dx 类只能沿 x 方向积分：主向在 x 且不能太短，否则频率参数病态
    if (std::abs(dx) >= 1.6) {
        add(make_ode_wave(dx, dy, origin.x(), amp * 0.5, 0.75));
        add(make_ode_wave(dx, dy, origin.x(), amp * 0.8, 1.5));
        add(make_ode_slope_step(dx, dy, origin.x(), amp * 0.4));
        add(make_ode_slope_step(dx, dy, origin.x(), -amp * 0.4));
    }
    // 圆弧：起始方向相对瞄准方向偏几档，两侧都试
    for (double dev : {-0.35, 0.35, -0.7, 0.7, -1.1, 1.1}) {
        CurveRequest req;
        if (make_ode_arc(dx, dy, dev, req)) add(req);
    }
}

}  // namespace fg
