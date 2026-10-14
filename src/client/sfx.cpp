#include "sfx.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <raylib.h>

namespace fg {

namespace {

constexpr int kRate = 44100;

Sound g_shot{}, g_hit{}, g_death{}, g_explode{}, g_turn{};
bool g_loaded = false;

// 把采样函数渲染成 16bit 单声道 Wave 并加载为 Sound
template <typename F>
Sound make_sound(float seconds, float volume, F sample) {
    const int n = static_cast<int>(seconds * kRate);
    std::vector<int16_t> pcm(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / kRate;
        const float v = std::clamp(sample(t, seconds), -1.0f, 1.0f);
        pcm[static_cast<size_t>(i)] = static_cast<int16_t>(v * 32767.0f);
    }
    Wave w{};
    w.frameCount = static_cast<unsigned int>(n);
    w.sampleRate = kRate;
    w.sampleSize = 16;
    w.channels = 1;
    w.data = pcm.data();
    Sound s = LoadSoundFromWave(w);
    SetSoundVolume(s, volume);
    return s;
}

void play(Sound s) {
    if (g_loaded && IsAudioDeviceReady()) PlaySound(s);
}

}  // namespace

void sfx_init() {
    if (g_loaded || !IsAudioDeviceReady()) return;

    // 发射："咻"——正弦 260→900Hz 扫频（相位按扫频公式积分），快出快收
    g_shot = make_sound(0.16f, 0.30f, [](float t, float T) {
        constexpr float f0 = 260.0f, f1 = 900.0f;
        const float phase = 2.0f * PI * (f0 * t + (f1 - f0) * t * t / (2.0f * T));
        const float env = 1.0f - t / T;
        return std::sin(phase) * env * env * 0.9f;
    });

    // 命中："啪"——方波短打 + 极短噪声毛刺
    g_hit = make_sound(0.10f, 0.32f, [](float t, float T) {
        const float sq = std::sin(2.0f * PI * 210.0f * t) >= 0.0f ? 1.0f : -1.0f;
        const float noise = (static_cast<float>(GetRandomValue(-100, 100)) / 100.0f) *
                            std::max(0.0f, 1.0f - t / 0.02f);
        const float env = 1.0f - t / T;
        return (sq * 0.7f + noise * 0.5f) * env * env;
    });

    // 阵亡："咚"——180→45Hz 下坠方波（参考实现的广场波下坠）
    g_death = make_sound(0.45f, 0.34f, [](float t, float T) {
        constexpr float f0 = 180.0f, f1 = 45.0f;
        const float phase = 2.0f * PI * (f0 * t + (f1 - f0) * t * t / (2.0f * T));
        const float sq = std::sin(phase) >= 0.0f ? 1.0f : -1.0f;
        const float env = 1.0f - t / T;
        return (sq * 0.6f + std::sin(phase) * 0.4f) * env * env;
    });

    // 爆炸："轰"——白噪声 + 两级平滑（低通出"闷"感）
    {
        const int n = static_cast<int>(0.40f * kRate);
        std::vector<float> buf(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            buf[static_cast<size_t>(i)] =
                static_cast<float>(GetRandomValue(-100, 100)) / 100.0f;
        }
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 1; i < n; ++i) {
                buf[static_cast<size_t>(i)] =
                    0.5f * (buf[static_cast<size_t>(i)] + buf[static_cast<size_t>(i - 1)]);
            }
        }
        g_explode = make_sound(0.40f, 0.40f, [&](float t, float T) {
            const int i = std::min(n - 1, static_cast<int>(t * kRate));
            const float env = 1.0f - t / T;
            return buf[static_cast<size_t>(i)] * 3.2f * env * env;
        });
    }

    // 回合提示："叮"——880Hz + 五度泛音，轻而短
    g_turn = make_sound(0.15f, 0.22f, [](float t, float T) {
        const float env = 1.0f - t / T;
        return (std::sin(2.0f * PI * 880.0f * t) * 0.7f +
                std::sin(2.0f * PI * 1320.0f * t) * 0.3f) *
               env * env;
    });

    g_loaded = true;
}

void sfx_shutdown() {
    if (!g_loaded) return;
    UnloadSound(g_shot);
    UnloadSound(g_hit);
    UnloadSound(g_death);
    UnloadSound(g_explode);
    UnloadSound(g_turn);
    g_loaded = false;
}

bool sfx_ready() { return g_loaded; }

void sfx_shot() { play(g_shot); }
void sfx_hit() { play(g_hit); }
void sfx_death() { play(g_death); }
void sfx_explode() { play(g_explode); }
void sfx_turn() { play(g_turn); }

}  // namespace fg
