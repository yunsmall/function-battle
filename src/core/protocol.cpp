#include "protocol.h"

#include <cmath>
#include <cstring>

namespace fg {

const char* curve_mode_str(CurveMode m) {
    switch (m) {
        case CurveMode::YofX: return "y_of_x";
        case CurveMode::XofY: return "x_of_y";
        case CurveMode::Polar: return "polar";
        case CurveMode::Parametric: return "parametric";
        case CurveMode::Ode1: return "ode1";
        case CurveMode::Ode2: return "ode2";
        case CurveMode::Ypp: return "ypp";
        case CurveMode::Count: break;  // 哨兵值，落到下面的兜底
    }
    return "y_of_x";
}

std::optional<CurveMode> curve_mode_from_str(const std::string& s) {
    if (s == "y_of_x") return CurveMode::YofX;
    if (s == "x_of_y") return CurveMode::XofY;
    if (s == "polar") return CurveMode::Polar;
    if (s == "parametric") return CurveMode::Parametric;
    if (s == "ode1") return CurveMode::Ode1;
    if (s == "ode2") return CurveMode::Ode2;
    if (s == "ypp") return CurveMode::Ypp;
    return std::nullopt;
}

const char* fire_dir_str(FireDir d) { return d == FireDir::Positive ? "pos" : "neg"; }

std::optional<FireDir> fire_dir_from_str(const std::string& s) {
    if (s == "pos") return FireDir::Positive;
    if (s == "neg") return FireDir::Negative;
    return std::nullopt;
}

json vec2_to_json(const Vec2& v) { return json::array({v.x(), v.y()}); }

Vec2 vec2_from_json(const json& j) {
    // 结构不符时返回原点（调用方多为渲染/状态更新，容错即可）
    if (!j.is_array() || j.size() < 2 || !j[0].is_number() || !j[1].is_number()) {
        return Vec2(0.0, 0.0);
    }
    return Vec2(j[0].get<double>(), j[1].get<double>());
}

json to_json(const CurveRequest& req) {
    json j;
    j["mode"] = curve_mode_str(req.mode);
    j["expr_a"] = req.expr_a;
    j["expr_b"] = req.expr_b;
    j["dir"] = fire_dir_str(req.dir);
    if (req.p_start) j["p_start"] = *req.p_start;
    if (req.p_end) j["p_end"] = *req.p_end;
    if (req.mirror) j["mirror"] = true;  // 缺省即 false，不必输出
    if (req.angle) j["angle"] = *req.angle;
    return j;
}

std::optional<CurveRequest> curve_request_from_json(const json& j, Error& error) {
    if (!j.is_object()) {
        error = {"bad_request_format", "提交格式错误：不是 JSON 对象"};
        return std::nullopt;
    }
    const auto mode = curve_mode_from_str(json_str(j, "mode"));
    if (!mode) {
        error = {"unknown_curve_mode", "未知的曲线模式"};
        return std::nullopt;
    }
    const auto dir = fire_dir_from_str(json_str(j, "dir", "pos"));
    if (!dir) {
        error = {"unknown_fire_dir", "未知的发射方向"};
        return std::nullopt;
    }
    CurveRequest req;
    req.mode = *mode;
    req.expr_a = json_str(j, "expr_a");
    req.expr_b = json_str(j, "expr_b");
    req.dir = *dir;
    if (j.contains("p_start") && j["p_start"].is_number()) req.p_start = j["p_start"].get<double>();
    if (j.contains("p_end") && j["p_end"].is_number()) req.p_end = j["p_end"].get<double>();
    req.mirror = json_bool(j, "mirror", false);
    if (j.contains("angle") && j["angle"].is_number()) req.angle = j["angle"].get<double>();
    return req;
}

json to_json(const Player& p) {
    json j;
    j["id"] = p.id;
    j["seat"] = p.seat;
    j["team"] = (p.team == Team::A) ? "A" : "B";
    j["name"] = p.name;
    json soldiers = json::array();
    for (const auto& s : p.soldiers) {
        soldiers.push_back(json{{"pos", vec2_to_json(s.pos)},
                                {"hp", s.hp},
                                {"max_hp", s.max_hp},
                                {"alive", s.alive}});
    }
    j["soldiers"] = std::move(soldiers);
    j["is_ai"] = p.is_ai;
    j["ai_difficulty"] = p.ai_difficulty;
    return j;
}

json to_json(const Obstacle& ob) {
    json j;
    j["is_circle"] = ob.is_circle;
    j["center"] = vec2_to_json(ob.center);
    j["radius"] = ob.radius;
    j["half_w"] = ob.half_w;
    j["half_h"] = ob.half_h;
    json craters = json::array();
    for (const auto& c : ob.craters) craters.push_back(vec2_to_json(c));
    j["craters"] = std::move(craters);
    return j;
}

json to_json(const RoomOptions& ro) {
    json j;
    j["soldiers_per_player"] = ro.soldiers_per_player;
    // 曲线参考系（房间属性）：字符串形式，语义见 config.h 的 CoordOrigin
    j["coord_origin"] = (ro.coord_origin == CoordOrigin::Field) ? "field" : "soldier";
    return j;
}

json to_json(const WorldConstants& wc) {
    json j;
    j["map_x_min"] = wc.map_x_min;
    j["map_x_max"] = wc.map_x_max;
    j["map_y_min"] = wc.map_y_min;
    j["map_y_max"] = wc.map_y_max;
    j["player_half_size"] = wc.player_half_size;
    j["projectile_speed"] = wc.projectile_speed;
    j["max_travel"] = wc.max_travel;
    j["crater_radius"] = wc.crater_radius;
    return j;
}

// 下一把配置与本局实际配置的序列化只差 seed：前者空 → null（每把随机），后者恒实值
json to_json(const MatchConfig& mc) {
    json j;
    j["seed"] = mc.seed ? json(*mc.seed) : json(nullptr);
    j["hp"] = mc.hp;
    // 键名 obstacle_count：与 game_start 的 obstacles 数组（障碍物对象列表）区分开，
    // 客户端不会把数量误传成数组
    j["obstacle_count"] = mc.obstacle_count;
    j["input_seconds"] = mc.input_seconds;
    j["turn_delay"] = mc.turn_delay;
    j["mirror_view"] = mc.mirror_view;
    j["first_team"] = mc.first_team;
    return j;
}

json to_json(const ResolvedMatchConfig& mr) {
    json j;
    j["seed"] = mr.seed;
    j["hp"] = mr.hp;
    j["obstacle_count"] = mr.obstacle_count;
    j["input_seconds"] = mr.input_seconds;
    j["turn_delay"] = mr.turn_delay;
    j["mirror_view"] = mr.mirror_view;
    j["first_team"] = mr.first_team;
    return j;
}

RoomOptions room_options_from_json(const json& j) {
    RoomOptions ro;
    ro.soldiers_per_player = json_int(j, "soldiers_per_player", ro.soldiers_per_player);
    ro.coord_origin = (json_str(j, "coord_origin", "soldier") == "field")
                          ? CoordOrigin::Field
                          : CoordOrigin::Soldier;
    return ro;
}

WorldConstants world_constants_from_json(const json& j) {
    WorldConstants wc;
    wc.map_x_min = json_double(j, "map_x_min", wc.map_x_min);
    wc.map_x_max = json_double(j, "map_x_max", wc.map_x_max);
    wc.map_y_min = json_double(j, "map_y_min", wc.map_y_min);
    wc.map_y_max = json_double(j, "map_y_max", wc.map_y_max);
    wc.player_half_size = json_double(j, "player_half_size", wc.player_half_size);
    wc.projectile_speed = json_double(j, "projectile_speed", wc.projectile_speed);
    wc.max_travel = json_double(j, "max_travel", wc.max_travel);
    wc.crater_radius = json_double(j, "crater_radius", wc.crater_radius);
    return wc;
}

MatchConfig match_config_from_json(const json& j) {
    MatchConfig mc;
    // seed：null 或缺失 = 每把随机；非负数值 = 固定种子（其余类型按随机，下行不会这样发）
    const auto sit = j.find("seed");
    if (sit != j.end()) {
        if (sit->is_number_unsigned()) {
            mc.seed = sit->get<uint64_t>();
        } else if (sit->is_number_integer()) {
            const int64_t v = sit->get<int64_t>();
            if (v >= 0) mc.seed = static_cast<uint64_t>(v);
        }
    }
    mc.hp = json_int(j, "hp", mc.hp);
    mc.obstacle_count = json_int(j, "obstacle_count", mc.obstacle_count);
    mc.input_seconds = json_double(j, "input_seconds", mc.input_seconds);
    mc.turn_delay = json_double(j, "turn_delay", mc.turn_delay);
    mc.mirror_view = json_bool(j, "mirror_view", mc.mirror_view);
    mc.first_team = json_int(j, "first_team", mc.first_team);
    return mc;
}

ResolvedMatchConfig resolved_match_from_json(const json& j) {
    ResolvedMatchConfig mr;
    mr.seed = json_uint(j, "seed", mr.seed);
    mr.hp = json_int(j, "hp", mr.hp);
    mr.obstacle_count = json_int(j, "obstacle_count", mr.obstacle_count);
    mr.input_seconds = json_double(j, "input_seconds", mr.input_seconds);
    mr.turn_delay = json_double(j, "turn_delay", mr.turn_delay);
    mr.mirror_view = json_bool(j, "mirror_view", mr.mirror_view);
    mr.first_team = json_int(j, "first_team", mr.first_team);
    return mr;
}

GameConfig config_from_json(const json& room_j, const json& world_j, const json& match_j) {
    return GameConfig{room_options_from_json(room_j), world_constants_from_json(world_j),
                      resolved_match_from_json(match_j)};
}

Player player_from_json(const json& j) {
    Player p;
    p.id = json_int(j, "id", 0);
    p.seat = json_int(j, "seat", 1);
    p.team = (json_str(j, "team", "A") == "B") ? Team::B : Team::A;
    p.name = json_str(j, "name");
    for (const auto& sj : json_array(j, "soldiers")) {
        Soldier s;
        if (sj.contains("pos")) s.pos = vec2_from_json(sj["pos"]);
        s.hp = json_int(sj, "hp", 1);
        s.max_hp = json_int(sj, "max_hp", 1);
        s.alive = json_bool(sj, "alive", true);
        p.soldiers.push_back(s);
    }
    // 缺 soldiers 字段的输入（旧格式/手拼消息）：按顶层 pos/hp 补一个，保证非空
    if (p.soldiers.empty()) {
        Soldier s;
        if (j.contains("pos")) s.pos = vec2_from_json(j["pos"]);
        s.hp = json_int(j, "hp", 1);
        s.max_hp = json_int(j, "max_hp", 1);
        s.alive = json_bool(j, "alive", true);
        p.soldiers.push_back(s);
    }
    p.is_ai = json_bool(j, "is_ai", false);
    p.ai_difficulty = json_int(j, "ai_difficulty", 1);
    return p;
}

Obstacle obstacle_from_json(const json& j) {
    Obstacle ob;
    ob.is_circle = json_bool(j, "is_circle", true);
    if (j.contains("center")) ob.center = vec2_from_json(j["center"]);
    ob.radius = json_double(j, "radius", 0.0);
    ob.half_w = json_double(j, "half_w", 0.0);
    ob.half_h = json_double(j, "half_h", 0.0);
    for (const auto& c : json_array(j, "craters")) ob.craters.push_back(vec2_from_json(c));
    return ob;
}

Error apply_room_config_json(const json& j, RoomOptions& ro) {
    if (!j.is_object()) return {"config_not_object", "配置必须是 JSON 对象"};
    // 房间属性里玩家可选的就士兵数与坐标系两项；其余（世界常量等）由代码固定，
    // 不提供上行入口。出现其它字段说明客户端想改不允许改的东西，明确报错而不是静默忽略
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() != "soldiers_per_player" && it.key() != "coord_origin") {
            return {"bad_room_field", "房间属性不支持该字段：" + it.key()};
        }
    }
    // 先在副本上改、全部校验通过后才整体生效：中途校验失败时不能留下改了一半的配置
    RoomOptions tmp = ro;
    if (j.contains("soldiers_per_player")) {
        if (!j["soldiers_per_player"].is_number_integer())
            return {"bad_soldiers_per_player", "soldiers_per_player 需要是整数"};
        const int sp = static_cast<int>(j["soldiers_per_player"].get<int64_t>());
        if (sp < 1 || sp > 4)
            return {"bad_soldiers_per_player_range", "soldiers_per_player 需要在 1~4 之间"};
        tmp.soldiers_per_player = sp;
    }
    if (j.contains("coord_origin")) {
        if (!j["coord_origin"].is_string())
            return {"bad_coord_origin", "coord_origin 需要是字符串（\"soldier\" 或 \"field\"）"};
        const std::string v = j["coord_origin"].get<std::string>();
        if (v != "soldier" && v != "field")
            return {"bad_coord_origin", "coord_origin 需要是 \"soldier\" 或 \"field\""};
        tmp.coord_origin = (v == "field") ? CoordOrigin::Field : CoordOrigin::Soldier;
    }
    ro = tmp;
    return {};
}

Error apply_match_config_json(const json& j, MatchConfig& mc) {
    if (!j.is_object()) return {"config_not_object", "配置必须是 JSON 对象"};
    // 白名单：只接受 MatchConfig 的字段，未知键明确拒绝（键名打错不会被静默忽略）
    static const char* kKeys[] = {"seed",        "hp",           "obstacle_count", "input_seconds",
                                  "turn_delay",  "mirror_view",  "first_team"};
    for (auto it = j.begin(); it != j.end(); ++it) {
        bool known = false;
        for (const char* k : kKeys) {
            if (it.key() == k) {
                known = true;
                break;
            }
        }
        if (!known) return {"unknown_match_field", "下一把配置不支持该字段：" + it.key()};
    }
    // 先在副本上改、全部校验通过后才整体生效：中途校验失败时不能留下改了一半的配置
    MatchConfig tmp = mc;
    if (j.contains("hp")) {
        if (!j["hp"].is_number_integer()) return {"bad_hp", "hp 需要是整数"};
        const int hp = static_cast<int>(j["hp"].get<int64_t>());
        if (hp < 1 || hp > 999) return {"bad_hp_range", "hp 需要在 1~999 之间"};
        tmp.hp = hp;
    }
    if (j.contains("obstacle_count")) {
        if (!j["obstacle_count"].is_number_integer())
            return {"bad_obstacles", "obstacle_count 需要是整数"};
        const int oc = static_cast<int>(j["obstacle_count"].get<int64_t>());
        if (oc < 0 || oc > 300) return {"bad_obstacles_range", "obstacle_count 需要在 0~300 之间"};
        tmp.obstacle_count = oc;
    }
    if (j.contains("input_seconds")) {
        if (!j["input_seconds"].is_number()) return {"bad_input_seconds", "input_seconds 需要是数字"};
        const double s = j["input_seconds"].get<double>();
        if (s < 3.0 || s > 600.0)
            return {"bad_input_seconds_range", "input_seconds 需要在 3~600 之间"};
        tmp.input_seconds = s;
    }
    if (j.contains("turn_delay")) {
        if (!j["turn_delay"].is_number()) return {"bad_turn_delay", "turn_delay 需要是数字"};
        const double d = j["turn_delay"].get<double>();
        if (d < 0.0 || d > 10.0)
            return {"bad_turn_delay_range", "turn_delay 需要在 0~10 秒之间"};
        tmp.turn_delay = d;
    }
    if (j.contains("mirror_view")) {
        if (!j["mirror_view"].is_boolean()) return {"bad_mirror_view", "mirror_view 需要是布尔值"};
        tmp.mirror_view = j["mirror_view"].get<bool>();
    }
    if (j.contains("first_team")) {
        if (!j["first_team"].is_number_integer()) return {"bad_first_team", "first_team 需要是整数"};
        const int ft = static_cast<int>(j["first_team"].get<int64_t>());
        if (ft < -1 || ft > 1)
            return {"bad_first_team_range", "first_team 需要是 -1（随机）、0（A）或 1（B）"};
        tmp.first_team = ft;
    }
    // seed：null = 改为"每把随机"（清空固定种子）；非负整数 = 固定种子。
    // 接受 number_unsigned 与 number_integer 两种：JSON 文本里的 12345 解析后是
    // number_unsigned（本项目用 nlohmann 3.12 值语义；旧版 3.10 把文本整数一律存成
    // number_integer，不要按旧版行为把这行报成 bug），而内存里用 C++ int 构造的
    // json 是 number_integer——两种来源都算合法，负数/小数/字符串才是坏输入
    if (j.contains("seed")) {
        if (j["seed"].is_null()) {
            tmp.seed.reset();
        } else if (j["seed"].is_number_unsigned()) {
            tmp.seed = j["seed"].get<uint64_t>();
        } else if (j["seed"].is_number_integer()) {
            const int64_t v = j["seed"].get<int64_t>();
            if (v < 0) return {"bad_seed", "seed 需要是非负整数或 null（null = 每把随机）"};
            tmp.seed = static_cast<uint64_t>(v);
        } else {
            return {"bad_seed", "seed 需要是非负整数或 null（null = 每把随机）"};
        }
    }
    mc = tmp;
    return {};
}

namespace {
// 遮挡物密度交叉校验：数量必须与地图面积匹配（单项检查 0~300 防不住
// "小地图 + 大量遮挡物"——半场会被盖满、出生点无处安放，世界层只能回退到
// 半场中心，玩家出生即卡在遮挡物里）。面积/32 意为每个遮挡物平均至少留出
// 32 平方单位：默认地图 48x30 面积 1440，上限 45 个（创建界面最大档 30，够用）
Error density_check(const WorldConstants& wc, int obstacle_count) {
    const double area = (wc.map_x_max - wc.map_x_min) * (wc.map_y_max - wc.map_y_min);
    if (static_cast<double>(obstacle_count) > area / 32.0) {
        return {"obstacles_too_dense", "遮挡物对当前地图过密（上限约为地图面积/32，当前最多 " +
                                           std::to_string(static_cast<int>(area / 32.0)) +
                                           " 个）"};
    }
    return {};
}
}  // namespace

Error check_obstacle_density(const WorldConstants& wc, const MatchConfig& mc) {
    return density_check(wc, mc.obstacle_count);
}

Error check_obstacle_density(const WorldConstants& wc, const ResolvedMatchConfig& mc) {
    return density_check(wc, mc.obstacle_count);
}

bool is_room_config_key(const std::string& key) {
    // "创建时定死、之后不可改"的字段：房间属性（RoomOptions）+ 世界常量（WorldConstants）
    static const char* kKeys[] = {"soldiers_per_player", "coord_origin", "map_x_min",
                                  "map_x_max",          "map_y_min",    "map_y_max",
                                  "player_half_size",   "projectile_speed",
                                  "max_travel",         "crater_radius"};
    for (const char* k : kKeys) {
        if (key == k) return true;
    }
    return false;
}

namespace msg {

json make_hello(int conn_id) { return json{{"type", msg_type::kHello}, {"conn", conn_id}}; }

json make_error(const Error& e, const std::string& context) {
    json m{{"type", msg_type::kError}, {"code", e.code}, {"message", e.message}};
    if (!context.empty()) m["context"] = context;
    return m;
}

json make_room_list(const json& rooms) {
    return json{{"type", msg_type::kRoomList}, {"rooms", rooms}};
}

json make_joined(int room_id, int member_id, int host) {
    return json{{"type", msg_type::kJoined},
                {"room_id", room_id},
                {"member_id", member_id},
                {"host", host}};
}

json make_left_room() { return json{{"type", msg_type::kLeftRoom}}; }

json make_kicked() { return json{{"type", msg_type::kKicked}}; }

json make_room_member(int id, const std::string& name, bool is_ai, int difficulty,
                      const std::string& team, const std::string& role) {
    return json{{"id", id},
                {"name", name},
                {"is_ai", is_ai},
                {"difficulty", difficulty},
                {"team", team},
                {"role", role}};
}

json make_room_state(const RoomOptions& room, const WorldConstants& world,
                     const MatchConfig& next_match, int host, bool in_game, json members) {
    return json{{"type", msg_type::kRoomState},
                {"room", to_json(room)},
                {"world", to_json(world)},
                {"next_match", to_json(next_match)},
                {"host", host},
                {"in_game", in_game},
                {"members", std::move(members)}};
}

json make_game_start(const RoomOptions& room, const WorldConstants& world,
                     const ResolvedMatchConfig& match, const std::vector<Player>& players,
                     const std::vector<Obstacle>& obstacles, int round) {
    json player_list = json::array();
    for (const auto& p : players) player_list.push_back(to_json(p));
    json obstacle_list = json::array();
    for (const auto& ob : obstacles) obstacle_list.push_back(to_json(ob));
    json m{{"type", msg_type::kGameStart},
           {"room", to_json(room)},
           {"world", to_json(world)},
           {"match", to_json(match)},
           {"players", std::move(player_list)},
           {"obstacles", std::move(obstacle_list)}};
    // round 仅在"中途观战快照"场景带上（当前进行到第几回合）；
    // 正常开局的 game_start 不带（紧随其后的 turn_begin 里有 round）
    if (round > 0) m["round"] = round;
    return m;
}

json make_turn_begin(int player_id, int soldier, double input_seconds, int round, int next_player,
                     int next_soldier) {
    json m{{"type", msg_type::kTurnBegin},
           {"player_id", player_id},
           {"soldier", soldier},
           {"input_seconds", input_seconds}};
    if (round > 0) m["round"] = round;
    // 下一位出手者（界面高亮"接下来是谁"）；没有可预告的玩家时不带这两个字段
    if (next_player >= 0) {
        m["next_player"] = next_player;
        m["next_soldier"] = next_soldier;
    }
    return m;
}

json make_turn_skipped(int player_id) {
    return json{{"type", msg_type::kTurnSkipped}, {"player_id", player_id}};
}

json make_skip_vote(const std::vector<int>& voted, int total) {
    return json{{"type", msg_type::kSkipVote}, {"voted", voted}, {"total", total}};
}

json make_function_preview(int member_id, const std::string& preview) {
    return json{
        {"type", msg_type::kFunctionPreview}, {"member_id", member_id}, {"preview", preview}};
}

namespace {
// 轨迹折线 → 协议 JSON（shot.trail 与 curve_preview.trail 共用）：
// Web 端不共享 C++ core、无法本地重采样、预览也没法本地算，弹道只能从服务器拿；
// 一条弹道最多四万个采样点，原样序列化几百 KB，这里抽稀到 ≤4000 点、
// 坐标留 4 位小数（地图单位下是亚像素级，肉眼看不出）
json path_to_trail_json(const Path& path) {
    constexpr size_t kTrailMaxPoints = 4000;
    constexpr double kTrailScale = 10000.0;
    json trail = json::array();
    const size_t n = path.points.size();
    if (n == 0) return trail;
    const size_t stride = n > kTrailMaxPoints ? (n + kTrailMaxPoints - 1) / kTrailMaxPoints : 1;
    for (size_t i = 0; i < n; i += stride) {
        const Vec2& p = path.points[i];
        trail.push_back(json::array({std::round(p.x() * kTrailScale) / kTrailScale,
                                     std::round(p.y() * kTrailScale) / kTrailScale}));
    }
    // 末点必须保留：抽稀恰好跳过它时，渲染的收尾位置会差一整段
    if ((n - 1) % stride != 0) {
        const Vec2& p = path.points[n - 1];
        trail.push_back(json::array({std::round(p.x() * kTrailScale) / kTrailScale,
                                     std::round(p.y() * kTrailScale) / kTrailScale}));
    }
    return trail;
}
}  // namespace

json make_shot(int id, int owner, int owner_soldier, const CurveRequest& request, double speed,
               const Path& path) {
    return json{{"type", msg_type::kShot},
                {"id", id},
                {"owner", owner},
                {"soldier", owner_soldier},
                {"request", to_json(request)},
                {"speed", speed},
                {"trail", path_to_trail_json(path)}};
}

json make_curve_preview(const Path& path) {
    return json{{"type", msg_type::kCurvePreview}, {"trail", path_to_trail_json(path)}};
}

json make_projectiles(const std::vector<ProjectileState>& list) {
    json arr = json::array();
    for (const auto& p : list) {
        arr.push_back(json{{"id", p.id},
                           {"pos", vec2_to_json(p.pos)},
                           {"travel", p.travel},
                           {"length", p.length}});
    }
    return json{{"type", msg_type::kProjectiles}, {"list", std::move(arr)}};
}

json make_hit(int target, int target_soldier, int by, int hp_left, const Vec2& pos) {
    return json{{"type", msg_type::kHit},
                {"target", target},
                {"soldier", target_soldier},
                {"by", by},
                {"hp_left", hp_left},
                {"pos", vec2_to_json(pos)}};
}

json make_death(int player_id, int soldier, int by) {
    return json{{"type", msg_type::kDeath},
                {"player_id", player_id},
                {"soldier", soldier},
                {"by", by}};
}

json make_crater(int obstacle_index, const Vec2& pos) {
    return json{{"type", msg_type::kCrater},
                {"obstacle", obstacle_index},
                {"pos", vec2_to_json(pos)}};
}

json make_game_over(int winner) {
    return json{{"type", msg_type::kGameOver}, {"winner", winner}};
}

}  // namespace msg

std::string encode_frame(const json& msg) {
    const std::string text = msg.dump();
    const uint32_t n = static_cast<uint32_t>(text.size());
    std::string out;
    out.reserve(4 + text.size());
    out.push_back(static_cast<char>(n & 0xFF));
    out.push_back(static_cast<char>((n >> 8) & 0xFF));
    out.push_back(static_cast<char>((n >> 16) & 0xFF));
    out.push_back(static_cast<char>((n >> 24) & 0xFF));
    out += text;
    return out;
}

DecodeStatus decode_frame(std::string& buffer, json& out, std::string& error) {
    if (buffer.size() < 4) return DecodeStatus::Incomplete;
    const auto* b = reinterpret_cast<const unsigned char*>(buffer.data());
    const uint32_t n = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
                       (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    if (n > kMaxFrameSize) {
        error = "帧长度超出上限";
        return DecodeStatus::Error;
    }
    if (buffer.size() < 4 + n) return DecodeStatus::Incomplete;
    const std::string text = buffer.substr(4, n);
    buffer.erase(0, 4 + n);
    // 用不抛异常的解析：失败时返回 discarded，由这里转成错误状态
    out = json::parse(text, nullptr, false);
    if (out.is_discarded()) {
        error = "JSON 解析失败";
        return DecodeStatus::Error;
    }
    return DecodeStatus::Ok;
}

}  // namespace fg
