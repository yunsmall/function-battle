// Room 的成员身份测试：参战者与观战者是两类不同的成员（阵容参与、容量、
// 局中限制、队伍语义、提交权限都各算各的）
#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <string>
#include <thread>

#include "protocol.h"
#include "room.h"

namespace {

fg::GameConfig room_cfg() {
    fg::GameConfig cfg;
    cfg.match.seed = 42;
    cfg.match.obstacle_count = 0;  // 排除地图生成对断言数量的干扰
    cfg.match.first_team = 0;      // 固定 A 先手，顺序可预测
    return cfg;
}

// 从一份完整配置建房间（与单机/自检同款：本局配置 → 模板）
fg::Room make_room(const fg::GameConfig& cfg) {
    return fg::Room(cfg.room, fg::match_config_from_resolved(cfg.match));
}

}  // namespace

TEST(Room, SpectatorNotInWorld) {
    fg::Room room = make_room(room_cfg());
    room.add_member("房主", false, 0, fg::Team::A);
    room.add_member("AI-B", true, 1, fg::Team::B);
    const int sid = room.add_member("观众", false, 0, std::nullopt, fg::MemberRole::Spectator);
    ASSERT_GE(sid, 0);

    ASSERT_TRUE(room.start_game(room.host_id()).ok());
    EXPECT_EQ(room.world().players().size(), 2u);  // 观战者不进对局阵容
    for (const auto& p : room.world().players()) {
        EXPECT_NE(p.id, sid);
    }
}

TEST(Room, SpectatorsDoNotCountForStart) {
    fg::Room room = make_room(room_cfg());
    room.add_member("房主", false, 0, fg::Team::A);  // 只有 1 名参战者
    room.add_member("观众", false, 0, std::nullopt, fg::MemberRole::Spectator);
    // 观战者再多也凑不成一局
    EXPECT_EQ(room.start_game(room.host_id()).code, "need_two_players");
}

TEST(Room, SpectatorCanJoinInGameButPlayerCannot) {
    fg::Room room = make_room(room_cfg());
    room.add_member("房主", false, 0, fg::Team::A);
    room.add_member("AI-B", true, 1, fg::Team::B);
    ASSERT_TRUE(room.start_game(room.host_id()).ok());

    // 对局中：参战者进不来（阵容锁定），观战者随时可以进（观战的核心场景）
    EXPECT_LT(room.add_member("晚到玩家", false, 0, std::nullopt, fg::MemberRole::Player), 0);
    EXPECT_GE(room.add_member("晚到观众", false, 0, std::nullopt, fg::MemberRole::Spectator), 0);
}

TEST(Room, SpectatorCannotSubmit) {
    fg::Room room = make_room(room_cfg());
    room.add_member("房主", false, 0, fg::Team::A);
    room.add_member("AI-B", true, 1, fg::Team::B);
    const int sid = room.add_member("观众", false, 0, std::nullopt, fg::MemberRole::Spectator);
    ASSERT_TRUE(room.start_game(room.host_id()).ok());

    fg::CurveRequest req;  // 内容无所谓：身份检查发生在采样之前
    req.expr_a = "x";
    EXPECT_EQ(room.submit(sid, req).code, "spectator_cannot_submit");
}

TEST(Room, PreviewCurveSamplesWithoutAdvancing) {
    fg::Room room = make_room(room_cfg());
    const int a = room.add_member("房主", false, 0, fg::Team::A);
    room.add_member("AI-B", true, 1, fg::Team::B);  // id = 1
    const int sid = room.add_member("观众", false, 0, std::nullopt, fg::MemberRole::Spectator);
    ASSERT_TRUE(room.start_game(room.host_id()).ok());

    fg::CurveRequest req;
    req.expr_a = "0.1*x - 3";

    // 资格同 submit：观战者 / 非当前回合者都不行；坏表达式给采样错误
    fg::Path path;
    EXPECT_EQ(room.preview_curve(sid, req, path).code, "spectator_cannot_submit");
    EXPECT_EQ(room.preview_curve(1, req, path).code, "not_your_turn");
    fg::CurveRequest bad;
    EXPECT_EQ(room.preview_curve(a, bad, path).code, "expr_empty");

    // 预览不推进对局：同一请求随后照常提交成功；预览只是实弹轨迹的前缀段
    //（只取起点起 kPreviewTravel，防剧透），与前缀逐点重合
    ASSERT_TRUE(room.preview_curve(a, req, path).ok());
    ASSERT_TRUE(path.valid());
    EXPECT_LE(path.length(), fg::kPreviewTravel + 1.0) << "预览应截到起点起一小段";
    (void)room.take_outgoing();  // 清掉开局广播，下面只找 shot
    ASSERT_TRUE(room.submit(a, req).ok());
    room.tick(1.0 / 60.0);  // 世界的 Shot 事件在下一次 tick 里才转成广播
    bool found = false;
    for (const auto& out : room.take_outgoing()) {
        if (fg::json_str(out.msg, "type") != fg::msg_type::kShot) continue;
        found = true;
        const auto& trail = out.msg["trail"];
        ASSERT_GE(trail.size(), 2u);
        // 完整弹道（此处点数未触发抽稀）以预览为前缀：逐点核对
        ASSERT_LE(path.points.size(), trail.size());
        for (size_t i = 0; i < path.points.size(); ++i) {
            EXPECT_NEAR(trail[i][0].get<double>(), path.points[i].x(), 1e-3);
            EXPECT_NEAR(trail[i][1].get<double>(), path.points[i].y(), 1e-3);
        }
    }
    EXPECT_TRUE(found);
}

TEST(Room, SetSpectateOnlyWhenWaiting) {
    fg::Room room = make_room(room_cfg());
    const int hid = room.add_member("房主", false, 0, fg::Team::A);
    room.add_member("AI-B", true, 1, fg::Team::B);

    // 等待/局间：可自由切换，数量即时变化
    ASSERT_TRUE(room.set_spectate(hid, true).ok());
    EXPECT_EQ(room.player_count(), 1u);
    EXPECT_EQ(room.spectator_count(), 1u);
    ASSERT_TRUE(room.set_spectate(hid, false).ok());
    EXPECT_EQ(room.player_count(), 2u);

    // 对局中：切换被拒（阵容锁定）
    ASSERT_TRUE(room.start_game(room.host_id()).ok());
    EXPECT_EQ(room.set_spectate(hid, true).code, "in_game_spectate");
}

TEST(Room, SpectatorHasNoTeam) {
    fg::Room room = make_room(room_cfg());
    room.add_member("房主", false, 0, fg::Team::A);
    const int sid = room.add_member("观众", false, 0, std::nullopt, fg::MemberRole::Spectator);
    EXPECT_EQ(room.set_member_team(room.host_id(), sid, fg::Team::B).code, "member_is_spectator");
}

TEST(Room, SnapshotCoversLateSpectator) {
    fg::Room room = make_room(room_cfg());
    room.add_member("房主", false, 0, fg::Team::A);
    room.add_member("AI-B", true, 1, fg::Team::B);

    // 不在对局中：没有快照可发
    EXPECT_TRUE(room.game_snapshot_msgs().empty());

    ASSERT_TRUE(room.start_game(room.host_id()).ok());
    // 开局后处于输入阶段：快照应含 game_start + turn_begin，
    // 且 turn_begin 报的是"剩余时间"（客户端据此从当前进度接着倒计）
    const auto snap = room.game_snapshot_msgs();
    ASSERT_EQ(snap.size(), 2u);
    EXPECT_EQ(fg::json_str(snap[0], "type"), std::string(fg::msg_type::kGameStart));
    // game_start 也带当前回合号（飞行阶段进入的观战者没有 turn_begin 可依托）
    EXPECT_EQ(fg::json_int(snap[0], "round", 0), 1);
    EXPECT_EQ(fg::json_str(snap[1], "type"), std::string(fg::msg_type::kTurnBegin));
    EXPECT_EQ(fg::json_int(snap[1], "player_id", -1), 0);
    EXPECT_GT(fg::json_double(snap[1], "input_seconds", 0.0), 0.0);
    // 快照带服务器权威回合号：中途观战者据此对齐回合显示，而不是从 0 数起
    EXPECT_EQ(fg::json_int(snap[1], "round", 0), 1);
}

TEST(Room, StartRequiresEqualTeamSizes) {
    fg::Room room = make_room(room_cfg());
    const int h = room.add_member("甲", false, 0, fg::Team::A);
    room.add_member("乙", false, 0, fg::Team::B);
    room.add_member("丙", false, 0, fg::Team::B);  // 1v2
    // 多打少被拒（手动换队很容易造成这种阵容）
    EXPECT_EQ(room.start_game(h).code, "need_equal_teams");
    // 再进一人：自动平衡到人少的 A 队（2v2）后可以开局
    ASSERT_GE(room.add_member("丁", false, 0), 0);
    EXPECT_TRUE(room.start_game(h).ok());
}

TEST(Room, SpectatorCanLeaveMidGameButPlayerCannot) {
    fg::Room room = make_room(room_cfg());
    const int hid = room.add_member("房主", false, 0, fg::Team::A);
    room.add_member("AI-B", true, 1, fg::Team::B);
    const int sid = room.add_member("观众", false, 0, std::nullopt, fg::MemberRole::Spectator);
    ASSERT_TRUE(room.start_game(room.host_id()).ok());

    // 对局中：观战者可以走（不占阵容），参战者被锁定
    EXPECT_TRUE(room.is_spectator(sid));
    room.remove_member(sid);
    EXPECT_FALSE(room.has_member(sid));
    room.remove_member(hid);
    EXPECT_TRUE(room.has_member(hid));
}

TEST(Room, HostCanKickSpectatorMidGame) {
    fg::Room room = make_room(room_cfg());
    const int hid = room.add_member("房主", false, 0, fg::Team::A);
    room.add_member("AI-B", true, 1, fg::Team::B);
    const int sid = room.add_member("观众", false, 0, std::nullopt, fg::MemberRole::Spectator);
    ASSERT_TRUE(room.start_game(room.host_id()).ok());

    EXPECT_TRUE(room.kick_member(hid, sid).ok());          // 观战者可踢
    EXPECT_FALSE(room.has_member(sid));
    const int pid = room.add_member("观众2", false, 0, std::nullopt, fg::MemberRole::Spectator);
    EXPECT_EQ(room.kick_member(hid, pid).code, "");        // 房间已开局但观战者仍可作为成员加入
}

TEST(Room, SetSpectateChecksCapAndBalance) {
    fg::Room room = make_room(room_cfg());
    for (int i = 0; i < 10; ++i) {  // 填满参赛位（自动平衡成 5 A + 5 B）
        ASSERT_GE(room.add_member("玩家" + std::to_string(i + 1), false, 0), 0);
    }
    const int sid = room.add_member("观众", false, 0, std::nullopt, fg::MemberRole::Spectator);
    ASSERT_GE(sid, 0);

    // 参赛位已满：观战者转参战被拒（与 add_member 同一道关，不能绕过上限）
    EXPECT_EQ(room.set_spectate(sid, false).code, "room_full");

    // 有人让出参赛位后可以转进来，且平衡到人少的一队（第一个成员是 A 队，转走后 A 少一人）
    ASSERT_TRUE(room.set_spectate(room.members()[0].id, true).ok());
    ASSERT_TRUE(room.set_spectate(sid, false).ok());
    EXPECT_EQ(room.player_count(), 10u);
    for (const auto& m : room.members()) {
        if (m.id == sid) EXPECT_EQ(m.team, fg::Team::A);
    }

    // 反方向同样受容量约束：观战席满员（20）后，参战者不能转为观战
    for (int i = 0; i < 19; ++i) {
        ASSERT_GE(room.add_member("观" + std::to_string(i + 1), false, 0, std::nullopt,
                                  fg::MemberRole::Spectator),
                  0);
    }
    EXPECT_EQ(room.spectator_count(), 20u);
    EXPECT_EQ(room.set_spectate(room.members()[1].id, true).code, "spectator_full");
}

TEST(Room, DefaultNamesAvoidCollisions) {
    // 默认名按"最小空闲序号"生成：踢掉中间成员后再添加不会与现存成员重名
    fg::Room room = make_room(room_cfg());
    room.add_member("房主", false, 0, fg::Team::A);
    room.add_member("", true, 1, fg::Team::A);  // AI-A1
    room.add_member("", true, 1, fg::Team::A);  // AI-A2
    room.remove_member(room.members()[1].id);   // 踢掉 AI-A1（中间的）
    room.add_member("", true, 1, fg::Team::A);  // 新加：应复用空闲的 AI-A1
    EXPECT_EQ(room.members().back().name, "AI-A1");

    // 观战者同理：离开一个后新进的不重名
    const int s1 =
        room.add_member("", false, 0, std::nullopt, fg::MemberRole::Spectator);  // 观战者1
    room.add_member("", false, 0, std::nullopt, fg::MemberRole::Spectator);      // 观战者2
    room.remove_member(s1);
    room.add_member("", false, 0, std::nullopt, fg::MemberRole::Spectator);  // 复用观战者1

    std::set<std::string> names;
    for (const auto& m : room.members()) names.insert(m.name);
    EXPECT_EQ(names.size(), room.members().size());  // 全员名字无重复
}

TEST(Room, LongNameTruncatedOnUtf8Boundary) {
    fg::Room room = make_room(room_cfg());
    // 100 个汉字（300 字节）远超上限（64 字节）：截断到 ≤64 且不切开字符
    std::string long_name;
    for (int i = 0; i < 100; ++i) long_name += "汉";
    ASSERT_GE(room.add_member(long_name, false, 0, fg::Team::A), 0);
    const std::string& got = room.members()[0].name;
    EXPECT_LE(got.size(), 64u);
    EXPECT_EQ(got.size() % 3, 0u);  // 全汉字（3 字节）：按字符边界截断则为 3 的倍数
}

TEST(Room, ZombieGameRoomIsCollectible) {
    // 纯人类对局全部断线：没有 AI 能终结对局，房间不能被僵尸对局永久占住
    fg::Room room = make_room(room_cfg());
    const int h1 = room.add_member("甲", false, 0, fg::Team::A);
    const int h2 = room.add_member("乙", false, 0, fg::Team::B);
    ASSERT_TRUE(room.start_game(room.host_id()).ok());
    EXPECT_TRUE(room.has_human());

    room.mark_disconnected(h1);
    EXPECT_TRUE(room.has_human());  // 还有人在线，房间有归属
    room.mark_disconnected(h2);
    EXPECT_FALSE(room.has_human());  // 人类全断线：可被服务器回收
}

TEST(Room, SubmitAttemptsLimitedPerInputPhase) {
    fg::Room room = make_room(room_cfg());
    const int a = room.add_member("甲", false, 0, fg::Team::A);  // first_team=0：甲先手
    room.add_member("乙", true, 1, fg::Team::B);
    ASSERT_TRUE(room.start_game(room.host_id()).ok());

    fg::CurveRequest bad;
    bad.expr_a = "(";  // 语法错误：每次都被拒，但每次都消耗一次尝试
    for (int i = 0; i < 16; ++i) {
        const fg::Error e = room.submit(a, bad);
        EXPECT_FALSE(e.ok());
        EXPECT_NE(e.code, "submit_too_many");
    }
    EXPECT_EQ(room.submit(a, bad).code, "submit_too_many");
}

TEST(Room, AiThinksOffThreadThenSubmits) {
    // 注入思考池：AI 的决策在池线程里算（tick 本身不阻塞），
    // 结果就绪、思考时间从"轮到它"起走完后照样提交
    fg::ThinkPool pool(1);
    fg::GameConfig cfg = room_cfg();
    cfg.match.input_seconds = 30.0;  // 与真实房间同量级：AI 的提交窗口要留足余量
    fg::Room room = make_room(cfg);
    room.set_think_pool(&pool);
    const int h = room.add_member("人", false, 0, fg::Team::A);
    const int ai = room.add_member("AI", true, 0, fg::Team::B);  // 简单：思考 1~2.5 秒
    ASSERT_TRUE(room.start_game(room.host_id()).ok());
    room.take_outgoing();

    // 人类直接空放一条水平直线让出回合（不用等超时——把 input_seconds 压到 1 秒
    // 会把 AI 自己的时限也压到 1 秒，提交窗口贴边、整套测试一起跑时偶发超时弃权）
    fg::CurveRequest shot;
    shot.mode = fg::CurveMode::YofX;
    shot.expr_a = "0*x";
    ASSERT_TRUE(room.submit(h, shot).ok());

    bool fired = false;
    int previews = 0;
    for (int i = 0; i < 60 * 30 && !fired; ++i) {  // 模拟时间上限 30 秒
        room.tick(1.0 / 60.0);
        for (const auto& m : room.take_outgoing()) {
            const std::string t = fg::json_str(m.msg, "type");
            // 人类那一发也在同一队列里，按 owner 过滤才能认准 AI 提交的那一发
            if (t == fg::msg_type::kShot && fg::json_int(m.msg, "owner", -1) == ai) {
                fired = true;
            }
            if (t == fg::msg_type::kFunctionPreview) ++previews;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // 给池线程运行时间
    }
    EXPECT_TRUE(fired) << "AI 的异步决策应在思考时间走完后提交";
    EXPECT_GT(previews, 0) << "AI 应在思考期间流式广播'正在写：'预览";
}

TEST(Room, OtherPlayersSubmitDoNotBurnCurrentPlayerAttempts) {
    // 只有当前回合玩家自己的尝试计入上限：观战者与非当前回合的参战者怎么刷，
    // 都不能把当前回合玩家的 16 次额度消耗光（否则一局被卡到超时）
    fg::Room room = make_room(room_cfg());  // first_team=0：甲先手
    const int a = room.add_member("甲", false, 0, fg::Team::A);
    const int b = room.add_member("乙", false, 0, fg::Team::B);
    const int s = room.add_member("观众", false, 0, std::nullopt, fg::MemberRole::Spectator);
    ASSERT_TRUE(room.start_game(room.host_id()).ok());

    fg::CurveRequest bad;
    bad.expr_a = "(";  // 语法错误：每次都被拒
    for (int i = 0; i < 40; ++i) {
        room.submit(s, bad);  // 观战者狂刷
        room.submit(b, bad);  // 非当前回合的参战者也狂刷
    }

    // 甲的额度完好：还能连续提交 16 次语法错误，第 17 次才触发上限
    for (int i = 0; i < 16; ++i) {
        const fg::Error e = room.submit(a, bad);
        EXPECT_FALSE(e.ok());
        EXPECT_NE(e.code, "submit_too_many") << "第 " << (i + 1) << " 次不应超限";
    }
    EXPECT_EQ(room.submit(a, bad).code, "submit_too_many");
}

TEST(Room, SubmitTriesResetOnNewTurn) {
    fg::GameConfig cfg = room_cfg();
    cfg.match.input_seconds = 1.0;
    fg::Room room = make_room(cfg);
    const int a = room.add_member("甲", false, 0, fg::Team::A);
    const int b = room.add_member("乙", false, 0, fg::Team::B);
    ASSERT_TRUE(room.start_game(room.host_id()).ok());

    fg::CurveRequest bad;
    bad.expr_a = "(";
    for (int i = 0; i < 17; ++i) room.submit(a, bad);  // 打满（含触发 too_many 的那次）
    EXPECT_EQ(room.submit(a, bad).code, "submit_too_many");

    // 甲超时被跳过、轮到乙：新的输入阶段，尝试次数重新计
    room.tick(1.5);
    const fg::Error e = room.submit(b, bad);
    EXPECT_FALSE(e.ok());
    EXPECT_NE(e.code, "submit_too_many");
}

TEST(Room, StateJsonCarriesRole) {
    fg::Room room = make_room(room_cfg());
    room.add_member("房主", false, 0, fg::Team::A);
    room.add_member("观众", false, 0, std::nullopt, fg::MemberRole::Spectator);

    const fg::json state = room.state_json();
    const fg::json members = fg::json_array(state, "members");
    ASSERT_EQ(members.size(), 2u);
    EXPECT_EQ(fg::json_str(members[0], "role", ""), "player");
    EXPECT_EQ(fg::json_str(members[1], "role", ""), "spectator");
}

// ---- 跳过投票 / 每人士兵数锁定 / 实时预览 ----

TEST(Room, SkipVoteBroadcastsAndRestarts) {
    fg::Room room = make_room(room_cfg());
    const int a = room.add_member("甲", false, 0, fg::Team::A);
    const int b = room.add_member("乙", false, 0, fg::Team::B);
    const int c = room.add_member("丙", false, 0, fg::Team::A);
    const int d = room.add_member("丁", false, 0, fg::Team::B);  // 2v2：开局要求两队等人数
    ASSERT_TRUE(room.start_game(room.host_id()).ok());
    room.take_outgoing();

    // 一票：广播投票状态（voted 含投票者、total 为参战人类数）
    room.vote_skip(a, true);
    {
        auto out = room.take_outgoing();
        ASSERT_FALSE(out.empty());
        EXPECT_EQ(fg::json_str(out.back().msg, "type"), fg::msg_type::kSkipVote);
        EXPECT_EQ(fg::json_array(out.back().msg, "voted").size(), 1u);
        EXPECT_EQ(fg::json_int(out.back().msg, "total", 0), 4);
    }

    // 撤回：广播内容随之清空
    room.vote_skip(a, false);
    {
        auto out = room.take_outgoing();
        ASSERT_FALSE(out.empty());
        EXPECT_EQ(fg::json_array(out.back().msg, "voted").size(), 0u);
    }

    // 观战者没有票：状态不变、也不广播
    const int s = room.add_member("观众", false, 0, std::nullopt, fg::MemberRole::Spectator);
    room.take_outgoing();
    room.vote_skip(s, true);
    EXPECT_TRUE(room.take_outgoing().empty()) << "观战者没有票，不应产生广播";

    // 乙投一票：广播里应恰好只有乙
    room.vote_skip(b, true);
    {
        auto out = room.take_outgoing();
        ASSERT_FALSE(out.empty());
        EXPECT_EQ(fg::json_array(out.back().msg, "voted").size(), 1u);
    }

    // 甲、丙、丁补上：全员同意 → 立即重开新图（广播新的开局消息）
    room.vote_skip(a, true);
    room.vote_skip(c, true);
    room.vote_skip(d, true);
    bool restarted = false;
    for (const auto& m : room.take_outgoing()) {
        if (fg::json_str(m.msg, "type") == fg::msg_type::kGameStart) restarted = true;
    }
    EXPECT_TRUE(restarted) << "全票同意后应重开新图";
    EXPECT_TRUE(room.in_game());
    EXPECT_EQ(room.world().phase(), fg::Phase::Input);
}

TEST(Room, SkipVoteIgnoresDisconnectedHumans) {
    fg::Room room = make_room(room_cfg());
    const int a = room.add_member("甲", false, 0, fg::Team::A);
    const int b = room.add_member("乙", false, 0, fg::Team::B);
    const int c = room.add_member("丙", false, 0, fg::Team::A);
    const int d = room.add_member("丁", false, 0, fg::Team::B);  // 2v2：开局要求两队等人数
    ASSERT_TRUE(room.start_game(room.host_id()).ok());
    room.take_outgoing();

    // 甲先投一票，然后断线（对局中只记账、仍留在成员表里）
    room.vote_skip(a, true);
    room.take_outgoing();
    room.mark_disconnected(a);

    // 剩下的人投票：全集应排除断线的甲——乙投完差丙、丁两票（广播 total=3）
    room.vote_skip(b, true);
    auto out = room.take_outgoing();
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(fg::json_int(out.back().msg, "total", 0), 3) << "全集应只含仍未断线的人类";
    EXPECT_EQ(fg::json_array(out.back().msg, "voted").size(), 1u)
        << "甲断线前的旧票不应再出现在投票状态里";

    // 乙、丙、丁全票 → 重开新图：断线者不会把全票卡死
    room.vote_skip(c, true);
    room.vote_skip(d, true);
    bool restarted = false;
    for (const auto& m : room.take_outgoing()) {
        if (fg::json_str(m.msg, "type") == fg::msg_type::kGameStart) restarted = true;
    }
    EXPECT_TRUE(restarted) << "断线者不应阻塞跳过地图";
}

TEST(Room, SkipVoteForcesNewMapWithFixedSeed) {
    // "跳过地图"的意图就是换一张图：固定种子的房间全票通过后也必须换成随机新图，
    // 否则重开还是同一张、玩家眼里就是点了没反应；模板种子不受影响，下一把照旧
    fg::Room room = make_room(room_cfg());  // room_cfg 固定 seed=42
    const int a = room.add_member("甲", false, 0, fg::Team::A);
    room.add_member("AI-B", true, 1, fg::Team::B);
    ASSERT_TRUE(room.start_game(room.host_id()).ok());
    room.take_outgoing();
    ASSERT_EQ(room.world().config().match.seed, 42ULL);

    room.vote_skip(a, true);  // 人类只有甲，一票即全票
    uint64_t after_skip = 0;
    for (const auto& m : room.take_outgoing()) {
        if (fg::json_str(m.msg, "type") == fg::msg_type::kGameStart && m.msg.contains("match")) {
            after_skip = fg::json_uint(m.msg.at("match"), "seed", 0);
        }
    }
    EXPECT_NE(after_skip, 0ULL) << "全票后应广播新的 game_start";
    EXPECT_NE(after_skip, 42ULL) << "固定种子房间跳过地图也应换新图";
    EXPECT_EQ(room.world().config().match.seed, after_skip) << "世界里的种子应与广播一致";

    // 再跳一次再换一张（票在重开时已清空，一票重新凑满）
    const uint64_t first = after_skip;
    room.vote_skip(a, true);
    EXPECT_NE(room.world().config().match.seed, first);
}

TEST(Room, RoomPropertiesLockedAfterCreate) {
    fg::GameConfig cfg = room_cfg();
    cfg.room.soldiers_per_player = 2;
    fg::Room room = make_room(cfg);
    const int h = room.add_member("房主", false, 0, fg::Team::A);
    room.add_member("AI-B", true, 1, fg::Team::B);

    // 房间的属性没有任何修改途径：补丁里出现即拒绝——哪怕值与当前相同也不接受，
    // 协议上没有"改房间属性"这种语义（明确报错好过含糊放行）
    for (const auto& patch :
         {fg::json{{"soldiers_per_player", 3}}, fg::json{{"soldiers_per_player", 2}},
          fg::json{{"coord_origin", "field"}}, fg::json{{"map_x_max", 99.0}}}) {
        EXPECT_EQ(room.apply_config(h, patch).code, "cannot_change_room");
    }
    // "下一把"配置照常可改
    EXPECT_TRUE(room.apply_config(h, fg::json{{"hp", 2}}).ok());

    // 配置真的用在世界里：开局后每人 2 兵
    ASSERT_TRUE(room.start_game(room.host_id()).ok());
    for (const auto& p : room.world().players()) {
        EXPECT_EQ(p.soldiers.size(), 2u);
    }
}

TEST(Room, PreviewOnlyFromCurrentPlayer) {
    fg::Room room = make_room(room_cfg());  // first_team=0：甲先手
    const int a = room.add_member("甲", false, 0, fg::Team::A);
    const int b = room.add_member("乙", false, 0, fg::Team::B);
    room.add_member("观众", false, 0, std::nullopt, fg::MemberRole::Spectator);
    ASSERT_TRUE(room.start_game(room.host_id()).ok());
    ASSERT_EQ(room.world().current_player_id(), a);
    room.take_outgoing();

    // 非当前回合玩家的预览：丢弃
    room.broadcast_preview(b, "sin(x)");
    EXPECT_TRUE(room.take_outgoing().empty());
    // 当前回合玩家：转发（带 member_id）
    room.broadcast_preview(a, "sin(x)");
    auto out = room.take_outgoing();
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(fg::json_str(out[0].msg, "type"), fg::msg_type::kFunctionPreview);
    EXPECT_EQ(fg::json_int(out[0].msg, "member_id", -1), a);
    EXPECT_EQ(fg::json_str(out[0].msg, "preview"), "sin(x)");
}

TEST(Room, PasswordGate) {
    // 无密码房恒过（客户端字段残留不该拦住进无密码房）；有密码房要求哈希完全一致；
    // 状态下行只报"有没有密码"，绝不含哈希本身（哈希=口令，泄露即可被直接使用）
    fg::RoomOptions ro;
    fg::MatchConfig mc;
    fg::Room room(ro, mc);
    EXPECT_FALSE(room.has_password());
    EXPECT_TRUE(room.check_password(""));
    EXPECT_TRUE(room.check_password("anything"));
    EXPECT_FALSE(room.state_json()["has_password"].get<bool>());

    room.set_password_hash(std::string(64, 'a'));
    EXPECT_TRUE(room.has_password());
    EXPECT_TRUE(room.check_password(std::string(64, 'a')));
    EXPECT_FALSE(room.check_password(""));                   // 缺省字段不能进门
    EXPECT_FALSE(room.check_password(std::string(64, 'b')));

    const fg::json state = room.state_json();
    EXPECT_TRUE(state["has_password"].get<bool>());
    EXPECT_EQ(state.dump().find(std::string(64, 'a')), std::string::npos)
        << "房间状态泄露了密码哈希";
}
