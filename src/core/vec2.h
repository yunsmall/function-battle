#pragma once
// 二维向量与几何辅助。底层用 Eigen（Eigen::Vector2d），后续如需线性代数可直接复用；
// 这里只补充游戏需要的安全语义和常用几何函数。

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

}  // namespace fg
