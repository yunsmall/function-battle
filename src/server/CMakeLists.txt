find_package(asio CONFIG REQUIRED)
find_package(cxxopts CONFIG REQUIRED)

add_executable(fg_server
    main.cpp
    net.cpp
    server.cpp
    web_front.cpp
    web_static.cpp
)
# websocketpp（third_party 里的 0.8.2 源码）与项目一致用 standalone asio：
# ASIO_STANDALONE 让它 #include <asio.hpp> 而不是 boost 版
target_include_directories(fg_server PRIVATE ${CMAKE_SOURCE_DIR}/third_party/websocketpp)
target_compile_definitions(fg_server PRIVATE ASIO_STANDALONE)
target_link_libraries(fg_server PRIVATE fg_core asio::asio cxxopts::cxxopts)
