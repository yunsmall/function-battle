// AI 对打评测工具：让各难度的 AI 两两对打若干局，统计从"低难度方"视角看的胜率，
// 用来检验"难度越高越强"的梯度是否真的成立（各档参数亮眼不等于对局能赢）。
// 不进常规单测：动辄几百局、分钟级耗时，按需手动跑（每局完全由种子决定，可复现）。
//
// 对局驱动与房间层的 AI 路径同构：每个 AI 一个持久 Rng（同一套种子公式）、
// 维护"上一发"供拟人化微调；思考延时与展示停顿不影响胜负，评测里直接提交。
// 同一地图种子跑两个方向（难度换边）：一份地形两种座次，把先手/地形偏向平均掉。

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <cxxopts.hpp>

#include "ai.h"
#include "ai_curves.h"
#include "expr.h"
#include "world.h"

namespace {

// 单局结果（winner 3 = 到达步数上限仍未分胜负，正常对局不会出现）
struct AiTiming {
    long long calls = 0;
    long long total_ns = 0;
    long long max_ns = 0;
};

struct GameOutcome {
    int winner = 2;
    long long turns = 0;
    long long submit_fail = 0;  // AI 提交被拒次数（生成器缺陷的信号，必须为 0）
    // 出手类型分布：玩家 id -> 类型名 -> 次数（提交成功才计）
    std::map<int, std::map<std::string, long long>> types;
    // 决策耗时：玩家 id -> 计时（单次 ai_decide 的平均/最大是服务器 tick 的实时性指标）
    std::map<int, AiTiming> ai_time;
    // 命中次数（PlayerHit 事件）：玩家 id -> 次数——"简单档一连几局一枪不中"的量化
    std::map<int, long long> hits;
    // 每发炮弹"打中几人"的分布：玩家 id -> [中 0/1/2/3+ 人的发数]——穿透机制下
    // 一发多杀是核心观感（"简单档一发只打一个"就是拿它对表验证）
    std::map<int, std::array<long long, 4>> shot_hits;
    // 每发的结局四桶：玩家 id -> [撞墙未命中, 撞墙且命中, 干净命中, 纯放空] 的发数
    // ——"空炮"里多少是炸墙、多少是脱靶/出界，靠它区分
    std::map<int, std::array<long long, 4>> shot_ends;
    // 击杀类型分布（PlayerDied 时按"最近一次提交的类型"归类）——"用什么函数干掉对手"
    // 的口径。不用 w.projectile()：穿杀后同一帧撞墙的炮弹已被 reset，会漏计
    std::map<int, std::map<std::string, long long>> kill_types;
};

// 形状分类直接复用 core 的（原先在这里自己实现过一份，漏了 floor 导致统计失真）
const char* kind_name(fg::CurveKind k) {
    switch (k) {
        case fg::CurveKind::Poly: return "多项式";
        case fg::CurveKind::Wave: return "波浪";
        case fg::CurveKind::Stair: return "阶梯";
        case fg::CurveKind::Bend: return "折线";
        case fg::CurveKind::Polar: return "极坐标";
        case fg::CurveKind::Parametric: return "参数方程";
        case fg::CurveKind::Ode1: return "dy/dx";
        case fg::CurveKind::Ode2: return "(x',y')";
        case fg::CurveKind::Ypp: return "y''";
    }
    return "?";
}

const char* const kTypeCols[] = {"多项式", "波浪",  "阶梯",  "折线",      "极坐标",
                                 "参数方程", "dy/dx", "(x',y')", "y''"};

GameOutcome run_game(int diff_a, int diff_b, uint64_t map_seed, int soldiers, int obstacles) {
    fg::GameConfig cfg;
    cfg.match.seed = map_seed;
    cfg.match.obstacle_count = obstacles;
    // 展示停顿只影响观感节奏，不影响任何判定：评测设为 0 省掉空转的 tick
    cfg.match.turn_delay = 0.0;
    cfg.room.soldiers_per_player = soldiers;

    const std::vector<fg::PlayerSpec> specs = {
        fg::PlayerSpec(1, "A", fg::Team::A, true, diff_a),
        fg::PlayerSpec(2, "B", fg::Team::B, true, diff_b),
    };
    fg::World w;
    w.start(cfg, specs);

    // 与房间层同构的随机源：不同 id 得到不同序列，同种子完全可复现
    std::map<int, fg::Rng> rngs;
    for (const auto& s : specs) {
        rngs.emplace(s.id, fg::Rng(map_seed * 1000ULL + static_cast<uint64_t>(s.id) * 17ULL + 1ULL));
    }
    // "微调上一发"的基准：该 AI 上一回合成功提交的请求（房间层的 last_request）
    std::map<int, fg::CurveRequest> last_req;
    std::map<int, bool> has_last;

    GameOutcome out;
    // 每个玩家最近一次提交成功的类型：PlayerDied 用它归类（一次只有一发炮弹在飞，
    // 阵亡必然是"发射者最近那一发"打的——即使该发同帧撞墙被销毁口径也不丢）
    std::map<int, std::string> last_kind;
    // 当前这一发（最近一次 Shot 之后）打中的人数与是否撞上障碍：炮弹没有"结束事件"，
    // 发与发的边界靠下一个 Shot（一局同时只有一发在飞）；局末由收尾处补一次入桶
    int cur_shot_owner = -1;
    int cur_shot_hits = 0;
    bool cur_shot_wall = false;  // 本发是否撞上障碍爆炸（CraterMade）
    const auto flush_shot = [&out, &cur_shot_owner, &cur_shot_hits, &cur_shot_wall]() {
        if (cur_shot_owner >= 0) {
            ++out.shot_hits[cur_shot_owner][static_cast<size_t>(std::min(cur_shot_hits, 3))];
            // 四桶互斥：撞墙未命中 / 撞墙且命中 / 干净命中 / 纯放空（含出界）
            const size_t bucket = cur_shot_wall ? (cur_shot_hits > 0 ? 1u : 0u)
                                                : (cur_shot_hits > 0 ? 2u : 3u);
            ++out.shot_ends[cur_shot_owner][bucket];
        }
        cur_shot_owner = -1;
        cur_shot_hits = 0;
        cur_shot_wall = false;
    };
    const auto drain_events = [&out, &last_kind, &cur_shot_owner, &cur_shot_hits, &cur_shot_wall,
                               &flush_shot](const std::vector<fg::Event>& evs) {
        for (const auto& e : evs) {
            if (e.type == fg::EventType::TurnBegin) ++out.turns;
            if (e.type == fg::EventType::Shot) {
                flush_shot();
                cur_shot_owner = e.player_id;
            }
            if (e.type == fg::EventType::CraterMade) cur_shot_wall = true;
            if (e.type == fg::EventType::PlayerHit) {
                ++out.hits[e.player_id];
                ++cur_shot_hits;
            }
            if (e.type == fg::EventType::PlayerDied) {
                const auto it = last_kind.find(e.player_id);
                if (it != last_kind.end()) ++out.kill_types[e.player_id][it->second];
            }
        }
    };
    drain_events(w.take_events());

    constexpr double kDt = 1.0 / 60.0;
    constexpr long long kMaxSteps = 60LL * 1800;  // 30 分钟游戏时间的防僵持上限
    long long last_attempt_turn = -1;
    for (long long steps = 0; w.phase() != fg::Phase::GameOver && steps < kMaxSteps; ++steps) {
        if (w.phase() == fg::Phase::Input) {
            const int pid = w.current_player_id();
            // 同一回合只尝试一次：提交被拒后不重试，等输入超时自然跳过
            if (pid > 0 && out.turns != last_attempt_turn) {
                last_attempt_turn = out.turns;
                const int diff = (pid == 1) ? diff_a : diff_b;
                const auto tb = std::chrono::steady_clock::now();
                const fg::AiDecision dec = fg::ai_decide(
                    w, pid, diff, rngs.at(pid), has_last[pid] ? &last_req.at(pid) : nullptr);
                const long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - tb)
                                         .count();
                AiTiming& tm = out.ai_time[pid];
                ++tm.calls;
                tm.total_ns += ns;
                tm.max_ns = std::max(tm.max_ns, ns);
                if (w.submit(pid, dec.request).ok()) {
                    last_req[pid] = dec.request;
                    has_last[pid] = true;
                    last_kind[pid] = kind_name(fg::classify_curve(dec.request));
                    ++out.types[pid][last_kind[pid]];
                } else {
                    ++out.submit_fail;
                }
            }
        }
        w.update(kDt);
        drain_events(w.take_events());
    }
    flush_shot();  // 局末最后一发没有下一个 Shot 来收尾，在这里入桶
    out.winner = (w.phase() == fg::Phase::GameOver) ? w.winner() : 3;
    return out;
}

// 一对难度的汇总（全部从"低难度方"视角计）
struct PairStat {
    long long low_win = 0;
    long long high_win = 0;
    long long draw = 0;
    long long unfinished = 0;
    long long submit_fail = 0;
    long long turns = 0;  // 所有局回合数之和，用于算均值
    long long games = 0;
};

const char* diff_name(int d) {
    switch (d) {
        case 0: return "简单";
        case 1: return "普通";
        case 2: return "困难";
        default: return "大神";
    }
}

// 性能探针（--probe）：量化"单条曲线采样 / 单条候选评估 / 整批候选生成"的耗时，
// 定位 AI 单次决策的耗时大头——简化难度前必须知道钱花在哪
int run_probe() {
    fg::GameConfig cfg;
    cfg.match.seed = 20261015;
    cfg.room.soldiers_per_player = 3;
    fg::World w;
    w.start(cfg, {fg::PlayerSpec(1, "A", fg::Team::A, true, 2),
                  fg::PlayerSpec(2, "B", fg::Team::B, true, 2)});
    const fg::Vec2 origin = w.players()[0].soldiers[0].pos;
    const fg::Vec2 aim = w.players()[1].soldiers[0].pos;
    const double dx = aim.x() - origin.x();
    const double dy = aim.y() - origin.y();
    const bool horizontal = std::abs(dx) >= std::abs(dy);

    const auto bench = [](const char* name, int n, auto&& fn) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < n; ++i) fn(i);
        const double us =
            std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0)
                .count() /
            n;
        std::printf("%-26s %9.1f 微秒/次（%d 次）\n", name, us, n);
    };

    std::printf("=== 性能探针 ===\n");
    // 分解实验：编译成本 vs 求值成本——短路径曲线（点数少一个量级）若耗时接近，
    // 说明大头在 ExprTk 编译（与表达式/点数无关），否则大头在逐点求值
    bench("expr 编译单条", 500, [&](int i) {
        char text[64];
        std::snprintf(text, sizeof(text), "%.4f*x+%.4f*x^2", 0.5 + 0.001 * i, 0.03);
        fg::Error err;
        const auto e = fg::compile_expression(text, {"x"}, err);
        (void)e;
    });
    bench("sample_curve 单条", 500, [&](int i) {
        const fg::CurveResult r =
            fg::sample_curve(fg::make_parabola(horizontal, dx, dy, 2.0 + 0.001 * i), origin, cfg);
        (void)r;
    });
    bench("sample_curve 短路径", 500, [&](int) {
        const fg::CurveResult r =
            fg::sample_curve(fg::make_parabola(horizontal, 3.0, 1.0, 0.5), origin, cfg);
        (void)r;
    });
    bench("evaluate_candidate 单条", 500, [&](int i) {
        const fg::CurveCandidate c = fg::evaluate_candidate(
            fg::make_parabola(horizontal, dx, dy, 2.0 + 0.001 * i), origin, aim, w);
        (void)c;
    });
    for (const int diff : {0, 1, 2}) {
        char name[40];
        std::snprintf(name, sizeof(name), "generate_candidates d%d", diff);
        bench(name, 100, [&](int) {
            fg::Rng local(777);
            const auto v = fg::generate_candidates(horizontal, dx, dy, diff, origin, aim, w, local);
            (void)v;
        });
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    cxxopts::Options options("fb_ai_bench", "AI 对打评测：各难度两两对打、统计胜率矩阵");
    std::string pairs_opt;
    options.add_options()("games", "每对每方向的对局数",
                          cxxopts::value<int>()->default_value("50"))(
        "seed", "地图种子基准（同一种子评测完全可复现）",
        cxxopts::value<uint64_t>()->default_value("20261015"))(
        "soldiers", "每玩家士兵数", cxxopts::value<int>()->default_value("3"))(
        "obstacles", "地图障碍数", cxxopts::value<int>()->default_value("15"))(
        "threads", "并行线程数", cxxopts::value<int>()->default_value("16"))(
        "pairs", "逗号分隔的难度对（低-高），默认全部相邻与非相邻组合",
        cxxopts::value<std::string>()->default_value("0-1,0-2,0-3,1-2,1-3,2-3"))(
        "profile", "调参专用：档位参数覆盖，格式 \"dif:key=val,key=val;...\"，"
                   "key ∈ {pos,angle,k2,k3,think_min,think_max,cap}，未给的键取默认档位表",
        cxxopts::value<std::string>()->default_value(""))(
        "json", "在文本报告后追加一段以 === JSON === 标记的机读结果（调参脚本读它）",
        cxxopts::value<bool>()->default_value("false"))(
        "probe", "性能探针：量化采样/评估/候选生成的耗时构成", cxxopts::value<bool>()->default_value("false"))(
        "h,help", "显示帮助");

    int games = 50, soldiers = 3, obstacles = 15, threads = 16;
    uint64_t seed_base = 20261015;
    std::string profile_opt;
    bool json_out = false;
    try {
        const auto result = options.parse(argc, argv);
        if (result.count("help")) {
            std::printf("%s\n", options.help().c_str());
            return 0;
        }
        if (result["probe"].as<bool>()) return run_probe();
        games = std::max(1, result["games"].as<int>());
        seed_base = result["seed"].as<uint64_t>();
        soldiers = std::clamp(result["soldiers"].as<int>(), 1, 4);
        obstacles = std::max(0, result["obstacles"].as<int>());
        threads = std::clamp(result["threads"].as<int>(), 1, 32);
        pairs_opt = result["pairs"].as<std::string>();
        profile_opt = result["profile"].as<std::string>();
        json_out = result["json"].as<bool>();
    } catch (const cxxopts::exceptions::exception& e) {
        std::fprintf(stderr, "参数错误：%s\n%s\n", e.what(), options.help().c_str());
        return 1;
    }

    // 档位参数覆盖（见 ai.h）：以默认档位表起底、按 key=val 逐项替换；只在评测进程内
    // 生效。解析失败一律报错退出（调参脚本拼错参数要能立刻看出来）
    if (!profile_opt.empty()) {
        try {
            size_t i = 0;
            while (i < profile_opt.size()) {
                const size_t semi = profile_opt.find(';', i);
                const std::string seg =
                    profile_opt.substr(i, semi == std::string::npos ? semi : semi - i);
                i = (semi == std::string::npos) ? profile_opt.size() : semi + 1;
                const size_t colon = seg.find(':');
                if (colon == std::string::npos) throw std::invalid_argument("段缺 ':'：" + seg);
                const int d = std::stoi(seg.substr(0, colon));
                if (d < 0 || d >= fg::kAiDifficultyCount) throw std::invalid_argument("难度越界");
                fg::AiProfile prof = fg::ai_profile(d);  // 默认档位表起底
                std::vector<std::string> seen_keys;      // 同档重复键报错，防静默覆盖
                for (size_t j = colon + 1; j < seg.size();) {
                    const size_t comma = seg.find(',', j);
                    const std::string kv =
                        seg.substr(j, comma == std::string::npos ? comma : comma - j);
                    j = (comma == std::string::npos) ? seg.size() : comma + 1;
                    const size_t eq = kv.find('=');
                    if (eq == std::string::npos) throw std::invalid_argument("项缺 '='：" + kv);
                    const std::string key = kv.substr(0, eq);
                    if (std::find(seen_keys.begin(), seen_keys.end(), key) != seen_keys.end()) {
                        throw std::invalid_argument("重复键：" + key);
                    }
                    seen_keys.push_back(key);
                    // 数值要"整段被消费"且有限：stod 会静默接受 "0.1abc" 的前缀，
                    // 也会正常解析出 nan/inf——调参可以轻易拼出这类值，静默吃进去
                    // 比报错难查得多
                    const std::string val_s = kv.substr(eq + 1);
                    size_t consumed = 0;
                    const double val = std::stod(val_s, &consumed);
                    if (consumed != val_s.size()) {
                        throw std::invalid_argument("数值含多余字符：" + kv);
                    }
                    if (!std::isfinite(val)) throw std::invalid_argument("数值非有限：" + kv);
                    if (key == "pos") prof.pos_error = val;
                    else if (key == "angle") prof.angle_error = val;
                    else if (key == "k2") prof.multi_keep2 = val;
                    else if (key == "k3") prof.multi_keep3 = val;
                    else if (key == "think_min") prof.think_min = val;
                    else if (key == "think_max") prof.think_max = val;
                    else if (key == "cap") prof.aim_slack_cap = val;
                    else if (key == "jitter") {
                        // 转 int 前先限界：lround 超范围/NaN 是未指定行为
                        if (val < 0.0 || val > 1000.0) {
                            throw std::invalid_argument("jitter 越界（0..1000）：" + kv);
                        }
                        prof.jitter_rounds = static_cast<int>(std::lround(val));
                    } else {
                        throw std::invalid_argument("未知键：" + key);
                    }
                }
                fg::set_ai_profile_override(d, prof);
                std::printf("  [profile] 档位 %d 覆盖：pos=%.4f angle=%.4f k2=%.2f k3=%.2f cap=%.2f\n",
                            d, prof.pos_error, prof.angle_error, prof.multi_keep2, prof.multi_keep3,
                            prof.aim_slack_cap);
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "profile 参数解析失败：%s\n", e.what());
            return 1;
        }
    }

    struct PairDef {
        int low = 0, high = 1;
    };
    std::vector<PairDef> defs;
    try {
        for (size_t i = 0; i < pairs_opt.size();) {
            const size_t comma = pairs_opt.find(',', i);
            const std::string tok =
                pairs_opt.substr(i, comma == std::string::npos ? comma : comma - i);
            i = comma == std::string::npos ? pairs_opt.size() : comma + 1;
            const size_t dash = tok.find('-');
            if (dash == std::string::npos) {
                std::fprintf(stderr, "无法解析难度对：%s\n", tok.c_str());
                return 1;
            }
            int a = std::stoi(tok.substr(0, dash));
            int b = std::stoi(tok.substr(dash + 1));
            if (a < 0 || b < 0 || a >= fg::kAiDifficultyCount || b >= fg::kAiDifficultyCount ||
                a == b) {
                std::fprintf(stderr, "难度对越界或相等：%s\n", tok.c_str());
                return 1;
            }
            if (a > b) std::swap(a, b);
            defs.push_back({a, b});
        }
    } catch (const std::exception&) {
        // stoi 对空串/非数字会抛异常：友好报错而不是 terminate
        std::fprintf(stderr, "无法解析难度对列表：%s\n", pairs_opt.c_str());
        return 1;
    }
    std::sort(defs.begin(), defs.end(), [](const PairDef& x, const PairDef& y) {
        return x.low != y.low ? x.low < y.low : x.high < y.high;
    });
    defs.erase(std::unique(defs.begin(), defs.end(), [](const PairDef& x, const PairDef& y) {
                   return x.low == y.low && x.high == y.high;
               }),
               defs.end());
    if (defs.empty()) {
        std::fprintf(stderr, "没有可评测的难度对\n");
        return 1;
    }

    // 任务编号 -> (对, 局, 方向)：方向 0 = 低难度坐 A 队，1 = 低难度坐 B 队
    const long long per_pair = static_cast<long long>(games) * 2;
    const long long total = per_pair * static_cast<long long>(defs.size());
    std::vector<PairStat> stats(defs.size());
    // 出手类型分布：按出手者难度归集（跨所有组合），看各档的"花样"差异
    std::map<int, std::map<std::string, long long>> diff_types;
    std::map<int, std::map<std::string, long long>> diff_kill_types;  // 击杀类型按出手难度
    std::map<int, long long> diff_zero_shot;  // 整局一枪没发的局数（按被零封者难度）
    std::map<int, AiTiming> diff_ai_time;     // 决策耗时按出手难度归集
    std::map<int, long long> diff_games;      // 该难度参与的局数
    std::map<int, long long> diff_hit_games;  // 其中至少命中过一次的局数
    std::map<int, long long> diff_hits;       // 命中总次数
    // 每发"打中几人"的分布（按出手难度）：[0]/[1]/[2]/[3+] 各自发射数
    std::map<int, std::array<long long, 4>> diff_shot_hits;
    std::map<int, std::array<long long, 4>> diff_shot_ends;
    std::mutex mu;
    std::atomic<long long> next{0};
    std::atomic<long long> done{0};

    std::printf("=== AI 对打评测 ===\n");
    std::printf("每对 %d 局×双向 / 每玩家 %d 兵 / 障碍 %d / 线程 %d / 种子基 %llu\n", games, soldiers,
                obstacles, threads, static_cast<unsigned long long>(seed_base));

    const auto t0 = std::chrono::steady_clock::now();
    const auto worker = [&]() {
        for (;;) {
            const long long task = next.fetch_add(1);
            if (task >= total) break;
            const int pair_idx = static_cast<int>(task / per_pair);
            const int game = static_cast<int>((task % per_pair) / 2);
            const bool low_is_a = (task % 2) == 0;
            const uint64_t map_seed = seed_base + static_cast<uint64_t>(pair_idx) * 100000ULL +
                                      static_cast<uint64_t>(game);
            const int diff_a = low_is_a ? defs[pair_idx].low : defs[pair_idx].high;
            const int diff_b = low_is_a ? defs[pair_idx].high : defs[pair_idx].low;
            const GameOutcome o = run_game(diff_a, diff_b, map_seed, soldiers, obstacles);

            std::lock_guard<std::mutex> lk(mu);
            PairStat& st = stats[static_cast<size_t>(pair_idx)];
            ++st.games;
            st.turns += o.turns;
            st.submit_fail += o.submit_fail;
            // 某玩家整局没提交过是真实存在的（被先手一发多杀清场、轮不到出手）。
            // 按"被零封者的难度"统计——观感上这是最惨的局（全程没摸到键盘）
            const auto add_types = [&](int pid, int diff) {
                if (const auto it = o.types.find(pid); it != o.types.end()) {
                    for (const auto& [t, n] : it->second) diff_types[diff][t] += n;
                } else {
                    ++diff_zero_shot[diff];
                }
                if (const auto it = o.kill_types.find(pid); it != o.kill_types.end()) {
                    for (const auto& [t, n] : it->second) diff_kill_types[diff][t] += n;
                }
            };
            add_types(1, diff_a);
            add_types(2, diff_b);
            for (const auto& [pid, tm] : o.ai_time) {
                AiTiming& g = diff_ai_time[pid == 1 ? diff_a : diff_b];
                g.calls += tm.calls;
                g.total_ns += tm.total_ns;
                g.max_ns = std::max(g.max_ns, tm.max_ns);
            }
            for (const int pid : {1, 2}) {
                const int d = pid == 1 ? diff_a : diff_b;
                ++diff_games[d];
                const auto hit_it = o.hits.find(pid);
                if (hit_it != o.hits.end() && hit_it->second > 0) {
                    ++diff_hit_games[d];
                    diff_hits[d] += hit_it->second;
                }
                if (const auto sh = o.shot_hits.find(pid); sh != o.shot_hits.end()) {
                    for (size_t b = 0; b < sh->second.size(); ++b) {
                        diff_shot_hits[d][b] += sh->second[b];
                    }
                }
                if (const auto se = o.shot_ends.find(pid); se != o.shot_ends.end()) {
                    for (size_t b = 0; b < se->second.size(); ++b) {
                        diff_shot_ends[d][b] += se->second[b];
                    }
                }
            }
            if (o.winner == 2) {
                ++st.draw;
            } else if (o.winner == 3) {
                ++st.unfinished;
            } else if ((o.winner == 0) == low_is_a) {
                ++st.low_win;
            } else {
                ++st.high_win;
            }
            const long long n = done.fetch_add(1) + 1;
            if (n % 50 == 0 || n == total) {
                std::printf("[进度] %lld/%lld\n", n, total);
                std::fflush(stdout);
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads));
    for (int t = 0; t < threads; ++t) pool.emplace_back(worker);
    for (auto& th : pool) th.join();
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("\n组合                       低方胜  高方胜   平局   未分  高方胜率   均回合  提交失败\n");
    bool suspect = false;
    for (size_t i = 0; i < defs.size(); ++i) {
        const PairStat& st = stats[i];
        const long long decided = st.low_win + st.high_win;
        const double rate = decided > 0 ? static_cast<double>(st.high_win) / decided : 0.0;
        std::printf("%d(%s) vs %d(%s)   %8lld%8lld%7lld%7lld%9.1f%%%9.1f%10lld\n", defs[i].low,
                    diff_name(defs[i].low), defs[i].high, diff_name(defs[i].high), st.low_win,
                    st.high_win, st.draw, st.unfinished, rate * 100.0,
                    static_cast<double>(st.turns) / static_cast<double>(std::max(1LL, st.games)),
                    st.submit_fail);
        // 相邻档之间高难度胜率应显著过半；低于这个线基本说明"更难的一档没更强"
        if (defs[i].high == defs[i].low + 1 && decided > 0 && rate < 0.55) suspect = true;
        if (st.submit_fail > 0 || st.unfinished > 0) suspect = true;
    }
    // 出手类型分布：识别"某一档只会用一两种形状"这类单一化问题
    std::printf("\n=== 出手类型分布（按出手难度，跨全部对局） ===\n");
    std::printf("难度      ");
    for (const char* name : kTypeCols) std::printf("%8s", name);
    std::printf("%10s\n", "出手数");
    for (int d = 0; d < fg::kAiDifficultyCount; ++d) {
        bool in_play = false;
        for (const auto& def : defs) in_play = in_play || def.low == d || def.high == d;
        if (!in_play) continue;
        long long total = 0;
        for (const auto& [t, n] : diff_types[d]) total += n;
        std::printf("%d(%s)   ", d, diff_name(d));
        for (const char* name : kTypeCols) {
            const auto it = diff_types[d].find(name);
            const long long n = it == diff_types[d].end() ? 0 : it->second;
            const double pct = total > 0 ? 100.0 * static_cast<double>(n) /
                                               static_cast<double>(total)
                                         : 0.0;
            std::printf("%7.1f%%", pct);
        }
        std::printf("%10lld\n", total);
    }
    std::printf("被零封局数（整局一枪未发，按被零封者难度）：");
    for (int d = 0; d < fg::kAiDifficultyCount; ++d) {
        const auto it = diff_zero_shot.find(d);
        if (it == diff_zero_shot.end() || it->second == 0) continue;
        std::printf(" %d(%s)=%lld", d, diff_name(d), it->second);
    }
    // 击杀类型分布：用户视角的"用什么函数干掉对手"——比出手分布更贴近观感
    std::printf("\n=== 击杀类型分布（致死的炮弹按出手难度归类） ===\n");
    std::printf("难度      ");
    for (const char* name : kTypeCols) std::printf("%8s", name);
    std::printf("%10s\n", "击杀数");
    for (int d = 0; d < fg::kAiDifficultyCount; ++d) {
        auto it = diff_kill_types.find(d);
        if (it == diff_kill_types.end()) continue;
        long long total = 0;
        for (const auto& [t, n] : it->second) total += n;
        std::printf("%d(%s)   ", d, diff_name(d));
        for (const char* name : kTypeCols) {
            const auto kt = it->second.find(name);
            const long long n = kt == it->second.end() ? 0 : kt->second;
            std::printf("%7.1f%%", total > 0 ? 100.0 * static_cast<double>(n) /
                                                   static_cast<double>(total)
                                             : 0.0);
        }
        std::printf("%10lld\n", total);
    }

    std::printf("\n");
    // 命中情况：简单档"好几局一枪不中"的观感问题靠这组数看（命中局占比越高越不闷）
    std::printf("=== 命中情况（按难度，跨全部对局） ===\n");
    for (const auto& [d, games_n] : diff_games) {
        const long long hg = diff_hit_games.count(d) ? diff_hit_games.at(d) : 0;
        const long long hits_n = diff_hits.count(d) ? diff_hits.at(d) : 0;
        std::printf("%d(%s)：≥1 次命中的局占 %.1f%% / 平均每局命中 %.2f 次（%lld 局）\n", d,
                    diff_name(d), 100.0 * static_cast<double>(hg) /
                                      static_cast<double>(std::max(1LL, games_n)),
                    static_cast<double>(hits_n) / static_cast<double>(std::max(1LL, games_n)),
                    games_n);
    }
    // 每发"打中几人"分布：空炮=0 中；2+/3+ 即穿透多杀（简单档的目标是"只打一个"，
    // 用它对表验证；困难/大神靠刻意多杀拉开收益）
    std::printf("=== 每发打中人数分布（按出手难度，跨全部对局） ===\n");
    for (const auto& [d, arr] : diff_shot_hits) {
        const long long tot = arr[0] + arr[1] + arr[2] + arr[3];
        if (tot == 0) continue;
        std::printf("%d(%s)：发数 %lld | 空炮 %.1f%% / 中1人 %.1f%% / 中2人 %.1f%% / 中3+人 %.1f%%\n",
                    d, diff_name(d), tot, 100.0 * static_cast<double>(arr[0]) / tot,
                    100.0 * static_cast<double>(arr[1]) / tot,
                    100.0 * static_cast<double>(arr[2]) / tot,
                    100.0 * static_cast<double>(arr[3]) / tot);
    }
    // 每发结局四桶：区分"炸墙"与"放空/出界"——回答"AI 是不是老撞障碍"
    std::printf("=== 每发结局（按出手难度） ===\n");
    for (const auto& [d, arr] : diff_shot_ends) {
        const long long tot = arr[0] + arr[1] + arr[2] + arr[3];
        if (tot == 0) continue;
        std::printf("%d(%s)：发数 %lld | 撞墙未命中 %.1f%% / 撞墙且命中 %.1f%% / 干净命中 %.1f%% / 纯放空 %.1f%%\n",
                    d, diff_name(d), tot, 100.0 * static_cast<double>(arr[0]) / tot,
                    100.0 * static_cast<double>(arr[1]) / tot,
                    100.0 * static_cast<double>(arr[2]) / tot,
                    100.0 * static_cast<double>(arr[3]) / tot);
    }
    // 单次决策耗时：服务器 tick 里 AI 是丢线程池跑的，这里衡量的是"计算本身"的重度
    std::printf("=== AI 单次决策耗时（求平均与最大） ===\n");
    for (const auto& [d, tm] : diff_ai_time) {
        if (tm.calls == 0) continue;
        std::printf("%d(%s)：平均 %.2f ms / 最大 %.1f ms（%lld 次决策）\n", d, diff_name(d),
                    static_cast<double>(tm.total_ns) / static_cast<double>(tm.calls) / 1e6,
                    static_cast<double>(tm.max_ns) / 1e6, tm.calls);
    }

    std::printf("\n总耗时 %.1f 秒。高方胜率 = 高方胜/(低方胜+高方胜)，不含平局与未分。\n", secs);
    if (suspect) {
        std::printf("[!] 存在可疑项：相邻档高方胜率 <55%%，或有提交失败/未分胜负——梯度需要调校。\n");
    } else {
        std::printf("梯度检查通过：相邻档高方胜率均 >=55%%，无提交失败、无未分胜负。\n");
    }

    // 机读结果（调参脚本用）：文本报告之后再打一段带标记的 JSON，脚本只解析标记之后的部分
    if (json_out) {
        std::printf("\n=== JSON ===\n{");
        std::printf("\"games_per_dir\":%d,\"pairs\":[", games);
        for (size_t k = 0; k < defs.size(); ++k) {
            const PairStat& ps = stats[k];
            std::printf("%s{\"low\":%d,\"high\":%d,\"games\":%lld,\"low_wins\":%lld,"
                        "\"high_wins\":%lld,\"draws\":%lld,\"unfinished\":%lld}",
                        k == 0 ? "" : ",", defs[k].low, defs[k].high, ps.games, ps.low_win,
                        ps.high_win, ps.draw, ps.unfinished);
        }
        std::printf("],\"hit\":{");
        bool first = true;
        for (const auto& [d, n] : diff_games) {
            std::printf("%s\"%d\":{\"games\":%lld,\"hit_games\":%lld,\"hits\":%lld}",
                        first ? "" : ",", d, n,
                        diff_hit_games.count(d) ? diff_hit_games.at(d) : 0,
                        diff_hits.count(d) ? diff_hits.at(d) : 0);
            first = false;
        }
        std::printf("},\"shot_hits\":{");
        first = true;
        for (const auto& [d, arr] : diff_shot_hits) {
            std::printf("%s\"%d\":[%lld,%lld,%lld,%lld]", first ? "" : ",", d, arr[0], arr[1],
                        arr[2], arr[3]);
            first = false;
        }
        std::printf("},\"shot_ends\":{");
        first = true;
        for (const auto& [d, arr] : diff_shot_ends) {
            std::printf("%s\"%d\":[%lld,%lld,%lld,%lld]", first ? "" : ",", d, arr[0], arr[1],
                        arr[2], arr[3]);
            first = false;
        }
        std::printf("},\"types\":{");
        first = true;
        for (const auto& [d, m] : diff_types) {
            std::printf("%s\"%d\":{", first ? "" : ",", d);
            bool f2 = true;
            for (const auto& [t, n] : m) {
                std::printf("%s\"%s\":%lld", f2 ? "" : ",", t.c_str(), n);
                f2 = false;
            }
            std::printf("}");
            first = false;
        }
        std::printf("},\"kill_types\":{");
        first = true;
        for (const auto& [d, m] : diff_kill_types) {
            std::printf("%s\"%d\":{", first ? "" : ",", d);
            bool f2 = true;
            for (const auto& [t, n] : m) {
                std::printf("%s\"%s\":%lld", f2 ? "" : ",", t.c_str(), n);
                f2 = false;
            }
            std::printf("}");
            first = false;
        }
        std::printf("}}\n");
    }
    return 0;
}
