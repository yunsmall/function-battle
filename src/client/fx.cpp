#include "fx.h"

#include <algorithm>
#include <cmath>

#include "render.h"
#include "ui_style.h"

namespace fg {

namespace {

float randf(float a, float b) {
    return a + (b - a) * (static_cast<float>(GetRandomValue(0, 10000)) / 10000.0f);
}

// 快出慢停：环一秒就冲到尽头不好看，留一段"慢慢定格"的尾巴
float ease_out(float t) { return 1.0f - (1.0f - t) * (1.0f - t); }

}  // namespace

void FxSystem::update(float dt) {
    for (auto& r : rings) r.age += dt;
    rings.erase(std::remove_if(rings.begin(), rings.end(),
                               [](const FxRing& r) { return r.age >= r.life; }),
                rings.end());
    for (auto& p : parts) {
        p.age += dt;
        p.pos += p.vel * static_cast<double>(dt);
        p.vel *= std::max(0.0, 1.0 - 3.0 * static_cast<double>(dt));  // 阻尼
        p.rot += p.rot_v * dt;
    }
    parts.erase(std::remove_if(parts.begin(), parts.end(),
                               [](const FxParticle& p) { return p.age >= p.life; }),
                parts.end());
}

void FxSystem::draw(const View& view) const {
    for (const auto& r : rings) {
        const float t = std::clamp(r.age / r.life, 0.0f, 1.0f);
        const float radius = (r.r0 + (r.r1 - r.r0) * ease_out(t)) * view.scale;
        Color c = r.color;
        c.a = static_cast<unsigned char>(static_cast<float>(c.a) * (1.0f - t));
        const Vector2 center = view.to_screen(r.pos);
        const float inner = std::max(0.0f, radius - r.width);
        DrawRing(center, inner, radius, 0.0f, 360.0f, 48, c);
    }
    for (const auto& p : parts) {
        const float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
        Color c = p.color;
        c.a = static_cast<unsigned char>(static_cast<float>(c.a) * (1.0f - t));
        const Vector2 s = view.to_screen(p.pos);
        const float px = std::max(1.5f, p.size * view.scale);
        DrawRectanglePro(Rectangle{s.x - px * 0.5f, s.y - px * 0.5f, px, px},
                         Vector2{px * 0.5f, px * 0.5f}, p.rot * 57.29578f, c);
    }
}

void FxSystem::spawn_hit(const Vec2& pos, Color team_color) {
    FxRing white;
    // 高光白环跟随主题（浅色主题取深色，否则白环糊在浅底上看不见）
    white.color = kProjectile;
    white.pos = pos;
    white.life = 0.30f;
    white.r0 = 0.25f;
    white.r1 = 2.4f;
    white.width = 3.0f;
    rings.push_back(white);

    FxRing tint;
    tint.pos = pos;
    tint.life = 0.55f;
    tint.r0 = 0.2f;
    tint.r1 = 3.6f;
    tint.width = 1.8f;
    tint.color = team_color;
    tint.color.a = 210;
    rings.push_back(tint);

    for (int i = 0; i < 8; ++i) {
        const float a = randf(0.0f, 6.2832f);
        FxParticle p;
        p.pos = pos;
        p.vel = Vec2(std::cos(a), std::sin(a)) * static_cast<double>(randf(2.5f, 7.0f));
        p.life = 0.30f + randf(0.0f, 0.25f);
        p.size = 0.07f + randf(0.0f, 0.08f);
        p.rot_v = randf(-9.0f, 9.0f);
        p.color = kSpark;
        parts.push_back(p);
    }
}

void FxSystem::spawn_explosion(const Vec2& pos) {
    FxRing flash;
    flash.pos = pos;
    flash.life = 0.16f;
    flash.r0 = 0.1f;
    flash.r1 = 1.7f;
    flash.width = 6.0f;
    flash.color = kProjectile;  // 爆炸白闪同样跟随主题高光色
    rings.push_back(flash);

    FxRing fire;
    fire.pos = pos;
    fire.life = 0.45f;
    fire.r0 = 0.3f;
    fire.r1 = 3.4f;
    fire.width = 3.0f;
    fire.color = Color{255, 160, 70, 235};
    rings.push_back(fire);

    FxRing dust;
    dust.pos = pos;
    dust.life = 0.8f;
    dust.r0 = 0.6f;
    dust.r1 = 4.8f;
    dust.width = 1.5f;
    dust.color = Color{198, 130, 80, 130};
    rings.push_back(dust);

    // 调色板随主题：橙火花/金黄用主题色（浅色主题自动取深色一档），灰烬固定
    const Color palette[] = {kSpark, kGold, Color{152, 142, 132, 255}};
    for (int i = 0; i < 14; ++i) {
        const float a = randf(0.0f, 6.2832f);
        FxParticle p;
        p.pos = pos;
        p.vel = Vec2(std::cos(a), std::sin(a)) * static_cast<double>(randf(2.0f, 7.5f));
        p.life = 0.45f + randf(0.0f, 0.5f);
        p.size = 0.10f + randf(0.0f, 0.20f);
        p.rot_v = randf(-10.0f, 10.0f);
        p.color = palette[GetRandomValue(0, 2)];
        parts.push_back(p);
    }
}

void FxSystem::spawn_death(const Vec2& pos, Color team_color) {
    FxRing ring;
    ring.pos = pos;
    ring.life = 0.35f;
    ring.r0 = 0.2f;
    ring.r1 = 2.0f;
    ring.width = 2.5f;
    ring.color = team_color;
    ring.color.a = 200;
    rings.push_back(ring);

    for (int i = 0; i < 10; ++i) {
        const float a = randf(0.0f, 6.2832f);
        FxParticle p;
        p.pos = pos;
        p.vel = Vec2(std::cos(a), std::sin(a)) * static_cast<double>(randf(2.0f, 5.5f));
        p.life = 0.5f + randf(0.0f, 0.5f);
        p.size = 0.12f + randf(0.0f, 0.16f);
        p.rot_v = randf(-11.0f, 11.0f);
        p.color = (i % 2 == 0) ? team_color : Color{112, 112, 122, 255};
        parts.push_back(p);
    }
}

}  // namespace fg
