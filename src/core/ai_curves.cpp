#include "ai_curves.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <string>

namespace fg {

// ---------- 表达式文本工具 ----------

std::string num_text(double v) {
    if (v == 0.0) return "0";  // 含 -0.0：格式化会留下 "-0" 这种写法
    std::string s = std::format("{:.4f}", v);
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s.empty() || s == "-") s = "0";
    return s;
}

std::string poly_text(const std::vector<double>& coeffs, const char* var) {
    std::string s;
    for (int i = static_cast<int>(coeffs.size()) - 1; i >= 0; --i) {
        const double c = coeffs[static_cast<size_t>(i)];
        if (std::abs(c) < 1e-9) continue;
        std::string term;
        if (i == 0) {
            term = num_text(std::abs(c));
        } else if (i == 1) {
            term = std::format("{}*{}", num_text(std::abs(c)), var);
        } else {
            term = std::format("{}*{}^{}", num_text(std::abs(c)), var, i);
        }
        if (s.empty()) {
            s = (c < 0.0 ? "-" : "") + term;
        } else {
            s += (c < 0.0 ? " - " : " + ") + term;
        }
    }
    if (s.empty()) s = "0";
    return s;
}

namespace {

// ---------- 各模式生成器 ----------
// 统一约定：horizontal = 主向沿 x（用 y = f(x) 类模式），否则沿 y（x = f(y) 类模式）；
// t = 主向相对位移（带符号），v = 另一轴的相对位移。所有形状都精确过瞄准点 (t, v)。

// 各生成器的公共字段
CurveRequest base_axis_curve(bool horizontal, double t) {
    CurveRequest req;
    req.mode = horizontal ? CurveMode::YofX : CurveMode::XofY;
    req.dir = (t >= 0.0) ? FireDir::Positive : FireDir::Negative;
    return req;
}

// 三角波：f(u) = m*u + A*sin(w*u)，波长固定为 |t|/waves
CurveRequest make_trig(bool horizontal, double dx, double dy, double amp, double waves) {
    const double t = horizontal ? dx : dy;
    const double v = horizontal ? dy : dx;
    CurveRequest req = base_axis_curve(horizontal, t);
    const char* var = horizontal ? "x" : "y";
    const double w = 2.0 * std::numbers::pi * waves / std::abs(t);
    const double m = (v - amp * std::sin(w * t)) / t;
    req.expr_a = std::format("{}*{}+{}*sin({}*{})", num_text(m), var, num_text(amp), num_text(w),
                             var);
    return req;
}

// 绝对值折线：f(u) = a*u + b*(|u - t/2| - |t/2|)，折点取在中点。
// 折线项在 u = t 处恰好归零（|t - t/2| = |t/2|），所以折点偏移只改形状、不影响过瞄准点
CurveRequest make_vee(bool horizontal, double dx, double dy, double offset) {
    const double t = horizontal ? dx : dy;
    const double v = horizontal ? dy : dx;
    CurveRequest req = base_axis_curve(horizontal, t);
    const char* var = horizontal ? "x" : "y";
    const double p = t * 0.5;
    const double b = -offset / std::abs(p);  // 折点相对直线的偏移量等于 offset
    const double a = v / t;
    req.expr_a = std::format("{}*{}+{}*(abs({}-{})-{})", num_text(a), var, num_text(b), var,
                             num_text(p), num_text(std::abs(p)));
    return req;
}

// 阶梯：f(u) = a*u + A*floor(u/step_w)，每隔 step_w 抬升 A（垂直跳变）
CurveRequest make_stairs(bool horizontal, double dx, double dy, double step_h, double step_w) {
    const double t = horizontal ? dx : dy;
    const double v = horizontal ? dy : dx;
    CurveRequest req = base_axis_curve(horizontal, t);
    const char* var = horizontal ? "x" : "y";
    const double a = (v - step_h * std::floor(t / step_w)) / t;
    req.expr_a = std::format("{}*{}+{}*floor({}/{})", num_text(a), var, num_text(step_h), var,
                             num_text(step_w));
    return req;
}

// 锯齿：f(u) = a*u + A*(u/w - floor(u/w))，齿宽 w，每齿爬升 A 后落回
CurveRequest make_sawtooth(bool horizontal, double dx, double dy, double amp, double tooth_w) {
    const double t = horizontal ? dx : dy;
    const double v = horizontal ? dy : dx;
    CurveRequest req = base_axis_curve(horizontal, t);
    const char* var = horizontal ? "x" : "y";
    const double a = (v - amp * (t / tooth_w - std::floor(t / tooth_w))) / t;
    req.expr_a = std::format("{}*{}+{}*({}/{}-floor({}/{}))", num_text(a), var, num_text(amp), var,
                             num_text(tooth_w), var, num_text(tooth_w));
    return req;
}

// 波浪 + 折线混合：f(u) = m*u + A*sin(w*u) + b*(|u - t/2| - |t/2|)
CurveRequest make_wave_vee(bool horizontal, double dx, double dy, double amp, double offset) {
    const double t = horizontal ? dx : dy;
    const double v = horizontal ? dy : dx;
    CurveRequest req = base_axis_curve(horizontal, t);
    const char* var = horizontal ? "x" : "y";
    const double w = 2.0 * std::numbers::pi * 0.75 / std::abs(t);
    const double p = t * 0.5;
    const double b = -offset / std::abs(p);
    const double m = (v - amp * std::sin(w * t)) / t;
    req.expr_a = std::format("{}*{}+{}*sin({}*{})+{}*(abs({}-{})-{})", num_text(m), var,
                             num_text(amp), num_text(w), var, num_text(b), var, num_text(p),
                             num_text(std::abs(p)));
    return req;
}

// 阶梯 + 正弦混合：f(u) = a*u + A*floor(u/step_w) + B*sin(w2*u)
CurveRequest make_stairs_wave(bool horizontal, double dx, double dy, double step_h, double step_w,
                              double amp) {
    const double t = horizontal ? dx : dy;
    const double v = horizontal ? dy : dx;
    CurveRequest req = base_axis_curve(horizontal, t);
    const char* var = horizontal ? "x" : "y";
    const double w = 2.0 * std::numbers::pi * 0.75 / std::abs(t);
    const double a =
        (v - step_h * std::floor(t / step_w) - amp * std::sin(w * t)) / t;
    req.expr_a = std::format("{}*{}+{}*floor({}/{})+{}*sin({}*{})", num_text(a), var,
                             num_text(step_h), var, num_text(step_w), num_text(amp), num_text(w),
                             var);
    return req;
}

// 参数方程二次拱：弦 (dx, dy) 向法向拱起 sag。
// t 扫到 1.15 而不是 1：炮弹"走到轨迹尽头"的那一格会跳过命中判定（先判尽头再判位置），
// 让路径越过瞄准点一点，避免目标恰好在终点被漏掉
CurveRequest make_arc(double dx, double dy, double sag) {
    const double d = std::hypot(dx, dy);
    const double nx = -dy / d;
    const double ny = dx / d;
    const double kx = 4.0 * sag * nx;
    const double ky = 4.0 * sag * ny;
    CurveRequest req;
    req.mode = CurveMode::Parametric;
    req.expr_a = poly_text({0.0, dx + kx, -kx}, "t");
    req.expr_b = poly_text({0.0, dy + ky, -ky}, "t");
    req.p_start = 0.0;
    req.p_end = 1.15;
    return req;
}

// 参数方程拱 + 正弦波浪：y 方向叠 A*sin(pi*t)，在 t = 1 处恰好归零、不影响过点
CurveRequest make_arc_wave(double dx, double dy, double sag, double amp) {
    const double d = std::hypot(dx, dy);
    const double nx = -dy / d;
    const double ny = dx / d;
    const double kx = 4.0 * sag * nx;
    const double ky = 4.0 * sag * ny;
    CurveRequest req;
    req.mode = CurveMode::Parametric;
    req.expr_a = poly_text({0.0, dx + kx, -kx}, "t");
    req.expr_b = std::format("{}*t+{}*(t-t^2)+{}*sin(pi*t)", num_text(dy), num_text(ky),
                             num_text(amp));
    req.p_start = 0.0;
    req.p_end = 1.15;
    return req;
}

// 阿基米德螺线 r = (d/theta_aim)*theta：theta = 0 处 r = 0，锚点（起点）正好落在
// 发射者身上；扫到瞄准点方位角时 r 恰好等于到瞄准点的距离，之后继续向外旋出。
// 注意锚点是 L(p_start) 而不是极坐标原点，所以起点必须选在 r = 0 的 theta = 0
CurveRequest make_polar(double dx, double dy, double tail) {
    const double d = std::hypot(dx, dy);
    const double th = std::atan2(dy, dx);
    CurveRequest req;
    req.mode = CurveMode::Polar;
    req.expr_a = std::format("{}*theta", num_text(d / th));
    req.p_start = 0.0;
    req.p_end = th + (th >= 0.0 ? tail : -tail);  // 过瞄准点后再向外旋一点
    return req;
}

// 螺线 + 正弦混合：r = k*theta + swing*sin(n*theta)。theta = 0 处 r = 0（起点在发射者
// 身上），k 由"瞄准方位角处 r 正好等于距离 d"解出
CurveRequest make_polar_wave(double dx, double dy, double swing, double n, double tail) {
    const double d = std::hypot(dx, dy);
    const double th = std::atan2(dy, dx);
    CurveRequest req;
    req.mode = CurveMode::Polar;
    const double k = (d - swing * std::sin(n * th)) / th;
    req.expr_a = std::format("{}*theta+{}*sin({}*theta)", num_text(k), num_text(swing),
                             num_text(n));
    req.p_start = 0.0;
    req.p_end = th + (th >= 0.0 ? tail : -tail);
    return req;
}

}  // namespace

// ---------- 评估与汇总生成 ----------

CurveCandidate evaluate_candidate(const CurveRequest& req, const Vec2& origin, const Vec2& aim,
                                  const World& world) {
    CurveCandidate c;
    c.req = req;
    c.sampled = sample_curve(req, origin, world.config());
    if (!c.sampled.ok()) return c;
    const GameConfig& cfg = world.config();
    const double crater = cfg.world.crater_radius;
    const double hit_r = cfg.world.player_half_size;
    // 按飞行顺序评估（与 step_flight 同粒度、同次序——命中结算先于撞墙）：只按
    // "几何上经过瞄准点"判 reach，会把"先撞墙"的曲线也算成打得到，AI 于是会挑
    // 出实际半路炸掉的线——看起来就是"不会绕障碍、明知有墙硬撞"
    double gap = 1e18;
    bool hit = false;      // 撞墙之前碰到了瞄准点
    bool blocked = false;  // 途中有遮挡物（命中点之后撞的也算，clear 用）
    const Path& path = c.sampled.path;
    for (size_t i = 0; i + 1 < path.points.size() && !blocked; ++i) {
        const Vec2& a = path.points[i];
        const Vec2& b = path.points[i + 1];
        // step 跳变这类相邻顶点可以相距极远，细分到飞行步长粒度才能不漏障碍
        const size_t n = std::max<size_t>(
            1, static_cast<size_t>(std::ceil((b - a).norm() / kAiFlightStep)));
        for (size_t s = 1; s <= n; ++s) {
            const double t = static_cast<double>(s) / static_cast<double>(n);
            const Vec2 p(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t);
            const double d = distance(p, aim);
            gap = std::min(gap, d);
            if (d <= hit_r) hit = true;
            // 同一步内命中先于撞墙（与 step_flight 的结算次序一致）
            for (const auto& ob : world.obstacles()) {
                if (obstacle_blocks(ob, p, crater)) {
                    blocked = true;
                    break;
                }
            }
            if (blocked) break;
            // 采样会保留少量出界段（给"绕回来"留的宽限），AI 评估按严格地图边界
            // 截断：只评估可见段，保守但不出错
            if (p.x() < cfg.world.map_x_min || p.x() > cfg.world.map_x_max ||
                p.y() < cfg.world.map_y_min || p.y() > cfg.world.map_y_max) {
                break;
            }
        }
    }
    c.aim_gap = gap;
    c.reaches = hit;
    // clear：命中且全程（含命中点之后）不撞遮挡物——打完后也不炸墙的干净弹道，
    // 择优时优先；"先撞墙"的曲线 reaches 已为 false，不会靠 clear 复活
    c.clear = hit && !blocked;
    return c;
}

CurveRequest make_parabola(bool horizontal, double dx, double dy, double offset) {
    const double t = horizontal ? dx : dy;
    const double v = horizontal ? dy : dx;
    CurveRequest req = base_axis_curve(horizontal, t);
    const double a = 4.0 * offset / (t * t);
    const double k = (v - a * t * t) / t;  // 保证曲线在 u = t 处经过瞄准点
    req.expr_a = poly_text({0.0, k, a}, horizontal ? "x" : "y");
    return req;
}

CurveRequest make_parabola_through(bool horizontal, double dx1, double dy1, double dx2,
                                   double dy2) {
    // f(u) = a*u^2 + k*u 在两点处的高度差等于目标高度差（常数项被锚点平移消掉）：
    //   a*u1^2 + k*u1 = v1，a*u2^2 + k*u2 = v2 —— 克拉默法则解出 a、k
    const double u1 = horizontal ? dx1 : dy1;
    const double v1 = horizontal ? dy1 : dx1;
    const double u2 = horizontal ? dx2 : dy2;
    const double v2 = horizontal ? dy2 : dx2;
    CurveRequest req = base_axis_curve(horizontal, u1);
    const double det = u1 * u2 * (u1 - u2);
    const double a = (v1 * u2 - v2 * u1) / det;
    const double k = (u1 * u1 * v2 - u2 * u2 * v1) / det;
    req.expr_a = poly_text({0.0, k, a}, horizontal ? "x" : "y");
    return req;
}

CurveRequest make_obstacle_arc(bool horizontal, double dx, double dy, double t_ob, double h_ob) {
    const double t = horizontal ? dx : dy;
    const double v = horizontal ? dy : dx;
    CurveRequest req = base_axis_curve(horizontal, t);
    // 同样克拉默法则：f(0)=0、f(t_ob)=h_ob、f(t)=v 三点唯一确定 a、k
    const double det = t * t_ob * (t_ob - t);
    if (std::abs(det) < 1e-9) return req;  // 障碍贴原点/瞄准点时病态：留空表达式，评估丢弃
    const double a = (h_ob * t - v * t_ob) / det;
    const double k = (t_ob * t_ob * v - t * t * h_ob) / det;
    req.expr_a = poly_text({0.0, k, a}, horizontal ? "x" : "y");
    return req;
}

// ---------- 多杀骨架 ----------

CurveRequest make_kink_chain(bool horizontal, const std::vector<Vec2>& rel) {
    // 节点：原点 + 所有目标点，按自变量升序，相邻节点连成线段
    std::vector<std::pair<double, double>> nodes = {{0.0, 0.0}};
    for (const auto& p : rel) {
        nodes.push_back(horizontal ? std::pair{double(p.x()), double(p.y())}
                                   : std::pair{double(p.y()), double(p.x())});
    }
    std::sort(nodes.begin(), nodes.end());
    CurveRequest req = base_axis_curve(horizontal, nodes.front().first);
    const char* var = horizontal ? "x" : "y";

    // 从最左段那条直线出发，每越过一个节点叠加一次"斜率变化 * (u - ui) * step(u - ui)"
    //（该乘积刚好是折线在节点之后的斜率增量，节点处连续）
    std::vector<double> slope(nodes.size() - 1);
    for (size_t i = 0; i + 1 < nodes.size(); ++i) {
        slope[i] = (nodes[i + 1].second - nodes[i].second) / (nodes[i + 1].first - nodes[i].first);
    }
    std::string s = num_text(nodes[0].second - slope[0] * nodes[0].first) + "+" +
                    num_text(slope[0]) + "*" + var;
    // 只在"内部节点"叠斜率变化：最后一个节点之后没有线段，不存在斜率差
    for (size_t i = 1; i + 1 < nodes.size(); ++i) {
        const double d = slope[i] - slope[i - 1];
        if (std::abs(d) < 1e-9) continue;
        const std::string u = std::format("({} - {})*step({} - {})", var, num_text(nodes[i].first),
                                          var, num_text(nodes[i].first));
        s += (d < 0.0 ? " - " : " + ") + num_text(std::abs(d)) + "*" + u;
    }
    req.expr_a = s;
    return req;
}

bool make_kink_bend(bool horizontal, double dx, double dy, double ox, double oy, double p,
                    CurveRequest& out) {
    const double t1 = horizontal ? dx : dy;
    const double v1 = horizontal ? dy : dx;
    const double t2 = horizontal ? ox : oy;
    const double v2 = horizontal ? oy : ox;
    if (std::abs(t1) < 1e-9 || std::abs(t2 - t1) < 1e-9 || std::abs(p) < 1e-9 ||
        std::abs(t2 - p) < 1e-9) {
        return false;
    }
    double m1 = 0.0;
    double m2 = 0.0;
    const bool between_targets = (p > t1 && p < t2) || (p < t1 && p > t2);
    const bool before_first = (p > 0.0 && p < t1) || (p < 0.0 && p > t1);
    if (between_targets) {
        // 折点在两目标之间：起点→第一目标是最初的直线段，剩下的反推第二段斜率
        m1 = v1 / t1;
        m2 = (v2 - m1 * p) / (t2 - p);
    } else if (before_first) {
        // 折点在起点与第一目标之间：两目标之间的连线是第二段，反推起点段斜率
        m2 = (v2 - v1) / (t2 - t1);
        m1 = (v1 - m2 * (t1 - p)) / p;
    } else {
        return false;  // p 越过第二目标或与节点重合：形状不再是"先到一、再折向二"
    }
    CurveRequest req = base_axis_curve(horizontal, t1);
    const char* var = horizontal ? "x" : "y";
    // max(u-p, 0) = ((u-p)+|u-p|)/2：折点前斜率 m1、折点后斜率 m2 的 abs 复合写法
    const double k = (m2 - m1) * 0.5;
    req.expr_a = std::format("{}*{}+{}*(({}-{})+abs({}-{}))", num_text(m1), var, num_text(k), var,
                             num_text(p), var, num_text(p));
    out = req;
    return true;
}

bool make_wave_through(bool horizontal, double dx, double dy, double ox, double oy, double w,
                       CurveRequest& out) {
    const double t1 = horizontal ? dx : dy;
    const double v1 = horizontal ? dy : dx;
    const double t2 = horizontal ? ox : oy;
    const double v2 = horizontal ? oy : ox;
    const double s1 = std::sin(w * t1);
    const double s2 = std::sin(w * t2);
    const double det = t1 * s2 - t2 * s1;
    if (std::abs(det) < 1e-9 * (std::abs(t1 * t2) + 1.0)) return false;
    const double m = (v1 * s2 - v2 * s1) / det;
    const double a = (t1 * v2 - t2 * v1) / det;
    // 系数离谱（曲线会飞出地图）时不值得放进评估，直接丢弃
    if (std::abs(m) > 20.0 || std::abs(a) > 60.0) return false;
    CurveRequest req = base_axis_curve(horizontal, t1);
    req.expr_a = std::format("{}*{}+{}*sin({}*{})", num_text(m), horizontal ? "x" : "y",
                             num_text(a), num_text(w), horizontal ? "x" : "y");
    out = req;
    return true;
}

bool make_poly_wave_through(bool horizontal, double dx, double dy, double ox, double oy, double c,
                            CurveRequest& out) {
    const double t1 = horizontal ? dx : dy;
    const double v1 = horizontal ? dy : dx;
    const double t2 = horizontal ? ox : oy;
    const double v2 = horizontal ? oy : ox;
    const double span = std::abs(t2 - t1);
    if (span < 1e-9) return false;
    const double w = 2.0 * std::numbers::pi * 1.25 / span;
    // 先把正弦项的贡献从目标高度里减掉，剩下的用抛物线过点（与 make_parabola_through 同解）
    const double u1 = v1 - c * std::sin(w * t1);
    const double u2 = v2 - c * std::sin(w * t2);
    const double det = t1 * t2 * (t1 - t2);
    if (std::abs(det) < 1e-9) return false;
    const double a2 = (u1 * t2 - u2 * t1) / det;
    const double b = (t1 * t1 * u2 - t2 * t2 * u1) / det;
    CurveRequest req = base_axis_curve(horizontal, t1);
    const char* var = horizontal ? "x" : "y";
    req.expr_a = std::format("{}*{}^2+{}*{}+{}*sin({}*{})", num_text(a2), var, num_text(b), var,
                             num_text(c), num_text(w), var);
    out = req;
    return true;
}

bool make_spiral_through(double dx, double dy, double ox, double oy, CurveRequest& out) {
    const double d1 = std::hypot(dx, dy);
    const double d2 = std::hypot(ox, oy);
    const double th1 = std::atan2(dy, dx);
    const double th2 = std::atan2(oy, ox);
    // 螺旋只朝一个旋向扫：两人必须同侧、离正前方不能太近、角差不能太小（否则解病态）
    if (th1 * th2 <= 0.0) return false;
    if (std::abs(th1) < 0.02 || std::abs(th2) < 0.02) return false;
    if (std::abs(th2 - th1) < 0.05) return false;
    const double det = th1 * th2 * (th2 - th1);
    const double k = (d1 * th2 * th2 - d2 * th1 * th1) / det;
    const double c = (d2 * th1 - d1 * th2) / det;
    if (std::abs(k) > 500.0 || std::abs(c) > 500.0) return false;
    CurveRequest req;
    req.mode = CurveMode::Polar;
    req.expr_a = std::format("{}*theta+{}*theta^2", num_text(k), num_text(c));
    const double far = (std::abs(th2) > std::abs(th1)) ? th2 : th1;
    req.p_start = 0.0;
    req.p_end = far + (far >= 0.0 ? 0.35 : -0.35);  // 过最远那个目标后再扫一小段
    out = req;
    return true;
}

bool make_poly_wave3_through(bool horizontal, const Vec2& p1, const Vec2& p2, const Vec2& p3,
                             double w, CurveRequest& out) {
    const double t1 = horizontal ? p1.x() : p1.y();
    const double v1 = horizontal ? p1.y() : p1.x();
    const double t2 = horizontal ? p2.x() : p2.y();
    const double v2 = horizontal ? p2.y() : p2.x();
    const double t3 = horizontal ? p3.x() : p3.y();
    const double v3 = horizontal ? p3.y() : p3.x();
    const double s1 = std::sin(w * t1);
    const double s2 = std::sin(w * t2);
    const double s3 = std::sin(w * t3);
    // 三元一次方程组：a*t^2 + b*t + c*sin(w*t) = v（三个点三条方程），克拉默法则
    const double det = t1 * t1 * (t2 * s3 - t3 * s2) - t1 * (t2 * t2 * s3 - t3 * t3 * s2) +
                       s1 * (t2 * t2 * t3 - t3 * t3 * t2);
    if (std::abs(det) < 1e-9) return false;
    const double det_a = v1 * (t2 * s3 - t3 * s2) - t1 * (v2 * s3 - v3 * s2) +
                         s1 * (v2 * t3 - v3 * t2);
    const double det_b = t1 * t1 * (v2 * s3 - v3 * s2) - v1 * (t2 * t2 * s3 - t3 * t3 * s2) +
                         s1 * (t2 * t2 * v3 - t3 * t3 * v2);
    const double det_c = t1 * t1 * (t2 * v3 - t3 * v2) - t1 * (t2 * t2 * v3 - t3 * t3 * v2) +
                         v1 * (t2 * t2 * t3 - t3 * t3 * t2);
    const double a = det_a / det;
    const double b = det_b / det;
    const double c = det_c / det;
    // 系数离谱（形状飞出地图）时不值得评估
    if (std::abs(a) > 50.0 || std::abs(b) > 20.0 || std::abs(c) > 60.0) return false;
    CurveRequest req = base_axis_curve(horizontal, t1);
    const char* var = horizontal ? "x" : "y";
    req.expr_a = std::format("{}*{}^2+{}*{}+{}*sin({}*{})", num_text(a), var, num_text(b), var,
                             num_text(c), num_text(w), var);
    out = req;
    return true;
}

CurveRequest make_ypp(double dx, double dy, double offset) {
    // 过瞄准点的抛物线 y(u) = a*u^2 + k*u（u 是沿行进方向的位移，t = |dx|）
    // 等价于 y'' = 2a、y'(0) = k；ypp 模式用发射角表达初斜率，取 atan(k)
    const double t = std::abs(dx);
    CurveRequest req;
    req.mode = CurveMode::Ypp;
    req.dir = (dx >= 0.0) ? FireDir::Positive : FireDir::Negative;
    const double a = 4.0 * offset / (t * t);
    const double k = (dy - a * t * t) / t;
    req.expr_a = num_text(2.0 * a);
    req.angle = std::atan(k);
    return req;
}

std::vector<CurveCandidate> generate_candidates(bool horizontal, double dx, double dy,
                                                int difficulty, const Vec2& origin, const Vec2& aim,
                                                const World& world, Rng& rng) {
    std::vector<CurveCandidate> out;
    const double d = std::hypot(dx, dy);
    if (d < 1e-6) return out;
    const double t = horizontal ? dx : dy;
    const double v = horizontal ? dy : dx;
    const double amp = std::max(2.4, 0.4 * std::abs(v));

    auto add = [&](const CurveRequest& req) {
        out.push_back(evaluate_candidate(req, origin, aim, world));
    };

    // 直线与抛物线：任何难度都生成（直线还是最终兜底）
    add(make_parabola(horizontal, dx, dy, 0.0));
    add(make_parabola(horizontal, dx, dy, amp));
    add(make_parabola(horizontal, dx, dy, -amp));
    // 二阶微分方程 y'' = 常值：与抛物线同族的"发射角"表达，只沿 x 展开
    if (horizontal) {
        add(make_ypp(dx, dy, 0.0));
        add(make_ypp(dx, dy, amp));
        add(make_ypp(dx, dy, -amp));
    }
    // 直线被障碍挡住时，按"挡路障碍的高度"精确起拱：两侧各一条大拱抛物线。
    // 拱高不是拍脑袋的档位（±amp 的覆盖面有限、高障碍绕不过去），而是过障碍
    // 顶（底）外扩 4 单位的实算值——曲线在障碍处真正高过它、在瞄准点仍精确过点
    if (out[0].sampled.ok()) {
        const int ob_idx = first_blocking_obstacle(out[0].sampled.path, world);
        if (ob_idx >= 0) {
            const Obstacle& ob = world.obstacles()[static_cast<size_t>(ob_idx)];
            const Vec2 rel = ob.center - origin;
            const double t_ob = horizontal ? rel.x() : rel.y();
            const double hv = horizontal ? rel.y() : rel.x();
            const double ext =
                (ob.is_circle ? ob.radius : (horizontal ? ob.half_h : ob.half_w)) + 4.0;
            if (std::abs(t_ob) > 0.5 && std::abs(t_ob - t) > 1.5) {
                add(make_obstacle_arc(horizontal, dx, dy, t_ob, hv + ext));
                add(make_obstacle_arc(horizontal, dx, dy, t_ob, hv - ext));
            }
        }
    }
    if (difficulty == 0) return out;  // 简单难度只玩这些基础形状

    // 与主向相关的形状：主向太短时频率参数会病态，直接跳过
    if (std::abs(t) >= 1.6) {
        add(make_trig(horizontal, dx, dy, amp * 0.6, 0.75));
        add(make_trig(horizontal, dx, dy, amp * 0.9, 1.25));
        add(make_vee(horizontal, dx, dy, amp * 0.8));
        add(make_vee(horizontal, dx, dy, -amp * 0.8));
        add(make_stairs(horizontal, dx, dy, amp * 0.35, std::abs(t) / 4.0));
        add(make_sawtooth(horizontal, dx, dy, amp * 0.5, std::abs(t) / 3.0));
        add(make_wave_vee(horizontal, dx, dy, amp * 0.6, amp * 0.6));
        add(make_stairs_wave(horizontal, dx, dy, amp * 0.3, std::abs(t) / 4.0, amp * 0.4));
    }

    // 与主向无关的形状
    add(make_arc(dx, dy, amp));
    add(make_arc(dx, dy, -amp));
    add(make_arc(dx, dy, 2.0 * amp));
    add(make_arc_wave(dx, dy, amp, amp * 0.4));
    // 阿基米德螺线：目标几乎正右侧时 theta_aim ≈ 0，螺线退化成朝外直线，跳过。
    // 阈值不必再放大（实测：|θ|≤0.005 时采样本身失败、丢弃成本只有一次微秒级
    // 评估；θ≈0.05 起是能命中的有效候选，若放大到该值会误伤中间区间）
    if (std::abs(std::atan2(dy, dx)) > 1e-3) {
        add(make_polar(dx, dy, 0.4));
        add(make_polar(dx, dy, 0.9));
        add(make_polar_wave(dx, dy, amp * 0.5, 2.0, 0.6));
    }

    // 普通及以上叠加微分方程类（强度差异由瞄准误差与择优选法体现，不靠限制形状）
    if (difficulty >= 1) add_ode_candidates(out, horizontal, dx, dy, origin, aim, world, rng);
    return out;
}

int first_blocking_obstacle(const Path& path, const World& world) {
    const double crater = world.config().world.crater_radius;
    for (size_t i = 0; i + 1 < path.points.size(); ++i) {
        const Vec2& a = path.points[i];
        const Vec2& b = path.points[i + 1];
        const size_t n = std::max<size_t>(
            1, static_cast<size_t>(std::ceil((b - a).norm() / kAiFlightStep)));
        for (size_t s = 1; s <= n; ++s) {
            const double t = static_cast<double>(s) / static_cast<double>(n);
            const Vec2 p(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t);
            for (size_t k = 0; k < world.obstacles().size(); ++k) {
                if (obstacle_blocks(world.obstacles()[k], p, crater)) {
                    return static_cast<int>(k);
                }
            }
        }
    }
    return -1;
}

}  // namespace fg
