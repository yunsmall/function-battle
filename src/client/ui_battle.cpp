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
#include "expr.h"
#include "render.h"
#include "ui_style.h"

namespace fg {

// ---------------- 对战界面 ----------------

void App::update_battle() {
    if (online_) {
        process_network();
    } else {
        tick_local();
    }

    // 参数范围框的过滤规则随模式每帧同步（放 update 而非 draw：draw 里设置会慢一帧，
    // 模式切换后的第一帧会按旧规则放行输入）
    const bool range_mode = needs_range(kModes[mode_index_]);
    p_start_.digits_only = range_mode;
    p_end_.digits_only = range_mode;
    angle_box_.digits_only = (kModes[mode_index_] == CurveMode::Ypp);
    angle_box_.allow_e = false;  // 角度是"度"：输入 1e2 会被读成 100 再夹到 85，误导玩家
    // 表达式框按服务器的字符白名单过滤：非法字符在输入时就被挡掉，不用白跑一趟提交
    expr_a_.expr_chars_only = true;
    expr_b_.expr_chars_only = true;

    // 倒计时（显示用；实际判定在服务器/Room 内）。
    // 按"回合开始时刻 + 时间差"重算而不是逐帧自减：掉帧或拖动窗口后，
    // 显示不会与服务器的判定漂移
    if (battle_.active && battle_.current_player >= 0) {
        battle_.input_left =
            std::max(0.0, battle_.turn_start_left - (GetTime() - battle_.turn_begin_time));
    }

    // 帮助面板打开时只保留模拟/网络推进（上面已做），暂停玩家输入与提交
    if (help_open_) return;

    // 文本输入
    expr_a_.handle_input();
    expr_b_.handle_input();
    p_start_.handle_input();
    p_end_.handle_input();
    angle_box_.handle_input();

    update_preview();

    // 实时函数预览：轮到我时把"正在写"的内容节流广播给全场（含观战者）。
    // 内容没变不重发、200ms 间隔；服务器另有 100ms 限频与"只转发当前回合玩家"的把关
    if (online_ && can_submit() && !expr_a_.text.empty()) {
        const CurveMode pm = kModes[mode_index_];
        std::string preview = std::string(expr_a_label(pm)) + " " + expr_a_.text;
        if (needs_expr_b(pm) && !expr_b_.text.empty()) preview += "，" + expr_b_.text;
        const double now = GetTime();
        if (preview != sent_preview_text_ && now - last_preview_sent_ >= 0.2) {
            sent_preview_text_ = preview;
            last_preview_sent_ = now;
            net_.send(json{{"type", msg_type::kFunctionPreview}, {"preview", preview}});
        }
    }

    if (IsKeyPressed(KEY_ENTER) && can_submit()) submit_current();
}

void App::draw_battle() {
    const float top_h = 46.0f;
    const float right_w = 300.0f;
    const float bottom_h = 214.0f;
    const Rectangle map_area{12.0f, top_h + 8.0f, kWinW - right_w - 30.0f,
                             kWinH - top_h - bottom_h - 22.0f};
    const Rectangle side{map_area.x + map_area.width + 8.0f, map_area.y, right_w - 10.0f,
                         map_area.height};
    const Rectangle bottom{12.0f, map_area.y + map_area.height + 8.0f, kWinW - 24.0f, bottom_h - 10.0f};

    // ---- 顶部信息条 ----
    {
        const Rectangle bar{12.0f, 8.0f, kWinW - 24.0f, top_h - 10.0f};
        DrawRectangleRounded(bar, 0.5f, 8, kPanel);
        DrawRectangleRoundedLines(bar, 0.5f, 8, kPanelBorder);

        std::string turn_label = "等待开始";
        Color turn_color = kTextDim;
        // 两个操作数都是 std::vector<Player> 的 lvalue（仅 cv 不同）：按 [expr.cond]
        // 结果为 glvalue 而非按值产出，这里不发生整表拷贝（已实测 0 次拷贝；任一侧
        // 换成返回 vector 的函数调用等 prvalue 才会拷贝）
        const std::vector<Player>& players = battle_.players ? *battle_.players : battle_.online_players;
        for (const auto& p : players) {
            if (p.id == battle_.current_player) {
                turn_label = (p.id == battle_.my_id) ? "你的回合！" : ("轮到 " + p.name + " 的回合");
                turn_color = (p.id == battle_.my_id) ? kGold : kTextMain;
            }
        }
        if (battle_.winner >= 0) {
            turn_label = "对局结束";
            turn_color = kGold;
        }
        text("第 " + std::to_string(std::max(1, battle_.round)) + " 回合", 28.0f, 20.0f, 19.0f,
             kTextDim);
        text(turn_label, 150.0f, 19.0f, 21.0f, turn_color);

        // 倒计时进度条
        if (battle_.active && battle_.current_player >= 0 && battle_.cfg.match.input_seconds > 0.0) {
            const float frac = static_cast<float>(
                std::clamp(battle_.input_left / battle_.cfg.match.input_seconds, 0.0, 1.0));
            const Rectangle track{kWinW - 470.0f, 18.0f, 300.0f, 12.0f};
            DrawRectangleRounded(track, 0.5f, 6, Color{16, 20, 31, 255});
            const bool urgent = battle_.input_left < 5.0 &&
                                battle_.current_player == battle_.my_id;
            const Color cc = urgent
                                 ? Fade(kDanger, 0.6f + 0.4f * static_cast<float>(
                                                            std::sin(GetTime() * 12.0)))
                                 : (battle_.current_player == battle_.my_id ? kGold : kAccent);
            DrawRectangleRounded(Rectangle{track.x, track.y, track.width * frac, track.height},
                                 0.5f, 6, cc);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "剩余 %.1f 秒", battle_.input_left);
            // 小一号字 + 更靠左：5 位数的剩余时间（如 120.0）不放任右端溢出窗口
            text(buf, track.x + track.width + 12.0f, 17.0f, 17.0f, kTextDim);
        }
        // 观战者在对局界面的下车点：参战者对局中阵容锁定（服务器会拒绝），
        // 只有观战者看得到这个按钮；单机的"只看人机互打"不走联机，不显示。
        // 点一下即隐藏直到服务器回应（防连点触发第二发 leave_room 被当成
        // "不在房间"报错），回大厅由 kLeftRoom 统一处理
        if (online_ && am_spectating() && !battle_.leaving_spectate) {
            if (ui_button(Rectangle{kWinW - 696.0f, 12.0f, 104.0f, 26.0f}, "退出观战")) {
                battle_.leaving_spectate = true;
                net_.send(json{{"type", msg_type::kLeaveRoom}});
            }
        }
        // 单机对局的中途出口：没有队友要等，随时退回去改设置（联机参战者对局中
        // 阵容锁定、没有此按钮；位置与"退出观战"共用——两者条件天然互斥）
        if (!online_ && local_ && battle_.active && battle_.winner < 0) {
            if (ui_button(Rectangle{kWinW - 696.0f, 12.0f, 104.0f, 26.0f}, "退出对局")) {
                local_.reset();
                battle_ = Battle{};
                screen_ = Screen::LocalSetup;
                return;  // battle_ 已清空，本帧不再继续画对局画面
            }
        }
        // 跳过当前地图（全员投票）：参战人类点一下投/撤，票数实时显示
        if (battle_.active && battle_.winner < 0 && !am_spectating() && my_member_id_ >= 0) {
            const bool voted = std::find(battle_.skip_voted.begin(), battle_.skip_voted.end(),
                                         my_member_id_) != battle_.skip_voted.end();
            std::string label = voted ? "已投跳过" : "跳过地图";
            if (battle_.skip_total > 0) {
                label += " " + std::to_string(battle_.skip_voted.size()) + "/" +
                         std::to_string(battle_.skip_total);
            }
            if (ui_button(Rectangle{kWinW - 820.0f, 12.0f, 112.0f, 26.0f}, label, voted)) {
                if (online_) {
                    net_.send(json{{"type", msg_type::kVoteSkip}, {"vote", !voted}});
                } else if (local_) {
                    // 单机直接驱动本地房间（人类只有我一个：点一下就是全票）
                    local_->vote_skip(my_member_id_, !voted);
                }
            }
        }
        // 观战者的视角切换（房间开了镜像规则才有意义）：与投票按钮同位置显示——
        // 观战者没有票、参战者不需要切视角，天然互斥。
        // 注意比投票按钮再左移 16px：观战者同时有"退出观战"按钮（左缘 kWinW-696），
        // 同位置时会与它重叠 8px、点交界处同时触发两个操作
        if (online_ && am_spectating() && battle_.cfg.match.mirror_view) {
            if (ui_button(Rectangle{kWinW - 836.0f, 12.0f, 132.0f, 26.0f},
                          spectate_mirror_ ? "视角：镜像" : "视角：常规")) {
                toggle_spectate_view();
            }
        }
        // 函数帮助入口（顶部栏中段空着）
        if (ui_button(Rectangle{kWinW - 580.0f, 12.0f, 96.0f, 26.0f}, "函数帮助")) {
            help_open_ = true;
            clear_text_focus();  // 打开面板时先松开输入焦点，避免按键继续打进表达式框
        }
        if (!status_.empty()) {
            text(status_, 520.0f, 20.0f, 17.0f, kDanger);
        }
    }

    // ---- 地图 ----
    DrawRectangleRounded(map_area, 0.02f, 8, Color{15, 18, 28, 255});
    View view = View::fit(battle_.cfg, map_area);
    view.mirror = am_i_mirrored();
    RenderScene scene;
    scene.cfg = &battle_.cfg;
    scene.players = battle_.players;
    scene.obstacles = battle_.obstacles;
    scene.preview = battle_.has_preview ? &battle_.preview_path : nullptr;
    scene.has_projectile = battle_.has_projectile;
    scene.projectile = battle_.projectile;
    scene.projectile_path = battle_.has_path ? &battle_.projectile_path : nullptr;
    scene.my_id = battle_.my_id;
    scene.current_player = battle_.current_player;
    scene.next_player = battle_.next_player;
    scene.next_soldier = battle_.next_soldier;
    scene.last_func = &battle_.last_func;
    scene.fonts = &fonts_;
    scene.time = static_cast<float>(GetTime());
    BeginScissorMode(static_cast<int>(map_area.x), static_cast<int>(map_area.y),
                     static_cast<int>(map_area.width), static_cast<int>(map_area.height));
    draw_scene(view, scene);
    EndScissorMode();
    DrawRectangleRoundedLines(map_area, 0.02f, 8, kPanelBorder);

    // ---- 右侧队伍面板 ----
    DrawRectangleRounded(side, 0.05f, 8, kPanel);
    DrawRectangleRoundedLines(side, 0.05f, 8, kPanelBorder);
    {
        // 行步长按总行数自适应：两队各 5 人（共 10 行）时固定 46 会让最后一人的
        // 行画出面板外（98 = 上下边距 14 + 两个队标题 32×2 + 队间 10 + 底部余量 10）
        const float row_step =
            (battle_.players && !battle_.players->empty())
                ? std::min(46.0f,
                           (side.height - 98.0f) / static_cast<float>(battle_.players->size()))
                : 46.0f;
        const float row_h = std::min(40.0f, row_step - 6.0f);
        float y = side.y + 14.0f;
        const char* team_names[] = {"A 队", "B 队"};
        const Color team_colors[] = {kTeamA, kTeamB};
        static_assert(sizeof(team_names) / sizeof(team_names[0]) ==
                          sizeof(team_colors) / sizeof(team_colors[0]),
                      "队名与队色表必须一一对应");
        for (int team = 0; team < static_cast<int>(std::size(team_names)); ++team) {
            text(team_names[team], side.x + 18.0f, y, 22.0f, team_colors[team]);
            y += 32.0f;
            if (battle_.players) {
                for (const auto& p : *battle_.players) {
                    if (static_cast<int>(p.team) != team) continue;
                    const bool cur = (p.id == battle_.current_player);
                    const Rectangle row{side.x + 12.0f, y, side.width - 24.0f, row_h};
                    if (cur) {
                        DrawRectangleRounded(row, 0.4f, 6, Color{46, 58, 84, 255});
                    }
                    // 存活指示灯（行内元素都按行高居中：行高会随人数收缩）
                    const Vector2 dot{row.x + 16.0f, row.y + row.height * 0.5f};
                    DrawCircleV(dot, 5.0f,
                                p.any_alive() ? team_colors[team] : Color{70, 74, 88, 255});
                    std::string label = p.name;
                    if (p.id == battle_.my_id) label += "（你）";
                    if (p.is_ai) label += "（人机）";
                    text(label, row.x + 30.0f, row.y + (row.height - 22.0f) * 0.5f, 17.0f,
                         p.any_alive() ? kTextMain : kTextFaint);
                    // 士兵格：每个士兵一格、格内按血量填充（单士兵时就是普通血条）；
                    // 当前轮到的士兵用金框标出，阵亡士兵只剩暗色底槽
                    const int ns = static_cast<int>(p.soldiers.size());
                    if (ns > 0) {
                        constexpr float bar_w = 74.0f;
                        constexpr float gap = 3.0f;
                        const float seg_w =
                            (bar_w - gap * static_cast<float>(ns - 1)) / static_cast<float>(ns);
                        float hx = row.x + row.width - bar_w;
                        const float hy = row.y + row.height * 0.5f - 3.5f;
                        for (int si = 0; si < ns; ++si) {
                            const Soldier& s = p.soldiers[static_cast<size_t>(si)];
                            const Rectangle hb{hx, hy, seg_w, 7.0f};
                            DrawRectangleRounded(hb, 0.5f, 4, Color{16, 20, 31, 255});
                            if (s.alive) {
                                const float frac = static_cast<float>(s.hp) /
                                                   static_cast<float>(std::max(1, s.max_hp));
                                DrawRectangleRounded(
                                    Rectangle{hb.x, hb.y, hb.width * std::max(0.0f, frac),
                                              hb.height},
                                    0.5f, 4,
                                    frac > 0.5f ? kSuccess : (frac > 0.0f ? kGold : kDanger));
                            }
                            if (cur && si == p.current_soldier) {
                                DrawRectangleRoundedLines(hb, 0.5f, 4, kGold);
                            }
                            hx += seg_w + gap;
                        }
                    }
                    y += row_step;
                }
            }
            y += 10.0f;
        }
    }

    // ---- 底部输入区 ----
    DrawRectangleRounded(bottom, 0.04f, 8, kPanel);
    DrawRectangleRoundedLines(bottom, 0.04f, 8, kPanelBorder);
    {
        // 模式选择行
        float bx = bottom.x + 18.0f;
        for (int i = 0; i < kModeCount; ++i) {
            const float w = 150.0f;
            const bool enabled = can_submit();
            if (ui_button(Rectangle{bx, bottom.y + 14.0f, w, 34.0f}, kModeNames[i],
                          mode_index_ == i, enabled)) {
                mode_index_ = i;
                clear_text_focus();  // 与 ui_textbox 同一规则：转移焦点前先清全部
                expr_a_.focused = true;
            }
            bx += w + 8.0f;
        }

        // 表达式输入
        const CurveMode mode = kModes[mode_index_];
        const float row_y = bottom.y + 62.0f;
        text(expr_a_label(mode), bottom.x + 18.0f, row_y + 10.0f, 19.0f, kTextDim);
        ui_textbox(expr_a_, Rectangle{bottom.x + 96.0f, row_y, 470.0f, 40.0f}, "输入表达式");
        if (needs_expr_b(mode)) {
            // 第二个表达式框与第一个等宽（参数方程/双分量微分方程时并排显示）
            text(expr_b_label(mode), bottom.x + 596.0f, row_y + 10.0f, 19.0f, kTextDim);
            ui_textbox(expr_b_, Rectangle{bottom.x + 740.0f, row_y, 470.0f, 40.0f}, "第二个表达式");
        }
        text(var_hint(mode), bottom.x + 20.0f, row_y + 46.0f, 15.0f, kTextFaint);
        // 别人"正在写"的实时预览（自己的表达式就在输入框里，不重复显示）。
        // 预览可能含中文标点（如参数方程的"，"），按字节截断要回退到 UTF-8 字符边界，
        // 否则切出孤立续字节会让 raylib 解码出乱码
        if (!battle_.preview_text.empty() && battle_.current_player != battle_.my_id) {
            std::string pv = battle_.preview_text;
            if (pv.size() > 64) {
                size_t cut = 64;
                while (cut > 0 && (static_cast<unsigned char>(pv[cut]) & 0xC0) == 0x80) --cut;
                pv = pv.substr(0, cut) + "…";
            }
            text("正在写：" + pv, bottom.x + 540.0f, row_y + 44.0f, 15.0f, kGold);
        }

        // 方向 / 参数范围 / 提交
        const float ctrl_y = bottom.y + 148.0f;
        const char* d0 = kDirNames[mode_index_][0];
        if (d0[0] != '\0') {
            text("方向", bottom.x + 18.0f, ctrl_y + 8.0f, 19.0f, kTextDim);
            if (ui_button(Rectangle{bottom.x + 76.0f, ctrl_y, 110.0f, 36.0f}, d0,
                          dir_index_ == 0, can_submit())) {
                dir_index_ = 0;
            }
            if (ui_button(Rectangle{bottom.x + 192.0f, ctrl_y, 110.0f, 36.0f},
                          kDirNames[mode_index_][1], dir_index_ == 1, can_submit())) {
                dir_index_ = 1;
            }
        }
        if (needs_range(mode)) {
            text("参数范围", bottom.x + 330.0f, ctrl_y + 8.0f, 19.0f, kTextDim);
            ui_textbox(p_start_, Rectangle{bottom.x + 430.0f, ctrl_y, 110.0f, 36.0f}, "起点");
            text("到", bottom.x + 548.0f, ctrl_y + 8.0f, 18.0f, kTextDim);
            ui_textbox(p_end_, Rectangle{bottom.x + 578.0f, ctrl_y, 110.0f, 36.0f}, "终点");
        } else if (mode == CurveMode::Ypp) {
            // 发射角（度）：相对行进方向的仰角，正数朝上方偏；空 = 0（沿轴出发）
            text("发射角(度)", bottom.x + 330.0f, ctrl_y + 8.0f, 19.0f, kTextDim);
            ui_textbox(angle_box_, Rectangle{bottom.x + 452.0f, ctrl_y, 110.0f, 36.0f}, "0");
        }

        // 提交按钮
        const bool can = can_submit();
        const Rectangle submit_btn{bottom.x + bottom.width - 218.0f, ctrl_y, 200.0f, 46.0f};
        const bool hover = CheckCollisionPointRec(GetMousePosition(), submit_btn);
        DrawRectangleRounded(submit_btn, 0.4f, 8,
                             can ? (hover ? Color{96, 186, 255, 255} : kAccent)
                                 : Color{52, 58, 76, 255});
        // 禁用时的文案按身份/阶段区分：观战者是"观战中…"；参战者在等对手回合是
        // "等待对手…"（旧文案对参战者显示"观战中"会让人误解）；结算后是"对局结束"
        std::string stext = "提交函数";
        if (!can) {
            if (battle_.submitted) stext = "已提交";
            else if (am_spectating()) stext = "观战中…";
            else if (battle_.winner >= 0) stext = "对局结束";
            else stext = "等待对手…";
        }
        const float stw = text_width(stext, 22.0f);
        text(stext, submit_btn.x + (submit_btn.width - stw) * 0.5f, submit_btn.y + 11.0f, 22.0f,
             can ? Color{10, 18, 30, 255} : kTextFaint);
        // 手绘按钮同样要消费点击（否则连带清掉输入框焦点）；帮助面板打开时不可响应
        if (can && hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !help_open_) {
            ui_click_consumed_ = true;
            submit_current();
        }
    }

    // ---- 结算遮罩 ----
    if (battle_.winner >= 0) {
        DrawRectangle(0, 0, kWinW, kWinH, Color{8, 10, 16, 180});
        const Rectangle card{kWinW * 0.5f - 280.0f, kWinH * 0.5f - 160.0f, 560.0f, 320.0f};
        DrawRectangleRounded(card, 0.06f, 10, kPanel);
        DrawRectangleRoundedLines(card, 0.06f, 10, kAccent);
        const std::string title = "对局结束";
        float tw = text_width(title, 40.0f);
        text(title, card.x + (card.width - tw) * 0.5f, card.y + 30.0f, 40.0f, kTextMain);
        tw = text_width(result_text_, 30.0f);
        text(result_text_, card.x + (card.width - tw) * 0.5f, card.y + 92.0f, 30.0f, kGold);

        // 联机时"再来一局"只有房主可用（其他人回准备界面等房主）
        const bool can_restart = !online_ || is_host();
        const float bw2 = 236.0f;
        const Rectangle again{card.x + 30.0f, card.y + card.height - 84.0f, bw2, 54.0f};
        const Rectangle back{card.x + card.width - 30.0f - bw2, card.y + card.height - 84.0f, bw2,
                             54.0f};
        if (can_restart && !battle_.restart_sent) {
            if (ui_button(again, "再来一局")) {
                if (online_) {
                    battle_.restart_sent = true;  // 防连点：等开局消息或错误响应回来再恢复
                    net_.send(json{{"type", msg_type::kStart}});
                } else {
                    start_local_game();  // 会重建本地对局并清空 battle_
                    return;
                }
            }
        } else {
            DrawRectangleRounded(again, 0.4f, 8, Color{44, 50, 66, 255});
            const std::string s = can_restart ? "等待开始" : "等待房主";
            const float w = text_width(s, 20.0f);
            text(s, again.x + (again.width - w) * 0.5f, again.y + 15.0f, 20.0f, kTextFaint);
        }
        if (ui_button(back, "返回准备界面")) {
            result_text_.clear();
            if (online_) {
                screen_ = Screen::Lobby;  // 房间还在：回到队伍与配置界面
            } else {
                // 单机对局唯一的离开出口（对局进行中没有返回入口），local_ 在此统一清理，
                // 保证后续界面不会带着残留的旧 Room
                local_.reset();
                battle_ = Battle{};
                screen_ = Screen::LocalSetup;
            }
        }
    }

    // ---- 函数帮助面板（模态）----
    if (help_open_) {
        DrawRectangle(0, 0, kWinW, kWinH, Color{8, 10, 16, 205});
        const Rectangle card{kWinW * 0.5f - 430.0f, 80.0f, 860.0f, 680.0f};
        DrawRectangleRounded(card, 0.03f, 10, kPanel);
        DrawRectangleRoundedLines(card, 0.03f, 10, kAccent);
        text("函数帮助", card.x + 28.0f, card.y + 20.0f, 30.0f, kTextMain);
        text("表达式里可用的全部内容（与 sin、cos 等随意嵌套组合）", card.x + 190.0f,
             card.y + 31.0f, 17.0f, kTextFaint);

        // 关闭按钮自己判定：面板打开期间 ui_button 整体被锁，这里绕过锁
        const Rectangle close{card.x + card.width - 116.0f, card.y + 18.0f, 88.0f, 36.0f};
        const bool close_hover = CheckCollisionPointRec(GetMousePosition(), close);
        DrawRectangleRounded(close, 0.35f, 8, close_hover ? Color{52, 62, 86, 255} : kPanelSoft);
        DrawRectangleRoundedLines(close, 0.35f, 8, kPanelBorder);
        text("关闭", close.x + 26.0f, close.y + 8.0f, 19.0f, kTextMain);
        if (close_hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            help_open_ = false;
            ui_click_consumed_ = true;
        }

        // 函数清单来自 core 的 kExprFuncGroups（与白名单的一致性由单测保证），
        // 界面这边只负责排版：类别 + 函数名 + 说明
        float y = card.y + 82.0f;
        for (const auto& g : kExprFuncGroups) {
            text(g.title, card.x + 30.0f, y, 18.0f, kAccent);
            float vx = card.x + 132.0f;
            if (g.funcs[0] != '\0') {
                text(g.funcs, vx, y, 17.0f, kTextMain);
                vx += text_width(g.funcs, 17.0f) + 18.0f;
            }
            if (g.note[0] != '\0') {
                // "基本运算"这类只有说明没有函数名的行，说明即主体内容，用主色画
                const bool note_is_main = g.funcs[0] == '\0';
                text(g.note, vx, y, note_is_main ? 17.0f : 16.0f,
                     note_is_main ? kTextMain : kTextFaint);
            }
            y += 30.0f;
        }
        // 规则与示例（与函数清单无关的用法说明）。坐标系的说明随房间属性
        // （battle_.cfg.room.coord_origin）在两套文案间切换，都与 curve.cpp 的实现对应
        y += 14.0f;  // 函数与规则两组之间留空
        struct HelpRow {
            const char* k;
            const char* v;
        };
        const HelpRow rules_soldier[] = {
            {"常量", "pi（圆周率）    e（自然常数）"},
            {"变量", "y=f(x)→x     x=f(y)→y    r=f(t)→theta"},
            {"", "参数方程→t    dy/dx→x,y    (x',y')→t,x,y    y''→x,y,dy"},
            {"坐标系", "只影响 y=f(x) 与 x=f(y)：本房间以你为原点（x=0、y=0 就在你身上）"},
            {"", "y=f(x)：曲线只做上下平移，使 x=0 处的点正好落在你身上"},
            {"", "x=f(y)：曲线只做左右平移，使 y=0 处的点正好落在你身上"},
            {"", "其他模式不受影响：极坐标 / 参数方程从你出发；微分方程本来就按地图坐标"},
            {"", "y''：dy 是当前斜率，初值由发射角决定（度，正数朝发射方向上方偏）"},
            {"平移", "曲线会被整体平移，让出发点正好落在你身上；加任意常数（如 +5）不影响弹道"},
            {"示例", "2*sin(0.5*x) + 0.2*x"},
            {"", "5*step(x-10)（前进 10 后抬升）"},
        };
        const HelpRow rules_field[] = {
            {"常量", "pi（圆周率）    e（自然常数）"},
            {"变量", "y=f(x)→x     x=f(y)→y    r=f(t)→theta"},
            {"", "参数方程→t    dy/dx→x,y    (x',y')→t,x,y    y''→x,y,dy"},
            {"坐标系", "只影响 y=f(x) 与 x=f(y)：本房间按地图坐标（函数里的数字就是地图上的）"},
            {"", "y=f(x)：只做上下平移——往上平移 y0-f(x0) 后压到你身上"},
            {"", "x=f(y)：只做左右平移——往右平移 x0-f(y0) 后压到你身上。(x0,y0) 是你的位置"},
            {"", "其他模式不受影响：极坐标 / 参数方程从你出发；微分方程本来就按地图坐标"},
            {"", "y''：dy 是当前斜率，初值由发射角决定（度，正数朝发射方向上方偏）"},
            {"平移", "曲线会被整体平移，让出发点正好落在你身上；加任意常数（如 +5）不影响弹道"},
            {"示例", "2*sin(0.5*x) + 0.2*x"},
            {"", "5*step(x-10)（前进 10 后抬升）"},
        };
        auto draw_rows = [&](const HelpRow* rows, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                text(rows[i].k, card.x + 30.0f, y, 18.0f, kAccent);
                if (rows[i].v[0] != '\0') text(rows[i].v, card.x + 132.0f, y, 17.0f, kTextMain);
                y += 30.0f;
            }
        };
        if (battle_.cfg.room.coord_origin == CoordOrigin::Field) {
            draw_rows(rules_field, std::size(rules_field));
        } else {
            draw_rows(rules_soldier, std::size(rules_soldier));
        }
    }
}
}  // namespace fg
