find_package(asio CONFIG REQUIRED)
find_package(cxxopts CONFIG REQUIRED)

add_executable(fg_server
    main.cpp
    exe_path.cpp      # 跨平台取 exe 目录：按 exe 位置定位网页静态文件
    net.cpp
    server.cpp
    web_front.cpp
    web_static.cpp
)
# websocketpp（third_party 里的 0.8.2 源码）与项目一致用 standalone asio：
# ASIO_STANDALONE 让它 #include <asio.hpp> 而不是 boost 版。
# _WEBSOCKETPP_CPP11_STL_：MSVC 默认不按标准报 __cplusplus（199711L），
# websocketpp 探测不到 C++11 会退回 boost 实现；这个宏是它给构建系统预留的开关
target_include_directories(fg_server PRIVATE ${CMAKE_SOURCE_DIR}/third_party/websocketpp)
target_compile_definitions(fg_server PRIVATE ASIO_STANDALONE _WEBSOCKETPP_CPP11_STL_)
target_link_libraries(fg_server PRIVATE fg_core asio::asio cxxopts::cxxopts)

# 发布布局：exe 与 web/ 同层（配合顶层 CMakeLists 的打包规则）
install(TARGETS fg_server RUNTIME DESTINATION .)
