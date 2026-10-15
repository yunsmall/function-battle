#pragma once
// 渲染层函数行的折行算法（士兵名字牌下的表达式文本）：按可用宽度切行、
// 优先在 + / - 处断行（下一行以符号开头、像数学排版）。宽度测量以回调注入——
// 渲染层传字体实测、测试传固定字宽；算法本身与渲染无关，放这里是为了
// 能被单测直接编进 fb_tests（与 web_static.cpp 直编进测试同理）

#include <algorithm>
#include <string>
#include <vector>

namespace fg {

// text：待折行文本；max_w：行宽上限（与 width 回调同单位）；width：测宽回调。
// 表达式一般是 num_text 生成的 ASCII，但"正在写"是玩家输入，按 UTF-8 字符簇
// 整体推进，防止把多字节字符切成孤立字节。返回各行（不含换行符）；空输入返回空
template <typename WidthFn>
std::vector<std::string> wrap_by_width(const std::string& text, float max_w, WidthFn width) {
    std::vector<std::string> lines;
    std::string cur;
    size_t last_break = std::string::npos;  // cur 内最近的 +/- 起始偏移（在符号前断）
    for (size_t i = 0; i < text.size();) {
        const unsigned char c0 = static_cast<unsigned char>(text[i]);
        const size_t n = c0 < 0x80 ? 1 : (c0 < 0xE0 ? 2 : (c0 < 0xF0 ? 3 : 4));
        const size_t take = std::min(n, text.size() - i);
        const std::string ch = text.substr(i, take);
        i += take;
        cur += ch;
        if (width(cur) > max_w && cur.size() > take) {
            const size_t cut = last_break != std::string::npos ? last_break : cur.size() - take;
            lines.push_back(cur.substr(0, cut));
            cur = cur.substr(cut);
            last_break = std::string::npos;
        }
        if ((ch == "+" || ch == "-") && cur.size() > take) last_break = cur.size() - take;
    }
    if (!cur.empty()) lines.push_back(cur);
    return lines;
}

}  // namespace fg
