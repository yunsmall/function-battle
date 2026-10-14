// 协议消息编码的单测：只覆盖易被改动的编码细节（错误消息的字段集）
#include <gtest/gtest.h>

#include "error.h"
#include "protocol.h"

TEST(Protocol, MakeErrorCarriesCodeAndMessage) {
    const fg::json m = fg::msg::make_error({"not_your_turn", "还没轮到你"});
    EXPECT_EQ(fg::json_str(m, "type"), "error");
    EXPECT_EQ(fg::json_str(m, "code"), "not_your_turn");
    EXPECT_EQ(fg::json_str(m, "message"), "还没轮到你");
    // 不带来源域的错误不输出 context 字段（客户端以字段缺失判定"无关错误"）
    EXPECT_FALSE(m.contains("context"));
}

TEST(Protocol, MakeErrorCarriesContextWhenGiven) {
    const fg::json m =
        fg::msg::make_error({"not_input_phase", "当前不是输入阶段"}, "submit");
    EXPECT_EQ(fg::json_str(m, "code"), "not_input_phase");
    EXPECT_EQ(fg::json_str(m, "context"), "submit");
}

TEST(Protocol, ObstacleDensityCheckedAgainstMapArea) {
    // 默认地图 48×30（面积 1440，上限 45）：45 通过、80 被拒
    //（否则半场会被遮挡物盖满、出生点无处安放）
    fg::WorldConstants wc;
    fg::MatchConfig mc;
    mc.obstacle_count = 45;
    EXPECT_TRUE(fg::check_obstacle_density(wc, mc).ok());
    mc.obstacle_count = 80;
    EXPECT_EQ(fg::check_obstacle_density(wc, mc).code, "obstacles_too_dense");

    // 大图（120×80，面积 9600，上限 300）：300 通过。世界常量在进程内可直接构造
    //（协议上行不接受地图字段，见 RoomOptionsValidateAndRoundTrip 的 bad_room_field）
    wc.map_x_min = -60.0;
    wc.map_x_max = 60.0;
    wc.map_y_min = -40.0;
    wc.map_y_max = 40.0;
    mc.obstacle_count = 300;
    EXPECT_TRUE(fg::check_obstacle_density(wc, mc).ok());

    // 大图改成小图、同时保留 300 个遮挡物：必须按"最终组合"拒绝
    //（若实现只按应用 obstacle_count 那一刻的地图校验，这条会漏放行）
    wc.map_x_min = -24.0;
    wc.map_x_max = 24.0;
    wc.map_y_min = -15.0;
    wc.map_y_max = 15.0;
    EXPECT_EQ(fg::check_obstacle_density(wc, mc).code, "obstacles_too_dense");
}

// ---- 本批次新增字段的编解码 ----

TEST(Protocol, CurveRequestMirrorAndAngleRoundTrip) {
    fg::CurveRequest req;
    req.mode = fg::CurveMode::Ypp;
    req.expr_a = "-0.25";
    req.dir = fg::FireDir::Negative;
    req.angle = -0.4;
    req.mirror = true;

    fg::Error err;
    const auto back = fg::curve_request_from_json(fg::to_json(req), err);
    ASSERT_TRUE(back.has_value()) << err.message;
    EXPECT_EQ(back->mode, fg::CurveMode::Ypp);
    EXPECT_EQ(back->dir, fg::FireDir::Negative);
    EXPECT_TRUE(back->mirror);
    ASSERT_TRUE(back->angle.has_value());
    EXPECT_DOUBLE_EQ(*back->angle, -0.4);

    // 普通请求往返后也不应凭空带上 mirror/angle（缺省即 false/无）
    fg::CurveRequest plain;
    plain.mode = fg::CurveMode::YofX;
    plain.expr_a = "x";
    const auto back2 = fg::curve_request_from_json(fg::to_json(plain), err);
    ASSERT_TRUE(back2.has_value()) << err.message;
    EXPECT_FALSE(back2->mirror);
    EXPECT_FALSE(back2->angle.has_value());
}

TEST(Protocol, RoomOptionsValidateAndRoundTrip) {
    fg::RoomOptions ro;
    EXPECT_EQ(fg::apply_room_config_json(fg::json{{"soldiers_per_player", 0}}, ro).code,
              "bad_soldiers_per_player_range");
    EXPECT_EQ(fg::apply_room_config_json(fg::json{{"soldiers_per_player", 5}}, ro).code,
              "bad_soldiers_per_player_range");
    EXPECT_EQ(fg::apply_room_config_json(fg::json{{"soldiers_per_player", "two"}}, ro).code,
              "bad_soldiers_per_player");
    EXPECT_EQ(fg::apply_room_config_json(fg::json{{"coord_origin", "middle"}}, ro).code,
              "bad_coord_origin");
    // 世界常量没有上行入口：出现即拒绝（明确报错，不是静默忽略）
    EXPECT_EQ(fg::apply_room_config_json(fg::json{{"map_x_max", 100.0}}, ro).code,
              "bad_room_field");
    EXPECT_EQ(fg::apply_room_config_json(fg::json{{"projectile_speed", 50.0}}, ro).code,
              "bad_room_field");

    // 合法值真的落进配置，且能经 to_json 往返
    ASSERT_TRUE(fg::apply_room_config_json(
                    fg::json{{"soldiers_per_player", 3}, {"coord_origin", "field"}}, ro)
                    .ok());
    EXPECT_EQ(ro.soldiers_per_player, 3);
    EXPECT_EQ(ro.coord_origin, fg::CoordOrigin::Field);
    const fg::json out = fg::to_json(ro);
    EXPECT_EQ(fg::json_int(out, "soldiers_per_player", 0), 3);
    EXPECT_EQ(fg::json_str(out, "coord_origin"), "field");
}

TEST(Protocol, MatchConfigValidateAndRoundTrip) {
    fg::MatchConfig mc;
    EXPECT_EQ(fg::apply_match_config_json(fg::json{{"turn_delay", -0.5}}, mc).code,
              "bad_turn_delay_range");
    EXPECT_EQ(fg::apply_match_config_json(fg::json{{"turn_delay", 11.0}}, mc).code,
              "bad_turn_delay_range");
    EXPECT_EQ(fg::apply_match_config_json(fg::json{{"turn_delay", "long"}}, mc).code,
              "bad_turn_delay");
    EXPECT_EQ(fg::apply_match_config_json(fg::json{{"mirror_view", 1}}, mc).code, "bad_mirror_view");
    EXPECT_EQ(fg::apply_match_config_json(fg::json{{"first_team", 2}}, mc).code,
              "bad_first_team_range");
    EXPECT_EQ(fg::apply_match_config_json(fg::json{{"seed", -5}}, mc).code, "bad_seed");
    // 白名单：未知键明确拒绝（键名打错不会被静默忽略）
    EXPECT_EQ(fg::apply_match_config_json(fg::json{{"hpp", 3}}, mc).code, "unknown_match_field");

    // seed：null = 每把随机（清空固定种子）；数值 = 固定种子——两者都经 to_json 往返
    ASSERT_TRUE(fg::apply_match_config_json(fg::json{{"seed", nullptr}}, mc).ok());
    EXPECT_FALSE(mc.seed.has_value());
    EXPECT_TRUE(fg::to_json(mc)["seed"].is_null());
    ASSERT_TRUE(fg::apply_match_config_json(fg::json{{"seed", 7}}, mc).ok());
    ASSERT_TRUE(mc.seed.has_value());
    EXPECT_EQ(*mc.seed, 7u);
    EXPECT_EQ(fg::json_uint(fg::to_json(mc), "seed", 0), 7u);

    // 合法值真的落进配置，且能经 to_json 往返
    ASSERT_TRUE(fg::apply_match_config_json(
                    fg::json{{"hp", 3}, {"turn_delay", 1.5}, {"mirror_view", true}}, mc)
                    .ok());
    EXPECT_EQ(mc.hp, 3);
    EXPECT_DOUBLE_EQ(mc.turn_delay, 1.5);
    EXPECT_TRUE(mc.mirror_view);
    const fg::json out = fg::to_json(mc);
    EXPECT_EQ(fg::json_int(out, "hp", 0), 3);
    EXPECT_DOUBLE_EQ(fg::json_double(out, "turn_delay", 0.0), 1.5);
    EXPECT_TRUE(fg::json_bool(out, "mirror_view", false));
}

TEST(Protocol, PlayerSoldiersRoundTrip) {
    fg::Player p;
    p.id = 2;
    p.seat = 1;
    p.team = fg::Team::B;
    p.name = "测试";
    fg::Soldier s0;
    s0.pos = fg::Vec2(1.0, 2.0);
    s0.max_hp = 3;
    s0.hp = 2;
    fg::Soldier s1;
    s1.pos = fg::Vec2(-1.0, 0.5);
    s1.max_hp = 3;
    s1.hp = 0;
    s1.alive = false;
    p.soldiers = {s0, s1};

    const fg::Player back = fg::player_from_json(fg::to_json(p));
    ASSERT_EQ(back.soldiers.size(), 2u);
    EXPECT_DOUBLE_EQ(back.soldiers[0].pos.x(), 1.0);
    EXPECT_DOUBLE_EQ(back.soldiers[0].pos.y(), 2.0);
    EXPECT_EQ(back.soldiers[0].hp, 2);
    EXPECT_TRUE(back.soldiers[0].alive);
    EXPECT_DOUBLE_EQ(back.soldiers[1].pos.y(), 0.5);
    EXPECT_EQ(back.soldiers[1].hp, 0);
    EXPECT_FALSE(back.soldiers[1].alive);
    EXPECT_EQ(back.name, "测试");
    EXPECT_EQ(back.team, fg::Team::B);
}

TEST(Protocol, PasswordHashFormat) {
    // 房间密码哈希的格式校验：只收 64 位小写十六进制（与客户端 SHA-256 输出一致）
    EXPECT_TRUE(fg::is_sha256_hex(
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    EXPECT_FALSE(fg::is_sha256_hex(""));
    EXPECT_FALSE(fg::is_sha256_hex(std::string(63, 'a')));
    EXPECT_FALSE(fg::is_sha256_hex(std::string(65, 'a')));
    EXPECT_FALSE(fg::is_sha256_hex(std::string(64, 'G')));
    EXPECT_FALSE(fg::is_sha256_hex(std::string(64, 'A')));  // 大写不接受：协议固定小写
}
