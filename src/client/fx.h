#pragma once
// 一次性战斗特效（命中冲击环 / 炸坑爆炸 / 阵亡碎裂）。
// 由对局消息驱动生成、按寿命衰减；纯表现层，不参与任何对局逻辑。

#include <vector>

#include <raylib.h>

#include "vec2.h"

namespace fg {

struct View;

// 扩散环：冲击波的"圈"
struct FxRing {
    Vec2 pos;
    float age = 0.0f, life = 0.4f;
    float r0 = 0.1f, r1 = 2.5f;  // 世界单位的起始/终止半径
    float width = 2.5f;          // 像素线宽（不随缩放）
    Color color{255, 255, 255, 255};
};

// 碎屑/火花粒子：简单阻尼 + 自旋，短命
struct FxParticle {
    Vec2 pos, vel;
    float age = 0.0f, life = 0.6f;
    float size = 0.15f;  // 世界单位（边长）
    float rot = 0.0f, rot_v = 0.0f;
    Color color{255, 255, 255, 255};
};

struct FxSystem {
    std::vector<FxRing> rings;
    std::vector<FxParticle> parts;

    void update(float dt);
    void draw(const View& view) const;

    // 事件特效（由 app 在对局消息处调用）
    void spawn_hit(const Vec2& pos, Color team_color);        // 命中士兵：冲击环 + 火花
    void spawn_explosion(const Vec2& pos);                    // 炸坑：闪光 + 火环 + 碎屑
    void spawn_death(const Vec2& pos, Color team_color);      // 阵亡：碎裂方块

    bool empty() const { return rings.empty() && parts.empty(); }
};

}  // namespace fg
