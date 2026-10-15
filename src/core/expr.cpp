#include "expr.h"

#include <array>
#include <cctype>
#include <cmath>
#include <deque>
#include <numbers>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <exprtk.hpp>

namespace fg {

namespace {

// 阶跃函数 H(x)：x < 0 得 0，x >= 0 得 1。ExprTk 没有内置，注册成自定义函数，
// symbol_table 只保存函数对象的指针，因此该对象必须由编译槽持有（见下）。
// 同一对象以 "heaviside" 与短别名 "step" 两个名字注册。
struct HeavisideFn : public exprtk::ifunction<double> {
    using exprtk::ifunction<double>::operator();
    HeavisideFn() : exprtk::ifunction<double>(1) {}
    double operator()(const double& x) override { return (x < 0.0) ? 0.0 : 1.0; }
};

// 变量全集 = 所有采样模式用到的变量名的并集（curve.cpp 各处 compile_expression
// 的 vars 参数只会传它的子集）。编译槽初始化时把它一次性注册进符号表并长期复用，
// 出现全集外的名字直接拒绝——那是调用方与这里脱节的编程错误，由测试
// VariableSetMatchesAllCallers 守着两边同步
constexpr const char* kAllVars[] = {"x", "y", "t", "theta", "dy"};
constexpr size_t kAllVarCount = sizeof(kAllVars) / sizeof(kAllVars[0]);

size_t all_var_index(const std::string& name) {
    for (size_t i = 0; i < kAllVarCount; ++i) {
        if (name == kAllVars[i]) return i;
    }
    return kAllVarCount;
}

typedef std::pair<std::string, exprtk::parser<double>::symbol_type> CollectSym;

// 编译槽：单条表达式的成本里，"把 ExprTk 的一切现搭起来"（符号表/表达式对象/
// parser、常量与函数注册、变量绑定、逐项 settings）占了 ~95%——实测 950µs 里
// 解析本身只要 ~20µs。把搭建好的对象缓存在线程内反复使用，编译成本降两个数量级
struct CompileSlot {
    // 变量值存储用 deque：元素地址稳定，ExprTk 内部持有的引用不会因扩容失效
    std::deque<double> storage;
    std::unordered_map<std::string, size_t> index;  // 本次 vars 名 -> storage 下标（每次编译重建）
    HeavisideFn heaviside;
    exprtk::symbol_table<double> symtab;
    exprtk::expression<double> expr;
    std::optional<exprtk::parser<double>> parser;  // settings 只能构造时传入、parser 不可拷贝，故延迟构造
    bool init = false;
    bool in_use = false;
};

// 同时活跃的表达式数量：采样时最多两个（参数方程/二阶 ODE 的 x、y 各一个），
// 4 个槽留足余量；全被占用（理论上到不了）就单独分配一个，功能不受影响
constexpr size_t kSlotCount = 4;

void init_slot(CompileSlot& s) {
    s.symtab.add_constant("pi", std::numbers::pi);
    s.symtab.add_constant("e", std::numbers::e);
    s.symtab.add_function("heaviside", s.heaviside);
    s.symtab.add_function("step", s.heaviside);
    for (size_t i = 0; i < kAllVarCount; ++i) s.storage.emplace_back(0.0);
    for (size_t i = 0; i < kAllVarCount; ++i) s.symtab.add_variable(kAllVars[i], s.storage[i]);
    s.expr.register_symbol_table(s.symtab);

    typedef exprtk::parser<double> parser_t;
    typedef parser_t::settings_t settings_t;
    // 依赖核对改为"编译时顺带收集"：collect 开在 settings 上后，编译产物里的
    // 符号列表直接给出本次表达式用到的函数与变量，省掉两次独立的 collect_*
    // 全量解析（实测占单条编译 ~76%）。判定规则不变，见 check_collected
    settings_t opts(settings_t::default_compile_all_opts | settings_t::e_collect_vars |
                    settings_t::e_collect_funcs);
    s.parser.emplace(opts);
    // 第三层限制：即使前面两层有疏漏，ExprTk 也不会解析出控制流/赋值/逻辑运算
    s.parser->settings()
        .disable_all_control_structures()
        .disable_all_logic_ops()
        .disable_all_assignment_ops()
        .disable_all_inequality_ops()
        .disable_local_vardef();
    s.init = true;
}

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

// 第二层限制（独立版）：ExprTk 的 collect_functions / collect_variables 报告
// 表达式实际用到的函数与名字（词法边界——数字字面量、科学计数法、符号分隔——全由库处理），
// 这里只按白名单放行；未注册的名字在后面的真实编译里还会被再拦一次（ERR234）。
// 返回 ok 的错误结构表示通过。调用前须已通过字符白名单。
// 只在"编译失败"的稀有路径调用：用于给出与旧版完全一致的精确错误
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

// 第二层限制（收集版）：数据源换成编译时顺带收集的符号列表。判定与独立版完全同构
// （先函数后变量、变量分支的 pi/e 与函数白名单兜底）——两者的判定与错误文案已在
// 等价性实验里逐例比对过（见 review/2026-10-15_perf_round1.md 的对应改动）
Error check_collected(const std::vector<CollectSym>& syms, const std::vector<std::string>& vars) {
    for (const auto& [name, ty] : syms) {
        if (ty == exprtk::parser<double>::e_st_function && !is_allowed_function(name)) {
            return {"expr_forbidden_function", "不允许的函数：" + name};
        }
    }
    for (const auto& [name, ty] : syms) {
        if (ty != exprtk::parser<double>::e_st_variable) continue;  // 编译成功时不会有其他类型
        bool ok = (name == "pi" || name == "e");
        for (const auto& v : vars) {
            if (name == v) ok = true;
        }
        if (!ok && is_allowed_function(name)) ok = true;
        if (!ok) return {"expr_unknown_name", "未知名称：" + name + "（本模式不允许该变量）"};
    }
    return {};
}

}  // namespace

struct Expression::Impl {
    // 编译槽句柄：槽来自线程内池（from_pool）时析构归还；池满时单独分配的直接销毁
    CompileSlot* slot = nullptr;
    bool from_pool = false;

    ~Impl() {
        if (!slot) return;
        if (from_pool) {
            slot->in_use = false;
        } else {
            delete slot;
        }
    }
};

Expression::Expression() = default;
Expression::~Expression() = default;
Expression::Expression(Expression&&) noexcept = default;
Expression& Expression::operator=(Expression&&) noexcept = default;

bool Expression::set_var(const std::string& name, double value) {
    if (!impl_ || !impl_->slot) return false;
    const auto it = impl_->slot->index.find(name);
    if (it == impl_->slot->index.end()) return false;
    impl_->slot->storage[it->second] = value;
    return true;
}

double* Expression::var_ref(const std::string& name) {
    if (!impl_ || !impl_->slot) return nullptr;
    const auto it = impl_->slot->index.find(name);
    if (it == impl_->slot->index.end()) return nullptr;
    return &impl_->slot->storage[it->second];
}

double Expression::eval() const { return (impl_ && impl_->slot) ? impl_->slot->expr.value() : 0.0; }

bool Expression::eval_finite(double& out) const {
    if (!impl_ || !impl_->slot) return false;
    const double v = impl_->slot->expr.value();
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

    for (const auto& name : vars) {
        if (all_var_index(name) == kAllVarCount) {
            error = {"expr_unknown_name", "未知名称：" + name + "（本模式不允许该变量）"};
            return std::nullopt;
        }
    }

    static thread_local std::array<CompileSlot, kSlotCount> pool;
    CompileSlot* slot = nullptr;
    bool from_pool = false;
    for (auto& s : pool) {
        if (!s.in_use) {
            s.in_use = true;
            slot = &s;
            from_pool = true;
            break;
        }
    }
    if (!slot) slot = new CompileSlot;

    // 守卫：任何提前返回都归还槽；成功路径置 released，所有权交给 Expression
    struct Guard {
        CompileSlot* slot;
        bool pooled;
        bool released = false;
        ~Guard() {
            if (released || !slot) return;
            if (pooled) slot->in_use = false;
            else delete slot;
        }
    } guard{slot, from_pool};

    if (!slot->init) init_slot(*slot);

    if (!slot->parser->compile(text, slot->expr)) {
        // 编译失败多因名字非法或语法问题：独立解析的报错更精确（点名"未知名称/
        // 不允许的函数"），没有则回落到 ExprTk 的编译错误。此路径只在非法表达式
        // 上走，慢一些无所谓
        error = check_dependencies(text, vars);
        if (error.ok()) error = {"expr_compile_failed", "表达式错误：" + slot->parser->error()};
        return std::nullopt;
    }

    std::vector<CollectSym> syms;
    slot->parser->dec().symbols(syms);
    error = check_collected(syms, vars);
    if (!error.ok()) return std::nullopt;

    // 名称映射按本次 vars 重建（set_var/var_ref 只认本次注册的名字，与旧版逐名
    // 注册的语义一致）；用到的变量值清零——槽复用时这些位置可能留着上一表达式
    // 的残留值，旧版"新建即 0"的初始状态必须保持一致
    slot->index.clear();
    for (const auto& name : vars) {
        const size_t i = all_var_index(name);
        slot->index[name] = i;
        slot->storage[i] = 0.0;
    }

    Expression e;
    e.impl_ = std::make_unique<Expression::Impl>();
    e.impl_->slot = slot;
    e.impl_->from_pool = from_pool;
    guard.released = true;
    return e;
}

}  // namespace fg
