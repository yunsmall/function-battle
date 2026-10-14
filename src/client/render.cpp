#include "render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

#include "ui_style.h"

namespace fg {

namespace {
// 弹坑/地图底色：必须与 ui_battle 里地图区域的底色一致（弹坑恢复"正常空地"时铺的就是
// 这层色，用其他深浅会看出色块接缝）
constexpr Color kBackground{15, 18, 28, 255};
constexpr Color kFine{24, 28, 38, 255};
constexpr Color kMid{32, 37, 50, 255};
constexpr Color kGrid{38, 42, 56, 255};
constexpr Color kAxis{70, 78, 100, 255};
constexpr Color kObstacle{78, 86, 110, 255};
constexpr Color kObstacleEdge{110, 120, 150, 255};
constexpr Color kTeamA{70, 140, 255, 255};
constexpr Color kTeamB{255, 90, 90, 255};
constexpr Color kDead{70, 70, 76, 255};
constexpr Color kPreview{255, 214, 90, 255};
constexpr Color kNextMark{150, 195, 255, 210};
constexpr Color kProjectile{255, 255, 255, 255};
constexpr Color kPathFaint{255, 255, 255, 60};
}  // namespace

View View::fit(const GameConfig& cfg, Rectangle area) {
    View v;
    v.wx_min = cfg.world.map_x_min;
    v.wx_max = cfg.world.map_x_max;
    v.wy_max = cfg.world.map_y_max;
    const double world_w = cfg.world.map_x_max - cfg.world.map_x_min;
    const double world_h = cfg.world.map_y_max - cfg.world.map_y_min;
    const float sx = area.width / static_cast<float>(world_w);
    const float sy = area.height / static_cast<float>(world_h);
    v.scale = std::min(sx, sy) * 0.96f;
    v.screen_x = area.x + (area.width - static_cast<float>(world_w) * v.scale) * 0.5f;
    v.screen_y = area.y + (area.height - static_cast<float>(world_h) * v.scale) * 0.5f;
    return v;
}

void draw_path(const View& view, const Path& path, Color color, float thickness) {
    if (path.points.size() < 2) return;
    // 屏幕空间抽稀：与上一个落笔点不足 2px 的中间点直接跳过——长曲线可达数万个点，
    // 逐点连线每帧要发上万条线段直吃帧率，而 2px 内的折线在视觉上无差别；
    // 末点必须落笔，保证终点位置准确
    Vector2 prev = view.to_screen(path.points.front());
    for (size_t i = 1; i < path.points.size(); ++i) {
        const Vector2 next = view.to_screen(path.points[i]);
        const bool last_point = (i + 1 == path.points.size());
        if (!last_point && std::abs(next.x - prev.x) < 2.0f &&
            std::abs(next.y - prev.y) < 2.0f) {
            continue;
        }
        // 段太长（跨大距离）时跳过，避免画到区域外造成视觉混乱
        if (std::abs(next.x - prev.x) < 2000.0f && std::abs(next.y - prev.y) < 2000.0f) {
            DrawLineEx(prev, next, thickness, color);
        }
        prev = next;
    }
}

namespace {

// 某一档网格线的颜色与线宽（0 是坐标轴最亮；10 的倍数主线；5 的倍数中线；其余细线）
void grid_line_style(int v, Color& color, float& width) {
    if (v == 0) {
        color = kAxis;
        width = 2.0f;
    } else if (v % 10 == 0) {
        color = kGrid;
        width = 1.0f;
    } else if (v % 5 == 0) {
        color = kMid;
        width = 1.0f;
    } else {
        color = kFine;
        width = 1.0f;
    }
}

void draw_grid(const View& view, const GameConfig& cfg) {
    // 网格三级：每 1 单位细线（玩家要数格子读坐标）、每 5 单位中线、每 10 单位主线，轴最亮
    for (int x = static_cast<int>(std::ceil(cfg.world.map_x_min)); x <= static_cast<int>(cfg.world.map_x_max);
         ++x) {
        const Vector2 a = view.to_screen(Vec2(x, cfg.world.map_y_min));
        const Vector2 b = view.to_screen(Vec2(x, cfg.world.map_y_max));
        Color color = kFine;
        float width = 1.0f;
        grid_line_style(x, color, width);
        DrawLineEx(a, b, width, color);
    }
    for (int y = static_cast<int>(std::ceil(cfg.world.map_y_min)); y <= static_cast<int>(cfg.world.map_y_max);
         ++y) {
        const Vector2 a = view.to_screen(Vec2(cfg.world.map_x_min, y));
        const Vector2 b = view.to_screen(Vec2(cfg.world.map_x_max, y));
        Color color = kFine;
        float width = 1.0f;
        grid_line_style(y, color, width);
        DrawLineEx(a, b, width, color);
    }
}

// 坐标刻度：没有数字玩家没法对着坐标写函数瞄准。
// x 值标在 x 轴下方（居中于刻度线），y 值标在 y 轴左侧（右对齐），每 5 单位一个
void draw_ticks(const View& view, const GameConfig& cfg, const std::map<int, Font>* fonts) {
    constexpr Color kTick{128, 138, 175, 255};
    const auto [font, fs] = pick_tier_font(fonts, 13.0f);
    const Vector2 origin = view.to_screen(Vec2(0.0, 0.0));
    char buf[16];
    auto label = [&](float cx, float cy, bool centered) {
        const Vector2 m = MeasureTextEx(font, buf, fs, 1.0f);
        const float x = centered ? cx - m.x * 0.5f : cx - m.x;
        const float y = cy - m.y * 0.5f;
        // 淡底牌：数字压到网格、障碍或名字上也能读
        DrawRectangleRounded(Rectangle{x - 4.0f, y - 2.0f, m.x + 8.0f, m.y + 4.0f}, 0.5f, 4,
                             Color{10, 12, 20, 170});
        DrawTextEx(font, buf, Vector2{x, y}, fs, 1.0f, kTick);
    };
    for (double x = std::ceil(cfg.world.map_x_min / 5.0) * 5.0; x <= cfg.world.map_x_max; x += 5.0) {
        // 镜像视角下显示的是取负的世界坐标。刻度数字是"世界坐标的展示"而不是
        // 表达式坐标：无论哪个视角，玩家读自己与目标的数字差即为表达式的相对坐标
        //（镜像下左右翻转，取负后符号正好对齐）；屏幕摆放由 to_screen 另行翻转，
        // 两者不必相等（地图范围是否对称都不影响这套读法）
        std::snprintf(buf, sizeof(buf), "%.0f", view.mirror ? -x : x);
        label(view.to_screen(Vec2(x, 0.0)).x, origin.y + 17.0f, true);
    }
    for (double y = std::ceil(cfg.world.map_y_min / 5.0) * 5.0; y <= cfg.world.map_y_max; y += 5.0) {
        if (std::abs(y) < 1e-9) continue;  // 0 交给 x 轴标注，避免两个 0 叠在一起
        std::snprintf(buf, sizeof(buf), "%.0f", y);
        label(origin.x - 8.0f, view.to_screen(Vec2(0.0, y)).y, false);
    }
}

// 点是否在障碍物形状内（画弹坑"炸裂截面"弧段时用；不看坑本身）
bool shape_contains(const Obstacle& ob, double x, double y) {
    if (ob.is_circle) {
        const double dx = x - ob.center.x();
        const double dy = y - ob.center.y();
        return dx * dx + dy * dy <= ob.radius * ob.radius;
    }
    return std::abs(x - ob.center.x()) <= ob.half_w && std::abs(y - ob.center.y()) <= ob.half_h;
}

// 该点是否已被"别的坑"炸成空地（重叠坑的断面弧不该画到空地上）
bool inside_other_crater(const std::vector<Vec2>& craters, size_t skip, double x, double y,
                         double radius) {
    for (size_t i = 0; i < craters.size(); ++i) {
        if (i == skip) continue;  // 本坑圆周到本坑中心的距离恰为半径，不跳过会整圈都不画
        const double dx = x - craters[i].x();
        const double dy = y - craters[i].y();
        if (dx * dx + dy * dy < radius * radius) return true;
    }
    return false;
}

// 点是否落在这些坑的洞里（"洞"的判定与物理一致：到坑心距离不超过坑半径）
bool in_crater_hole(const std::vector<Vec2>& craters, double x, double y, double radius) {
    for (const auto& c : craters) {
        const double dx = x - c.x();
        const double dy = y - c.y();
        if (dx * dx + dy * dy <= radius * radius) return true;
    }
    return false;
}

// 点是否被"别的障碍"的实体覆盖（重叠的障碍连成一体：藏在别的石头下的线不画）
bool covered_by_other(const std::vector<Obstacle>& obstacles, size_t self, double x, double y,
                      double crater_radius) {
    for (size_t i = 0; i < obstacles.size(); ++i) {
        if (i == self) continue;
        if (obstacle_blocks(obstacles[i], Vec2(x, y), crater_radius)) return true;
    }
    return false;
}

// "坑"的像素级剔除 shader：把"形状 − 坑的并集"直接按像素判定（在形状内、不在任何
// 坑内才输出颜色，否则 discard）——与网页端"离屏层逐坑抠洞"的同效果实现，洞的边界
// 是像素级精确的，不会有分块近似的锯齿。uniform 全部走世界坐标（含镜像视角的仿射），
// 坑最多传 64 个
const char* kHoleMaskFs = R"GLSL(
#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform float u_scr_h;      // 绘制目标高度（像素）：gl_FragCoord 的 y 向上，翻回屏幕坐标
uniform vec2 u_origin;      // 世界原点 (0,0) 的屏幕坐标
uniform vec2 u_scale;       // 世界→屏幕 每单位像素数（x 为负 = 镜像视角）
uniform int u_is_circle;
uniform vec4 u_shape;       // 圆: (cx, cy, r, _)；矩形: (cx, cy, half_w, half_h)
uniform int u_crater_count;
uniform vec3 u_craters[64]; // 每个坑 (x, y, 半径)
out vec4 finalColor;
void main() {
    vec2 scr = vec2(gl_FragCoord.x, u_scr_h - gl_FragCoord.y);
    vec2 w = vec2((scr.x - u_origin.x) / u_scale.x, (scr.y - u_origin.y) / u_scale.y);
    bool inside = (u_is_circle == 1)
        ? (distance(w, u_shape.xy) <= u_shape.z)
        : (abs(w.x - u_shape.x) <= u_shape.z && abs(w.y - u_shape.y) <= u_shape.w);
    if (!inside) discard;
    for (int i = 0; i < u_crater_count; i++) {
        vec3 c = u_craters[i];
        if (distance(w, c.xy) <= c.z) discard;
    }
    finalColor = fragColor;
}
)GLSL";

// 洞剔除 shader 的惰性加载与 uniform 位置缓存（进程内只加载一次）
struct HoleShader {
    Shader sh{};
    int scr_h = -1, origin = -1, scale = -1, is_circle = -1;
    int shape = -1, count = -1, craters = -1;
};
HoleShader& hole_shader() {
    static HoleShader hs;
    static bool tried = false;
    if (!tried) {
        tried = true;
        hs.sh = LoadShaderFromMemory(nullptr, kHoleMaskFs);
        if (hs.sh.id != 0) {
            hs.scr_h = GetShaderLocation(hs.sh, "u_scr_h");
            hs.origin = GetShaderLocation(hs.sh, "u_origin");
            hs.scale = GetShaderLocation(hs.sh, "u_scale");
            hs.is_circle = GetShaderLocation(hs.sh, "u_is_circle");
            hs.shape = GetShaderLocation(hs.sh, "u_shape");
            hs.count = GetShaderLocation(hs.sh, "u_crater_count");
            hs.craters = GetShaderLocation(hs.sh, "u_craters");
        } else {
            std::printf("洞剔除 shader 加载失败，带坑障碍退化为整块绘制\n");
        }
    }
    return hs;
}

// 形状"带洞"填充：shader 按像素剔除"坑的并集"（见 kHoleMaskFs 注释）。网格画在
// 障碍之前，洞区不画后网格自然透出
void fill_shape_with_holes(const View& view, const Obstacle& ob, const GameConfig& cfg) {
    // 屏幕包围盒（留 1px 余量接抗锯齿边）
    float minx, maxx, miny, maxy;
    if (ob.is_circle) {
        const Vector2 c = view.to_screen(ob.center);
        const float r = static_cast<float>(ob.radius * view.scale);
        minx = c.x - r;
        maxx = c.x + r;
        miny = c.y - r;
        maxy = c.y + r;
    } else {
        const Vector2 q0 =
            view.to_screen(Vec2(ob.center.x() - ob.half_w, ob.center.y() + ob.half_h));
        const Vector2 q1 =
            view.to_screen(Vec2(ob.center.x() + ob.half_w, ob.center.y() + ob.half_h));
        const Vector2 q2 =
            view.to_screen(Vec2(ob.center.x() + ob.half_w, ob.center.y() - ob.half_h));
        const Vector2 q3 =
            view.to_screen(Vec2(ob.center.x() - ob.half_w, ob.center.y() - ob.half_h));
        minx = std::min(std::min(q0.x, q1.x), std::min(q2.x, q3.x));
        maxx = std::max(std::max(q0.x, q1.x), std::max(q2.x, q3.x));
        miny = std::min(std::min(q0.y, q1.y), std::min(q2.y, q3.y));
        maxy = std::max(std::max(q0.y, q1.y), std::max(q2.y, q3.y));
    }
    const Rectangle rec{minx - 1.0f, miny - 1.0f, (maxx - minx) + 2.0f, (maxy - miny) + 2.0f};
    const HoleShader& hs = hole_shader();
    if (hs.sh.id == 0) {
        DrawRectangleRec(rec, kObstacle);  // 保底：整块画（洞看不见，形状还在）
        return;
    }
    const Vector2 o = view.to_screen(Vec2(0.0, 0.0));
    const Vector2 ex = view.to_screen(Vec2(1.0, 0.0));
    const Vector2 ey = view.to_screen(Vec2(0.0, 1.0));
    const float scr_h = static_cast<float>(GetScreenHeight());  // 绘制目标=窗口/整屏 RT
    const float origin[2] = {o.x, o.y};
    const float scale[2] = {ex.x - o.x, ey.y - o.y};
    const int is_circle = ob.is_circle ? 1 : 0;
    const float shape[4] = {static_cast<float>(ob.center.x()), static_cast<float>(ob.center.y()),
                            static_cast<float>(ob.is_circle ? ob.radius : ob.half_w),
                            static_cast<float>(ob.is_circle ? 0.0 : ob.half_h)};
    // 坑最多传 64 个：超了按"离障碍中心近"取——远处的坑在形状外，无可见影响
    constexpr size_t kMax = 64;
    std::vector<size_t> order(ob.craters.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    if (order.size() > kMax) {
        std::partial_sort(order.begin(), order.begin() + kMax, order.end(),
                          [&](size_t a, size_t b) {
                              return (ob.craters[a] - ob.center).squaredNorm() <
                                     (ob.craters[b] - ob.center).squaredNorm();
                          });
    }
    const int n = static_cast<int>(std::min(order.size(), kMax));
    float buf[kMax * 3] = {};
    for (int i = 0; i < n; ++i) {
        const Vec2& c = ob.craters[order[static_cast<size_t>(i)]];
        buf[i * 3 + 0] = static_cast<float>(c.x());
        buf[i * 3 + 1] = static_cast<float>(c.y());
        buf[i * 3 + 2] = static_cast<float>(cfg.world.crater_radius);
    }
    SetShaderValue(hs.sh, hs.scr_h, &scr_h, SHADER_UNIFORM_FLOAT);
    SetShaderValue(hs.sh, hs.origin, origin, SHADER_UNIFORM_VEC2);
    SetShaderValue(hs.sh, hs.scale, scale, SHADER_UNIFORM_VEC2);
    SetShaderValue(hs.sh, hs.is_circle, &is_circle, SHADER_UNIFORM_INT);
    SetShaderValue(hs.sh, hs.shape, shape, SHADER_UNIFORM_VEC4);
    SetShaderValue(hs.sh, hs.count, &n, SHADER_UNIFORM_INT);
    if (n > 0) SetShaderValueV(hs.sh, hs.craters, buf, SHADER_UNIFORM_VEC3, n);
    BeginShaderMode(hs.sh);
    DrawRectangleRec(rec, kObstacle);
    EndShaderMode();
}

// 形状轮廓：沿边缘分段画线，跳过两类段——① 被自己洞咬掉的；② 外侧被别的障碍盖住的
// （重叠的障碍算连成一体，中间不画边界线）。洞缘的断口线由 stroke_crater_rims 单独画
void stroke_shape_outline(const View& view, const std::vector<Obstacle>& obstacles, size_t idx,
                          const GameConfig& cfg) {
    const Obstacle& ob = obstacles[idx];
    const double hole_r = cfg.world.crater_radius;
    auto keep = [&](const Vec2& mid, const Vec2& outward) {
        if (in_crater_hole(ob.craters, mid.x(), mid.y(), hole_r)) return false;
        // 探针取外侧一点：另一边还是石头，说明这条线是重叠区里的内部边界
        const Vec2 probe = mid + outward * 0.15;
        return !covered_by_other(obstacles, idx, probe.x(), probe.y(), hole_r);
    };
    if (ob.is_circle) {
        constexpr int kSeg = 120;
        for (int i = 0; i < kSeg; ++i) {
            const double a0 = 2.0 * std::numbers::pi * i / kSeg;
            const double a1 = 2.0 * std::numbers::pi * (i + 1) / kSeg;
            const double am = 0.5 * (a0 + a1);
            const Vec2 outward(std::cos(am), std::sin(am));
            if (!keep(ob.center + outward * ob.radius, outward)) continue;
            const Vec2 p0 = ob.center + Vec2(std::cos(a0), std::sin(a0)) * ob.radius;
            const Vec2 p1 = ob.center + Vec2(std::cos(a1), std::sin(a1)) * ob.radius;
            DrawLineEx(view.to_screen(p0), view.to_screen(p1), 1.0f, kObstacleEdge);
        }
        return;
    }
    const Vec2 corners[4] = {
        Vec2(ob.center.x() - ob.half_w, ob.center.y() + ob.half_h),
        Vec2(ob.center.x() + ob.half_w, ob.center.y() + ob.half_h),
        Vec2(ob.center.x() + ob.half_w, ob.center.y() - ob.half_h),
        Vec2(ob.center.x() - ob.half_w, ob.center.y() - ob.half_h),
    };
    for (int e = 0; e < 4; ++e) {
        const Vec2 v0 = corners[e];
        const Vec2 v1 = corners[(e + 1) % 4];
        const Vec2 dir = v1 - v0;
        const double len = dir.norm();
        if (len < 1e-12) continue;
        // 外法向：两个候选里取离中心更远的那个（与顶点绕向无关）
        Vec2 n(-dir.y() / len, dir.x() / len);
        const Vec2 mid_edge = (v0 + v1) * 0.5;
        if ((mid_edge + n - ob.center).squaredNorm() <
            (mid_edge - n - ob.center).squaredNorm()) {
            n = Vec2(-n.x(), -n.y());
        }
        const int nseg = std::max(1, static_cast<int>(std::ceil(len / 0.1)));
        for (int i = 0; i < nseg; ++i) {
            const Vec2 p0 = v0 + dir * (static_cast<double>(i) / nseg);
            const Vec2 p1 = v0 + dir * (static_cast<double>(i + 1) / nseg);
            if (!keep((p0 + p1) * 0.5, n)) continue;
            DrawLineEx(view.to_screen(p0), view.to_screen(p1), 1.5f, kObstacleEdge);
        }
    }
}

// 弹坑洞缘的"炸裂截面"线：坑圆周落在障碍形状内的弧段（伸到空地的不画），再排除
// 落在本障碍别的坑里的（重叠洞）和盖在别的障碍实体下的（被后画的石头挡住）
void stroke_crater_rims(const View& view, const std::vector<Obstacle>& obstacles, size_t idx,
                        const GameConfig& cfg) {
    const Obstacle& ob = obstacles[idx];
    const double cr = cfg.world.crater_radius;
    constexpr int kSeg = 48;
    for (size_t k = 0; k < ob.craters.size(); ++k) {
        const Vec2& c = ob.craters[k];
        for (int i = 0; i < kSeg; ++i) {
            const double a0 = 2.0 * std::numbers::pi * i / kSeg;
            const double a1 = 2.0 * std::numbers::pi * (i + 1) / kSeg;
            const double am = 0.5 * (a0 + a1);
            const double ax = c.x() + cr * std::cos(am);
            const double ay = c.y() + cr * std::sin(am);
            if (!shape_contains(ob, ax, ay)) continue;
            if (inside_other_crater(ob.craters, k, ax, ay, cr)) continue;
            if (covered_by_other(obstacles, idx, ax, ay, cr)) continue;
            const Vector2 p0 =
                view.to_screen(Vec2(c.x() + cr * std::cos(a0), c.y() + cr * std::sin(a0)));
            const Vector2 p1 =
                view.to_screen(Vec2(c.x() + cr * std::cos(a1), c.y() + cr * std::sin(a1)));
            DrawLineEx(p0, p1, 1.0f, kObstacleEdge);
        }
    }
}

void draw_obstacles(const View& view, const std::vector<Obstacle>& obstacles,
                    const GameConfig& cfg) {
    for (size_t idx = 0; idx < obstacles.size(); ++idx) {
        const Obstacle& ob = obstacles[idx];
        // 填充：没被炸过的整块画（快路径）；带坑的"带洞画"，洞区留给网格透出
        if (ob.craters.empty()) {
            if (ob.is_circle) {
                const Vector2 c = view.to_screen(ob.center);
                DrawCircleV(c, static_cast<float>(ob.radius * view.scale), kObstacle);
            } else {
                // 镜像时矩形的两个角在屏幕上左右互换：统一取屏幕坐标的 min 当左上角
                const Vector2 c1 =
                    view.to_screen(Vec2(ob.center.x() - ob.half_w, ob.center.y() + ob.half_h));
                const Vector2 c2 =
                    view.to_screen(Vec2(ob.center.x() + ob.half_w, ob.center.y() - ob.half_h));
                DrawRectangleV(Vector2{std::min(c1.x, c2.x), std::min(c1.y, c2.y)},
                               Vector2{std::abs(c2.x - c1.x), std::abs(c2.y - c1.y)}, kObstacle);
            }
        } else {
            fill_shape_with_holes(view, ob, cfg);
        }
        stroke_shape_outline(view, obstacles, idx, cfg);
        stroke_crater_rims(view, obstacles, idx, cfg);
    }
}

void draw_player(const View& view, double half_size, const Player& p, bool is_me,
                 bool is_current, bool is_next, int next_soldier, const std::string* last_func,
                 const std::map<int, Font>* fonts, float time) {
    // 半边长与命中判定同源（cfg.world.player_half_size）：画得比判定大，玩家对着方块
    // "看起来会命中"的边缘瞄准却判打空，是每一发都在影响瞄准的显示错误
    const float half = static_cast<float>(view.scale * half_size);
    // 名字只画一次：挂在该玩家第一个存活士兵头上（全灭后挂在第一个士兵的残骸处）
    const Soldier* anchor = nullptr;
    for (const auto& s : p.soldiers) {
        if (s.alive) {
            anchor = &s;
            break;
        }
    }
    if (anchor == nullptr && !p.soldiers.empty()) anchor = &p.soldiers.front();
    for (size_t si = 0; si < p.soldiers.size(); ++si) {
        const Soldier& s = p.soldiers[si];
        const Vector2 c = view.to_screen(s.pos);
        const Color body = !s.alive ? kDead : (p.team == Team::A ? kTeamA : kTeamB);
        // 当前出手的士兵：呼吸光圈（一眼看出轮到谁）
        const bool cur = is_current && static_cast<int>(si) == p.current_soldier;
        if (cur && s.alive) {
            const float pulse = 0.5f + 0.5f * std::sin(time * 4.0f);
            DrawCircleV(c, half * 2.0f + pulse * 5.0f, Fade(kPreview, 0.16f + 0.20f * pulse));
        }
        const Rectangle rect{c.x - half, c.y - half, half * 2.0f, half * 2.0f};
        if (s.alive) {
            DrawRectangleRec(rect, Color{0, 0, 0, 90});  // 轻微投影
        }
        const Rectangle face{rect.x, rect.y - 1.0f, rect.width, rect.height};
        DrawRectangleRec(face, body);
        if (is_me) {
            DrawRectangleLinesEx(face, 2.0f, WHITE);
        } else if (cur) {
            DrawRectangleLinesEx(face, 2.0f, kPreview);
        }
        // 下一个出手的士兵：细环提示（提前知道下一发是谁打）
        if (is_next && s.alive && static_cast<int>(si) == next_soldier) {
            DrawCircleLinesV(c, half * 1.9f, kNextMark);
        }
        // 血量条
        if (s.max_hp > 1 && s.alive) {
            const float w = 30.0f;
            const float frac = static_cast<float>(s.hp) / static_cast<float>(s.max_hp);
            const Rectangle bar{c.x - w / 2, c.y + half + 4.0f, w, 5.0f};
            DrawRectangleRounded(bar, 0.5f, 4, Color{30, 33, 44, 255});
            if (frac > 0.0f) {
                DrawRectangleRounded(Rectangle{bar.x, bar.y, w * frac, bar.height}, 0.5f, 4,
                                     frac > 0.5f ? Color{90, 220, 120, 255}
                                                 : Color{235, 190, 80, 255});
            }
        }
    }
    if (anchor != nullptr) {
        // 名字（带半透明底牌，避免压在网格上看不清）+ 最近用过的函数（名字下方一行小字）
        const Vector2 c = view.to_screen(anchor->pos);
        const auto [font, fs] = pick_tier_font(fonts, 14.0f);
        const Vector2 m = MeasureTextEx(font, p.name.c_str(), fs, 1.0f);
        const bool has_fn = (last_func != nullptr && !last_func->empty());
        const auto [fn_font, fn_fs] = pick_tier_font(fonts, 12.0f);
        const Vector2 np{c.x - m.x * 0.5f,
                         c.y - half - fs - 9.0f - (has_fn ? fn_fs + 3.0f : 0.0f)};
        if (anchor->alive) {
            const float box_h = fs + 5.0f + (has_fn ? fn_fs + 4.0f : 0.0f);
            DrawRectangleRounded(Rectangle{np.x - 5.0f, np.y - 2.0f, m.x + 10.0f, box_h}, 0.5f, 5,
                                 Color{10, 12, 20, 165});
        }
        DrawTextEx(font, p.name.c_str(), np, fs, 1.0f, anchor->alive ? LIGHTGRAY : GRAY);
        if (has_fn) {
            const Vector2 fm = MeasureTextEx(fn_font, last_func->c_str(), fn_fs, 1.0f);
            DrawTextEx(fn_font, last_func->c_str(), Vector2{c.x - fm.x * 0.5f, np.y + fs + 2.0f},
                       fn_fs, 1.0f, Color{150, 158, 180, 235});
        }
    }
}

}  // namespace

void draw_scene(const View& view, const RenderScene& scene) {
    if (!scene.cfg) return;

    // 炮弹的完整轨迹（淡色，帮助理解弹道）
    if (scene.projectile_path) {
        draw_path(view, *scene.projectile_path, kPathFaint, 1.0f);
    }
    draw_grid(view, *scene.cfg);
    if (scene.obstacles) {
        draw_obstacles(view, *scene.obstacles, *scene.cfg);
    }
    // 预览轨迹：先画宽而淡的一层做出辉光，再叠细实色
    if (scene.preview) {
        draw_path(view, *scene.preview, Fade(kPreview, 0.22f), 7.0f);
        draw_path(view, *scene.preview, kPreview, 2.2f);
    }
    // 玩家
    if (scene.players) {
        for (const auto& p : *scene.players) {
            const std::string* fn = nullptr;
            if (scene.last_func != nullptr) {
                const auto it = scene.last_func->find(p.id);
                if (it != scene.last_func->end()) fn = &it->second;
            }
            draw_player(view, scene.cfg->world.player_half_size, p, p.id == scene.my_id,
                        p.id == scene.current_player, p.id == scene.next_player,
                        scene.next_soldier, fn, scene.fonts, scene.time);
        }
    }
    // 炮弹：外圈辉光 + 亮核
    if (scene.has_projectile) {
        const Vector2 c = view.to_screen(scene.projectile);
        DrawCircleV(c, 11.0f, Fade(Color{255, 226, 150, 255}, 0.18f));
        DrawCircleV(c, 6.5f, Fade(kProjectile, 0.55f));
        DrawCircleV(c, 3.5f, kProjectile);
    }
    // 刻度最后画：任何元素都不遮住坐标数字（信息优先）
    draw_ticks(view, *scene.cfg, scene.fonts);
}

}  // namespace fg
