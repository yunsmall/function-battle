// 主题切换：把 ui_style.h 的色板（运行时变量）按主题批量改写。
// 三套主题性格不同，但语义色保持稳定——A 队永远是蓝、B 队永远是红、金/危险/成功
// 各自换主题后依然一眼可辨；换的只是底色、面板、文字与战场基础层
#include "ui_style.h"

namespace fg {

namespace {

// 一套主题的全部颜色；字段与 ui_style.h 的色板一一对应
struct Palette {
    Color bg_top, bg_bottom, panel, panel_soft, panel_border, accent, team_a, team_b;
    Color text_main, text_dim, text_faint, text_hover, on_accent, gold, danger, success;
    Color arena_bg, arena_fine, arena_mid, arena_grid, arena_axis, arena_tick;
    Color obstacle, obstacle_edge, unit_a, unit_b, unit_dead, name_alive, name_dead, preview,
        next_mark, projectile, path_faint, trail, spark, plate_bg, input_bg;
};

// 指定初始化器把字段名写全：加颜色时既不会漏初始化，也不会因顺序笔误而错位
const Palette kPalettes[] = {
    // 深空（默认）：深色底 + 青蓝强调
    {.bg_top = {12, 15, 23, 255},
     .bg_bottom = {21, 26, 40, 255},
     .panel = {25, 30, 45, 245},
     .panel_soft = {31, 37, 55, 255},
     .panel_border = {50, 58, 82, 255},
     .accent = {78, 168, 255, 255},
     .team_a = {78, 168, 255, 255},
     .team_b = {255, 107, 107, 255},
     .text_main = {228, 233, 245, 255},
     .text_dim = {138, 148, 172, 255},
     .text_faint = {96, 104, 124, 255},
     .text_hover = {255, 255, 255, 255},
     .on_accent = {12, 20, 34, 255},
     .gold = {255, 214, 92, 255},
     .danger = {255, 82, 82, 255},
     .success = {91, 217, 139, 255},
     .arena_bg = {15, 18, 28, 255},
     .arena_fine = {24, 28, 38, 255},
     .arena_mid = {32, 37, 50, 255},
     .arena_grid = {38, 42, 56, 255},
     .arena_axis = {70, 78, 100, 255},
     .arena_tick = {128, 138, 175, 255},
     .obstacle = {78, 86, 110, 255},
     .obstacle_edge = {110, 120, 150, 255},
     .unit_a = {70, 140, 255, 255},
     .unit_b = {255, 90, 90, 255},
     .unit_dead = {70, 70, 76, 255},
     .name_alive = {200, 200, 200, 255},
     .name_dead = {130, 130, 130, 255},
     .preview = {255, 214, 90, 255},
     .next_mark = {150, 195, 255, 210},
     .projectile = {255, 255, 255, 255},
     .path_faint = {255, 255, 255, 60},
     .trail = {255, 208, 120, 215},
     .spark = {255, 236, 190, 255},
     .plate_bg = {10, 12, 20, 170},
     .input_bg = {16, 20, 31, 255}},

    // 明亮：白天/投影场景用，浅底深字；战场上弹头与命中高光取深色才看得见
    {.bg_top = {238, 241, 247, 255},
     .bg_bottom = {226, 231, 240, 255},
     .panel = {252, 253, 255, 245},
     .panel_soft = {238, 241, 248, 255},
     .panel_border = {188, 196, 214, 255},
     .accent = {38, 120, 220, 255},
     .team_a = {38, 120, 220, 255},
     .team_b = {225, 72, 72, 255},
     .text_main = {28, 34, 50, 255},
     .text_dim = {92, 102, 126, 255},
     .text_faint = {144, 153, 172, 255},
     .text_hover = {10, 14, 24, 255},
     .on_accent = {255, 255, 255, 255},
     .gold = {186, 140, 16, 255},
     .danger = {210, 52, 52, 255},
     .success = {24, 156, 84, 255},
     .arena_bg = {246, 247, 250, 255},
     .arena_fine = {232, 235, 242, 255},
     .arena_mid = {219, 224, 234, 255},
     .arena_grid = {206, 212, 225, 255},
     .arena_axis = {118, 128, 152, 255},
     .arena_tick = {92, 102, 126, 255},
     .obstacle = {158, 166, 184, 255},
     .obstacle_edge = {118, 128, 150, 255},
     .unit_a = {35, 110, 235, 255},
     .unit_b = {222, 62, 62, 255},
     .unit_dead = {152, 152, 158, 255},
     .name_alive = {70, 78, 96, 255},
     .name_dead = {150, 152, 158, 255},
     .preview = {200, 148, 28, 255},
     .next_mark = {48, 108, 198, 210},
     .projectile = {40, 46, 60, 255},
     .path_faint = {40, 46, 60, 70},
     .trail = {200, 148, 28, 235},
     .spark = {180, 110, 30, 255},
     .plate_bg = {252, 253, 255, 205},
     .input_bg = {232, 236, 244, 255}},

    // 终端绿：黑底荧光绿的老式终端风
    {.bg_top = {4, 10, 6, 255},
     .bg_bottom = {8, 18, 10, 255},
     .panel = {10, 22, 13, 245},
     .panel_soft = {14, 30, 17, 255},
     .panel_border = {30, 70, 38, 255},
     .accent = {80, 220, 120, 255},
     .team_a = {90, 170, 255, 255},
     .team_b = {255, 100, 95, 255},
     .text_main = {180, 255, 190, 255},
     .text_dim = {110, 170, 120, 255},
     .text_faint = {76, 128, 88, 255},
     .text_hover = {220, 255, 225, 255},
     .on_accent = {6, 18, 10, 255},
     .gold = {240, 220, 80, 255},
     .danger = {255, 90, 80, 255},
     .success = {120, 240, 140, 255},
     .arena_bg = {6, 14, 8, 255},
     .arena_fine = {12, 26, 15, 255},
     .arena_mid = {18, 38, 22, 255},
     .arena_grid = {26, 52, 30, 255},
     .arena_axis = {60, 120, 70, 255},
     .arena_tick = {100, 160, 110, 255},
     .obstacle = {40, 80, 50, 255},
     .obstacle_edge = {70, 130, 80, 255},
     .unit_a = {70, 140, 255, 255},
     .unit_b = {255, 90, 90, 255},
     .unit_dead = {60, 80, 62, 255},
     .name_alive = {190, 255, 200, 255},
     .name_dead = {110, 140, 118, 255},
     .preview = {230, 220, 90, 255},
     .next_mark = {150, 235, 170, 210},
     .projectile = {230, 255, 235, 255},
     .path_faint = {180, 255, 190, 55},
     .trail = {230, 220, 90, 225},
     .spark = {255, 236, 160, 255},
     .plate_bg = {4, 16, 6, 185},
     .input_bg = {8, 20, 10, 255}},
};

const char* const kThemeNames[] = {"深空", "明亮", "终端绿"};
static_assert(sizeof(kThemeNames) / sizeof(kThemeNames[0]) ==
                  sizeof(kPalettes) / sizeof(kPalettes[0]),
              "主题名与主题表必须一一对应");
constexpr int kThemeCount = static_cast<int>(sizeof(kPalettes) / sizeof(kPalettes[0]));

int g_theme = 0;  // 当前主题（默认深空；初始色板与之相同，无需构造期改写）

}  // namespace

int theme_count() { return kThemeCount; }

const char* theme_name(int i) {
    return (i >= 0 && i < kThemeCount) ? kThemeNames[i] : kThemeNames[0];
}

int theme_index() { return g_theme; }

void apply_theme(int i) {
    g_theme = ((i % kThemeCount) + kThemeCount) % kThemeCount;  // 负数也安全绕回
    const Palette& p = kPalettes[g_theme];
    kBgTop = p.bg_top;
    kBgBottom = p.bg_bottom;
    kPanel = p.panel;
    kPanelSoft = p.panel_soft;
    kPanelBorder = p.panel_border;
    kAccent = p.accent;
    kTeamA = p.team_a;
    kTeamB = p.team_b;
    kTextMain = p.text_main;
    kTextDim = p.text_dim;
    kTextFaint = p.text_faint;
    kTextHover = p.text_hover;
    kOnAccent = p.on_accent;
    kGold = p.gold;
    kDanger = p.danger;
    kSuccess = p.success;
    kArenaBg = p.arena_bg;
    kArenaFine = p.arena_fine;
    kArenaMid = p.arena_mid;
    kArenaGrid = p.arena_grid;
    kArenaAxis = p.arena_axis;
    kArenaTick = p.arena_tick;
    kObstacle = p.obstacle;
    kObstacleEdge = p.obstacle_edge;
    kUnitA = p.unit_a;
    kUnitB = p.unit_b;
    kUnitDead = p.unit_dead;
    kNameAlive = p.name_alive;
    kNameDead = p.name_dead;
    kPreview = p.preview;
    kNextMark = p.next_mark;
    kProjectile = p.projectile;
    kPathFaint = p.path_faint;
    kTrail = p.trail;
    kSpark = p.spark;
    kPlateBg = p.plate_bg;
    kInputBg = p.input_bg;
}

}  // namespace fg
