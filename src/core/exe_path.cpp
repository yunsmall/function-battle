// 跨平台取可执行文件所在目录。
// 单独成 TU 的原因：Windows 下 windows.h 与 asio 的 winsock2 有包含顺序冲突，
// 隔离在这个文件里就不影响其他源文件的包含环境
#include "exe_path.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>  // readlink
#endif

namespace fg {

std::string exe_dir() {
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    const std::string path(buf, n);
#else
    // Linux：/proc/self/exe 是标准接口（readlink 不写 NUL，返回字节数）
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    const std::string path(buf, static_cast<size_t>(n));
#endif
    const size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

}  // namespace fg
