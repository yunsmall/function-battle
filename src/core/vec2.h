#pragma once
// 二维向量与几何辅助。底层用 Eigen（Eigen::Vector2d），后续如需线性代数可直接复用；
// 这里只补充游戏需要的安全语义和常用几何函数。

#include <algorithm>

#include <Eigen/Core>

namespace fg {

using Vec2 = Eigen::Vector2d;

// 安全归一化：零向量返回零向量。
// Eigen 原生 normalized() 对零向量会产生 NaN，而炮弹方向一旦变 NaN 会污染整个物理模拟。
inline Vec2 safe_normalized(const Vec2& v) {
    const double n = v.norm();
    return (n > 1e-12) ? Vec2(v / n) : Vec2::Zero();
}

inline double distance(const Vec2& a, const Vec2& b) { return (a - b).norm(); }

// 点到线段的最近点参数（钳制到 [0,1]；退化段返回 0）：沿线段几何排序用
inline double point_segment_t(const Vec2& p, const Vec2& a, const Vec2& b) {
    const Vec2 ab = b - a;
    const double len2 = ab.squaredNorm();
    if (len2 < 1e-18) return 0.0;
    return std::clamp((p - a).dot(ab) / len2, 0.0, 1.0);
}

// 点到线段的距离（退化段按点处理）。飞行判定与 AI 评估共用它：两边对
// "掠过目标算不算命中"的结论必须一致，各写一份迟早漂移
inline double point_segment_distance(const Vec2& p, const Vec2& a, const Vec2& b) {
    return (p - (a + point_segment_t(p, a, b) * (b - a))).norm();
}

}  // namespace fg
