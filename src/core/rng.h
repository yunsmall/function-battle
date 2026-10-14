#pragma once
// 确定性随机数生成器（xoshiro256**）。地图生成、出生点、AI 决策全部走它，
// 保证同一种子在任何机器上得到完全相同的结果，联机时服务端与客户端可复现同一对局。

#include <cstdint>

namespace fg {

class Rng {
public:
    explicit Rng(uint64_t seed);

    uint64_t next_u64();
    // [0, 1) 均匀分布
    double uniform();
    // [lo, hi) 均匀分布
    double range(double lo, double hi);
    // [lo, hi] 整数均匀分布
    int range_int(int lo, int hi);
    // 以 p 概率返回 true
    bool chance(double p);

private:
    uint64_t state_[4];
};

}  // namespace fg
