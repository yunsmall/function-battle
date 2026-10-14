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

# websocketpp 0.8.2 的构造函数/析构函数带 C++20 起非法的 template-id 写法
# （GCC 14+ 直接报错，clang 与旧 GCC 只是宽容），下载后逐一修正为等价写法。
# 仅在文件仍含旧写法时替换写回：幂等，已有构建不会因每次 configure 被改动而重编
function(fg_ws_patch file)
    file(READ "${FG_WS_DIR}/websocketpp/${file}" txt)
    set(orig "${txt}")
    foreach(pair IN LISTS ARGN)
        string(FIND "${pair}" "|" sep)
        string(SUBSTRING "${pair}" 0 ${sep} old_s)
        math(EXPR after "${sep} + 1")
        string(SUBSTRING "${pair}" ${after} -1 new_s)
        string(REPLACE "${old_s}" "${new_s}" txt "${txt}")
    endforeach()
    if(NOT txt STREQUAL orig)
        file(WRITE "${FG_WS_DIR}/websocketpp/${file}" "${txt}")
    endif()
endfunction()

fg_ws_patch(logger/basic.hpp
    "~basic<concurrency,names>() {}|~basic() {}"
    "basic<concurrency,names>(channel_type_hint::value h =|basic(channel_type_hint::value h ="
    "basic<concurrency,names>(std::ostream * out)|basic(std::ostream * out)"
    "basic<concurrency,names>(level c, channel_type_hint::value h =|basic(level c, channel_type_hint::value h ="
    "basic<concurrency,names>(level c, std::ostream * out)|basic(level c, std::ostream * out)"
    "basic<concurrency,names>(basic<concurrency,names> const & other)|basic(basic<concurrency,names> const & other)"
    "basic<concurrency,names>(basic<concurrency,names> && other)|basic(basic<concurrency,names> && other)")
fg_ws_patch(logger/syslog.hpp
    "syslog<concurrency,names>(channel_type_hint::value hint =|syslog(channel_type_hint::value hint ="
    "syslog<concurrency,names>(level channels, channel_type_hint::value hint =|syslog(level channels, channel_type_hint::value hint =")
fg_ws_patch(endpoint.hpp
    "~endpoint<connection,config>() {}|~endpoint() {}")
fg_ws_patch(roles/server_endpoint.hpp
    "~server<config>() {}|~server() {}"
    "server<config>(server<config> &) = delete|server(server<config> &) = delete"
    "server<config>(server<config> && o)|server(server<config> && o)")

# 与项目一致用 standalone asio：ASIO_STANDALONE 让它 #include <asio.hpp> 而不是
# boost 版。_WEBSOCKETPP_CPP11_STL_：MSVC 默认不按标准报 __cplusplus（199711L），
# websocketpp 探测不到 C++11 会退回 boost 实现；这个宏是它给构建系统预留的开关
target_include_directories(fg_server PRIVATE ${FG_WS_DIR})
target_compile_definitions(fg_server PRIVATE ASIO_STANDALONE _WEBSOCKETPP_CPP11_STL_)
target_link_libraries(fg_server PRIVATE fg_core asio::asio cxxopts::cxxopts)

# 发布布局：exe 与 web/ 同层（配合顶层 CMakeLists 的打包规则）
install(TARGETS fg_server RUNTIME DESTINATION .)
