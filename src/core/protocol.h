#pragma once
// 客户端/服务器之间的 JSON 消息编解码。
// 帧格式：4 字节小端长度前缀 + UTF-8 JSON 文本。
// 炮弹轨迹不随协议传输：客户端用同一份 core 代码按 CurveRequest 本地重建（确定性一致）。

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

#include "config.h"
#include "curve.h"
#include "error.h"
#include "world.h"

namespace fg {

using json = nlohmann::json;

// ---- JSON 取值：先检查类型再取，不抛异常；缺失或类型不符时返回默认值 ----
// 本项目约定不用异常传递错误（nlohmann 的 value()/get()/at() 在类型不符时抛，故统一走这里）
inline int json_int(const json& j, const char* key, int def = 0) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return def;
    return static_cast<int>(it->get<int64_t>());
}
inline uint64_t json_uint(const json& j, const char* key, uint64_t def = 0) {
    const auto it = j.find(key);
    if (it == j.end()) return def;
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer()) {
        const int64_t v = it->get<int64_t>();
        return v >= 0 ? static_cast<uint64_t>(v) : def;
    }
    return def;
}
inline double json_double(const json& j, const char* key, double def = 0.0) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<double>();
}
inline bool json_bool(const json& j, const char* key, bool def = false) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}
inline std::string json_str(const json& j, const char* key, const std::string& def = {}) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return def;
    return it->get<std::string>();
}
// SHA-256 十六进制哈希的格式校验（64 个小写十六进制字符）：房间密码的传输格式
inline bool is_sha256_hex(const std::string& s) {
    if (s.size() != 64) return false;
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}
// 取数组字段；不是数组（或缺失）时返回空数组
inline json json_array(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return json::array();
    return *it;
}

// 单帧上限（防止恶意超长帧打爆内存）
inline constexpr uint32_t kMaxFrameSize = 8u * 1024u * 1024u;

// 模式 / 方向的字符串映射
const char* curve_mode_str(CurveMode m);
std::optional<CurveMode> curve_mode_from_str(const std::string& s);
const char* fire_dir_str(FireDir d);
std::optional<FireDir> fire_dir_from_str(const std::string& s);

// ---- 各类结构的编解码 ----
json vec2_to_json(const Vec2& v);
Vec2 vec2_from_json(const json& j);

json to_json(const CurveRequest& req);
std::optional<CurveRequest> curve_request_from_json(const json& j, Error& error);

json to_json(const Player& p);
json to_json(const Obstacle& ob);
// 配置分三个对象编解码：room（房间属性）、world（世界常量）、match（下一把配置）
json to_json(const RoomOptions& ro);
json to_json(const WorldConstants& wc);
json to_json(const MatchConfig& mc);          // seed 为空 → null
json to_json(const ResolvedMatchConfig& mr);  // 本局实际（seed 恒为实值）

// 反序列化（客户端从服务器消息重建本地配置；缺省字段用默认值）
RoomOptions room_options_from_json(const json& j);
WorldConstants world_constants_from_json(const json& j);
MatchConfig match_config_from_json(const json& j);  // seed: null → 空（每把随机）
ResolvedMatchConfig resolved_match_from_json(const json& j);
// 客户端从 room_state / game_start 的 room + world + match 三个对象合成完整配置
GameConfig config_from_json(const json& room_j, const json& world_j, const json& match_j);
Player player_from_json(const json& j);
Obstacle obstacle_from_json(const json& j);

// ---- 上行配置校验（服务器入口，全部带范围校验、失败不留半更新）----
// 创建房间的 room 对象：只认士兵数/坐标系（房间属性里仅有的两个玩家选项）；
// 出现 map_* 等世界常量字段回 bad_room_field——它们由代码固定，没有上行入口
Error apply_room_config_json(const json& j, RoomOptions& ro);
// 下一把配置（创建初值与 set_match_config 共用）；白名单字段 + 范围校验，
// 出现白名单之外的键回 unknown_match_field
Error apply_match_config_json(const json& j, MatchConfig& mc);
// 遮挡物密度对地图面积的交叉校验（创建与 set_match_config 改动后都要过一遍）
Error check_obstacle_density(const WorldConstants& wc, const MatchConfig& mc);
Error check_obstacle_density(const WorldConstants& wc, const ResolvedMatchConfig& mc);
// 字段名是否属于"创建时定死、之后不可改"的配置（房间属性 + 世界常量）：
// set_match_config 补丁里出现即拒绝（cannot_change_room）
bool is_room_config_key(const std::string& key);

// ============================================================================
// 通信协议（客户端与服务器共用的唯一事实来源；完整说明见项目根的 PROTOCOL.md）
//
// 客户端 → 服务器：
//   create_room {name?, room?, match?, trail?}
//                                  创建房间：room = 房间的属性（士兵数/坐标系，
//                                  唯一一次设置机会）；match = 下一把配置的初值
//   join_room {room_id, name?, spectate?}
//                                  用房间号加入房间；spectate=true 以观战者身份进入
//                                  （观战者对局进行中也能进，进后收到对局快照）
//   list_rooms {}                  索取房间列表
//   leave_room {}                  离开房间（对局进行中不允许）
//   set_spectate {spectate}        切换自己的身份：参战 <-> 观战（仅等待/局间）
//   add_ai {difficulty, team?}     房主添加人机（team: "A"/"B"/"auto"）
//   set_team {member_id, team}     房主调整成员队伍
//   kick {member_id}               房主把成员移出房间
//   start {}                       房主开始对局（用房间当前配置；种子为"随机"时每把换新图）
//   submit {request}               提交曲线（request 字段见 to_json(CurveRequest)）
//   set_match_config {match}       房主改"下一把"的规则（血量/时限/障碍/停顿/先手/种子等，
//                                  仅等待/局间允许；合并补丁，seed 传 null 表示"每把随机"）。
//                                  补丁里出现房间属性或世界常量字段一律拒绝（cannot_change_room）
//   vote_skip {vote?}              投票跳过当前地图（缺省 true；false = 撤回投票）；
//                                  全体参战人类同意后立即换一张随机新图（固定种子的
//                                  房间也只跳过这一把，下一把仍按模板种子）
//   function_preview {preview}     正在写的函数预览（仅当前回合玩家有效，≈100ms 限频）
//
// 服务器 → 客户端：
//   hello {conn}                              连接建立
//   error {code, message}                     请求失败：code 是稳定的机器可读标识
//                                             （小写下划线；客户端按码分支、多语言按码查文案表），
//                                             message 为中文说明（日志与默认展示）
//   room_list {rooms:[{id,players,in_game}]}  房间列表
//   joined {room_id, member_id, host}         已加入房间
//   left_room {} / kicked {}                  已离开 / 被移出
//   room_state {room, world, next_match, host, in_game, members:[...]}
//                                             房间与人员状态（room = 房间属性、world = 世界常量、
//                                             next_match = 下一把配置模板，seed 为 null 表示每把随机）
//   game_start {room, world, match, players:[...], obstacles:[...]} 对局开始的地图快照
//                                             （match = 本局实际配置，seed 恒为实值）
//   turn_begin {player_id, soldier, input_seconds, round?, next_player?, next_soldier?}
//                                             轮到某人的某个士兵输入
//   turn_skipped {player_id}                  某人超时空过
//   shot {id, owner, soldier, request, speed} 炮弹发射（轨迹由客户端本地重建）
//   projectiles {list:[{id,pos,travel,length}]} 飞行中的炮弹位置（约 20Hz）
//   hit {target, soldier, by, hp_left, pos}   命中某个士兵扣血（穿透后继续）
//   death {player_id, soldier, by}            某个士兵阵亡
//   crater {obstacle, pos}                    遮挡物被炸出坑
//   skip_vote {voted:[member_id], total}      跳过当前地图的投票状态（total = 参战人类数）
//   function_preview {member_id, preview}     某玩家正在写的函数（实时转播）
//   game_over {winner}                        结束（winner: 0=A 1=B 2=平局）
// ============================================================================

// 消息类型名（两端共用，避免字符串拼写错误）
namespace msg_type {
// 客户端 → 服务器
inline constexpr const char* kCreateRoom = "create_room";
inline constexpr const char* kJoinRoom = "join_room";
inline constexpr const char* kListRooms = "list_rooms";
inline constexpr const char* kLeaveRoom = "leave_room";
inline constexpr const char* kSetSpectate = "set_spectate";
inline constexpr const char* kAddAi = "add_ai";
inline constexpr const char* kSetTeam = "set_team";
inline constexpr const char* kKick = "kick";
inline constexpr const char* kStart = "start";
inline constexpr const char* kSubmit = "submit";
inline constexpr const char* kSetMatchConfig = "set_match_config";
inline constexpr const char* kVoteSkip = "vote_skip";
inline constexpr const char* kFunctionPreview = "function_preview";  // 上下行同名
// 预览请求：网页端不共享 core、无法本地采样，改请服务器算（C++ 客户端本地算，不用这条）
inline constexpr const char* kPreviewCurve = "preview_curve";
// 服务器 → 客户端
inline constexpr const char* kHello = "hello";
inline constexpr const char* kError = "error";
inline constexpr const char* kRoomList = "room_list";
inline constexpr const char* kJoined = "joined";
inline constexpr const char* kLeftRoom = "left_room";
inline constexpr const char* kKicked = "kicked";
inline constexpr const char* kRoomState = "room_state";
inline constexpr const char* kGameStart = "game_start";
inline constexpr const char* kTurnBegin = "turn_begin";
inline constexpr const char* kTurnSkipped = "turn_skipped";
inline constexpr const char* kShot = "shot";
inline constexpr const char* kProjectiles = "projectiles";
inline constexpr const char* kCurvePreview = "curve_preview";
inline constexpr const char* kHit = "hit";
inline constexpr const char* kDeath = "death";
inline constexpr const char* kCrater = "crater";
inline constexpr const char* kSkipVote = "skip_vote";
inline constexpr const char* kGameOver = "game_over";
}  // namespace msg_type

// 下行消息构造器：所有字段名只在这里出现，保证客户端与服务器永远一致
namespace msg {

// 飞行中炮弹的位置快照（约 20Hz 广播，客户端插值渲染）
struct ProjectileState {
    int id = 0;
    Vec2 pos;
    double travel = 0.0;
    double length = 0.0;
};

json make_hello(int conn_id);
// context 标明错误的来源域（"submit"=提交被拒、"leave"=离开房间被拒）：客户端据此
// 把"对某类请求的失败回应"与"无关错误"区分开（如只有提交被拒才复位本回合的提交
// 状态、只有退出观战被拒才恢复按钮）。空 = 不输出该字段
json make_error(const Error& e, const std::string& context = "");
json make_room_list(const json& rooms);
json make_joined(int room_id, int member_id, int host);
json make_left_room();
json make_kicked();
json make_room_member(int id, const std::string& name, bool is_ai, int difficulty,
                      const std::string& team, const std::string& role);
json make_room_state(const RoomOptions& room, const WorldConstants& world,
                     const MatchConfig& next_match, int host, bool in_game, json members);
// round 为中途观战快照场景下"当前进行到第几回合"（0 = 不带，正常开局用）
json make_game_start(const RoomOptions& room, const WorldConstants& world,
                     const ResolvedMatchConfig& match, const std::vector<Player>& players,
                     const std::vector<Obstacle>& obstacles, int round = 0);
// round 为服务器权威的回合号（中途观战者据此对齐显示）；0 = 不带该字段。
// soldier 是出手士兵序号；next_player/next_soldier 是"下一位出手者"（<0 表示不带）
json make_turn_begin(int player_id, int soldier, double input_seconds, int round = 0,
                     int next_player = -1, int next_soldier = 0);
json make_turn_skipped(int player_id);
json make_shot(int id, int owner, int owner_soldier, const CurveRequest& request, double speed,
               const Path& path);
json make_projectiles(const std::vector<ProjectileState>& list);
json make_hit(int target, int target_soldier, int by, int hp_left, const Vec2& pos);
json make_death(int player_id, int soldier, int by);
json make_crater(int obstacle_index, const Vec2& pos);
json make_skip_vote(const std::vector<int>& voted, int total);
json make_function_preview(int member_id, const std::string& preview);
json make_game_over(int winner);
// 预览采样结果：轨迹折线编码与 shot 的 trail 相同（见 make_shot 处注释）
json make_curve_preview(const Path& path);

}  // namespace msg

// ---- 帧 ----
std::string encode_frame(const json& msg);

enum class DecodeStatus { Incomplete, Ok, Error };
// 从缓冲区头部尝试取出一帧；Ok 时消费缓冲区并输出消息
DecodeStatus decode_frame(std::string& buffer, json& out, std::string& error);

}  // namespace fg
