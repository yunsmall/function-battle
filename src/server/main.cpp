// 函数对战游戏 —— 服务器入口。
// 默认进入多房间网络模式；--selftest N 则在本地无头跑 N 局人机对战做自检。

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include <asio.hpp>
#include <cxxopts.hpp>

#include "exe_path.h"
#include "room.h"
#include "server.h"
#include "world.h"

namespace {

struct GameOutcome {
    int winner = -1;
    int ticks = 0;
    bool finished = false;
    int mode_uses[fg::kCurveModeCount] = {0};  // 各曲线模式的发射次数（检查 AI 的多模式策略是否真的在用）
};

// 本地无头跑一整局（全 AI），返回胜负与耗时
GameOutcome run_one_game(uint64_t seed, fg::GameConfig cfg) {
    GameOutcome out;
    cfg.match.seed = seed;
    cfg.match.input_seconds = std::min(cfg.match.input_seconds, 6.0);  // 加快自测节奏

    fg::Room room(cfg.room, fg::match_config_from_resolved(cfg.match));
    // 自检固定 3v3（队规模是单机/自检的本地概念，不在房间配置里）
    for (int i = 0; i < 3; ++i) {
        room.add_member("A" + std::to_string(i + 1), true,
                        (i + static_cast<int>(seed)) % fg::kAiDifficultyCount);
        room.add_member("B" + std::to_string(i + 1), true,
                        (i + static_cast<int>(seed) + 1) % fg::kAiDifficultyCount);
    }
    const fg::Error err = room.start_game(room.host_id());
    if (!err.ok()) {
        std::printf("开局失败：%s\n", err.message.c_str());
        return out;
    }

    // 消息计数仅用于卡死诊断：分清是"AI 一直提交失败空转"还是"互相打不中僵持"
    int n_shot = 0, n_skip = 0, n_hit = 0, n_crater = 0, n_death = 0;
    const int max_ticks = 60 * 60 * 60;  // 60 分钟模拟时间上限，防死循环
    while (room.in_game() && out.ticks < max_ticks) {
        room.tick(1.0 / 60.0);
        for (const auto& m : room.take_outgoing()) {  // 无网络，只统计类型
            const std::string t = fg::json_str(m.msg, "type");
            if (t == fg::msg_type::kShot) {
                ++n_shot;
                if (m.msg.contains("request")) {
                    const std::string mode = fg::json_str(m.msg["request"], "mode");
                    for (int mi = 0; mi < fg::kCurveModeCount; ++mi) {
                        if (mode == fg::curve_mode_str(static_cast<fg::CurveMode>(mi))) {
                            ++out.mode_uses[mi];
                        }
                    }
                }
            } else if (t == fg::msg_type::kTurnSkipped) ++n_skip;
            else if (t == fg::msg_type::kHit) ++n_hit;
            else if (t == fg::msg_type::kCrater) ++n_crater;
            else if (t == fg::msg_type::kDeath) ++n_death;
        }
        ++out.ticks;
    }
    out.finished = !room.in_game();
    out.winner = room.world().winner();
    if (!out.finished) {
        std::printf("  [诊断] 发射 %d 次，超时 %d 次，命中 %d 次，炸坑 %d 次，阵亡 %d 人；phase=%d\n",
                    n_shot, n_skip, n_hit, n_crater, n_death,
                    static_cast<int>(room.world().phase()));
        for (const auto& p : room.world().players()) {
            std::printf("    %s", p.name.c_str());
            for (size_t i = 0; i < p.soldiers.size(); ++i) {
                const auto& s = p.soldiers[i];
                std::printf("  #%d %s hp=%d/%d pos=(%.1f, %.1f)", static_cast<int>(i),
                            s.alive ? "存活" : "阵亡", s.hp, s.max_hp, s.pos.x(), s.pos.y());
            }
            std::printf("\n");
        }
        if (room.world().projectile()) {
            const auto& pr = *room.world().projectile();
            std::printf("    炮弹：owner=%d travel=%.2f/%.2f\n", pr.owner_id, pr.travel,
                        pr.path.length());
        }
    }
    return out;
}

int run_selftest(int games, const fg::GameConfig& cfg) {
    std::printf("自检：%d 局人机对战\n", games);
    int a_wins = 0, b_wins = 0, draws = 0;
    int mode_total[fg::kCurveModeCount] = {0};
    for (int g = 0; g < games; ++g) {
        const GameOutcome r = run_one_game(cfg.match.seed + static_cast<uint64_t>(g), cfg);
        if (!r.finished) {
            std::printf("第 %d 局在 %d tick 内未结束（疑似死循环）\n", g, r.ticks);
            return 1;
        }
        if (r.winner == 0) ++a_wins;
        else if (r.winner == 1) ++b_wins;
        else ++draws;
        for (int i = 0; i < fg::kCurveModeCount; ++i) mode_total[i] += r.mode_uses[i];
        std::printf("第 %d 局：%s（%d tick ≈ %.1f 秒模拟时间）\n", g,
                    r.winner == 0 ? "A 队胜" : (r.winner == 1 ? "B 队胜" : "平局"), r.ticks,
                    r.ticks / 60.0);
    }
    std::printf("汇总：A 胜 %d，B 胜 %d，平局 %d\n", a_wins, b_wins, draws);
    const char* mode_names[] = {"y=f(x)",  "x=f(y)",  "极坐标",  "参数方程",
                                "dy/dx",   "(x',y')", "y''"};
    static_assert(sizeof(mode_names) / sizeof(mode_names[0]) ==
                      static_cast<size_t>(fg::kCurveModeCount),
                  "模式名表与 kCurveModeCount 必须一致");
    std::printf("AI 曲线模式分布：");
    for (int i = 0; i < fg::kCurveModeCount; ++i) {
        std::printf("%s%s %d", (i == 0 ? "" : " ｜ "), mode_names[i], mode_total[i]);
    }
    std::printf("\n");

    // 复现性检查：同种子两局应完全一致（胜者与耗时）
    const GameOutcome r1 = run_one_game(cfg.match.seed + 12345, cfg);
    const GameOutcome r2 = run_one_game(cfg.match.seed + 12345, cfg);
    if (r1.winner != r2.winner || r1.ticks != r2.ticks) {
        std::printf("复现性检查失败：同种子两次结果不同（%d/%d vs %d/%d）\n", r1.winner, r1.ticks,
                    r2.winner, r2.ticks);
        return 1;
    }
    std::printf("复现性检查通过（同种子结果一致：胜者 %d，%d tick）\n", r1.winner, r1.ticks);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    cxxopts::Options options("function_game_server", "函数对战游戏服务器");
    options.add_options()("port", "游戏端口（原生 TCP 协议）",
                          cxxopts::value<uint16_t>()->default_value("9527"))(
        "web-port", "网页端口（WebSocket + 静态页面）",
        cxxopts::value<uint16_t>()->default_value("8000"))(
        "static-dir", "网页静态文件目录",
        cxxopts::value<std::string>()->default_value("web/static"))(
        "seed", "自检用地图种子",
        cxxopts::value<uint64_t>()->default_value("12345"))(
        "hp", "每人血量（1 = 一击毙命）", cxxopts::value<int>()->default_value("1"))(
        "obstacles", "遮挡物数量", cxxopts::value<int>()->default_value("10"))(
        "input-seconds", "每回合输入时限（秒）",
        cxxopts::value<double>()->default_value("120"))(
        "selftest", "不开网络，本地跑 N 局人机对战自检",
        cxxopts::value<int>()->default_value("0"))("h,help", "显示帮助");

    fg::GameConfig cfg;
    uint16_t port = 9527;
    uint16_t web_port = 8000;
    std::string static_dir = "web/static";
    int selftest = 0;
    // 这里是第三方库（cxxopts）的边界：参数错误只会以异常报告，
    // 捕获后打印帮助并退出，异常不进入项目自身逻辑
    try {
        const auto result = options.parse(argc, argv);
        if (result.count("help")) {
            std::printf("%s\n", options.help().c_str());
            return 0;
        }
        port = result["port"].as<uint16_t>();
        web_port = result["web-port"].as<uint16_t>();
        static_dir = result["static-dir"].as<std::string>();
        cfg.match.seed = result["seed"].as<uint64_t>();
        cfg.match.hp = result["hp"].as<int>();
        cfg.match.obstacle_count = result["obstacles"].as<int>();
        cfg.match.input_seconds = result["input-seconds"].as<double>();
        selftest = result["selftest"].as<int>();
    } catch (const cxxopts::exceptions::exception& e) {
        std::printf("参数错误：%s\n\n%s\n", e.what(), options.help().c_str());
        return 1;
    }

    if (selftest > 0) return run_selftest(selftest, cfg);

    // 静态目录解析：先按参数给的路径（相对当前工作目录）找，找不到再按 exe 所在
    // 目录找——发布的 zip 解压后，从任何位置启动服务器都能定位到 web/static
    {
        std::error_code fs_ec;
        if (!std::filesystem::exists(static_dir, fs_ec)) {
            const std::string exe = fg::exe_dir();
            if (!exe.empty()) {
                const std::filesystem::path alt = std::filesystem::path(exe) / static_dir;
                if (std::filesystem::exists(alt, fs_ec)) static_dir = alt.string();
            }
        }
    }

    asio::io_context io;
    std::error_code ec;
    // 创建房间的默认"下一把"配置：血量/遮挡物/时限取服务器启动参数；
    // 种子留空 = 每把随机（--seed 只给自检的固定种子用，与联机创建无关）
    fg::MatchConfig default_match;
    default_match.hp = cfg.match.hp;
    default_match.obstacle_count = cfg.match.obstacle_count;
    default_match.input_seconds = cfg.match.input_seconds;
    fg::Server server(io, port, web_port, static_dir, default_match,
                      ec);  // 绑定失败（端口被占等）走 ec，不抛异常
    if (ec) {
        std::printf("服务器启动失败：%s\n", ec.message().c_str());
        return 1;
    }
    std::printf("服务器已启动：游戏端口 %u（原生协议）；网页 http://<本机>:%u/（WebSocket+静态页）\n",
                port, web_port);
    std::fflush(stdout);
    // 进程主循环兜底：handler 里万一有异常穿过了会话层，记录日志后继续服务，
    // 不能让个别会话把整个服务器拖崩（run() 抛出后要 restart 才能继续跑）
    for (;;) {
        try {
            io.run();
            break;
        } catch (const std::exception& e) {
            std::printf("io 异常：%s（服务器继续运行）\n", e.what());
            std::fflush(stdout);
            io.restart();
        }
    }
    return 0;
}
