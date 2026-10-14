#pragma once
// 客户端界面的共享样式与文案：配色、曲线模式表、全部界面文案。
// 字体加载（ui_common.cpp）会扫描 kUiTexts 提取需要的字形；界面文案统一在这里登记，
// 新加文案时记得同步添加，否则字体会缺字形显示成占位符（核验脚本见项目开发记录）。

#include <cmath>
#include <map>
#include <utility>

#include <raylib.h>

#include "config.h"

namespace fg {

inline constexpr int kWinW = 1280;
inline constexpr int kWinH = 800;

// 字体档位：UI 与场景文字用到的全部显示字号。每个档位单独一张字形图集（ui_common.cpp
// 加载），绘制时取"最近的档"并按档位自身的基字号渲染（缩放=1）——小字不再是从大图集
// 缩水来的，笔画才不会糊成一团
inline constexpr int kFontTiers[] = {12, 14, 16, 18, 20, 22, 24, 28, 32, 40, 64};

// 取显示 size 字号要用的字体与绘制尺寸。命中档位 → 档位图集（绘制尺寸=基字号，缩放=1）；
// null/空表（字体加载全失败）→ raylib 默认字体，按请求字号缩放。
// 返回 Font 拷贝：拷贝只占几十字节且共享图集数据，同时避免引用回退字体的悬垂
inline std::pair<Font, float> pick_tier_font(const std::map<int, Font>* fonts, float size) {
    if (fonts == nullptr || fonts->empty()) return {GetFontDefault(), size};
    auto best = fonts->begin();
    for (auto it = fonts->begin(); it != fonts->end(); ++it) {
        if (std::abs(it->first - size) < std::abs(best->first - size)) best = it;
    }
    return {best->second, static_cast<float>(best->first)};
}

// ---- 主题配色（深色底 + 青蓝/珊瑚红队伍色）----
inline constexpr Color kBgTop{12, 15, 23, 255};
inline constexpr Color kBgBottom{21, 26, 40, 255};
inline constexpr Color kPanel{25, 30, 45, 245};
inline constexpr Color kPanelSoft{31, 37, 55, 255};
inline constexpr Color kPanelBorder{50, 58, 82, 255};
inline constexpr Color kAccent{78, 168, 255, 255};
inline constexpr Color kTeamA{78, 168, 255, 255};
inline constexpr Color kTeamB{255, 107, 107, 255};
inline constexpr Color kTextMain{228, 233, 245, 255};
inline constexpr Color kTextDim{138, 148, 172, 255};
inline constexpr Color kTextFaint{96, 104, 124, 255};
inline constexpr Color kGold{255, 214, 92, 255};
inline constexpr Color kDanger{255, 82, 82, 255};
inline constexpr Color kSuccess{91, 217, 139, 255};

// ---- 模式表 ----
inline constexpr const char* kModeNames[] = {"y = f(x)",  "x = f(y)",        "r = f(t)",
                                             "参数方程",   "dy/dx = f(x,y)", "(x',y') = (f,g)",
                                             "y'' = f(x,y,dy)"};
inline constexpr CurveMode kModes[] = {CurveMode::YofX, CurveMode::XofY, CurveMode::Polar,
                                       CurveMode::Parametric, CurveMode::Ode1, CurveMode::Ode2,
                                       CurveMode::Ypp};
inline constexpr int kModeCount = static_cast<int>(sizeof(kModes) / sizeof(kModes[0]));
// 模式表与 core 的数量必须一致：加模式时忘改任何一张表在这里编译失败，而不是运行时崩
static_assert(sizeof(kModeNames) / sizeof(kModeNames[0]) == sizeof(kModes) / sizeof(kModes[0]),
              "kModeNames 与 kModes 必须一一对应");
static_assert(sizeof(kModes) / sizeof(kModes[0]) == static_cast<size_t>(kCurveModeCount),
              "kModes 与 core 的 kCurveModeCount 必须一致");

inline bool needs_expr_b(CurveMode m) {
    return m == CurveMode::Parametric || m == CurveMode::Ode2;
}
inline bool needs_range(CurveMode m) {
    return m == CurveMode::Polar || m == CurveMode::Parametric;
}
inline const char* expr_a_label(CurveMode m) {
    switch (m) {
        case CurveMode::YofX: return "y =";
        case CurveMode::XofY: return "x =";
        case CurveMode::Polar: return "r =";
        case CurveMode::Parametric: return "x(t) =";
        case CurveMode::Ode1: return "dy/dx =";
        case CurveMode::Ode2: return "x'(t,x,y) =";
        case CurveMode::Ypp: return "y'' =";
        case CurveMode::Count: break;  // 哨兵值，落到下面的兜底
    }
    return "f =";
}
inline const char* expr_b_label(CurveMode m) {
    return m == CurveMode::Parametric ? "y(t) =" : "y'(t,x,y) =";
}
inline const char* var_hint(CurveMode m) {
    switch (m) {
        case CurveMode::YofX:
            return "变量 x ｜ 支持 + - * / ^ sin cos tan exp log sqrt pow pi e 等";
        case CurveMode::XofY:
            return "变量 y ｜ 支持 + - * / ^ sin cos tan exp log sqrt pow pi e 等";
        case CurveMode::Polar: return "变量 t（角度）｜ 曲线从你出发";
        case CurveMode::Parametric: return "变量 t ｜ 曲线从你出发（常数项无影响）";
        case CurveMode::Ode1: return "变量 x, y ｜ 从你的位置开始积分";
        case CurveMode::Ode2: return "变量 t, x, y ｜ 从你的位置开始积分";
        case CurveMode::Ypp: return "变量 x, y, dy ｜ 从你的位置开始积分，初速由发射角决定";
        case CurveMode::Count: break;  // 哨兵值，落到下面的兜底
    }
    return "";
}

// ---- 方向按钮文案 ----
inline constexpr const char* kDirNames[][2] = {
    {"向右", "向左"},    // YofX
    {"向上", "向下"},    // XofY
    {"", ""},            // Polar（参数递减即可反向，无需选择）
    {"", ""},            // Parametric
    {"沿 +x", "沿 -x"},  // Ode1
    {"", ""},            // Ode2
    {"沿 +x", "沿 -x"},  // Ypp（初斜率方向随发射方向取号）
};
static_assert(sizeof(kDirNames) / sizeof(kDirNames[0]) == sizeof(kModes) / sizeof(kModes[0]),
              "kDirNames 与 kModes 必须一一对应");

// ---- 所有界面文案（字体加载时从中提取需要的字形）----
inline constexpr const char* kUiTexts[] = {
    "函数对战", "数学弹道对战 · MATH BATTLE", "单机人机", "联机对战", "退出游戏",
    "每队人数", "人机难度", "简单", "普通", "困难", "每人血量", "一击毙命", "发子弹",
    "遮挡物数量", "地图种子", "留空则随机", "开始对局", "返回主菜单", "配置",
    "服务器地址", "连接服务器", "断开连接", "房间列表", "暂无房间", "创建房间", "刷新", "加入",
    "房间", "人数", "等待中", "进行中", "房间成员", "房主", "添加人机", "难度",
    "加入 A 队", "加入 B 队", "换到 A 队", "换到 B 队", "踢出", "离开房间", "开始",
    "你是房主，可调整阵容后开始", "等待房主开始对局…", "已连接", "未连接",
    "第", "回合", "轮到", "的回合", "你的回合！", "思考中…", "剩余", "秒", "提交函数",
    "方向", "参数范围", "到", "预览", "A 队", "B 队", "人机", "你", "阵亡", "血量",
    "已离开房间", "被移出房间", "胜利", "失败", "平局", "对局结束", "再来一局",
    "返回菜单", "观战中…", "退出观战", "等待对手…", "共", "滚轮查看", "表达式不能为空", "曲线无效：", "提交失败：", "未知错误",
    "等待开始", "不能为空", "查看", "游戏进行中，阵容已锁定", "选择曲线类型后输入表达式",
    "先手队伍", "A 队先手", "B 队先手",
    "创建房间 · 地图设置", "地图设置", "地图大小", "小", "标准", "大", "留空 = 每把随机",
    "下一把配置", "每把随机", "固定种子", "固定种子填这里", "应用", "上一把：",
    "每开一把自动换新地图", "固定种子：每把同一张地图", "你是房主", "你是队员",
    "进入房间后还可以调整队伍与每一把的规则", "改完点\"开始游戏\"，对下一把生效",
    "只有房主可以修改（现在只读）", "等待房主", "返回准备界面",
    "进入房间后可以：", "· 调整队伍与阵容，添加人机",
    "· 设置下一把的规则（先手、血量、时限）", "· 点\"开始游戏\"进入对局",
    "· 打完一把回到这里，改完配置再开一局",
    // 函数帮助面板（入口按钮、标题、分组说明与用法规则）
    "函数帮助", "关闭", "表达式里可用的全部内容（与 sin、cos 等随意嵌套组合）",
    "基本运算", "+  -  *  /  ^（乘方）和括号 ( )",
    "三角", "双曲", "指数对数", "exp 即 e^x，pow(a,b) 为 a 的 b 次方",
    "取整符号", "sgn 取正负号（-1/0/1）", "多参数", "如 max(x,3)、hypot(a,b)、atan2(y,x)",
    "误差", "阶跃", "x<0 得 0，x≥0 得 1；step 是别名",
    "常量", "pi（圆周率）    e（自然常数）",
    "变量", "y=f(x)→x     x=f(y)→y    r=f(t)→theta",
    "参数方程→t    dy/dx→x,y    (x',y')→t,x,y",
    "坐标系", "直角 / 极坐标 / 参数：以你为原点，自变量从 0 起，另一轴是相对你的偏移",
    "微分方程：x、y 是地图坐标，从你所在的位置开始积分",
    "平移", "曲线整体平移使起点落在你的位置；加任意常数不影响弹道",
    "示例", "2*sin(0.5*x) + 0.2*x", "5*step(x-10)（前进 10 后抬升）",
    // 观战（成员身份与入口）
    "观战", "观战者", "转为观战", "转为参战", "对局身份", "参战",
    "（观战中）", "（只看人机互打，你不在队伍里）",
    // 借原版玩法的批次：镜像/实时预览/跳过投票/多士兵/ypp 等新文案
    "跳过地图", "已投跳过", "正在写：", "视角：镜像", "视角：常规",
    "发射角(度)", "每人士兵数", "展示停顿", "创建后不可更改", "（创建时定，不可更改）",
    "变量 x, y, dy ｜ 从你的位置开始积分，初速由发射角决定",
    "y''：dy 是当前斜率，初值由发射角决定（度，正数朝发射方向上方偏）",
    "参数方程→t    dy/dx→x,y    (x',y')→t,x,y    y''→x,y,dy",
    // 帮助面板与坐标系选择处的现行文案（ui_battle.cpp / ui_lobby.cpp / ui_menu.cpp）：
    // 整句登记保证界面首帧就完整（text() 的自动补登记只兜底漏网字，会闪一帧占位符）
    "只影响 y=f(x) 与 x=f(y)：本房间以你为原点（x=0、y=0 就在你身上）",
    "只影响 y=f(x) 与 x=f(y)：本房间按地图坐标（函数里的数字就是地图上的）",
    "y=f(x)：曲线只做上下平移，使 x=0 处的点正好落在你身上",
    "x=f(y)：曲线只做左右平移，使 y=0 处的点正好落在你身上",
    "y=f(x)：只做上下平移——往上平移 y0-f(x0) 后压到你身上",
    "x=f(y)：只做左右平移——往右平移 x0-f(y0) 后压到你身上。(x0,y0) 是你的位置",
    "其他模式不受影响：极坐标 / 参数方程从你出发；微分方程本来就按地图坐标",
    "以你为原点：函数从你写起，0 就在你身上",
    "按地图坐标写：曲线整体平移到你身上（y=f(x) 上下、x=f(y) 左右）", "大神",
    "曲线会被整体平移，让出发点正好落在你身上；加任意常数（如 +5）不影响弹道",
    // 兜底用字：零散出现在提示句、括号与控制台输出里，集中列出保证字形完整
    "。、上下与个二从似位保分变右名响多字存客家左常影循总我或截户打持支显本检次死汇沿点玩环疑示积程端终自要角起这里需项（）｜跳过",
};

}  // namespace fg
