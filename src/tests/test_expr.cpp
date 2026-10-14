// 表达式封装（ExprTk）的单元测试：求值能力 + "纯数学"限制
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "expr.h"

TEST(Expr, BasicEval) {
    fg::Error err;
    auto e = fg::compile_expression("x + 1", {"x"}, err);
    ASSERT_TRUE(e.has_value()) << err.message;
    EXPECT_TRUE(e->set_var("x", 2.0));
    EXPECT_DOUBLE_EQ(e->eval(), 3.0);
    EXPECT_FALSE(e->set_var("nope", 1.0));  // 未注册的变量名应失败
}

TEST(Expr, PowerOperator) {
    // 玩家写幂通常用 ^ 或 pow()，两者都要能用
    fg::Error err;
    auto e1 = fg::compile_expression("x^2", {"x"}, err);
    ASSERT_TRUE(e1.has_value()) << err.message;
    e1->set_var("x", 3.0);
    EXPECT_DOUBLE_EQ(e1->eval(), 9.0);

    auto e2 = fg::compile_expression("pow(x, 3)", {"x"}, err);
    ASSERT_TRUE(e2.has_value()) << err.message;
    e2->set_var("x", 2.0);
    EXPECT_DOUBLE_EQ(e2->eval(), 8.0);
}

TEST(Expr, BuiltinFunctions) {
    fg::Error err;

    auto e1 = fg::compile_expression("sin(pi/2)", {}, err);
    ASSERT_TRUE(e1.has_value()) << err.message;
    EXPECT_NEAR(e1->eval(), 1.0, 1e-12);

    auto e2 = fg::compile_expression("cos(x)", {"x"}, err);
    ASSERT_TRUE(e2.has_value()) << err.message;
    e2->set_var("x", 0.0);
    EXPECT_DOUBLE_EQ(e2->eval(), 1.0);

    auto e3 = fg::compile_expression("sqrt(abs(x))", {"x"}, err);
    ASSERT_TRUE(e3.has_value()) << err.message;
    e3->set_var("x", -9.0);
    EXPECT_DOUBLE_EQ(e3->eval(), 3.0);

    auto e4 = fg::compile_expression("log(exp(x))", {"x"}, err);
    ASSERT_TRUE(e4.has_value()) << err.message;
    e4->set_var("x", 2.5);
    EXPECT_NEAR(e4->eval(), 2.5, 1e-12);
}

TEST(Expr, Constants) {
    fg::Error err;
    auto e1 = fg::compile_expression("pi", {}, err);
    ASSERT_TRUE(e1.has_value()) << err.message;
    EXPECT_NEAR(e1->eval(), 3.141592653589793, 1e-12);

    auto e2 = fg::compile_expression("e", {}, err);
    ASSERT_TRUE(e2.has_value()) << err.message;
    EXPECT_NEAR(e2->eval(), std::exp(1.0), 1e-12);
}

TEST(Expr, MultiVars) {
    fg::Error err;
    auto e = fg::compile_expression("x*x + y*y + t", {"x", "y", "t"}, err);
    ASSERT_TRUE(e.has_value()) << err.message;
    e->set_var("x", 3.0);
    e->set_var("y", 4.0);
    e->set_var("t", 0.5);
    EXPECT_DOUBLE_EQ(e->eval(), 25.5);
}

TEST(Expr, ImplicitMultiplySupported) {
    // ExprTk 支持隐式乘法：2x 等同 2*x（玩家习惯的写法）
    fg::Error err;
    auto e = fg::compile_expression("2x", {"x"}, err);
    ASSERT_TRUE(e.has_value()) << err.message;
    e->set_var("x", 3.0);
    EXPECT_DOUBLE_EQ(e->eval(), 6.0);
}

TEST(Expr, StepFunction) {
    // 阶跃函数：heaviside 直接可用（用户常用），(1+sgn(x))/2 是等价写法
    fg::Error err;
    auto h = fg::compile_expression("heaviside(x)", {"x"}, err);
    ASSERT_TRUE(h.has_value()) << err.message;
    h->set_var("x", -1.0);
    EXPECT_DOUBLE_EQ(h->eval(), 0.0);
    h->set_var("x", 0.0);
    EXPECT_DOUBLE_EQ(h->eval(), 1.0);
    h->set_var("x", 2.5);
    EXPECT_DOUBLE_EQ(h->eval(), 1.0);

    auto s = fg::compile_expression("(1+sgn(x))/2", {"x"}, err);
    ASSERT_TRUE(s.has_value()) << err.message;
    s->set_var("x", -3.0);
    EXPECT_DOUBLE_EQ(s->eval(), 0.0);
    s->set_var("x", 3.0);
    EXPECT_DOUBLE_EQ(s->eval(), 1.0);
}

TEST(Expr, StepAlias) {
    // step 是 heaviside 的短别名，可与其他函数/运算随意组合
    fg::Error err;
    auto e = fg::compile_expression("10*step(x-2) + 2*x", {"x"}, err);
    ASSERT_TRUE(e.has_value()) << err.message;
    e->set_var("x", 1.0);
    EXPECT_DOUBLE_EQ(e->eval(), 2.0);
    e->set_var("x", 3.0);
    EXPECT_DOUBLE_EQ(e->eval(), 16.0);
}

TEST(Expr, HelpGroupsMatchWhitelist) {
    // 帮助面板的分组清单（kExprFuncGroups）与函数白名单（kAllowedFunctions）必须双向覆盖：
    // 以后增删函数时只改了一边，这里会失败并点名漏掉的名字
    std::vector<std::string> allowed;
    for (const char* n : fg::kAllowedFunctions) allowed.push_back(n);
    std::vector<std::string> grouped;
    for (const auto& g : fg::kExprFuncGroups) {
        const std::string s = g.funcs;
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && s[i] == ' ') ++i;
            size_t j = i;
            while (j < s.size() && s[j] != ' ') ++j;
            if (j > i) grouped.push_back(s.substr(i, j - i));
            i = j;
        }
    }
    auto contains = [](const std::vector<std::string>& v, const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };
    for (const auto& n : grouped) EXPECT_TRUE(contains(allowed, n)) << "帮助有、白名单没有：" << n;
    for (const auto& n : allowed) EXPECT_TRUE(contains(grouped, n)) << "白名单有、帮助没有：" << n;
}

TEST(Expr, ScientificNotation) {
    // 科学计数法里的 e 属于数字字面量，不是变量名（曾把 1e5 误判成未知名称 e5）
    fg::Error err;
    auto e1 = fg::compile_expression("1e5", {}, err);
    ASSERT_TRUE(e1.has_value()) << err.message;
    EXPECT_DOUBLE_EQ(e1->eval(), 100000.0);

    auto e2 = fg::compile_expression("x + 2.5e-3", {"x"}, err);
    ASSERT_TRUE(e2.has_value()) << err.message;
    e2->set_var("x", 1.0);
    EXPECT_NEAR(e2->eval(), 1.0025, 1e-12);
}

TEST(Expr, RejectsNonMathInput) {
    fg::Error err;
    // 比较运算、赋值、字符串、逻辑运算、控制流、非白名单函数：全部拒绝
    for (const char* bad : {"x > 0", "x := 5", "'abc'", "x and 1", "if(x>0,1,2)",
                            "while(x)", "avg(x, 1)", "not x", "x[0]", "sum(x)"}) {
        auto e = fg::compile_expression(bad, {"x"}, err);
        EXPECT_FALSE(e.has_value()) << "不应通过: " << bad;
        EXPECT_FALSE(err.ok());
    }
}

TEST(Expr, UnknownVariableRejected) {
    fg::Error err;
    auto e = fg::compile_expression("y + 1", {"x"}, err);
    EXPECT_FALSE(e.has_value());
    EXPECT_FALSE(err.ok());
}

TEST(Expr, SyntaxErrorRejected) {
    fg::Error err;
    auto e = fg::compile_expression("x +", {"x"}, err);
    EXPECT_FALSE(e.has_value());
    EXPECT_FALSE(err.ok());
}

TEST(Expr, EmptyAndTooLongRejected) {
    fg::Error err;
    EXPECT_FALSE(fg::compile_expression("", {"x"}, err).has_value());

    std::string long_expr(600, '1');
    EXPECT_FALSE(fg::compile_expression(long_expr, {"x"}, err).has_value());
    EXPECT_FALSE(err.ok());
}

TEST(Expr, NonFiniteDetected) {
    fg::Error err;
    auto e = fg::compile_expression("1/(x-x)", {"x"}, err);
    ASSERT_TRUE(e.has_value()) << err.message;
    e->set_var("x", 1.0);
    double out = 0.0;
    EXPECT_FALSE(e->eval_finite(out));  // 除以零 → Inf/NaN，应被识别

    auto e2 = fg::compile_expression("x", {"x"}, err);
    ASSERT_TRUE(e2.has_value()) << err.message;
    e2->set_var("x", 7.0);
    EXPECT_TRUE(e2->eval_finite(out));
    EXPECT_DOUBLE_EQ(out, 7.0);
}
