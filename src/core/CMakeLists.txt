# ExprTk（header-only）：优先仓库内 third_party 的锁定版本（跨平台一致、
# 不依赖包管理器——它的 vcpkg port 在 Linux 上构建失败过）；找不到时回退系统安装
find_path(EXPRTK_INCLUDE_DIR "exprtk.hpp"
          PATHS ${CMAKE_SOURCE_DIR}/third_party/exprtk NO_DEFAULT_PATH)
if(NOT EXPRTK_INCLUDE_DIR)
    find_path(EXPRTK_INCLUDE_DIR "exprtk.hpp")
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
target_include_directories(fg_core SYSTEM PUBLIC ${EXPRTK_INCLUDE_DIR})
# Eigen 同样以 SYSTEM 方式引入，避免其头文件的警告干扰
target_include_directories(fg_core SYSTEM PUBLIC ${EIGEN3_INCLUDE_DIR})
target_link_libraries(fg_core PUBLIC Eigen3::Eigen nlohmann_json::nlohmann_json)
