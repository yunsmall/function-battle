#include "rng.h"

namespace fg {
namespace {

// splitmix64：把单个 64 位种子扩展成 xoshiro256** 所需的 4 个非零状态字
uint64_t splitmix64(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

}  // namespace

Rng::Rng(uint64_t seed) {
    uint64_t s = seed;
    for (auto& v : state_) v = splitmix64(s);
}

uint64_t Rng::next_u64() {
    const uint64_t result = rotl(state_[1] * 5, 7) * 9;
    const uint64_t t = state_[1] << 17;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= t;
    state_[3] = rotl(state_[3], 45);
    return result;
}

// 取高 53 位映射到 [0,1)：double 尾数正好 52+1 位，这样不会产生取模偏置
double Rng::uniform() {
    return static_cast<double>(next_u64() >> 11) * (1.0 / 9007199254740992.0);
}

double Rng::range(double lo, double hi) { return lo + uniform() * (hi - lo); }

int Rng::range_int(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + static_cast<int>(next_u64() % static_cast<uint64_t>(hi - lo + 1));
}

bool Rng::chance(double p) { return uniform() < p; }

}  // namespace fg
