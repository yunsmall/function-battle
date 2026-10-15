#pragma once
// AI 的曲线生成策略：按不同数学模式构造"经过瞄准点"的候选曲线，由 ai.cpp 择优使用。
// 所有生成器都遵守"锚点对齐发射者"的定位规则（见 curve.h），因此只需让曲线在
// 局部坐标下经过瞄准点的相对位置 (dx, dy)。
//
// 代数与几何类策略（多项式/三角/折线/阶梯/极坐标/参数方程）在 ai_curves.cpp；
// 微分方程类（dy/dx 与 (x',y') 参数场）在 ai_ode.cpp。

#include <string>
#include <vector>

#include "rng.h"
#include "vec2.h"
#include "world.h"

namespace fg {

// AI 评估路径的推进步长：必须与 World::step_flight 的 kSubStep 一致。
// 评估碰撞必须按弧长细分推进，只检查采样顶点会漏掉 step 跳变这类相邻顶点相距
// 极远的段——评估说"打得通"、实际飞行沿段扫过去半路撞墙（有 60/60 全空炮的先例）
inline constexpr double kAiFlightStep = 0.25;

// 曲线形状分类（"按类型配平"的择优与出手多样性统计共用）：step/floor 形式都归
// 阶梯（折线链、锯齿是它的近亲）；微分方程按模式拆开——形状观感各不相同
enum class CurveKind { Poly, Wave, Stair, Bend, Polar, Parametric, Ode1, Ode2, Ypp };

// 按模式与表达式特征给曲线分类
CurveKind classify_curve(const CurveRequest& req);

// 候选曲线：生成后立即采样并评估
struct CurveCandidate {
    CurveRequest req;
    CurveResult sampled;
    CurveKind kind = CurveKind::Poly;
    // 按飞行顺序判定（命中结算先于撞墙，与 step_flight 同序）：
    bool reaches = false;   // 撞上遮挡物之前碰到了瞄准点（先撞墙的线打不到目标）
    bool clear = false;     // 命中且全程不撞遮挡物（含命中点之后）——择优偏好的干净弹道
    bool blocked = false;   // 全程碰到过遮挡物实体（不论是否已命中）——一致性测试比对用
    double aim_gap = 0.0;   // 路径到瞄准点的最近距离（越小打得越"正"，困难 AI 择优用）
};

// 数字 → 表达式文本（保留 4 位小数并去掉无意义尾零）
std::string num_text(double v);
// 升幂系数 → 多项式表达式文本，如 "0.12*x^2 - 1.5*x"
std::string poly_text(const std::vector<double>& coeffs, const char* var);

// 采样一条曲线，并评估"能否到达瞄准点"与"是否不撞遮挡物"
CurveCandidate evaluate_candidate(const CurveRequest& req, const Vec2& origin, const Vec2& aim,
                                  const World& world);

// 直线/抛物线（主轴上 u 的 a*u^2 + k*u，offset = 0 即直线）——
// 任何难度都用它做基础形状，offset = 0 的直线还是最终的兜底
CurveRequest make_parabola(bool horizontal, double dx, double dy, double offset);

// 二阶微分方程候选 y'' = 2a（常值加速度）：过瞄准点的解是一条抛物线，
// 与 make_parabola 的 (k, a) 同构；角度取行进方向上的初斜率（ypp 模式的表达方式）
CurveRequest make_ypp(double dx, double dy, double offset);

// 过两个相对点 (dx1,dy1)、(dx2,dy2) 的插值抛物线（y=f(x) 或 x=f(y) 由 horizontal 选）：
// "一发扫两个"的骨架。调用方保证两点在所选轴向同侧且坐标不同（单值曲线只朝一侧延伸）
CurveRequest make_parabola_through(bool horizontal, double dx1, double dy1, double dx2,
                                   double dy2);

// 绕障抛物线：同一 f(u) = a*u^2 + k*u 形式，过原点、在 u = t_ob 处高 h_ob（按挡路
// 障碍的顶/底缘实算，不用猜"拱多高才够"）、在 u = t 处仍经过瞄准点——三点唯一解
CurveRequest make_obstacle_arc(bool horizontal, double dx, double dy, double t_ob, double h_ob);

// 路径上第一个挡路的障碍索引（逐步长扫描，与飞行同序）；没有返回 -1
int first_blocking_obstacle(const Path& path, const World& world);

// 与"路径整体包围盒"相交的障碍索引列表。逐子段扫描前用它把远障碍剔掉：被剔的
// 障碍与任何子段的包围盒也无重叠（子段 ⊆ 路径），段判定内部本来就会首步 AABB
// 拒绝——跳过它们不改变任何判定结果，只省调用
std::vector<size_t> obstacles_near_path(const Path& path, const World& world);

// ---------- 多杀骨架（过多个目标点的复合曲线族） ----------
// 与上面单目标生成器不同：这些曲线一步就瞄准两个（或更多）敌人，供困难/大神 AI
// 的多目标规划挑选。骨架多样是为了观感与战术都丰富（阶梯/折线/波浪/螺线各有所长，
// 不同地形的可行形状不同）

// 折线链：分段直线顺序穿过所有插值点（rel 为相对坐标，原点由生成器补上），
// 每经过一个目标换一次斜率——"连点成线"式多杀。与阶梯互补：阶梯是平段+竖直跳变，
// 折线是连续斜段（step 复合形式：Σ 斜率差*(u-ui)*step(u-ui)）
CurveRequest make_kink_chain(bool horizontal, const std::vector<Vec2>& rel);

// 阶梯族：平段 + 垂直跳变（每隔 step_w 抬升 step_h）；锯齿是每齿爬升后落回。
// 公开给单测：评测发现普通档阶梯出手恒为 0%，需要直接构造候选验证可达性
CurveRequest make_stairs(bool horizontal, double dx, double dy, double step_h, double step_w);
CurveRequest make_stairs_wave(bool horizontal, double dx, double dy, double step_h, double step_w,
                              double amp);
CurveRequest make_sawtooth(bool horizontal, double dx, double dy, double amp, double tooth_w);

// 单折点折线（abs 复合形式）：折点在自变量 p 处，折点前后各一条直线，恰好穿过两个
// 目标点。p 在"起点与第一目标之间或两目标之间"挪动得到一族形状（先平后斜/先斜后平）。
// p 不在有效区间时返回 false
bool make_kink_bend(bool horizontal, double dx, double dy, double ox, double oy, double p,
                    CurveRequest& out);

// 波浪过两点：y = m*u + A*sin(w*u)（多项式+三角函数复合）。给定角频率 w 解出 (m, A)
// （两方程两未知数）；w 由调用方按两点跨度扫描——不同频率得到不同波形的多杀线。
// 系数病态（解发散）时返回 false
bool make_wave_through(bool horizontal, double dx, double dy, double ox, double oy, double w,
                       CurveRequest& out);

// 抛物+正弦合并：y = a*u^2 + b*u + c*sin(w*u)。w 随两点跨度固定（约 1.25 个波），
// c 由调用方给正负两组——正弦项把抛物线抬起/压下，形状比纯抛物线更"绕"，能借道避障
bool make_poly_wave_through(bool horizontal, double dx, double dy, double ox, double oy, double c,
                            CurveRequest& out);

// 螺线过两点：r = k*theta + c*theta^2（极坐标；theta=0 处 r=0，起点在发射者身上）。
// 要求两个目标在同一侧角度（同号）且角差足够大——螺旋弧线扫过两人，形状与直角类完全不同
bool make_spiral_through(double dx, double dy, double ox, double oy, CurveRequest& out);

// 三点版抛物+正弦合并：y = a*u^2 + b*u + c*sin(w*u) 过三个目标点（3 未知数 3 约束）。
// w 由调用方按首末点跨度扫描——不同频率给出不同波形，多边形与波形叠在一起的三人多杀线
bool make_poly_wave3_through(bool horizontal, const Vec2& p1, const Vec2& p2, const Vec2& p3,
                             double w, CurveRequest& out);

// 三点版双频波浪：y = a*u + b*sin(w*u) + c*sin(2*w*u) 过三个目标点（3 未知数 3 约束）。
// 与三点抛物波浪同族但基不同（线性主干+高低两个频率），绕障时摆动更自由
bool make_wave3_through(bool horizontal, const Vec2& p1, const Vec2& p2, const Vec2& p3, double w,
                        CurveRequest& out);

// dy/dx = k + A*heaviside(x - 折点) 的折线候选（定义在 ai_ode.cpp）：
// 暴露出来供单测直接验证积分方向的符号（历史上这里混用过 |dx| 与 dx）
CurveRequest make_ode_slope_step(double dx, double dy, double x0, double offset);

// 生成一批候选：代数与几何类（本文件）加微分方程类（ai_ode.cpp）。
// difficulty 决定形状集合与后续挑选策略：0 = 简单（仅直线/抛物线，不筛选）；
// 1 = 普通（全模式，随机挑可行的）；2 = 困难（全模式，每次都选最优 + 多目标规划）
std::vector<CurveCandidate> generate_candidates(bool horizontal, double dx, double dy,
                                                int difficulty, const Vec2& origin, const Vec2& aim,
                                                const World& world, Rng& rng);

// 微分方程类候选（定义在 ai_ode.cpp）
void add_ode_candidates(std::vector<CurveCandidate>& out, bool horizontal, double dx, double dy,
                        const Vec2& origin, const Vec2& aim, const World& world, Rng& rng);

}  // namespace fg
