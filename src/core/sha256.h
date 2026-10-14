#pragma once
// SHA-256（FIPS 180-4）的紧凑实现，供房间密码使用：客户端把明文密码在本地算成
// 十六进制哈希后再上行（传输中不出现明文），服务器只存/比对哈希字符串、不重算。
// 自写而非引依赖：算法固定、代码短，项目保持 standalone 的依赖面。

#include <string>
#include <string_view>

namespace fg {

// 返回 64 字符的小写十六进制
std::string sha256_hex(std::string_view data);

}  // namespace fg
