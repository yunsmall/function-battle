// 文件信箱式对局客户端入口：参数解析、连接、建房、开局
#include "app.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

#include <cxxopts.hpp>

#include "exe_path.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>  // SetConsoleOutputCP：让中文日志在 Windows 终端正常显示
#endif

namespace fbplay {

using namespace std::chrono_literals;

int play_main(int argc, char** argv) {
    cxxopts::Options options("fb_play",
                             "文件信箱式对局客户端：连服务器打一局（我 = A 队 vs 人机 AI）");
    options.add_options()
        ("seed", "固定地图种子（整数；'-'/'random' 或省略 = 每把随机）",
         cxxopts::value<std::string>()->default_value(""))
        ("difficulty", "AI 难度：0 简单 / 1 普通 / 2 困难 / 3 大神（默认 2）",
         cxxopts::value<int>()->default_value("2"))
        ("host", "服务器地址", cxxopts::value<std::string>()->default_value("127.0.0.1"))
        ("port", "游戏端口（原生协议）", cxxopts::value<uint16_t>()->default_value("9527"))
        ("name", "我的名字", cxxopts::value<std::string>()->default_value("Claude"))
        ("h,help", "显示帮助");
    options.parse_positional({"seed", "difficulty"});

    std::string seed_arg;
    try {
        const auto result = options.parse(argc, argv);
        if (result.count("help")) {
            std::printf("%s\n", options.help().c_str());
            return 0;
        }
        seed_arg = result.count("seed") ? result["seed"].as<std::string>() : "";
        // 名字/难度等直接读；难度的范围单独校验
        std::optional<uint64_t> seed;
        if (!seed_arg.empty() && seed_arg != "-" && seed_arg != "random" && seed_arg != "r") {
            try {
                seed = std::stoull(seed_arg);
            } catch (const std::exception&) {
                std::printf("种子必须是整数或 '-'（随机），收到：%s\n", seed_arg.c_str());
                return 1;
            }
        }

        asio::io_context io;
        App app(io);
        app.ai_diff = result["difficulty"].as<int>();
        if (app.ai_diff < 0 || app.ai_diff > 3) {
            std::printf("难度必须是 0-3\n");
            return 1;
        }
        app.my_name = result["name"].as<std::string>();
        const std::string host = result["host"].as<std::string>();
        const uint16_t port = result["port"].as<uint16_t>();

        // 文件信箱目录：程序所在目录旁的 session/（打包解压后即包内，从哪运行都固定）
        std::string base = fg::exe_dir();
        if (base.empty()) base = ".";
        const std::string dir = base + "/session";
        app.state_path = dir + "/state.txt";
        app.shot_path = dir + "/shot.txt";
        app.log_path = dir + "/log.txt";
        app.done_path = dir + "/done.txt";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::remove(app.shot_path, ec);
        std::filesystem::remove(app.done_path, ec);
        {
            std::ofstream f(app.log_path, std::ios::app);
            f << "---- 新对局 ----\n";
        }
        // 立个"准备中"占位：上一局的 state.txt 残留会让读取者把旧战场当成本局战况
        {
            std::ofstream f(app.state_path);
            f << "（正在建房/等开局：战场快照稍后覆盖到这里）\n";
        }

        // 连接（先阻塞 connect，随后切非阻塞轮询）
        app.sock.connect(asio::ip::tcp::endpoint(asio::ip::make_address(host), port), ec);
        if (ec) {
            log_line(app, "连不上服务器 " + host + ":" + std::to_string(port) + "：" +
                              ec.message());
            return 1;
        }
        app.sock.non_blocking(true, ec);

        // 等 hello（3 秒）
        {
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (app.recvbuf.empty() && !app.disconnected &&
                   std::chrono::steady_clock::now() < deadline) {
                pump_socket(app);
                std::this_thread::sleep_for(20ms);
            }
            drain_frames(app);
        }

        // 建房（配置与 py 版一致）；随后加 AI、开局
        json cfg_obj;
        cfg_obj["soldiers_per_player"] = 3;
        cfg_obj["obstacles"] = 15;
        cfg_obj["hp"] = 1;
        cfg_obj["first_team"] = 0;
        cfg_obj["input_seconds"] = 600.0;
        cfg_obj["turn_delay"] = 1.0;
        if (seed) cfg_obj["seed"] = *seed;
        {
            json msg;
            msg["type"] = "create_room";
            msg["name"] = app.my_name;
            msg["config"] = cfg_obj;
            send_msg(app, msg);
        }
        {
            // 等 joined（10 秒）
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (app.my_id < 0 && !app.disconnected &&
                   std::chrono::steady_clock::now() < deadline) {
                pump_socket(app);
                drain_frames(app);
                std::this_thread::sleep_for(20ms);
            }
        }
        if (app.my_id < 0) {
            log_line(app, "建房失败（未收到 joined）");
            return 1;
        }
        {
            json msg;
            msg["type"] = "add_ai";
            msg["difficulty"] = app.ai_diff;
            msg["team"] = "B";
            send_msg(app, msg);
        }
        std::this_thread::sleep_for(300ms);
        send_msg(app, json{{"type", "start"}});
        log_line(app, "已发 add_ai(难度 " + std::to_string(app.ai_diff) + ") 与 start");

        return run_game(app);
    } catch (const cxxopts::exceptions::exception& e) {
        std::printf("参数错误：%s\n\n%s\n", e.what(), options.help().c_str());
        return 1;
    }
}

}  // namespace fbplay

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    return fbplay::play_main(argc, argv);
}
