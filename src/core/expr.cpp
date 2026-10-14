#include "expr.h"

#include <cctype>
#include <cmath>
#include <deque>
#include <numbers>
#include <string>
#include <unordered_map>
#include <vector>

#include <exprtk.hpp>

namespace fg {

namespace {

// 阶跃函数 H(x)：x < 0 得 0，x >= 0 得 1。ExprTk 没有内置，注册成自定义函数，
// symbol_table 只保存函数对象的指针，因此该对象必须由 Impl 持有（见下）。
// 同一对象以 "heaviside" 与短别名 "step" 两个名字注册。
struct HeavisideFn : public exprtk::ifunction<double> {
    using exprtk::ifunction<double>::operator();
    HeavisideFn() : exprtk::ifunction<double>(1) {}
    double operator()(const double& x) override { return (x < 0.0) ? 0.0 : 1.0; }
};

}  // namespace

struct Expression::Impl {
    // 变量值存储用 deque：元素地址稳定，ExprTk 内部持有的引用不会因扩容失效
    std::deque<double> storage;
    std::unordered_map<std::string, size_t> index;  // 变量名 → storage 下标
    HeavisideFn heaviside;
    exprtk::symbol_table<double> symtab;
    exprtk::expression<double> expr;
};

namespace {

// 第一层限制：字符白名单。玩家能写的只有数学符号，
// if/赋值/比较/逻辑/字符串等所依赖的字符在这里就被挡掉。
bool is_allowed_char(char c) {
    const auto uc = static_cast<unsigned char>(c);
    if (std::isdigit(uc) || std::isalpha(uc) || c == '_') return true;
    switch (c) {
        case '+': case '-': case '*': case '/': case '^':
        case '(': case ')': case '.': case ',':
        case ' ': case '\t':
            return true;
        default:
            return false;
    }
}

bool is_allowed_function(const std::string& name) {
    for (const char* f : kAllowedFunctions) {
        if (name == f) return true;
    }
    return false;
}

// 第二层限制：依赖核对。ExprTk 的 collect_functions / collect_variables 报告表达式
// 实际用到的函数与名字（词法边界——数字字面量、科学计数法、符号分隔——全由库处理），
// 这里只按白名单放行；未注册的名字在后面的真实编译里还会被再拦一次（ERR234）。
// 返回 ok 的错误结构表示通过。调用前须已通过字符白名单。
Error check_dependencies(const std::string& text, const std::vector<std::string>& vars) {
    std::vector<std::string> funcs;
    exprtk::collect_functions(text, funcs);
    for (const auto& f : funcs) {
        if (!is_allowed_function(f)) return {"expr_forbidden_function", "不允许的函数：" + f};
    }

    std::vector<std::string> used;
    exprtk::collect_variables(text, used);
    for (const auto& name : used) {
        bool ok = (name == "pi" || name == "e");
        for (const auto& v : vars) {
            if (name == v) ok = true;
        }
        // 未注册的自定义函数（如 heaviside）会被收集器归入变量集：真实编译会正确受理它，
        // 这里按函数白名单放行，避免误判成未知变量
        if (!ok && is_allowed_function(name)) ok = true;
        if (!ok) return {"expr_unknown_name", "未知名称：" + name + "（本模式不允许该变量）"};
    }
    return {};
}

}  // namespace

Expression::Expression() = default;
Expression::~Expression() = default;
Expression::Expression(Expression&&) noexcept = default;
Expression& Expression::operator=(Expression&&) noexcept = default;

bool Expression::set_var(const std::string& name, double value) {
    if (!impl_) return false;
    auto it = impl_->index.find(name);
    if (it == impl_->index.end()) return false;
    impl_->storage[it->second] = value;
    return true;
}

double Expression::eval() const { return impl_ ? impl_->expr.value() : 0.0; }

bool Expression::eval_finite(double& out) const {
    if (!impl_) return false;
    const double v = impl_->expr.value();
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

std::optional<Expression> compile_expression(const std::string& text, const std::vector<std::string>& vars,
                                             Error& error) {
    if (text.empty()) {
        error = {"expr_empty", "表达式为空"};
        return std::nullopt;
    }
    if (text.size() > kMaxExpressionLength) {
        error = {"expr_too_long",
                 "表达式过长（最多 " + std::to_string(kMaxExpressionLength) + " 字符）"};
        return std::nullopt;
    }

    for (char c : text) {
        if (!is_allowed_char(c)) {
            error = {"expr_bad_char", std::string("表达式包含不允许的字符：'") + c + "'"};
            return std::nullopt;
        }
    }

    error = check_dependencies(text, vars);
    if (!error.ok()) return std::nullopt;

    Expression e;
    e.impl_ = std::make_unique<Expression::Impl>();
    auto& impl = *e.impl_;

    impl.symtab.add_constant("pi", std::numbers::pi);
    impl.symtab.add_constant("e", std::numbers::e);
    impl.symtab.add_function("heaviside", impl.heaviside);
    impl.symtab.add_function("step", impl.heaviside);

    for (const auto& name : vars) {
        impl.index[name] = impl.storage.size();
        impl.storage.emplace_back(0.0);
    }
    for (const auto& name : vars) {
        impl.symtab.add_variable(name, impl.storage[impl.index[name]]);
    }

    impl.expr.register_symbol_table(impl.symtab);

    exprtk::parser<double> parser;
    // 第三层限制：即使前面两层有疏漏，ExprTk 也不会解析出控制流/赋值/逻辑运算
    parser.settings()
        .disable_all_control_structures()
        .disable_all_logic_ops()
        .disable_all_assignment_ops()
        .disable_all_inequality_ops()
        .disable_local_vardef();

    if (!parser.compile(text, impl.expr)) {
        // ExprTk 的错误为英文（含位置信息），加中文前缀便于玩家理解
        error = {"expr_compile_failed", "表达式错误：" + parser.error()};
        return std::nullopt;
    }
    return e;
}

}  // namespace fg
