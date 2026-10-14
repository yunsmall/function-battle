#pragma once
// 房间逻辑：成员管理、开局、AI 驱动、把世界事件翻译成待广播的消息。
// 不依赖任何网络类型（便于单测）；网络层负责把消息真正发出去。

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "ai.h"
#include "config.h"
#include "error.h"
#include "protocol.h"
#include "rng.h"
#include "think_pool.h"
#include "world.h"

namespace fg {

// 成员身份。这是两类不同的成员，不是"玩家的一种状态"：
// 参战者进对局阵容（占参赛位、有队伍、能提交）；观战者只是观众
// （不占参赛位、没有队伍、不能提交，但房间里有他在就不会被服务器回收）。
enum class MemberRole { Player, Spectator };

struct RoomMember {
    int id = 0;  // 与 World 中的 player id 一致（观战者不在 World 里，id 仅用于会话）
    std::string name;
    Team team = Team::A;  // 仅对参战者有意义（观战者没有队伍）
    bool is_ai = false;
    int ai_difficulty = 1;
    MemberRole role = MemberRole::Player;
};

// 待发送消息：to = -1 表示广播给所有成员
struct OutgoingMessage {
    int to = -1;
    json msg;
};

class Room {
public:
    Room(const RoomOptions& room, const MatchConfig& match);

    // 配置分层分别暴露：房间的属性（创建后锁定）与下一把配置模板（房主可改）。
    // 没有"合起来可写"的入口——想改只有 apply_config 一条路，而它只够得着下一把配置
    const RoomOptions& room_options() const { return room_cfg_; }
    const MatchConfig& match_config() const { return match_cfg_; }
    const std::vector<RoomMember>& members() const { return members_; }
    const World& world() const { return world_; }
    bool in_game() const { return in_game_; }
    int host_id() const;  // 第一个人类成员；房主负责加 AI 和开局

    // 房间密码（SHA-256 十六进制，创建时可选设置；明文由客户端本地哈希，传输中不出现）。
    // 状态下行只暴露"有没有密码"，绝不下发哈希本身——否则拿到哈希的人可以直接进房
    void set_password_hash(std::string hash) { password_hash_ = std::move(hash); }
    bool has_password() const { return !password_hash_.empty(); }
    // 校验：无密码房恒过（客户端残留的哈希字段不该拦住无密码房）；有密码房要求完全一致
    bool check_password(const std::string& hash) const {
        return password_hash_.empty() || password_hash_ == hash;
    }

    // 成员管理；返回分配的成员 id（失败返回 -1）。
    // team 为空表示自动平衡到人少的一队（仅参战者参与平衡）。
    // 参战者：对局进行中一律拒绝（阵容调整在局间做）；观战者：随时可以加入，
    // 包括对局进行中（这正是观战的核心场景：进去看当前的局），容量单独限制。
    int add_member(const std::string& name, bool is_ai, int ai_difficulty,
                   std::optional<Team> team = std::nullopt,
                   MemberRole role = MemberRole::Player);
    void remove_member(int id);
    // 成员断线：对局中先记账（对局中不能改阵容），GameOver 时统一移除
    void mark_disconnected(int id);
    bool has_member(int id) const;
    // 某成员是否为观战者。观战者不占阵容：对局中也能离开/被移出（参战者阵容锁定）
    bool is_spectator(int member_id) const;
    // 有没有人类成员：人类全部离开后只剩 AI 的僵尸房间没有归属者，由服务器回收
    bool has_human() const;
    // 两类成员的数量（开局的"至少两名参与者"只看参战者；观战者不占参赛位）
    size_t player_count() const;
    size_t spectator_count() const;

    // 开局（仅房主）；返回 ok 的错误结构表示成功
    Error start_game(int requester);
    // 某成员提交曲线；返回 ok 的错误结构表示成功
    Error submit(int member_id, const CurveRequest& request);

    // 曲线预览采样（网页端用；C++ 客户端共享 core、本地自己算，不用这条）：
    // 资格检查与 submit 相同（轮到你、在输入阶段、非观战者），用当前出手士兵的
    // 位置采样，但不推进世界、不计入提交次数上限——纯粹是"算给客户端看一眼"
    Error preview_curve(int member_id, const CurveRequest& request, Path& out_path);

    // 修改"下一把"的配置（仅房主、仅等待/局间）：先手、血量、输入时限、遮挡物、
    // 停顿、镜像视角、种子（seed 传 null = 每把随机）。
    // 房间的属性（士兵数/坐标系/地图常量）不可修改：补丁里出现即拒绝（cannot_change_room）
    Error apply_config(int requester, const json& config);

    // 房主操作（仅等待/局间，对局进行中一律拒绝）：
    // 调整成员所在队伍 / 把成员移出房间
    Error set_member_team(int requester, int member_id, Team team);
    Error kick_member(int requester, int member_id);
    // 房主添加人机；成功后内部广播新状态
    Error add_ai_member(int requester, const std::string& name, int ai_difficulty,
                        std::optional<Team> team);

    // 切换自己的身份（参战 <-> 观战），仅等待/局间；成功后内部广播新状态。
    // 作用于 requester 自己（观战本来就是对每个人开放的选项）
    Error set_spectate(int requester, bool spectate);

    // 中途进入"进行中"对局的观战者需要一份"当前进行到哪"的快照：
    // 按与正常开局/回合完全相同的消息序列返回（客户端不必有单独的"中途进入"渲染路径）；
    // 不在对局中时返回空列表
    std::vector<json> game_snapshot_msgs() const;

    // 投票跳过当前地图（参战人类一人一票，全员同意则立即重开一张新图；
    // vote=false 撤回自己的票）。观战者与 AI 没有票
    void vote_skip(int member_id, bool vote);

    // 实时函数预览：只转发当前回合玩家正在写的内容（其余丢弃）
    void broadcast_preview(int member_id, const std::string& preview);

    // 推进一帧（由网络层定时器按真实经过时间驱动）
    void tick(double dt);

    // 注入 AI 思考线程池（可空 = 决策同步算，单测与确定性自检用）。
    // 池由上层（服务器/单机客户端）持有并保证生命周期覆盖本房间
    void set_think_pool(ThinkPool* pool) { think_pool_ = pool; }

    std::vector<OutgoingMessage> take_outgoing();

    // 房间状态快照（等待界面用）
    json state_json() const;

private:
    std::string password_hash_;  // 房间密码哈希（SHA-256 十六进制；空 = 无密码；绝不下发）
    void broadcast(json msg);
    void handle_event(const Event& e);
    void drive_ai(double dt);
    // start_game 校验通过后的实际开局流程（跳过投票时也用它直接重开）。
    // force_random_map：跳过地图的强制换图（固定种子房间也换随机新图）
    Error begin_game(bool force_random_map = false);
    // 广播当前投票状态；若参战人类已全员同意则重开新图
    void check_skip_votes();
    RoomMember* find_member(int id);
    // 自动平衡的目标队伍：参战人数少的一队（相同进 A 队）；观战者不参与计数
    Team balance_team() const;
    // 生成房间内不重名的默认名（prefix + 最小空闲序号）：踢掉中间成员后再添加
    // 不会与现存成员重名
    std::string fresh_name(const std::string& prefix) const;
    // 移除成员但不内部广播（调用方自己发状态）；返回是否真的移除了
    //（没找到、或对局中锁定的参战者时返回 false）
    bool remove_member_silent(int id);

    // 房间的属性：创建房间时确定，之后不可更改（修改路径只够得着 match_cfg_，编译期杜绝误改）
    RoomOptions room_cfg_;
    // 下一把的配置模板：房主可改（等待/局间），下一局生效；seed 空 = 每把随机
    MatchConfig match_cfg_;
    std::vector<RoomMember> members_;
    World world_;
    bool in_game_ = false;
    int next_member_id_ = 0;
    std::vector<int> disconnected_;  // 对局期间断线的成员 id，GameOver 后统一清理
    std::vector<OutgoingMessage> outgoing_;
    int tick_phase_ = 0;  // 用于给炮弹位置广播降频
    int round_ = 0;       // 服务器权威回合号（TurnBegin 时递增；快照/广播都带上）
    int submit_tries_ = 0;  // 本输入阶段的提交尝试次数（防连刷让采样重活拖慢全服）
    std::set<int> skip_votes_;  // 同意跳过当前地图的参战人类（每局开始时清空）

    // AI 状态：轮到 AI 时把决策快照投进思考池（可异步），
    // 结果就绪且"思考时间"（从轮到它起算）走完再真正提交
    std::map<int, Rng> ai_rngs_;
    // 一次 AI 思考的共享槽：快照与 rng 副本进池、结果与 rng 终态回传。
    // 用 shared_ptr 共享：房间被回收时任务只是往没人读的槽里写，不产生悬垂
    struct AiThink {
        World world_snapshot;  // 轮到 AI 时世界静止，值拷贝是安全且确定的输入
        Rng rng{1};            // 占位种子：begin_think 会立刻覆盖为该 AI 的确定性随机源
        CurveRequest prev;
        bool has_prev = false;
        int player_id = -1;
        int difficulty = 1;
        AiDecision out;
        std::atomic<bool> done{false};
    };
    struct AiPlan {
        // 思考流水线：Idle → Thinking（快照在池线程里算）→ Ready（等思考时间走完）→ 提交；
        // Failed = 本回合提交失败（确定性原因重投只会白算），弃权等超时，TurnBegin 复位
        enum class Think { Idle, Thinking, Ready, Failed };
        Think state = Think::Idle;
        double elapsed = 0.0;  // 从轮到它起累计的时间
        std::shared_ptr<AiThink> think;
        CurveRequest request;
        double delay = 0.0;
        // 打字机预览：结果就绪后按进度把表达式逐步广播出去（全场看到"正在写："
        // 与人类打字同一观感；表达式是 AI 用 num_text 生成的 ASCII，按字符切安全）
        double ready_at = 0.0;    // 进入 Ready 的时刻（进度起点）
        double preview_at = 0.0;  // 上次广播预览的时刻（节流用）
        size_t preview_sent = 0;  // 已广播的字符数
        // 该 AI 最近一次成功提交的请求：决策时交给 ai_decide 作变异素材
        CurveRequest last_request;
        bool has_last = false;
    };
    std::map<int, AiPlan> ai_plans_;
    ThinkPool* think_pool_ = nullptr;
    // 发起一次思考：拷快照与 rng 交给池（无池则当场算完），转入 Thinking
    void begin_think(int cur, const RoomMember& member, AiPlan& plan);
};

}  // namespace fg
