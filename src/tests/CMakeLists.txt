find_package(GTest CONFIG REQUIRED)

add_executable(fb_tests
    test_main.cpp
    test_expr.cpp
    test_curve.cpp
    test_world.cpp
    test_ai.cpp
    test_wrap.cpp
    test_room.cpp
    test_protocol.cpp
    test_web.cpp
    test_sha256.cpp
    # 静态文件路径/类型判定是纯函数（不依赖 websocketpp），直接编进来单测
    ../server/web_static.cpp
)
target_include_directories(fb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../server)
target_link_libraries(fb_tests PRIVATE fg_core GTest::gtest_main)

add_test(NAME fb_tests COMMAND fb_tests)
