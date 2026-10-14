// 冒烟测试：验证构建、测试框架与基础工具（Vec2 / Rng）可用
#include <gtest/gtest.h>

#include "rng.h"
#include "vec2.h"

TEST(Vec2, BasicOps) {
    fg::Vec2 a(3.0, 4.0);
    EXPECT_DOUBLE_EQ(a.norm(), 5.0);
    EXPECT_DOUBLE_EQ(a.squaredNorm(), 25.0);
    EXPECT_DOUBLE_EQ(fg::safe_normalized(a).norm(), 1.0);
    EXPECT_DOUBLE_EQ(fg::distance(fg::Vec2(0, 0), fg::Vec2(3, 4)), 5.0);
    EXPECT_DOUBLE_EQ((fg::Vec2(1, 2) + fg::Vec2(3, 4)).x(), 4.0);
    EXPECT_DOUBLE_EQ((fg::Vec2(1, 2) * 3.0).y(), 6.0);
}

TEST(Vec2, ZeroNormalize) {
    // 零向量归一化不应产生 NaN（Eigen 原生 normalized() 会给出 NaN）
    fg::Vec2 z = fg::safe_normalized(fg::Vec2(0.0, 0.0));
    EXPECT_DOUBLE_EQ(z.x(), 0.0);
    EXPECT_DOUBLE_EQ(z.y(), 0.0);
}

TEST(Rng, Deterministic) {
    fg::Rng a(42), b(42);
    for (int i = 0; i < 100; ++i) EXPECT_EQ(a.next_u64(), b.next_u64());

    fg::Rng c(43), d(42);
    EXPECT_NE(c.next_u64(), d.next_u64());
}

TEST(Rng, RangeBounds) {
    fg::Rng r(7);
    for (int i = 0; i < 1000; ++i) {
        double v = r.range(-5.0, 5.0);
        EXPECT_GE(v, -5.0);
        EXPECT_LT(v, 5.0);
        int k = r.range_int(0, 3);
        EXPECT_GE(k, 0);
        EXPECT_LE(k, 3);
    }
}

TEST(Rng, Uniformity) {
    // 粗查分布：1 万个样本的平均值应接近 0.5
    fg::Rng r(12345);
    double sum = 0.0;
    const int n = 10000;
    for (int i = 0; i < n; ++i) sum += r.uniform();
    EXPECT_NEAR(sum / n, 0.5, 0.02);
}
