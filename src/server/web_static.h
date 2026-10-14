#pragma once
// 静态文件服务的路径与类型判定（纯函数、不依赖网络与文件系统——单独放以便单测）。
// 行为与退役的 python 网关（web/serve.py）对齐：不做 URL 解码（编码穿越会按字面
// 查文件、自然落到 404，"先解码再检查"反而多一整类绕过），拒绝 ".." 段与反斜杠。

#include <string>

namespace fg {

// URL 路径 → 静态根下的相对路径（'/' 分隔）。
// 空串 = 拒绝（路径穿越等非法请求）；空路径/根路径返回 "index.html"
std::string resolve_static_path(const std::string& url_path);

// 按扩展名给 Content-Type（大小写不敏感）；未知类型回 application/octet-stream
//（表与 serve.py 的 CONTENT_TYPES 一致）
const char* mime_type_for(const std::string& rel_path);

}  // namespace fg
