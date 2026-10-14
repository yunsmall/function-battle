find_package(cxxopts CONFIG REQUIRED)

add_executable(fb_client
    main.cpp
    app.cpp           # 应用状态与主循环：网络消息处理、提交与预览
    fx.cpp            # 战斗特效（命中/爆炸/碎裂），纯表现层
    sfx.cpp           # 合成音效（无音频资源文件）
    ui_common.cpp     # 文本/按钮/输入框等 UI 小工具与字体加载
    ui_theme.cpp      # 主题切换（三套预设配色，改写 ui_style.h 的色板）
    ui_menu.cpp       # 主菜单 + 单机人机配置
    ui_lobby.cpp      # 联机：大厅 / 创建房间 / 房间准备界面
    ui_battle.cpp     # 对战界面与结算
    headless.cpp      # --selftest / --screenshot 开发工具
    render.cpp
    net_client.cpp
)
target_link_libraries(fb_client PRIVATE fg_core cxxopts::cxxopts)
target_include_directories(fb_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 发布布局：exe 与 web/ 同层（配合顶层 CMakeLists 的打包规则）
install(TARGETS fb_client RUNTIME DESTINATION .)

# 版本号单一来源：从顶层 project(VERSION) 注入，界面显示用（发布时只改一处）
target_compile_definitions(fb_client PRIVATE FG_VERSION="${PROJECT_VERSION}")

if(WIN32)
    # Windows 固定用 raylib 5.5 官方预编译包（动态库 + 导入库）：配置时从官方
    # release 自动下载并校验 SHA-256，不往仓库里放二进制。不用 vcpkg 的 6.0——
    # 它存在呈现回归：官方最小示例的客户区同样纯黑，同码换 5.5 即正常
    include(FetchContent)
    FetchContent_Declare(raylib_prebuilt
        URL https://github.com/raysan5/raylib/releases/download/5.5/raylib-5.5_win64_msvc16.zip
        URL_HASH SHA256=8d046084d12353183e701ef4c9d276c21fcd3243c2a368091fabfb2769b8507c
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(raylib_prebuilt)
    # 解压时会剥掉压缩包的顶层目录，SOURCE_DIR 直接就是包内容
    set(FG_RAYLIB_DIR ${raylib_prebuilt_SOURCE_DIR})
    target_link_libraries(fb_client PRIVATE ${FG_RAYLIB_DIR}/lib/raylibdll.lib)
    target_include_directories(fb_client PRIVATE ${FG_RAYLIB_DIR}/include)

    # 动态库与 exe 同目录部署（用 copy_if_different：vcpkg 的 applocal 若拷来同名 6.0 dll，这里会再盖回 5.5）
    add_custom_command(TARGET fb_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FG_RAYLIB_DIR}/lib/raylib.dll" "$<TARGET_FILE_DIR:fb_client>")
    install(FILES ${FG_RAYLIB_DIR}/lib/raylib.dll DESTINATION .)
else()
    # 非 Windows：找系统/vcpkg 安装的 raylib
    find_package(raylib 5.5 CONFIG REQUIRED)
    target_link_libraries(fb_client PRIVATE raylib)
endif()
