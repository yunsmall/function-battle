cmake_minimum_required(VERSION 3.21)
project(function_game VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# 源码注释为中文（UTF-8），MSVC 需要显式指定；clang/gcc 默认即 UTF-8
# /bigobj：exprtk 这类巨型模板头会让 MSVC 单个目标文件的节数超过默认上限
# （error C1128），放开限制（对其它编译单元无害）
if(MSVC)
    add_compile_options(/utf-8 /bigobj)
endif()

add_subdirectory(src)

# ---------- 安装与打包（zip 发布） ----------
# 扁平布局：exe、raylib.dll、MSVC 运行库、web/ 同层；服务器按 exe 所在目录找
# web/static（见 src/server/main.cpp），解压后从任何位置都能直接运行
install(DIRECTORY web/static DESTINATION web)
install(FILES README.md README.en.md LICENSE PROTOCOL.md DESTINATION .)

if(WIN32)
    # clang 也用 MSVC 运行库（exe 依赖 vcruntime140/msvcp140）：随包分发，
    # 别人机器没装 VC++ 运行库也能直接跑。clang 可能独立于 VS 安装（本机就在
    # llvm/ 下），用 vswhere（VS 自带定位工具，在系统盘固定位置）找 VS 再找 Redist
    find_program(FG_VSWHERE vswhere
        PATHS "C:/Program Files (x86)/Microsoft Visual Studio/Installer"
        NO_DEFAULT_PATH)
    if(FG_VSWHERE)
        execute_process(COMMAND "${FG_VSWHERE}" -latest -products "*"
                        -property installationPath
                        OUTPUT_VARIABLE FG_VS_DIR OUTPUT_STRIP_TRAILING_WHITESPACE)
        file(TO_CMAKE_PATH "${FG_VS_DIR}" FG_VS_DIR)  # vswhere 输出反斜杠路径，转成正斜杠
    endif()
    file(GLOB FG_REDIST_DIRS
         "${FG_VS_DIR}/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT"
         "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/*/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT")
    if(FG_REDIST_DIRS)
        list(SORT FG_REDIST_DIRS COMPARE NATURAL ORDER DESCENDING)
        list(GET FG_REDIST_DIRS 0 FG_REDIST_DIR)
        install(FILES
            "${FG_REDIST_DIR}/vcruntime140.dll"
            "${FG_REDIST_DIR}/vcruntime140_1.dll"
            "${FG_REDIST_DIR}/msvcp140.dll"
            DESTINATION .)
    else()
        message(WARNING "未找到 MSVC 运行库（Redist）目录，发布前请核对包内是否附带 vcruntime140/msvcp140")
    endif()
endif()

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME "function_game")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_FILE_NAME "function_game-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)  # zip 内带顶层目录，解压干净
include(CPack)
