#include "room.h"

#include <algorithm>
#include <random>

namespace fg {

namespace {
// 两类成员各自的名额：参赛位与观战位互不挤占
constexpr size_t kMaxPlayers = 10;
constexpr size_t kMaxSpectators = 20;
// 名字字节上限（约 21 个汉字）：名字会进广播并被客户端逐字载入字形，不限长的话
// 超长互异汉字串会撑爆受害客户端的字体图集，也白白放大广播带宽
constexpr size_t kMaxNameBytes = 64;

// 每输入阶段允许的提交尝试上限（含被拒的）：曲线采样是服务器 io 线程上的同步重活
// （最坏曲线单次约 3ms），不设限时一次连刷就能把全服务器的帧拖慢。
// 正常玩家有本地预览、一两次就能成功，16 次足够宽容
constexpr int kMaxSubmitTries = 16;

// 按 UTF-8 字符边界截断（不切开多字节字符，回退到上一个字符起点）
std::string truncate_utf8(const std::string& s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s;
    size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
    return s.substr(0, end);
}

// 把"下一把配置模板"解析成本局实际配置：seed 为空（每把随机）时用系统熵生成
//（不消耗世界内部的确定性随机流）。force_random_seed 供"跳过地图"用：换一张新图
// 的意图优先于固定种子的复现意图——只影响这一把，下一把仍按模板
ResolvedMatchConfig resolve_match(const MatchConfig& m, bool force_random_seed = false) {
    ResolvedMatchConfig r;
    if (m.seed && !force_random_seed) {
        r.seed = *m.seed;
    } else {
        std::random_device rd;
        r.seed = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
    }
    r.hp = m.hp;
    r.obstacle_count = m.obstacle_count;
    r.input_seconds = m.input_seconds;
    r.turn_delay = m.turn_delay;
    r.mirror_view = m.mirror_view;
    r.first_team = m.first_team;
    return r;
}
}  // namespace

Room::Room(const RoomOptions& room, const MatchConfig& match)
    : room_cfg_(room), match_cfg_(match) {}

int Room::host_id() const {
    for (const auto& m : members_) {
        if (!m.is_ai) return m.id;
    }
    return members_.empty() ? -1 : members_[0].id;
}

int Room::add_member(const std::string& name, bool is_ai, int ai_difficulty,
                     std::optional<Team> team, MemberRole role) {
    // 观战者与参战者是两类成员：容量、局中限制、队伍语义都各算各的
    if (role == MemberRole::Player) {
        if (in_game_) return -1;  // 阵容调整只在等待/局间进行
        if (player_count() >= kMaxPlayers) return -1;
    } else {
        if (is_ai) return -1;  // 人机不观战（AI 只会参战）
        if (spectator_count() >= kMaxSpectators) return -1;
    }

    // 队伍：指定优先，否则平衡到人少的一队；观战者不参与分队
    Team chosen = Team::A;
    if (role == MemberRole::Player) {
        chosen = team ? *team : balance_team();
    }

    RoomMember m;
    m.id = next_member_id_++;
    if (!name.empty()) {
        m.name = truncate_utf8(name, kMaxNameBytes);
    } else if (role == MemberRole::Spectator) {
        m.name = fresh_name("观战者");
    } else if (is_ai) {
        // 人机默认名按队内序号（与单机的 AI-A1 / AI-B1 风格一致）；
        // 用成员 id 编号会因观战者/玩家占号而出现 AI2、AI4 这类跳号
        m.name = fresh_name(std::string("AI-") + (chosen == Team::A ? "A" : "B"));
    } else {
        m.name = "玩家" + std::to_string(m.id + 1);
    }
    m.is_ai = is_ai;
    m.ai_difficulty = ai_difficulty;
    m.team = chosen;
    m.role = role;
    members_.push_back(m);
    return m.id;
}

Error Room::set_member_team(int requester, int member_id, Team team) {
    if (in_game_) return {"in_game_team_change", "对局进行中不能调整队伍"};
    if (requester != host_id()) return {"not_host_team_change", "只有房主可以调整队伍"};
    RoomMember* m = find_member(member_id);
    if (!m) return {"member_not_found", "成员不存在"};
    if (m->role != MemberRole::Player) return {"member_is_spectator", "观战者没有队伍"};
    m->team = team;
    broadcast(state_json());
    return {};
}

Error Room::set_spectate(int requester, bool spectate) {
    if (in_game_) return {"in_game_spectate", "对局进行中不能切换身份"};
    RoomMember* m = find_member(requester);
    if (!m) return {"member_not_found", "成员不存在"};
    // 两个方向都要过与 add_member 相同的容量关：转参战还要做自动分队平衡。
    // 绕过去会出现超员开局（出生点回退到同一格重叠）、转进来的人全堆一队
    //（观战期间没有队伍可计入平衡）或观战席超员
    if (spectate && m->role == MemberRole::Player) {
        if (spectator_count() >= kMaxSpectators) return {"spectator_full", "观战席位已满"};
    } else if (!spectate && m->role == MemberRole::Spectator) {
        if (player_count() >= kMaxPlayers) return {"room_full", "参赛席位已满"};
        m->team = balance_team();
    }
    m->role = spectate ? MemberRole::Spectator : MemberRole::Player;
    broadcast(state_json());
    return {};
}

Error Room::kick_member(int requester, int member_id) {
    if (requester != host_id()) return {"not_host_kick", "只有房主可以移除成员"};
    if (member_id == requester) return {"cannot_kick_self", "不能移除自己"};
    const RoomMember* m = find_member(member_id);
    if (!m) return {"member_not_found", "成员不存在"};
    // 对局中只能踢观战者：参战者阵容锁定（断线参战者由 disconnected_ 记账、局末清理）
    if (in_game_ && m->role != MemberRole::Spectator)
        return {"in_game_kick", "对局进行中不能移除参战者"};
    remove_member(member_id);
    // 被踢者的通知与"回到大厅"由网络层负责（Room 不认识连接）
    return {};
}

Error Room::add_ai_member(int requester, const std::string& name, int ai_difficulty,
                          std::optional<Team> team) {
    if (in_game_) return {"in_game_add_ai", "对局进行中不能添加人机"};
    if (requester != host_id()) return {"not_host_add_ai", "只有房主可以添加人机"};
    // 难度来自客户端 JSON，夹到合法档位：越界值会被 ai_profile 悄悄按普通处理，
    // 而界面上按 clamp 后显示，展示与实际行为会不一致
    const int diff = std::clamp(ai_difficulty, 0, kAiDifficultyCount - 1);
    if (add_member(name, true, diff, team) < 0) return {"room_full", "房间已满"};
    broadcast(state_json());
    return {};
}

void Room::remove_member(int id) {
    if (remove_member_silent(id)) broadcast(state_json());
}

bool Room::remove_member_silent(int id) {
    // 对局中只允许移除观战者：参战者阵容锁定（断线参战者由 disconnected_ 记账、局末清理）
    const RoomMember* m = find_member(id);
    if (!m) return false;
    if (in_game_ && m->role != MemberRole::Spectator) return false;
    const size_t before = members_.size();
    members_.erase(std::remove_if(members_.begin(), members_.end(),
                                  [id](const RoomMember& m) { return m.id == id; }),
                   members_.end());
    if (members_.size() == before) return false;
    // 被移除者的旧票作废：否则之后广播的投票状态里会挂着已不在房间的 id
    skip_votes_.erase(id);
    return true;
}

void Room::mark_disconnected(int id) {
    // 已不在成员表（如被踢/离开后连接才断）就无事可做，避免往 disconnected_
    // 里记幽灵 id
    if (!has_member(id)) return;
    // 参战者对局中只能记账（阵容锁定）；观战者不占阵容，随时移除
    if (in_game_ && !is_spectator(id)) {
        disconnected_.push_back(id);
    } else {
        remove_member(id);
    }
}

bool Room::is_spectator(int member_id) const {
    for (const auto& m : members_) {
        if (m.id == member_id) return m.role == MemberRole::Spectator;
    }
    return false;
}

bool Room::has_member(int id) const {
    for (const auto& m : members_) {
        if (m.id == id) return true;
    }
    return false;
}

bool Room::has_human() const {
    for (const auto& m : members_) {
        if (m.is_ai) continue;
        // 对局中已断线记账的参战者不算数：没有重连机制，他不会回来。
        // 否则两名人类对局全部断线后，谁也无法推进、永远打不完（纯人类局连
        // AI 都没有），房间被一场僵尸对局永久占住
        if (in_game_ && std::find(disconnected_.begin(), disconnected_.end(), m.id) !=
                            disconnected_.end()) {
            continue;
        }
        return true;  // 观战者也是人类：房间有观战者就不会被回收
    }
    return false;
}

size_t Room::player_count() const {
    size_t n = 0;
    for (const auto& m : members_) {
        if (m.role == MemberRole::Player) ++n;
    }
    return n;
}

size_t Room::spectator_count() const { return members_.size() - player_count(); }

RoomMember* Room::find_member(int id) {
    for (auto& m : members_) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

Team Room::balance_team() const {
    int count_a = 0, count_b = 0;
    for (const auto& x : members_) {
        if (x.role != MemberRole::Player) continue;  // 观战者不参与平衡
        (x.team == Team::A ? count_a : count_b)++;
    }
    return (count_a <= count_b) ? Team::A : Team::B;  // 相同进 A 队
}

std::string Room::fresh_name(const std::string& prefix) const {
    for (size_t n = 1;; ++n) {
        std::string cand = prefix + std::to_string(n);
        bool used = false;
        for (const auto& x : members_) {
            if (x.name == cand) {
                used = true;
                break;
            }
        }
        if (!used) return cand;
    }
}

Error Room::apply_config(int requester, const json& config) {
    if (in_game_) return {"in_game_config", "对局进行中不能修改配置"};
    if (requester != host_id()) return {"not_host_config", "只有房主可以修改配置"};
    // 房间的属性与世界常量创建时定死，没有任何修改途径：
    // 补丁里出现这些字段一律拒绝（明确报错而不是静默忽略，客户端才知道自己发了什么）
    for (auto it = config.begin(); it != config.end(); ++it) {
        if (is_room_config_key(it.key())) {
            return {"cannot_change_room",
                    "房间的属性（含地图等常量）在创建时确定，之后不可更改（不能修改：" +
                        it.key() + "）"};
        }
    }
    // 合并补丁语义：先在副本上改、全部校验通过后才整体生效（中途失败不留半更新状态）
    MatchConfig tmp = match_cfg_;
    const Error err = apply_match_config_json(config, tmp);
    if (!err.ok()) return err;
    // 遮挡物数量与地图的密度交叉校验（地图是世界常量、不可变，按当前值算）
    const Error derr = check_obstacle_density(kDefaultWorld, tmp);
    if (!derr.ok()) return derr;
    match_cfg_ = tmp;
    broadcast(state_json());
    return {};
}

Error Room::start_game(int requester) {
    if (in_game_) return {"already_started", "游戏正在进行中"};
    if (requester != host_id()) return {"not_host_start", "只有房主可以开始游戏"};
    return begin_game();
}

Error Room::begin_game(bool force_random_map) {
    // 只看参战者：观战者再多也不构成一局（"纯 AI 观战房"也必须两边都有 AI）
    size_t players = 0;
    size_t a_count = 0, b_count = 0;
    for (const auto& m : members_) {
        if (m.role != MemberRole::Player) continue;
        ++players;
        ++(m.team == Team::A ? a_count : b_count);
    }
    if (players < 2) return {"need_two_players", "至少需要两名参与者（可以添加人机）"};
    if (a_count == 0 || b_count == 0) return {"need_both_teams", "两队都需要至少一名参与者"};
    // 两队人数（= 士兵数，每人士兵数全场统一）必须相等：多打少的对局不公平，
    // 通常在手动换队后出现，属于误操作而非玩法
    if (a_count != b_count) {
        return {"need_equal_teams", "两队人数需要相等（换队调整后可再开局）"};
    }

    // 解析本局实际配置：seed 为"每把随机"（空）时在这里生成实际种子，本局内不再变化；
    // force_random_map（跳过地图重开）时固定种子也强制换成随机新种子
    const ResolvedMatchConfig resolved = resolve_match(match_cfg_, force_random_map);

    std::vector<PlayerSpec> specs;
    specs.reserve(members_.size());
    for (const auto& m : members_) {
        if (m.role != MemberRole::Player) continue;  // 观战者不进对局阵容
        // 显式带上成员 id：保证世界内的 player id 与房间成员 id 始终一致
        specs.push_back({m.id, m.name, m.team, m.is_ai, m.ai_difficulty});
    }
    world_.start(GameConfig{room_cfg_, kDefaultWorld, resolved}, specs);
    in_game_ = true;
    round_ = 0;
    submit_tries_ = 0;
    tick_phase_ = 0;
    skip_votes_.clear();  // 新一局票重新算
    ai_plans_.clear();
    ai_rngs_.clear();
    for (const auto& m : members_) {
        if (m.is_ai) {
            // 每个 AI 独立的确定性随机源；不同 id 得到不同序列
            ai_rngs_.emplace(m.id,
                             Rng(resolved.seed * 1000ULL + static_cast<uint64_t>(m.id) * 17ULL + 1ULL));
        }
    }

    broadcast(
        msg::make_game_start(room_cfg_, kDefaultWorld, resolved, world_.players(), world_.obstacles()));

    // 处理 world_.start 产生的首个 TurnBegin
    for (const auto& e : world_.take_events()) handle_event(e);
    return {};
}

Error Room::submit(int member_id, const CurveRequest& request) {
    if (!in_game_) return {"game_not_started", "游戏尚未开始"};
    // 观战者不在 World 里，直接给出明确的拒绝原因（而不是让 World 报"玩家不存在"）
    const RoomMember* m = find_member(member_id);
    if (m && m->role != MemberRole::Player) return {"spectator_cannot_submit", "观战者不能提交"};
    // 只有当前回合玩家自己的尝试计入上限：观战者或旁观参战者刷 submit（含恶意连接）
    // 不能把当前回合玩家的额度消耗光，否则一局会被卡到超时
    if (world_.current_player_id() == member_id && ++submit_tries_ > kMaxSubmitTries) {
        return {"submit_too_many", "本回合提交次数过多"};
    }
    return world_.submit(member_id, request);
}

Error Room::preview_curve(int member_id, const CurveRequest& request, Path& out_path) {
    if (!in_game_) return {"game_not_started", "游戏尚未开始"};
    const RoomMember* m = find_member(member_id);
    if (m && m->role != MemberRole::Player) return {"spectator_cannot_submit", "观战者不能提交"};
    if (world_.phase() != Phase::Input) return {"not_input_phase", "当前不是输入阶段"};
    if (world_.current_player_id() != member_id) return {"not_your_turn", "还没轮到你"};
    const Player* p = nullptr;
    for (const Player& pl : world_.players()) {
        if (pl.id == member_id) p = &pl;
    }
    if (!p || !p->any_alive()) return {"player_invalid", "玩家不存在或已阵亡"};
    // 起点取"下一发会出手的士兵"（current_soldier_index 即当前玩家的出手游标），
    // 与 World::submit 完全一致：预览线必须和真正打出去的那条重合（前段）
    const int si = world_.current_soldier_index();
    // 只取起点起 kPreviewTravel 的一段：预览用于确认起步形态，整条弹道提前全画
    // 出来等于剧透能否命中；真正打出去的轨迹不受影响
    GameConfig preview_cfg = world_.config();
    preview_cfg.world.max_travel = kPreviewTravel;
    CurveResult res =
        sample_curve(request, p->soldiers[static_cast<size_t>(si)].pos, preview_cfg);
    if (!res.ok()) return res.error;
    out_path = std::move(res.path);
    return {};
}

std::vector<json> Room::game_snapshot_msgs() const {
    std::vector<json> out;
    if (!in_game_) return out;
    // 复用正常对局的消息序列：客户端收到后走与开局完全相同的渲染路径
    // 快照的 game_start 直接带当前回合号：飞行阶段进入的观战者没有 turn_begin
    // 可依托，回合显示靠它初始化
    out.push_back(msg::make_game_start(room_cfg_, kDefaultWorld, world_.config().match,
                                       world_.players(), world_.obstacles(), round_));
    if (world_.phase() == Phase::Input) {
        // input_seconds 传"剩余时间"（客户端按收到消息时刻起算倒计时，与开局语义一致）；
        // round 带当前回合号，中途观战者的回合显示才能与真实进度对齐
        int np = -1, ns = 0;
        world_.peek_next(np, ns);
        out.push_back(msg::make_turn_begin(world_.current_player_id(),
                                           world_.current_soldier_index(),
                                           world_.input_time_left(), round_, np, ns));
    } else if (world_.phase() == Phase::Flight && world_.projectile()) {
        const Projectile& p = *world_.projectile();
        out.push_back(msg::make_shot(p.id, p.owner_id, p.owner_soldier, p.request, p.speed, p.path));
        std::vector<msg::ProjectileState> list;
        list.push_back({p.id, p.path.at(p.travel), p.travel, p.path.length()});
        out.push_back(msg::make_projectiles(list));
    }
    return out;
}

void Room::vote_skip(int member_id, bool vote) {
    if (!in_game_) return;
    const RoomMember* m = find_member(member_id);
    if (m == nullptr || m->is_ai || m->role != MemberRole::Player) return;  // 只有参战人类有票
    if (vote) {
        skip_votes_.insert(member_id);
    } else {
        skip_votes_.erase(member_id);
    }
    check_skip_votes();
}

void Room::check_skip_votes() {
    // 参战人类全集（投票的分母）。对局中断线的参战者仍在成员表里（阵容锁定、局末才清理），
    // 但他已无法投票——不计入全集，否则剩下的人永远凑不齐全票、跳图功能被卡死
    std::vector<int> humans;
    for (const auto& m : members_) {
        if (m.role != MemberRole::Player || m.is_ai) continue;
        if (std::find(disconnected_.begin(), disconnected_.end(), m.id) != disconnected_.end())
            continue;
        humans.push_back(m.id);
    }
    if (humans.empty()) return;
    // 广播只列仍有票的人：断线者在断线前投的旧票不再代表任何人
    std::vector<int> voted;
    for (int id : skip_votes_) {
        if (std::find(humans.begin(), humans.end(), id) != humans.end()) voted.push_back(id);
    }
    for (int id : humans) {
        if (skip_votes_.count(id) == 0) {
            broadcast(msg::make_skip_vote(voted, static_cast<int>(humans.size())));
            return;
        }
    }
    // 全员同意：立即重开新图（begin_game 会清空票并广播新的开局消息）。
    // 强制换随机新图：固定种子的房间"跳过地图"也必须真的换一张图，否则重开=同图、
    // 玩家点了个没反应（下一把开局仍按模板的固定种子）
    begin_game(/*force_random_map=*/true);
}

void Room::broadcast_preview(int member_id, const std::string& preview) {
    if (!in_game_ || world_.phase() != Phase::Input) return;
    if (world_.current_player_id() != member_id) return;
    broadcast(msg::make_function_preview(member_id, preview));
}

void Room::begin_think(int cur, const RoomMember& member, AiPlan& plan) {
    auto it = ai_rngs_.find(cur);
    if (it == ai_rngs_.end()) {
        it = ai_rngs_
                 .emplace(cur, Rng(world_.config().match.seed * 1000ULL +
                                   static_cast<uint64_t>(cur) * 17ULL + 1ULL))
                 .first;
    }
    // 决策跑在"世界快照 + rng 副本"上（异步时在思考池线程里）：轮到 AI 时世界静止，
    // 值拷贝是安全且确定的输入；结果与 rng 终态写回共享槽，主线程在 tick 里取
    plan.think = std::make_shared<AiThink>();
    plan.think->world_snapshot = world_;
    plan.think->rng = it->second;
    plan.think->player_id = cur;
    plan.think->difficulty = member.ai_difficulty;
    if (plan.has_last) {
        plan.think->prev = plan.last_request;
        plan.think->has_prev = true;
    }
    auto think = plan.think;
    auto run = [think] {
        try {
            think->out = ai_decide(think->world_snapshot, think->player_id, think->difficulty,
                                   think->rng, think->has_prev ? &think->prev : nullptr);
        } catch (...) {
            // 决策不应抛异常；万一抛了也要置位完成，不能让房间永远停在思考中
            think->out = AiDecision{};
        }
        think->done.store(true, std::memory_order_release);
    };
    if (think_pool_) {
        think_pool_->submit(std::move(run));
    } else {
        run();  // 无池：当场算完（单测/确定性自检的同步路径）
    }
    plan.request = CurveRequest{};
    plan.delay = 0.0;
    plan.elapsed = 0.0;
    plan.preview_sent = 0;
    plan.preview_at = 0.0;
    plan.state = AiPlan::Think::Thinking;
}

void Room::drive_ai(double dt) {
    if (world_.phase() != Phase::Input) return;
    const int cur = world_.current_player_id();
    if (cur < 0) return;
    const RoomMember* m = find_member(cur);
    if (!m || !m->is_ai) return;

    AiPlan& plan = ai_plans_[cur];
    // 本回合已放弃（提交失败）：什么都不做，等超时跳过或下回合 TurnBegin 复位
    if (plan.state == AiPlan::Think::Failed) return;
    // 发起：快照投池，思考时间从这一刻起算（计算耗时也计入思考，节奏是"真在算"）
    if (plan.state == AiPlan::Think::Idle) {
        begin_think(cur, *m, plan);
        return;
    }
    plan.elapsed += dt;

    if (plan.state == AiPlan::Think::Thinking) {
        if (!plan.think->done.load(std::memory_order_acquire)) return;
        // 结果就绪：写回 rng 终态（消费顺序因此与决策过程严格一致，同种子可复现）
        auto it = ai_rngs_.find(cur);
        if (it != ai_rngs_.end()) it->second = plan.think->rng;
        plan.request = std::move(plan.think->out.request);
        plan.delay = plan.think->out.delay;
        plan.think.reset();
        plan.state = AiPlan::Think::Ready;
        plan.ready_at = plan.elapsed;  // 打字预览从"结果就绪"这一刻起按进度播出
    }

    if (plan.state == AiPlan::Think::Ready) {
        // 打字机预览：趁等思考时间走完的空档，把表达式按进度逐步广播给全场。
        // 节流 0.25 秒/次防刷屏；最后一段必达（哪怕刚好赶上提交的那一 tick）
        const std::string& full = plan.request.expr_a;
        if (plan.preview_sent < full.size()) {
            const double span = std::max(0.05, plan.delay - plan.ready_at);
            const double prog = std::clamp((plan.elapsed - plan.ready_at) / span, 0.0, 1.0);
            const size_t want = static_cast<size_t>(prog * static_cast<double>(full.size()));
            if (want > plan.preview_sent &&
                (plan.elapsed - plan.preview_at >= 0.25 || want >= full.size())) {
                plan.preview_at = plan.elapsed;
                plan.preview_sent = want;
                // 走 broadcast_preview：与人类预览共享"仅当前回合玩家"同一道约束
                broadcast_preview(cur, full.substr(0, want));
            }
        }
    }

    // 思考时间走完才提交：人类看到的是"AI 想了一会儿"而不是瞬间回敬。
    // 输入时限逼近时兜底立即提交：多房间在池里排队时计算可能被挤后，
    // 不能因此压线超时弃权。
    // 走到这里 state 必为 Ready：Thinking 未完成时上面已 return、Failed 早就返回，
    // plan.request 一定是真决策的表达式——不存在"拿空请求碰运气"的路径
    if (plan.elapsed >= plan.delay || world_.input_time_left() <= 0.5) {
        // AI 用真实表达式走和人类完全一样的提交与校验路径
        const Error err = world_.submit(cur, plan.request);
        if (err.ok()) {
            plan.last_request = plan.request;
            plan.has_last = true;
            plan.state = AiPlan::Think::Idle;
        } else {
            // 失败若是确定性原因（如兜底空请求），重投只会白算：放弃本回合等超时兜底
            plan.state = AiPlan::Think::Failed;
        }
    }
}

void Room::tick(double dt) {
    if (!in_game_) return;

    drive_ai(dt);
    world_.update(dt);
    for (const auto& e : world_.take_events()) handle_event(e);

    // 飞行阶段：降频广播炮弹位置（客户端做插值渲染）
    if (world_.phase() == Phase::Flight && world_.projectile()) {
        if (++tick_phase_ % 3 == 0) {
            const Projectile& p = *world_.projectile();
            std::vector<msg::ProjectileState> list;
            list.push_back({p.id, p.path.at(p.travel), p.travel, p.path.length()});
            broadcast(msg::make_projectiles(list));
        }
    } else {
        tick_phase_ = 0;
    }
}

void Room::handle_event(const Event& e) {
    switch (e.type) {
        case EventType::TurnBegin: {
            ++round_;
            submit_tries_ = 0;  // 新的输入阶段：尝试次数重新计
            // 新回合开始：清掉该玩家可能残留的思考流水线（上一回合超时跳过等异常路径），
            // 防止把旧快照的结果当成新回合的决策
            auto pit = ai_plans_.find(e.player_id);
            if (pit != ai_plans_.end()) {
                pit->second.state = AiPlan::Think::Idle;
                pit->second.think.reset();
                pit->second.elapsed = 0.0;
            }
            int np = -1, ns = 0;
            world_.peek_next(np, ns);  // "再下一位出手者"（界面高亮用；当前这位已由 player_id 指明）
            broadcast(msg::make_turn_begin(e.player_id, e.soldier,
                                           world_.config().match.input_seconds, round_, np, ns));
            break;
        }
        case EventType::TurnSkipped: {
            // 被跳过者的思考流水线立即作废：任务结果不再有意义。TurnBegin 还会再清一遍，
            // 但那要等到他下次出手；在此期间旧槽白占内存
            auto pit = ai_plans_.find(e.player_id);
            if (pit != ai_plans_.end()) {
                pit->second.state = AiPlan::Think::Idle;
                pit->second.think.reset();
                pit->second.elapsed = 0.0;
            }
            broadcast(msg::make_turn_skipped(e.player_id));
            break;
        }
        case EventType::Shot:
            if (world_.projectile()) {
                const Projectile& p = *world_.projectile();
                broadcast(msg::make_shot(p.id, p.owner_id, p.owner_soldier, p.request, p.speed, p.path));
            }
            break;
        case EventType::PlayerHit:
            // 剩余血量由世界在生成事件时带出，不必重扫玩家表
            broadcast(msg::make_hit(e.target_id, e.target_soldier, e.player_id, e.hp_left, e.pos));
            break;
        case EventType::PlayerDied:
            broadcast(msg::make_death(e.target_id, e.target_soldier, e.player_id));
            break;
        case EventType::CraterMade:
            broadcast(msg::make_crater(e.obstacle_index, e.pos));
            break;
        case EventType::GameOver:
            in_game_ = false;
            // 对局期间断线的成员现在统一移除，否则房间会被再也踢不掉的幽灵成员永久占住。
            // 用不广播的变体：紧接着的状态广播已包含最终成员表，逐次广播是冗余消息
            for (int id : disconnected_) remove_member_silent(id);
            disconnected_.clear();
            broadcast(msg::make_game_over(e.winner));
            // 对局结束回到"局间"状态：无条件推一份房间状态，客户端据此回到准备界面。
            // 不能省成"仅依赖移除成员的广播"：断线列表为空时会一份都不发
            //（客户端卡在结算界面），这里无条件发一份才稳
            broadcast(state_json());
            break;
    }
}

std::vector<OutgoingMessage> Room::take_outgoing() {
    std::vector<OutgoingMessage> out = std::move(outgoing_);
    outgoing_.clear();
    return out;
}

void Room::broadcast(json msg) { outgoing_.push_back({-1, std::move(msg)}); }

json Room::state_json() const {
    json members = json::array();
    for (const auto& m : members_) {
        members.push_back(msg::make_room_member(m.id, m.name, m.is_ai, m.ai_difficulty,
                                                m.team == Team::A ? "A" : "B",
                                                m.role == MemberRole::Player ? "player"
                                                                             : "spectator"));
    }
    json state = msg::make_room_state(room_cfg_, kDefaultWorld, match_cfg_, host_id(), in_game_,
                                      std::move(members));
    // 密码只报"有没有"：哈希本身绝不能下发，否则拿到的人能直接进房
    state["has_password"] = has_password();
    return state;
}

}  // namespace fg
