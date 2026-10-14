#pragma once
// 统一错误结构：core 与服务器之间传递的所有"可预期失败"都经它往返。
// code 是稳定的机器可读标识（小写下划线风格的 slug）：客户端按码做程序化分支，
// 未来的多语言客户端按码查文案表（message 的中文原文只作为默认展示与日志）。
// 新增错误时：码一经发布就不再改动含义（改文案不改码），同句不同义必须给不同码。
#include <string>

namespace fg {

struct Error {
    std::string code;
    std::string message;

    bool ok() const { return code.empty(); }
};

}  // namespace fg
