find_package(GTest CONFIG REQUIRED)

add_executable(fg_tests
    test_main.cpp
    test_expr.cpp
    test_curve.cpp
    test_world.cpp
    test_ai.cpp
    test_room.cpp
    test_protocol.cpp
    test_web.cpp
    test_sha256.cpp
    # 静态文件路径/类型判定是纯函数（不依赖 websocketpp），直接编进来单测
    ../server/web_static.cpp
)
target_include_directories(fg_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../server)
target_link_libraries(fg_tests PRIVATE fg_core GTest::gtest_main)

add_test(NAME fg_tests COMMAND fg_tests)
