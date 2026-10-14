#pragma once
// 房间与地图的规则配置。服务器、客户端本地人机共用同一份配置结构，
// 保证"联机"和"单机人机"跑的是同一套规则。
//
// 配置在类型上分三层，生命周期各不相同：
// - RoomOptions：房间的属性（玩家选项）。创建房间时确定，之后不可更改；
//   协议里唯一一次出现是在 create_room（修改路径在类型上就够不着它）
// - WorldConstants：世界常量（地图范围、物理参数）。服务器与客户端共用同一份
//   代码常量（kDefaultWorld），协议只下行（客户端渲染与本地预览采样要读）；
//   不做"全房间存一份"——测试与未来多地图另说时才构造副本
// - MatchConfig：下一把的配置。房主在房间里可改（等待/局间），下一局生效；
//   开局时解析成本局实际的 ResolvedMatchConfig（seed 等由无值变有值）交给 World
// GameConfig 是三者合起来的一份"一局完整配置"：World 跑一局、客户端渲染一局时用。

#include <cstdint>
#include <optional>

namespace fg {

// 曲线提交模式
enum class CurveMode {
    YofX,        // 直角坐标 y = f(x)
    XofY,        // 直角坐标 x = f(y)
    Polar,       // 极坐标 r = f(theta)
    Parametric,  // 参数方程 x(t), y(t)
    Ode1,        // 微分方程 dy/dx = f(x, y)
    Ode2,        // 微分方程 (x', y') = (f(t,x,y), g(t,x,y))
    Ypp,         // 二阶微分方程 y'' = f(x, y, y')（初斜率由发射角决定，借用原版玩法）
    Count,       // 哨兵：模式总数。各处表格与统计数组一律用 kCurveModeCount，
                 // 不手写数字（历史上漏同步直接崩过）
};
inline constexpr int kCurveModeCount = static_cast<int>(CurveMode::Count);

// 发射方向：只允许单向（不允许双向）。
// 参数方程与极坐标由参数递增方向天然决定，此字段被忽略。
enum class FireDir {
    Positive,  // 向右 / 向上 / 沿 +x
    Negative,  // 向左 / 向下 / 沿 -x
};

// 曲线自变量的参考系（房间属性）。只影响 y=f(x) 与 x=f(y) 两种直角模式：
// - Soldier：以出手士兵为原点——自变量从 0 起，函数值是相对士兵的偏移
// - Field：自变量是地图坐标（玩家照屏幕上刻度显示的数字写）——曲线按形状画出后
//   整体平移使起点落在士兵位置（y=f(x) 竖直平移 y0-f(x0)，x=f(y) 水平平移
//   x0-f(y0)，常数项同样被抵消）；镜像房间里取屏幕读到的负值，所见即所写。
// 极坐标/参数/微分方程三种模式的语义与参考系无关（见 curve.cpp 顶部注释）。
enum class CoordOrigin {
    Soldier,
    Field,
};

// 房间的属性（玩家选项）：创建房间时确定，之后不可更改。
// 协议里唯一一次出现是 create_room 的 room 对象；没有修改入口
struct RoomOptions {
    int soldiers_per_player = 3;     // 每个玩家的士兵数（1~4，全场统一）
    CoordOrigin coord_origin = CoordOrigin::Soldier;  // 曲线参考系，见枚举注释
};

// 世界常量：地图范围与物理参数。玩家不可改、协议不提供上行入口（只随房间状态下行）；
// 服务器与客户端共用 kDefaultWorld，不随房间存副本
struct WorldConstants {
    // 地图默认 48x30（曾为 120x80）：屏幕上一单位约 20 像素、每格 1 单位可数，
    // 玩家方块（0.9x0.9）清晰可见，便于对着网格推坐标
    double map_x_min = -24.0;        // 地图 x 下界
    double map_x_max = 24.0;         // 地图 x 上界
    double map_y_min = -15.0;        // 地图 y 下界
    double map_y_max = 15.0;         // 地图 y 上界
    double player_half_size = 0.45;  // 玩家方块的半边长（方块 0.9x0.9，命中判定用正方形）
    double projectile_speed = 12.0;  // 炮弹速度（单位/秒）
    double max_travel = 300.0;       // 炮弹最大行程（单位），超过即销毁
    double crater_radius = 1.2;      // 炮弹炸出的坑半径
};

// 默认世界常量：联机房间与单机都用这一份
inline const WorldConstants kDefaultWorld{};

// 实时预览只显示起点起这么长的一段轨迹（纯显示截断，与炮弹真实飞行无关）。
// 预览的用途是确认起步形态与方向；整条弹道也提前画出来，命中与否全剧透、就没意思了。
// 12 ≈ 炮弹（速度 12 单位/秒）飞 1 秒的轨迹
inline constexpr double kPreviewTravel = 12.0;

// 下一把的配置：房主在房间里可改（等待/局间），下一局生效。
// seed 用 optional 表达策略：空 = 每把随机（开局时生成实际种子写入本局配置）；
// 有值 = 固定种子
struct MatchConfig {
    std::optional<uint64_t> seed;    // 地图种子（空 = 每把随机）
    int hp = 1;                      // 每个士兵的血量（1 = 一击毙命）
    int obstacle_count = 15;         // 遮挡物数量（默认多放一点，纯开阔图上先手一发清场太无敌）
    double input_seconds = 120.0;    // 每回合输入时限（秒）
    double turn_delay = 3.0;         // 炮弹消失后到下一回合开始的展示停顿（秒，0 = 不停顿）
    bool mirror_view = false;        // 镜像视角：开启后 B 队人类玩家以"自己队在左"的镜像
                                     // 视角操作（屏幕左右翻转、提交自动按镜像坐标解释）；
                                     // 观战者始终中立视角
    int first_team = -1;             // 先手队伍：-1 = 由地图种子随机决定，0 = A 队，1 = B 队
};

// 本局实际配置：开局时由 MatchConfig 解析而来——seed 一定是实值（每把随机时由房间
// 在开局时生成写入）。World 与 game_start 快照都用它；之后的修改只作用于下一把模板
struct ResolvedMatchConfig {
    uint64_t seed = 12345;
    int hp = 1;
    int obstacle_count = 15;
    double input_seconds = 120.0;
    double turn_delay = 3.0;
    bool mirror_view = false;
    int first_team = -1;
};

// 一局的完整配置（房间属性 + 世界常量 + 本局实际配置）
struct GameConfig {
    RoomOptions room;
    WorldConstants world;
    ResolvedMatchConfig match;
};

// 从"本局实际配置"生成下一把模板（seed 实值 → 固定种子）：单机/自检把一次性选定的
// 参数交给房间时用。联机路径的房间从默认模板开始，不走这里
inline MatchConfig match_config_from_resolved(const ResolvedMatchConfig& r) {
    MatchConfig m;
    m.seed = r.seed;
    m.hp = r.hp;
    m.obstacle_count = r.obstacle_count;
    m.input_seconds = r.input_seconds;
    m.turn_delay = r.turn_delay;
    m.mirror_view = r.mirror_view;
    m.first_team = r.first_team;
    return m;
}

}  // namespace fg
