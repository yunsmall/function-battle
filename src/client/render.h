#pragma once
// 世界渲染：把 core 的世界数据画到 raylib 窗口上（坐标网格、玩家方块、遮挡物与坑、轨迹、炮弹）。

#include <raylib.h>

#include <map>
#include <string>
#include <vector>

#include "config.h"
#include "curve.h"
#include "fx.h"
#include "world.h"

namespace fg {

// 世界坐标 ↔ 屏幕坐标的变换（世界 y 向上，屏幕 y 向下）
struct View {
    float scale = 1.0f;
    float screen_x = 0.0f;  // 地图区域左上角的屏幕坐标
    float screen_y = 0.0f;
    double wx_min = -60.0;
    double wx_max = 60.0;
    double wy_max = 40.0;
    // 镜像视角（B 队在 mirror_view 房间里的所见）：屏幕左右翻转、坐标刻度显示为 -x。
    // 玩家"所见即所写"——照着屏幕上看到的数字写函数，提交时在镜像空间里采样
    bool mirror = false;

    Vector2 to_screen(const Vec2& p) const {
        const double x = mirror ? (wx_min + wx_max - p.x()) : p.x();
        return Vector2{screen_x + static_cast<float>(x - wx_min) * scale,
                       screen_y + static_cast<float>(wy_max - p.y()) * scale};
    }
    // 按地图范围适配到给定屏幕区域（保持比例，居中）
    static View fit(const GameConfig& cfg, Rectangle area);
};

// 一帧要画的内容
struct RenderScene {
    const GameConfig* cfg = nullptr;
    const std::vector<Player>* players = nullptr;
    const std::vector<Obstacle>* obstacles = nullptr;
    const Path* preview = nullptr;  // 可选：输入框的本地预览轨迹
    bool has_projectile = false;
    Vec2 projectile;
    const Path* projectile_path = nullptr;  // 可选：当前炮弹的完整轨迹（淡色显示）
    double projectile_travel = 0.0;
    int my_id = -1;
    int current_player = -1;  // 当前回合玩家（高亮）
    int next_player = -1;     // 下一个出手的玩家（细环提示；-1 = 不显示）
    int next_soldier = 0;
    // 各玩家"最近用过的函数"（player id → 文本）：显示在名字牌下方（可空）
    const std::map<int, std::string>* last_func = nullptr;
    // 当前输入者"正在写"的实时预览（null = 没有人在写）：挂在当前士兵的名字牌下，
    // 优先级高于 last_func（正在写的才是"这时最该看的内容"，全桌人都看得见）
    const std::string* preview_text = nullptr;
    const std::map<int, Font>* fonts = nullptr;  // 档位字体表（App::fonts_）；文字按最近档绘制
    const FxSystem* fx = nullptr;  // 战斗特效（命中/爆炸/碎裂）
    float time = 0.0f;        // 秒；呼吸光圈、预览虚线相位等动画
};

// 绘制整个对战场景（调用方负责 BeginDrawing/EndDrawing 与裁剪区域）
void draw_scene(const View& view, const RenderScene& scene);

// 把路径画成折线
void draw_path(const View& view, const Path& path, Color color, float thickness);

}  // namespace fg
