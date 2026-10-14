# ExprTk（header-only）——vcpkg 给出的用法就是 find_path 定位头文件（走 vcpkg，
# 与其他依赖一致）
find_path(EXPRTK_INCLUDE_DIRS "exprtk.hpp")
if(NOT EXPRTK_INCLUDE_DIRS)
    message(FATAL_ERROR "未找到 exprtk.hpp——请先安装依赖：vcpkg install exprtk")
endif()
find_package(Eigen3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(fg_core STATIC
    rng.cpp
    expr.cpp
    curve.cpp
    world.cpp
    ai.cpp
    ai_curves.cpp
    ai_ode.cpp
    protocol.cpp
    think_pool.cpp
    room.cpp
    sha256.cpp
)
target_include_directories(fg_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# SYSTEM：ExprTk 头文件本身警告较多，不污染本工程的警告输出
target_include_directories(fg_core SYSTEM PUBLIC ${EXPRTK_INCLUDE_DIRS})
# Eigen 同样以 SYSTEM 方式引入，避免其头文件的警告干扰
target_include_directories(fg_core SYSTEM PUBLIC ${EIGEN3_INCLUDE_DIR})
target_link_libraries(fg_core PUBLIC Eigen3::Eigen nlohmann_json::nlohmann_json)
