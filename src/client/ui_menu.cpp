#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>

#include "ai.h"
#include "curve.h"
#include "render.h"
#include "ui_style.h"

// 版本号由 CMake 从顶层 project(VERSION) 注入（发布时只改一处）；
// 无定义时兜底，保证 IDE 单独解析等场景也能编译
#ifndef FG_VERSION
#define FG_VERSION "dev"
#endif

namespace fg {

// ---------------- 主菜单 ----------------

void App::update_menu() {
    // 菜单无需键盘输入
}

void App::draw_menu() {
    const float t = static_cast<float>(GetTime());
    // 背景：缓慢流动的函数曲线，呼应数学主题
    for (int layer = 0; layer < 3; ++layer) {
        const float alpha = 0.10f - layer * 0.025f;
        const Color c = (layer == 1) ? Fade(kTeamB, alpha) : Fade(kAccent, alpha);
        const float amp = 46.0f + layer * 26.0f;
        const float freq = 0.006f + layer * 0.0016f;
        const float speed = 0.25f + layer * 0.12f;
        Vector2 prev{0.0f, kWinH * 0.5f};
        for (float x = 0.0f; x <= kWinW; x += 8.0f) {
            const float y = kWinH * (0.32f + layer * 0.18f) +
                            std::sin(x * freq + t * speed + layer * 2.1f) * amp +
                            std::sin(x * freq * 2.7f - t * speed * 0.7f) * amp * 0.3f;
            const Vector2 cur{x, y};
            DrawLineEx(prev, cur, 2.0f, c);
            prev = cur;
        }
    }

    // 标题
    const float title_size = 64.0f;
    const std::string title = "函数对战";
    const float tw = text_width(title, title_size);
    // 标题发光
    text(title, kWinW * 0.5f - tw * 0.5f + 2.0f, 132.0f + 2.0f, title_size, Fade(kAccent, 0.35f));
    text(title, kWinW * 0.5f - tw * 0.5f, 132.0f, title_size, kTextMain);
    const std::string sub = "数学弹道对战 · MATH BATTLE";
    const float sw = text_width(sub, 20.0f);
    text(sub, kWinW * 0.5f - sw * 0.5f, 206.0f, 20.0f, kTextDim);

    // 按钮
    const float bw = 320.0f, bh = 62.0f;
    const float bx = kWinW * 0.5f - bw * 0.5f;
    if (ui_button(Rectangle{bx, 300.0f, bw, bh}, "单机人机")) {
        screen_ = Screen::LocalSetup;
        status_.clear();
        result_text_.clear();
    }
    if (ui_button(Rectangle{bx, 380.0f, bw, bh}, "联机对战")) {
        screen_ = Screen::Lobby;
        status_.clear();
    }
    if (ui_button(Rectangle{bx, 460.0f, bw, bh}, "退出游戏")) {
        quit_ = true;  // 交给主循环退出，让析构正常清理（网络连接等）
    }
    // 主题切换：循环预设配色（深空 → 明亮 → 终端绿），立即全界面生效
    if (ui_button(Rectangle{bx, 540.0f, bw, bh},
                  std::string("主题：") + theme_name(theme_index()))) {
        apply_theme(theme_index() + 1);
    }

    // 版本脚注（版本号来自 CMake 的 project(VERSION)，与发布包一致）
    text("v" FG_VERSION " · C++ / raylib", 16.0f, kWinH - 30.0f, 15.0f, kTextFaint);
}

// ---------------- 单机配置 ----------------

void App::update_local_setup() {
    seed_box_.handle_input();
    // uint64 十进制最长 20 位；截得太短会把用户输入静默变成另一个种子
    if (seed_box_.text.size() > 20) seed_box_.text.resize(20);
    name_box_.handle_input();
}

void App::draw_local_setup() {
    const Rectangle panel{340.0f, 76.0f, 600.0f, 712.0f};
    DrawRectangleRounded(panel, 0.04f, 10, kPanel);
    DrawRectangleRoundedLines(panel, 0.04f, 10, kPanelBorder);
    text("单机人机 · 对局配置", panel.x + 28.0f, panel.y + 22.0f, 26.0f, kTextMain);
    // 上一把打完后回到这里：显示结果，方便改配置再开
    if (!result_text_.empty()) {
        text("上一把：" + result_text_, panel.x + 28.0f, panel.y + 56.0f, 18.0f, kGold);
    }

    float y = panel.y + 84.0f;
    const float label_x = panel.x + 32.0f;
    const float ctrl_x = panel.x + 200.0f;

    // 对局身份：观战 = 你不在阵容里，只看双方人机互打
    text("对局身份", label_x, y + 6.0f, 20.0f, kTextDim);
    if (ui_button(Rectangle{ctrl_x, y, 88.0f, 34.0f}, "参战", !local_spectate_)) {
        local_spectate_ = false;
    }
    if (ui_button(Rectangle{ctrl_x + 96.0f, y, 88.0f, 34.0f}, "观战", local_spectate_)) {
        local_spectate_ = true;
    }
    if (local_spectate_) {
        text("（只看人机互打，你不在队伍里）", ctrl_x + 200.0f, y + 8.0f, 16.0f, kTextFaint);
    }
    y += 46.0f;

    // 每队人数
    text("每队人数", label_x, y + 6.0f, 20.0f, kTextDim);
    for (int i = 1; i <= 5; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", i);
        if (ui_button(Rectangle{ctrl_x + (i - 1) * 56.0f, y, 48.0f, 34.0f}, buf, set_team_size_ == i)) {
            set_team_size_ = i;
        }
    }
    y += 46.0f;

    // 每人士兵数（全场统一；单机每局重建房间，就按当前选择）
    text("每人士兵数", label_x, y + 6.0f, 20.0f, kTextDim);
    for (int i = 1; i <= 4; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", i);
        if (ui_button(Rectangle{ctrl_x + (i - 1) * 56.0f, y, 48.0f, 34.0f}, buf,
                      set_soldiers_ == i)) {
            set_soldiers_ = i;
        }
    }
    y += 46.0f;

    // 人机难度
    text("人机难度", label_x, y + 6.0f, 20.0f, kTextDim);
    const char* diffs[] = {"简单", "普通", "困难", "大神"};
    for (int i = 0; i < static_cast<int>(std::size(diffs)); ++i) {
        if (ui_button(Rectangle{ctrl_x + i * 96.0f, y, 88.0f, 34.0f}, diffs[i], set_difficulty_ == i)) {
            set_difficulty_ = i;
        }
    }
    y += 46.0f;

    // 每人血量
    text("每人血量", label_x, y + 6.0f, 20.0f, kTextDim);
    for (int i = 1; i <= 5; ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", i);
        if (ui_button(Rectangle{ctrl_x + (i - 1) * 56.0f, y, 48.0f, 34.0f}, buf, set_hp_ == i)) {
            set_hp_ = i;
        }
    }
    text(set_hp_ == 1 ? "（一击毙命）" : "（需要打中多次）", ctrl_x + 5 * 56.0f + 8.0f, y + 6.0f,
         16.0f, kTextFaint);
    y += 46.0f;

    // 遮挡物数量
    text("遮挡物数量", label_x, y + 6.0f, 20.0f, kTextDim);
    const int obstacles_options[] = {0, 5, 10, 15, 20, 30};
    for (int i = 0; i < static_cast<int>(std::size(obstacles_options)); ++i) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", obstacles_options[i]);
        if (ui_button(Rectangle{ctrl_x + i * 66.0f, y, 56.0f, 34.0f}, buf,
                      set_obstacles_ == obstacles_options[i])) {
            set_obstacles_ = obstacles_options[i];
        }
    }
    y += 44.0f;

    // 先手队伍
    text("先手队伍", label_x, y + 6.0f, 20.0f, kTextDim);
    const char* firsts[] = {"随机", "A 队先手", "B 队先手"};
    for (int i = 0; i < static_cast<int>(std::size(firsts)); ++i) {
        if (ui_button(Rectangle{ctrl_x + i * 112.0f, y, 104.0f, 34.0f}, firsts[i],
                      set_first_team_ == i - 1)) {
            set_first_team_ = i - 1;
        }
    }
    y += 44.0f;

    // 每回合输入时限
    text("输入时限", label_x, y + 6.0f, 20.0f, kTextDim);
    {
        const int options[] = {15, 30, 60, 120};
        for (int i = 0; i < static_cast<int>(std::size(options)); ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d 秒", options[i]);
            if (ui_button(Rectangle{ctrl_x + i * 76.0f, y, 68.0f, 34.0f}, buf,
                          set_input_seconds_ == options[i])) {
                set_input_seconds_ = options[i];
            }
        }
    }
    y += 44.0f;

    // 曲线参考系（房间属性）：直角模式（y=f(x)、x=f(y)）的自变量是相对士兵还是地图坐标。
    // 与联机创建界面同款：按钮下方一行小字随选择解释两种模式的差别
    text("坐标系", label_x, y + 6.0f, 20.0f, kTextDim);
    {
        const char* names[] = {"以你为原点", "地图坐标"};
        for (int i = 0; i < static_cast<int>(std::size(names)); ++i) {
            if (ui_button(Rectangle{ctrl_x + i * 116.0f, y, 108.0f, 34.0f}, names[i],
                          set_coord_field_ == i)) {
                set_coord_field_ = i;
            }
        }
        text(set_coord_field_ ? "按地图坐标写：曲线整体平移到你身上（y=f(x) 上下、x=f(y) 左右）"
                              : "以你为原点：函数从你写起，0 就在你身上",
             label_x, y + 44.0f, 15.0f, kTextFaint);
    }
    y += 74.0f;  // 行距含按钮下方的一行解释

    // 种子
    text("地图种子", label_x, y + 6.0f, 20.0f, kTextDim);
    // 种子是整数：只放行数字字符，字母/中文会被 strtoull 静默吃成 0（玩家会以为用了别的种子）
    seed_box_.digits_only = true;
    seed_box_.allow_e = false;
    ui_textbox(seed_box_, Rectangle{ctrl_x, y, 220.0f, 36.0f}, "留空则随机");
    y += 50.0f;

    // 名字
    text("你的名字", label_x, y + 6.0f, 20.0f, kTextDim);
    ui_textbox(name_box_, Rectangle{ctrl_x, y, 220.0f, 36.0f}, "玩家");
    if (name_box_.text.empty()) player_name_ = "玩家";
    else player_name_ = name_box_.text;

    // 开始 / 返回
    const float by = panel.y + panel.height - 92.0f;
    if (ui_button(Rectangle{panel.x + 32.0f, by, 200.0f, 56.0f}, "开始对局")) {
        start_local_game();
    }
    if (ui_button(Rectangle{panel.x + panel.width - 232.0f, by, 200.0f, 56.0f}, "返回主菜单")) {
        screen_ = Screen::Menu;
    }
    if (!status_.empty()) {
        text(status_, panel.x + 32.0f, by - 36.0f, 17.0f, kDanger);
    }
}
}  // namespace fg
