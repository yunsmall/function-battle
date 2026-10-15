# 文件信箱式对局客户端（脚本/助手用）：无图形界面，纯 asio + 共用 core
find_package(asio CONFIG REQUIRED)
find_package(cxxopts CONFIG REQUIRED)

add_executable(fb_play
    fb_play/main.cpp
    fb_play/net.cpp
    fb_play/app.cpp
    fb_play/render.cpp
)
target_link_libraries(fb_play PRIVATE fg_core asio::asio cxxopts::cxxopts)

# 发布布局：与其它可执行文件同层
install(TARGETS fb_play RUNTIME DESTINATION .)

# AI 对打评测工具：不进发布包也不进常规测试，本地调 AI 强度时用
add_executable(fb_ai_bench
    ai_bench.cpp
)
target_link_libraries(fb_ai_bench PRIVATE fg_core cxxopts::cxxopts)
