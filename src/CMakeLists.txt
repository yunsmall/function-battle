add_subdirectory(core)
add_subdirectory(server)
# 图形客户端固定用仓库内 third_party 的 Windows 预编译 raylib；非 Windows 或不编
# 客户端时关掉（-DFG_BUILD_CLIENT=OFF），核心/服务器/测试不依赖它
option(FG_BUILD_CLIENT "构建 raylib 图形客户端" ON)
if(FG_BUILD_CLIENT)
    add_subdirectory(client)
endif()
add_subdirectory(tests)
