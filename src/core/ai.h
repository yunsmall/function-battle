#pragma once
// 人机 AI：按难度生成"真实表达式"的曲线提交（界面上能看到 AI 到底写了什么函数）。
// AI 只负责决策，不直接改世界；上层（服务器/本地人机）在轮到 AI 时调用 ai_decide，
// 等过 delay 秒后再把 request 交给 World::submit。

#include "curve.h"
#include "rng.h"
#include "world.h"

namespace fg {

// AI 难度参数（可用哪些曲线形状由 ai_curves.h 的难度分级控制）
struct AiProfile {
    double pos_error = 0.05;    // 瞄准点位置误差（与目标距离的比例）
    double angle_error = 0.05;  // 绕自身旋转的角度误差（弧度）
    double think_min = 1.0;     // 思考时间下限（秒）
    double think_max = 3.0;     // 思考时间上限（秒，rng.range 为半开区间，实际取不到）
    // "多杀限流"：刻意瞄多个目标的曲线按此概率保留，否则退而只打单目标。
    // 无遮挡开阔图上"一发清场"是必胜打法，AI 每回合都这么干会让对局没有回合交锋，
    // 这里压成偶尔的杀手锏；曲线"顺路"扫中多人不算刻意，不受限（自然发生）
    double multi_keep2 = 0.5;   // 一次刻意扫两个
    double multi_keep3 = 0.25;  // 一次刻意扫三个及以上
};

// 多杀限流判定：reach 个目标的刻意多杀线是否保留（reach <= 1 恒保留且不掷骰）
bool multi_kill_kept(int reach, const AiProfile& prof, Rng& rng);

// AI 难度档数（0 简单 / 1 普通 / 2 困难 / 3 大神）：夹取范围与统计数组统一用它，不手写数字
inline constexpr int kAiDifficultyCount = 4;

AiProfile ai_profile(int difficulty);  // 0=简单，1=普通，2=困难，3=大神

struct AiDecision {
    CurveRequest request;
    double delay = 0.0;  // 思考时间（秒）
};

// 为 world 中 player_id 决策一次（出手的是他当前轮到的士兵）。rng 由上层持有
// （同种子初始化，保证对局可复现）。prev 为该 AI 上一回合成功提交的请求
//（可为空）：有一定概率生成"数字微调的上一发"作为候选，行为更像人在渐进瞄准
AiDecision ai_decide(const World& world, int player_id, int difficulty, Rng& rng,
                     const CurveRequest* prev = nullptr);

}  // namespace fg
