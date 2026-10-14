# 客户端固定使用仓库内 third_party 的官方预编译 raylib 5.5（动态库 + 导入库）：
# 本机 vcpkg 的 raylib 6.0 存在呈现回归——官方最小示例的客户区同样纯黑，同码换 5.5 即正常
set(FG_RAYLIB_DIR ${CMAKE_SOURCE_DIR}/third_party/raylib-5.5/raylib-5.5_win64_msvc16)

find_package(cxxopts CONFIG REQUIRED)

add_executable(fg_client
    main.cpp
    app.cpp           # 应用状态与主循环：网络消息处理、提交与预览
    ui_common.cpp     # 文本/按钮/输入框等 UI 小工具与字体加载
    ui_menu.cpp       # 主菜单 + 单机人机配置
    ui_lobby.cpp      # 联机：大厅 / 创建房间 / 房间准备界面
    ui_battle.cpp     # 对战界面与结算
    headless.cpp      # --selftest / --screenshot 开发工具
    render.cpp
    net_client.cpp
)
target_link_libraries(fg_client PRIVATE fg_core cxxopts::cxxopts ${FG_RAYLIB_DIR}/lib/raylibdll.lib)
target_include_directories(fg_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FG_RAYLIB_DIR}/include)

# 动态库与 exe 同目录部署（用 copy_if_different：vcpkg 的 applocal 若拷来同名 6.0 dll，这里会再盖回 5.5）
add_custom_command(TARGET fg_client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FG_RAYLIB_DIR}/lib/raylib.dll" "$<TARGET_FILE_DIR:fg_client>")
