#pragma once
// 曲线模块：把玩家提交的表达式（七种模式）采样成全局轨迹折线，并做弧长参数化。
//
// 统一规则：先在"局部坐标"里画出曲线 L(p)（p 是模式的自变量），
// 然后把参数起点的点 L(p_start) 平移到玩家位置，得到全局轨迹。
// 由此推导出的特例：
//   y = f(x)：全局轨迹 y = f(x - x0) + (y0 - f(0))，锚点是 (0, f(0))
//   x = f(y)：全局轨迹 x = f(y - y0) + (x0 - f(0))，锚点是 (f(0), 0)
// 这正是"把函数图像与 y 轴的交点移到发射位置"；函数里的常数项对轨迹没有任何影响。
//
// 镜像（mirror）：B 队人类玩家在镜像视角下操作（屏幕左右翻转、坐标显示为 -x），
// 提交时给请求打 mirror 标记。相对坐标模式（y=f(x) 等）的曲线关于"过锚点的竖轴"
// 翻转；微分方程类在玩家的镜像坐标空间里积分后逐点映射回全局——两类都等价于
// "把玩家自己的坐标系关于 y 轴镜像后照着原样画"，因此玩家始终所见即所写。

#include <optional>
#include <string>
#include <vector>

#include "config.h"
#include "error.h"
#include "vec2.h"

namespace fg {

// 玩家提交的曲线请求
struct CurveRequest {
    CurveMode mode = CurveMode::YofX;
    std::string expr_a;  // y=f(x) / x=f(y) / r=f(theta) / x(t) / dy-dx=f(x,y) / f(t,x,y) / y''
    std::string expr_b;  // 仅参数方程的 y(t) 与 (x',y') 的 g(t,x,y) 使用
    FireDir dir = FireDir::Positive;
    std::optional<double> p_start;  // 极坐标/参数方程自变量范围（空则为默认起点 0）
    std::optional<double> p_end;    // （空则为默认终点：极坐标 2*pi；参数方程 1000——
                                    //   t 本身无上限，扫到出界或长度到 2×斜对角线为止）
    bool mirror = false;            // 队伍镜像视角的提交（见文件头说明）
    // 仅 Ypp 模式的初斜率角（弧度）：y'(0) = tan(angle)；空 = 0（沿 x 轴出发）
    std::optional<double> angle;
};

// 折线路径 + 弧长参数化；炮弹沿它按弧长匀速运动
struct Path {
    std::vector<Vec2> points;
    std::vector<double> cum;  // cum[i] = 起点到 points[i] 的累计弧长

    double length() const { return cum.empty() ? 0.0 : cum.back(); }
    bool valid() const { return points.size() >= 2; }
    // 按弧长取位置：线性插值，越界钳制到端点
    Vec2 at(double s) const;
};

struct CurveResult {
    Path path;
    Error error;  // code 非空表示生成失败
    bool ok() const { return error.ok(); }
};

// 采样并定位曲线。origin 为玩家位置（发射点）。
CurveResult sample_curve(const CurveRequest& req, const Vec2& origin, const GameConfig& cfg);

}  // namespace fg
