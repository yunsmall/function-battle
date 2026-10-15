// 折行算法（core/text_wrap.h）的纯逻辑测试：宽度用固定字宽回调注入，
// 只验证切行规则本身（渲染层的字体测量接线由实机验证）
#include <gtest/gtest.h>

#include <string>

#include "text_wrap.h"

namespace {

// 等宽回调：每字节 10 单位宽
float fixed_width(const std::string& s) { return static_cast<float>(s.size()) * 10.0f; }

// 拼回检查：不丢字符、不加字符
void expect_rejoin(const std::vector<std::string>& lines, const std::string& text) {
    std::string joined;
    for (const auto& l : lines) joined += l;
    EXPECT_EQ(joined, text);
}

}  // namespace

TEST(Wrap, EmptyAndSingle) {
    EXPECT_TRUE(fg::wrap_by_width("", 100.0f, fixed_width).empty());
    const auto one = fg::wrap_by_width("x", 100.0f, fixed_width);
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0], "x");
}

TEST(Wrap, LinesFitAndRejoin) {
    // 每行不超过上限（折行时新字符整体推入下一行）；拼回等于原文
    const std::string text = "0.5000*x+2.0000*sin(0.3000*x)+1.5000*(abs(x-20.0000)-20.0000)";
    const auto lines = fg::wrap_by_width(text, 160.0f, fixed_width);  // 每行至多 16 字节
    ASSERT_GT(lines.size(), 1u);
    for (const auto& l : lines) {
        EXPECT_LE(l.size() * 10, 160u) << "行超宽: " << l;
        EXPECT_FALSE(l.empty());
    }
    expect_rejoin(lines, text);
}

TEST(Wrap, PrefersPlusMinusBreak) {
    // 优先在 + / - 处断：除第一行外，各行都以符号开头
    const auto lines = fg::wrap_by_width("aaaa+bbbb-cccc+dddd", 55.0f, fixed_width);
    ASSERT_GE(lines.size(), 2u);
    for (size_t i = 1; i < lines.size(); ++i) {
        EXPECT_TRUE(lines[i][0] == '+' || lines[i][0] == '-') << "未在符号处断: " << lines[i];
    }
    expect_rejoin(lines, "aaaa+bbbb-cccc+dddd");
}

TEST(Wrap, LeadingSignNoEmptyFirstLine) {
    // 行首的符号不是断点：不会产出空行（空行会让牌下多出一条空隙）
    const auto lines = fg::wrap_by_width("+aaaa+bbbb", 55.0f, fixed_width);
    for (const auto& l : lines) EXPECT_FALSE(l.empty());
    expect_rejoin(lines, "+aaaa+bbbb");
}

TEST(Wrap, LongRunHardCutNoEmptyLines) {
    // 无可断点的长串：按字符硬切，不产生空行、不丢字符
    const auto lines = fg::wrap_by_width("123456789", 25.0f, fixed_width);  // 每行至多 2 字节
    ASSERT_GE(lines.size(), 4u);
    for (const auto& l : lines) EXPECT_FALSE(l.empty());
    expect_rejoin(lines, "123456789");
}

TEST(Wrap, Utf8NeverSplits) {
    // 多字节字符整体推进：任何一行都不会以 UTF-8 续字节（0b10xxxxxx）开头。
    // "中"占 3 字节，4 单位的行宽装不下两个"中"，必然是整字折行
    std::string text;
    for (int i = 0; i < 30; ++i) text += "中";
    const auto lines = fg::wrap_by_width(text, 4.0f, fixed_width);
    ASSERT_GE(lines.size(), 10u);
    for (const auto& l : lines) {
        ASSERT_FALSE(l.empty());
        EXPECT_NE(static_cast<unsigned char>(l[0]) & 0xC0, 0x80) << "行首是孤立续字节";
    }
    expect_rejoin(lines, text);
}
