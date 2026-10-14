#pragma once
// 世界模块：地图生成、回合状态机、炮弹步进与碰撞、胜负判定。
// 服务器与本地人机共用同一份实现；所有随机性来自配置里的种子，保证可复现。
//
// 回合流程：轮到某玩家（跳过阵亡者）→ 输入阶段倒计时（超时空过）→
// 提交成功则进入飞行阶段（炮弹沿曲线按弧长匀速运动）→ 炮弹消失后轮下一位。

#include <optional>
#include <string>
#include <vector>

#include "config.h"
#include "curve.h"
#include "error.h"
#include "vec2.h"

namespace fg {

enum class Team { A = 0, B = 1 };

// 士兵：多士兵规则下玩家持有的独立个体（每玩家 soldiers_per_player 个，全场统一）。
// 回合以"玩家"为轮转单位，轮到谁由他当前游标指向的士兵出手；胜负按"一队全部士兵"判定
struct Soldier {
    Vec2 pos;
    int max_hp = 1;
    int hp = 1;
    bool alive = true;
};

struct Player {
    int id = 0;    // 全局唯一 id
    int seat = 1;  // 队伍内序号（1 起）
    Team team = Team::A;
    std::string name;
    bool is_ai = false;
    int ai_difficulty = 1;          // 0=简单 1=普通 2=困难（仅对 AI 有效）
    std::vector<Soldier> soldiers;  // 大小 = cfg.room.soldiers_per_player（至少 1）
    int current_soldier = 0;        // 出手轮换游标：本玩家下一次出手用哪个士兵

    const Soldier& current() const { return soldiers[static_cast<size_t>(current_soldier)]; }
    Soldier& current() { return soldiers[static_cast<size_t>(current_soldier)]; }
    bool any_alive() const {
        for (const auto& s : soldiers) {
            if (s.alive) return true;
        }
        return false;
    }
    // 游标轮换到下一个存活士兵（出手后、或轮到时发现当前士兵已阵亡）；调用方保证有存活者
    void advance_soldier() {
        const size_t n = soldiers.size();
        for (size_t i = 1; i <= n; ++i) {
            const size_t k = (static_cast<size_t>(current_soldier) + i) % n;
            if (soldiers[k].alive) {
                current_soldier = static_cast<int>(k);
                return;
            }
        }
    }
};

struct Obstacle {
    bool is_circle = true;
    Vec2 center;
    double radius = 0.0;  // 圆形半径
    double half_w = 0.0;  // 矩形半宽
    double half_h = 0.0;  // 矩形半高
    std::vector<Vec2> craters;  // 被炮弹炸出的坑（坑内不再阻挡后续炮弹）
};

// 点是否被遮挡物阻挡（在形状内且不在任何坑内）
bool obstacle_blocks(const Obstacle& ob, const Vec2& p, double crater_radius);

// 遮挡物是否与一个圆（炮弹爆炸范围）相交——爆炸点周围的所有障碍都要被炸出坑
bool obstacle_overlaps_circle(const Obstacle& ob, const Vec2& center, double radius);

struct Projectile {
    int id = 0;
    int owner_id = 0;
    int owner_soldier = 0;  // 发射者士兵序号（炮口保护针对的就是这个士兵）
    CurveRequest request;   // 原始提交（含展示用表达式文本）
    Path path;
    double speed = 0.0;
    double travel = 0.0;  // 已行进弧长
    // 已结算过伤害的士兵（key = player_id * 16 + soldier；穿透时每个士兵只结算一次）
    std::vector<long> hit_soldiers;
    bool armed = false;  // 炮口保护是否已解除（解除前发射者本人免疫本弹）
    double out_mark = -1.0;  // 出界瞬间的 travel（-1 = 当前在图内）；配 kOutGraceSec 计宽限
    double flight_time = 0.0;  // 飞行累计时长（秒）；配 kMaxFlightSec 兜底毒配置的锁死
    bool dead = false;
};

enum class Phase { Input, Flight, GameOver };

enum class EventType {
    TurnBegin,    // 轮到 player_id 输入
    TurnSkipped,  // player_id 超时跳过
    Shot,         // player_id 发射（炮弹见 world.projectile()）
    PlayerHit,    // target_id 被 player_id 的炮弹击中（hp 已扣，pos 为命中处）
    PlayerDied,   // target_id 阵亡
    CraterMade,   // obstacle_index 被炸出坑（pos 处）
    GameOver,     // winner：0=A 队胜 1=B 队胜 2=平局
};

struct Event {
    EventType type = EventType::TurnBegin;
    int player_id = -1;      // 出手方（TurnBegin = 轮到谁；Shot = 谁发射）
    int soldier = 0;         // 出手方的士兵序号
    int target_id = -1;      // 被命中/阵亡的玩家
    int target_soldier = 0;  // 被命中/阵亡的士兵序号
    int obstacle_index = -1;
    Vec2 pos;
    int winner = -1;
    // 仅 PlayerHit：受击士兵的剩余血量（生成事件时已知，省得上层重扫玩家表）
    int hp_left = 0;
};

// 创建玩家用的描述
struct PlayerSpec {
    int id = -1;  // 指定的玩家 id（-1 = 按出现顺序自动分配）；
                  // 房间层用成员 id 显式指定，保证成员 id 与世界内 player id 一致
    std::string name;
    Team team = Team::A;
    bool is_ai = false;
    int ai_difficulty = 1;

    // 自定义构造函数（而非聚合）以兼容两种调用形式：
    // 简单场景只给名字/队伍/人机信息；房间层则显式带上成员 id。
    PlayerSpec() = default;
    PlayerSpec(std::string name_, Team team_, bool is_ai_ = false, int ai_difficulty_ = 1)
        : name(std::move(name_)), team(team_), is_ai(is_ai_), ai_difficulty(ai_difficulty_) {}
    PlayerSpec(int id_, std::string name_, Team team_, bool is_ai_, int ai_difficulty_)
        : id(id_), name(std::move(name_)), team(team_), is_ai(is_ai_), ai_difficulty(ai_difficulty_) {}
};

class World {
public:
    // 生成地图与玩家并进入第一回合
    void start(const GameConfig& cfg, const std::vector<PlayerSpec>& specs);

    // 推进模拟（按固定 dt 调用，比如 1/60 秒）
    void update(double dt);

    // 当前轮到输入的玩家 id；非输入阶段返回 -1
    int current_player_id() const;
    // 当前回合出手的士兵序号；非输入阶段返回 -1
    int current_soldier_index() const;
    // 预览下一个出手的 (player_id, soldier)，没有则 (-1, -1)。与 advance_turn 同一套
    // 轮转规则（含跳过阵亡者），供界面高亮"下一位是谁"
    void peek_next(int& out_player_id, int& out_soldier) const;
    double input_time_left() const { return phase_time_; }

    // 当前玩家提交曲线；返回 ok 的错误结构表示成功
    Error submit(int player_id, const CurveRequest& request);

    // 取走累积的事件（消费）
    std::vector<Event> take_events();

    const GameConfig& config() const { return cfg_; }
    const std::vector<Player>& players() const { return players_; }
    const std::vector<Obstacle>& obstacles() const { return obstacles_; }
    const std::optional<Projectile>& projectile() const { return projectile_; }
    Phase phase() const { return phase_; }
    int winner() const { return winner_; }

private:
    Player* find_player(int id);
    const Player* find_player(int id) const;
    bool in_map(const Vec2& p) const;
    Vec2 find_free_spot(class Rng& rng, Team team, const std::vector<Vec2>& placed);
    bool spot_clear(const Vec2& p, const std::vector<Vec2>& placed) const;
    void advance_turn();
    void step_flight(double dt);
    bool check_game_over();

    GameConfig cfg_;
    std::vector<Player> players_;
    std::vector<Obstacle> obstacles_;
    std::vector<int> turn_order_;  // 轮转顺序（玩家 id 序列）
    int turn_index_ = 0;
    Phase phase_ = Phase::Input;
    double phase_time_ = 0.0;
    double turn_delay_left_ = 0.0;  // 炮弹消失后展示停顿的剩余秒数（>0 时等待再轮下一位）
    std::optional<Projectile> projectile_;
    int next_projectile_id_ = 1;
    int winner_ = -1;
    std::vector<Event> events_;
};

}  // namespace fg
