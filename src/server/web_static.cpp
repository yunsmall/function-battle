#include "web_static.h"

#include <cctype>

namespace fg {

std::string resolve_static_path(const std::string& url_path) {
    // 剥 query/fragment（与 serve.py 一致），再去掉前导 '/'
    const std::string bare = url_path.substr(0, url_path.find_first_of("?#"));
    const size_t start = bare.find_first_not_of('/');
    const std::string raw = (start == std::string::npos) ? std::string() : bare.substr(start);
    if (raw.empty()) return "index.html";

    // 逐段规范化：拒绝 ".."（目录穿越）；'\' 在 Windows 上也是分隔符、NUL 会截断
    // 路径，出现即拒绝（正常 URL 用不到）。"." 与空段（重复斜杠）直接省略
    std::string out;
    size_t pos = 0;
    while (true) {
        const size_t slash = raw.find('/', pos);
        const std::string seg =
            raw.substr(pos, (slash == std::string::npos) ? std::string::npos : slash - pos);
        if (seg == "..") return {};
        if (seg.find('\\') != std::string::npos || seg.find('\0') != std::string::npos) return {};
        if (!seg.empty() && seg != ".") {
            if (!out.empty()) out += '/';
            out += seg;
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return out.empty() ? std::string("index.html") : out;
}

const char* mime_type_for(const std::string& rel_path) {
    const size_t dot = rel_path.rfind('.');
    std::string ext = (dot == std::string::npos) ? std::string() : rel_path.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    struct Entry {
        const char* ext;
        const char* type;
    };
    static const Entry kTypes[] = {
        {".html", "text/html; charset=utf-8"},   {".js", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},     {".json", "application/json; charset=utf-8"},
        {".svg", "image/svg+xml"},               {".png", "image/png"},
        {".ico", "image/x-icon"},
    };
    for (const Entry& e : kTypes) {
        if (ext == e.ext) return e.type;
    }
    return "application/octet-stream";
}

}  // namespace fg
