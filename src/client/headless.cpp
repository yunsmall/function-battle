#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "ai.h"
#include "curve.h"
#include "render.h"
#include "ui_style.h"

namespace fg {

// ---------------- 无头自检与截图 ----------------

int App::run_headless(int games) {
    std::printf("客户端本地人机自检：%d 局\n", games);
    int a_wins = 0, b_wins = 0, draws = 0;
    for (int g = 0; g < games; ++g) {
        GameConfig cfg;
        cfg.room.soldiers_per_player = 1 + g % 2;  // 单双士兵数交替，覆盖多士兵轮换
        cfg.match.seed = 5000ULL + static_cast<uint64_t>(g);
        cfg.match.input_seconds = 6.0;
        Room room(cfg.room, match_config_from_resolved(cfg.match));
        for (int i = 0; i < 3; ++i) {  // 自检固定 3v3

            room.add_member("A" + std::to_string(i + 1), true, (i + g) % kAiDifficultyCount, Team::A);
            room.add_member("B" + std::to_string(i + 1), true, (i + g + 1) % kAiDifficultyCount, Team::B);
        }
        const Error err = room.start_game(room.host_id());
        if (!err.ok()) {
            std::printf("开局失败：%s\n", err.message.c_str());
            return 1;
        }
        int ticks = 0;
        while (room.in_game() && ticks < 60 * 60 * 60) {
            room.tick(1.0 / 60.0);
            room.take_outgoing();
            ++ticks;
        }
        if (room.in_game()) {
            std::printf("第 %d 局未结束（疑似死循环）\n", g);
            return 1;
        }
        const int w = room.world().winner();
        if (w == 0) ++a_wins;
        else if (w == 1) ++b_wins;
        else ++draws;
        std::printf("第 %d 局：%s（%d tick）\n", g,
                    w == 0 ? "A 队胜" : (w == 1 ? "B 队胜" : "平局"), ticks);
    }
    std::printf("汇总：A 胜 %d，B 胜 %d，平局 %d\n", a_wins, b_wins, draws);
    return 0;
}

// 5.5 在 DPI 缩放（如 125%）的机器上截屏会把缓冲按缩放后的物理尺寸读出：
// 画面只占左下窗口大小的区域、其余为黑，这里统一裁回窗口区域再存盘
static void take_shot(const std::string& file) {
    Image img = LoadImageFromScreen();
    if (img.width > kWinW && img.height > kWinH) {
        ImageCrop(&img, Rectangle{0.0f, static_cast<float>(img.height - kWinH),
                                  static_cast<float>(kWinW), static_cast<float>(kWinH)});
    }
    ExportImage(img, file.c_str());
    UnloadImage(img);
}

int App::run_screenshot(const std::string& path) {
    InitWindow(kWinW, kWinH, "函数对战 Function Battle");
    SetTargetFPS(60);
    load_ui_font();
    screenshot_mode_ = true;  // 切屏黑场过渡会压暗截图：工具模式直接跳过

    // 单机设置界面（含"对局身份"选择；选中"观战"以展示说明文案）
    screen_ = Screen::LocalSetup;
    local_spectate_ = true;
    for (int i = 0; i < 3; ++i) {
        update();
        BeginDrawing();
        draw();
        EndDrawing();
    }
    take_shot(path + "_localsetup.png");
    local_spectate_ = false;

    // 进入一局单机对局（5v5 每人 2 兵：截图长期覆盖队伍面板满员与多士兵格子的布局）
    set_team_size_ = 5;
    set_soldiers_ = 2;
    set_difficulty_ = 2;
    set_hp_ = 3;
    set_obstacles_ = 10;
    set_first_team_ = 0;  // A 先手：我（A1）第一回合就轮到，不必等 AI——AI 思考在
                          // 线程池里要真实时间，驱动循环快转时等不到它的回合走完
    player_name_ = "我";
    start_local_game();

    // 跑到轮到我（update_battle 在本地模式下内部会 tick_local，不能再手动 tick 一次，
    // 否则每帧推进两次模拟）
    for (int i = 0; i < 60 * 40 && battle_.current_player != battle_.my_id; ++i) {
        update_battle();
        if (screen_ != Screen::Battle) break;
    }
    // 填一条漂亮的曲线，截"输入 + 预览"图
    expr_a_.text = "2*sin(0.5*x) + 0.2*x";
    mode_index_ = 0;
    update_battle();

    // 截图读的是当前缓冲：先渲染几帧让缓冲有内容，
    // 否则首次交换后立刻截会得到全黑图
    for (int i = 0; i < 3; ++i) {
        BeginDrawing();
        draw();
        EndDrawing();
    }
    take_shot(path + "_input.png");

    // 参数方程模式（两个表达式框并排、等宽；含参数范围框）
    mode_index_ = 3;  // kModes[3] = 参数方程
    for (int i = 0; i < 3; ++i) {
        BeginDrawing();
        draw();
        EndDrawing();
    }
    take_shot(path + "_param.png");
    mode_index_ = 0;

    // 二阶微分方程模式（第 7 个模式按钮；底部出现发射角输入框，预览是抛物线）
    mode_index_ = 6;  // kModes[6] = y'' = f(x,y,dy)
    expr_a_.text = "-0.25";
    angle_box_.text = "25";
    update_battle();
    for (int i = 0; i < 3; ++i) {
        BeginDrawing();
        draw();
        EndDrawing();
    }
    take_shot(path + "_ypp.png");
    mode_index_ = 0;
    expr_a_.text = "2*sin(0.5*x) + 0.2*x";
    angle_box_.text.clear();
    update_battle();

    // 函数帮助面板
    help_open_ = true;
    for (int i = 0; i < 3; ++i) {  // 与上方同理：多渲染几帧确保画面已呈现再截图
        BeginDrawing();
        draw();
        EndDrawing();
    }
    take_shot(path + "_help.png");
    help_open_ = false;

    // 提交，等炮弹飞一会儿再截"飞行中"图（地图小了炮弹出界快，帧数不能多）
    submit_current();
    for (int i = 0; i < 20; ++i) {
        update_battle();
    }
    for (int i = 0; i < 3; ++i) {
        BeginDrawing();
        draw();
        EndDrawing();
    }
    take_shot(path + "_flight.png");

    // 让 AI 把这局打完，截"结算"图
    for (int i = 0; i < 60 * 400 && battle_.winner < 0 && screen_ == Screen::Battle; ++i) {
        update_battle();
    }
    if (battle_.winner >= 0) {
        for (int i = 0; i < 3; ++i) {
            BeginDrawing();
            draw();
            EndDrawing();
        }
        take_shot(path + "_result.png");
    }

    // 地图坐标模式（field）的预览与实弹：同一函数在 field 房间应按地图坐标整体平移
    // （往上平移 y0-f(x0)），而不是像 soldier 那样从士兵脚下起——用于对照检查显示链路
    local_.reset();
    set_coord_field_ = 1;
    set_first_team_ = 0;  // 同上：固定 A 先手，第一回合就轮到我
    start_local_game();
    for (int i = 0; i < 60 * 40 && battle_.current_player != battle_.my_id; ++i) {
        update_battle();
        if (screen_ != Screen::Battle) break;
    }
    mode_index_ = 0;
    expr_a_.text = "2.5*sin(0.6*x) + 0.3*x";  // 与上面 soldier 段不同，避免预览缓存混淆
    update_battle();
    for (int i = 0; i < 3; ++i) {
        BeginDrawing();
        draw();
        EndDrawing();
    }
    take_shot(path + "_field_input.png");
    submit_current();
    for (int i = 0; i < 20; ++i) {
        update_battle();
    }
    for (int i = 0; i < 3; ++i) {  // 与下方 flight 段同理：多渲染几帧确保画面已呈现再截图
        BeginDrawing();
        draw();
        EndDrawing();
    }
    take_shot(path + "_field_flight.png");
    set_coord_field_ = 0;

    // 单机观战一局：我不进阵容，只是看两边人机互打（推进到有炮弹在飞再截）
    local_.reset();
    local_spectate_ = true;
    start_local_game();
    for (int i = 0; i < 60 * 60 && !battle_.has_projectile && battle_.winner < 0; ++i) {
        update_battle();
    }
    for (int i = 0; i < 3; ++i) {
        BeginDrawing();
        draw();
        EndDrawing();
    }
    take_shot(path + "_spectate.png");

    // 镜像观战：观战者本地切视角（房间开了 mirror_view 规则才允许；这里直接置状态展示渲染）
    battle_.cfg.match.mirror_view = true;
    spectate_mirror_ = true;
    for (int i = 0; i < 3; ++i) {
        BeginDrawing();
        draw();
        EndDrawing();
    }
    take_shot(path + "_spectate_mirror.png");
    spectate_mirror_ = false;
    local_spectate_ = false;

    // 主菜单
    screen_ = Screen::Menu;
    local_.reset();
    for (int i = 0; i < 3; ++i) {
        update();
        BeginDrawing();
        draw();
        EndDrawing();
    }
    take_shot(path + "_menu.png");

    // 创建房间界面（纯界面，不需要服务器）
    screen_ = Screen::CreateRoom;
    for (int i = 0; i < 3; ++i) {
        update();
        BeginDrawing();
        draw();
        EndDrawing();
    }
    take_shot(path + "_createroom.png");

    // 联机大厅与房间准备界面：本机服务器在跑就真连一次，顺便验证 NetClient 通路
    bool got_server = false;
    if (net_.connect_to("127.0.0.1", 9527).empty()) {
        got_server = true;
        online_ = true;
        player_name_ = "截图";
        net_.send(json{{"type", msg_type::kListRooms}});
        screen_ = Screen::Lobby;
        for (int i = 0; i < 40; ++i) {  // SetTargetFPS 节流下约 0.7 秒，等消息往返
            update();
            BeginDrawing();
            draw();
            EndDrawing();
        }
        take_shot(path + "_lobby.png");

        // 创建房间 → 准备界面；加两个 AI 并改配置，让界面展示完整
        // （开镜像规则：联机观战截图里能同时看到"退出观战"与"视角"两个按钮的排布）
        json match = json::object();
        match["obstacle_count"] = 10;
        match["first_team"] = 0;
        match["mirror_view"] = true;
        net_.send(json{{"type", msg_type::kCreateRoom}, {"name", "截图"}, {"match", match}});
        for (int i = 0; i < 40; ++i) {
            update();
            BeginDrawing();
            draw();
            EndDrawing();
        }
        net_.send(json{{"type", msg_type::kAddAi}, {"difficulty", 2}, {"team", "B"}});
        net_.send(json{{"type", msg_type::kAddAi}, {"difficulty", 1}, {"team", "B"}});
        net_.send(json{{"type", msg_type::kSetMatchConfig},
                       {"match", json{{"hp", 3}, {"first_team", -1}}}});
        for (int i = 0; i < 40; ++i) {
            update();
            BeginDrawing();
            draw();
            EndDrawing();
        }
        take_shot(path + "_room.png");

        // 房主转观战 + 补两个 A 队 AI（与 B 队 2 个对称——开局要求两队等人数），
        // 然后开局：房间界面里观战者行、观战中的对局视图（没有"我"的方块、被动看 AI 打）
        net_.send(json{{"type", msg_type::kSetSpectate}, {"spectate", true}});
        net_.send(json{{"type", msg_type::kAddAi}, {"difficulty", 2}, {"team", "A"}});
        net_.send(json{{"type", msg_type::kAddAi}, {"difficulty", 1}, {"team", "A"}});
        for (int i = 0; i < 40; ++i) {
            update();
            BeginDrawing();
            draw();
            EndDrawing();
        }
        take_shot(path + "_room_spectate.png");
        net_.send(json{{"type", msg_type::kStart}});
        for (int i = 0; i < 80; ++i) {  // 等开局消息与 AI 的第一次出手
            update();
            BeginDrawing();
            draw();
            EndDrawing();
        }
        take_shot(path + "_battle_spectate.png");
        net_.disconnect();
        online_ = false;
    }

    CloseWindow();
    std::printf("截图已保存：%s_{input,param,ypp,help,flight,result,menu,createroom,localsetup,"
                "spectate,spectate_mirror}.png%s\n",
                path.c_str(),
                got_server ? " + {lobby,room,room_spectate,battle_spectate}.png"
                           : "（服务器未运行，跳过联机截图）");
    return 0;
}
}  // namespace fg
