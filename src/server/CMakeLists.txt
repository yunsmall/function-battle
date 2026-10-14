find_package(asio CONFIG REQUIRED)
find_package(cxxopts CONFIG REQUIRED)

add_executable(fg_server
    main.cpp
    net.cpp
    server.cpp
    web_front.cpp
    web_static.cpp
)
# websocketpp 0.8.2（header-only）：配置时从官方仓库下载并校验 SHA-256，源码不进
# 仓库。不用 vcpkg 的 port（会把 boost 依赖链拖进来）；也不用 FetchContent 的
# add_subdirectory（它自带 CMake 工程会找 boost 构建自己的测试），只要头文件
set(FG_WS_DIR ${CMAKE_BINARY_DIR}/_deps/websocketpp-0.8.2/websocketpp-0.8.2)
if(NOT EXISTS ${FG_WS_DIR}/websocketpp/endpoint.hpp)
    file(DOWNLOAD
         "https://github.com/zaphoyd/websocketpp/archive/refs/tags/0.8.2.tar.gz"
         ${CMAKE_BINARY_DIR}/_deps/websocketpp-0.8.2.tar.gz
         EXPECTED_HASH SHA256=6ce889d85ecdc2d8fa07408d6787e7352510750daa66b5ad44aacb47bea76755)
    # 压缩包内自带一层 websocketpp-0.8.2/ 顶层目录（见上面的路径）
    file(ARCHIVE_EXTRACT INPUT ${CMAKE_BINARY_DIR}/_deps/websocketpp-0.8.2.tar.gz
                         DESTINATION ${CMAKE_BINARY_DIR}/_deps/websocketpp-0.8.2)
endif()

# 与项目一致用 standalone asio：ASIO_STANDALONE 让它 #include <asio.hpp> 而不是
# boost 版。_WEBSOCKETPP_CPP11_STL_：MSVC 默认不按标准报 __cplusplus（199711L），
# websocketpp 探测不到 C++11 会退回 boost 实现；这个宏是它给构建系统预留的开关
target_include_directories(fg_server PRIVATE ${FG_WS_DIR})
target_compile_definitions(fg_server PRIVATE ASIO_STANDALONE _WEBSOCKETPP_CPP11_STL_)
target_link_libraries(fg_server PRIVATE fg_core asio::asio cxxopts::cxxopts)

# 发布布局：exe 与 web/ 同层（配合顶层 CMakeLists 的打包规则）
install(TARGETS fg_server RUNTIME DESTINATION .)
