#pragma once
// 数学表达式封装（基于 ExprTk），并把玩家输入限制为"纯数学表达式"：
//   1) 字符白名单：只允许 数字/字母/下划线 和 + - * / ^ ( ) . , 及空白
//      （< > = ! & | : ; ? [ ] { } ' " 等一概拒收，杜绝 if/赋值/字符串/逻辑运算）
//   2) 依赖核对：用 ExprTk 的 collect_variables/collect_functions 报告表达式实际用到的
//      变量与函数，按白名单放行——ExprTk 的内置函数远多于白名单（avg/sum/clamp 等），
//      必须依赖库自己的报告来核对，而不是手写词法扫描
//   3) ExprTk 设置：禁用控制结构/逻辑运算/赋值/不等式/局部变量定义
// ExprTk 头文件仅被 expr.cpp 包含，避免拖慢整个工程的编译。

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "error.h"

namespace fg {

// 表达式文本长度上限：防止超长表达式拖垮服务器解析
inline constexpr size_t kMaxExpressionLength = 500;

// 允许使用的数学函数（名称同时也是 ExprTk 认识的函数名；
// step / heaviside 由 expr.cpp 注册为同一个自定义函数：x<0 得 0，x>=0 得 1）
inline constexpr const char* kAllowedFunctions[] = {
    "abs",  "acos", "acosh",  "asin",  "asinh",   "atan",  "atanh", "atan2", "ceil", "cos",
    "cosh", "erf",  "erfc",   "exp",   "floor",   "hypot", "log",   "log10", "log2", "max",
    "min",  "pow",  "round",  "sgn",   "sin",     "sinh",  "sqrt",  "tan",   "tanh", "trunc",
    "heaviside", "step",
};

// 函数帮助面板的分组清单（客户端界面按类别展示"有哪些函数"）。
// funcs 为空格分隔的纯函数名，与 kAllowedFunctions 的双向覆盖由单测
// HelpGroupsMatchWhitelist 强制校验：增删函数时只改了一边，测试会失败并点名。
struct ExprFuncGroup {
    const char* title;  // 类别名
    const char* funcs;  // 该类别的函数名（空格分隔；可为空串，如"基本运算"只有运算符）
    const char* note;   // 补充说明（可为空串）
};
inline constexpr ExprFuncGroup kExprFuncGroups[] = {
    {"基本运算", "", "+  -  *  /  ^（乘方）和括号 ( )"},
    {"三角", "sin  cos  tan  asin  acos  atan", ""},
    {"双曲", "sinh  cosh  tanh  asinh  acosh  atanh", ""},
    {"指数对数", "exp  log  log10  log2  sqrt  pow", "exp 即 e^x，pow(a,b) 为 a 的 b 次方"},
    {"取整符号", "floor  ceil  round  trunc  abs  sgn", "sgn 取正负号（-1/0/1）"},
    {"多参数", "min  max  hypot  atan2", "如 max(x,3)、hypot(a,b)、atan2(y,x)"},
    {"误差", "erf  erfc", ""},
    {"阶跃", "step  heaviside", "x<0 得 0，x≥0 得 1；step 是别名"},
};

class Expression {
public:
    Expression();
    ~Expression();
    Expression(Expression&&) noexcept;
    Expression& operator=(Expression&&) noexcept;
    Expression(const Expression&) = delete;
    Expression& operator=(const Expression&) = delete;

    // 更新变量值；变量名必须是编译时注册过的，否则返回 false
    bool set_var(const std::string& name, double value);
    // 求值（变量为最近一次 set_var 的值）
    double eval() const;
    // 求值并要求结果有限（NaN/Inf 视为失败），供曲线采样时跳过无效段
    bool eval_finite(double& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend std::optional<Expression> compile_expression(const std::string&, const std::vector<std::string>&,
                                                        Error&);
};

// 编译表达式。vars 为允许使用的变量名列表（如 {"x"}）。
// 成功返回 Expression；失败返回 nullopt，error 为错误结构（code 机器可读、message 中文说明）。
std::optional<Expression> compile_expression(const std::string& text, const std::vector<std::string>& vars,
                                             Error& error);

}  // namespace fg
