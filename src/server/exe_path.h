#pragma once

#include <string>

namespace fg {

// 可执行文件所在目录（末尾不带分隔符）；取不到时返回空串
std::string exe_dir();

}  // namespace fg
