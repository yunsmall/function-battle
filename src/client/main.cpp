// 图形客户端入口。
//   function_battle_client                正常打开窗口（主菜单）
//   function_battle_client --selftest N   无头跑 N 局本地人机，检查不会卡死
//   function_battle_client --screenshot P 自动进一局并截图（P_input/_flight/_menu.png），用于开发验证
//   function_battle_client --theme N      启动主题（0 深空 / 1 明亮 / 2 终端绿）

#include <cstdio>
#include <string>

#include <cxxopts.hpp>

#if defined(_WIN32)
// 只声明用到的这一个 API：直接 include <windows.h> 会和 raylib 的 Rectangle 等类型重名冲突
extern "C" __declspec(dllimport) int __stdcall SetProcessDPIAware();
#endif

#include "app.h"
#include "ui_style.h"

int main(int argc, char** argv) {
#if defined(_WIN32)
    // 高 DPI 缩放的系统（如 125%）下，不感知 DPI 的进程会被系统做虚拟化缩放：
    // 客户区渲染尺寸与物理像素错位（画面经拉伸发糊）。声明感知后按物理像素渲染
    SetProcessDPIAware();
#endif
    cxxopts::Options options("function_battle_client", "函数对战游戏客户端");
    // 不用 implicit_value：设了它的选项只认 --opt=value 写法，"--opt value" 的值会被
    // 当成位置参数悄悄丢掉（截图路径变成默认值，bug 找半天）
    options.add_options()(
        "selftest", "无头本地人机自检 N 局", cxxopts::value<int>()->default_value("0"))(
        "screenshot", "自动对局并截图（路径前缀）",
        cxxopts::value<std::string>()->default_value(""))(
        "theme", "启动主题：0 深空 / 1 明亮 / 2 终端绿（游玩中也可在主菜单切换）",
        cxxopts::value<int>()->default_value("0"))("h,help", "显示帮助");

    int selftest = 0;
    int theme = 0;
    std::string screenshot;
    // 这里是第三方库（cxxopts）的边界：参数错误只会以异常报告，
    // 捕获后打印帮助并退出，异常不进入项目自身逻辑
    try {
        const auto result = options.parse(argc, argv);
        if (result.count("help")) {
            std::printf("%s\n", options.help().c_str());
            return 0;
        }
        // 与旧手写解析保持一致的宽容语义：写了选项但数值无效（0/负数）时按默认 3 局
        selftest = (result.count("selftest") > 0 && result["selftest"].as<int>() <= 0)
                       ? 3
                       : result["selftest"].as<int>();
        screenshot = result["screenshot"].as<std::string>();
        theme = result["theme"].as<int>();
    } catch (const cxxopts::exceptions::exception& e) {
        std::printf("参数错误：%s\n\n%s\n", e.what(), options.help().c_str());
        return 1;
    }

    fg::apply_theme(theme);  // --theme：截图与自动化用；游玩中在主菜单点按钮切换
    fg::App app;
    if (selftest > 0) return app.run_headless(selftest);
    if (!screenshot.empty()) return app.run_screenshot(screenshot);
    return app.run();
}
