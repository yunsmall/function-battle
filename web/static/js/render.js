// 战场渲染：src/client/render.cpp 的逐项移植。网页端没有 raylib，这里用 Canvas 画同样的东西：
// 颜色常量、坐标变换（View::fit / to_screen）、折线抽稀、绘制顺序与尺寸全部以 C++ 客户端为准，
// 两边看起来必须一样（改显示先改 C++，再回来同步这里）。
// 不持有游戏状态：每帧从 app 传入的 scene 现算现画；命中、血量、胜负一律以服务器事件为准。
(function (global) {
    'use strict';

    // 与 render.cpp 内的常量一一对应（含 raylib 的 LIGHTGRAY/GRAY 展开值），值照抄
    var COL = {
        background: [15, 18, 28],
        grid: [38, 42, 56],
        axis: [70, 78, 100],
        obstacle: [78, 86, 110],
        obstacleEdge: [110, 120, 150],
        teamA: [70, 140, 255],
        teamB: [255, 90, 90],
        dead: [70, 70, 76],
        preview: [255, 214, 90],
        nextMark: [150, 195, 255, 210],
        projectile: [255, 255, 255],
        pathFaint: [255, 255, 255, 60],
        trail: [255, 208, 120, 215],
        spark: [255, 236, 190, 255],
        plateBg: [8, 10, 18, 170],
        fine: [24, 28, 38],
        mid: [32, 37, 50],
        tick: [128, 138, 175],
        nameAlive: [200, 200, 200],   // raylib LIGHTGRAY
        nameDead: [130, 130, 130],    // raylib GRAY
        namePlate: [10, 12, 20, 165],
        lastFunc: [150, 158, 180, 235],
        tickPlate: [10, 12, 20, 170],
        shadow: [0, 0, 0, 90],
        hpBack: [30, 33, 44],
        hpHigh: [90, 220, 120],
        hpLow: [235, 190, 80],
    };

    var FONT = 'system-ui, "Microsoft YaHei", "PingFang SC", sans-serif';

    // ---- 配色主题（与 C++ 客户端 ui_theme.cpp 的三套预设对齐）----
    // COL 是运行时可变对象：换主题 = 先恢复基底再覆盖"色系"字段。语义色（队伍/金/
    // 血量）跨主题保持性格——A 队永远蓝、B 队永远红——只调底色、网格、障碍与文字系
    var COL_BASE = {};
    for (var _bk in COL) COL_BASE[_bk] = COL[_bk];
    var THEME_OVERRIDES = {
        dark: {},  // 深空 = 基底原值
        bright: {
            background: [246, 247, 250], fine: [232, 235, 242], mid: [219, 224, 234],
            grid: [206, 212, 225], axis: [118, 128, 152], tick: [92, 102, 126],
            obstacle: [158, 166, 184], obstacleEdge: [118, 128, 150],
            teamA: [35, 110, 235], teamB: [222, 62, 62], dead: [152, 152, 158],
            preview: [200, 148, 28], nextMark: [48, 108, 198, 210],
            projectile: [40, 46, 60], pathFaint: [40, 46, 60, 70], trail: [200, 148, 28, 235],
            nameAlive: [70, 78, 96], nameDead: [150, 152, 158],
            namePlate: [252, 253, 255, 205], tickPlate: [252, 253, 255, 205],
            lastFunc: [110, 120, 140, 235], hpBack: [224, 228, 236],
            hpHigh: [24, 156, 84], hpLow: [186, 140, 16],
            plateBg: [252, 253, 255, 205],
            spark: [180, 110, 30, 255],
        },
        terminal: {
            background: [6, 14, 8], fine: [12, 26, 15], mid: [18, 38, 22],
            grid: [26, 52, 30], axis: [60, 120, 70], tick: [100, 160, 110],
            obstacle: [40, 80, 50], obstacleEdge: [70, 130, 80],
            dead: [60, 80, 62], preview: [230, 220, 90],
            nextMark: [150, 235, 170, 210], projectile: [230, 255, 235],
            pathFaint: [180, 255, 190, 55], trail: [230, 220, 90, 225],
            nameAlive: [190, 255, 200], nameDead: [110, 140, 118],
            namePlate: [4, 16, 6, 185], tickPlate: [4, 16, 6, 185],
            lastFunc: [120, 180, 130, 235], hpBack: [10, 24, 13],
            hpHigh: [120, 240, 140], hpLow: [240, 220, 80],
            plateBg: [4, 16, 6, 185],
            spark: [255, 236, 160, 255],
        },
    };
    function setTheme(name) {
        var k;
        for (k in COL_BASE) COL[k] = COL_BASE[k];
        var ov = THEME_OVERRIDES[name] || THEME_OVERRIDES.dark;
        for (k in ov) COL[k] = ov[k];
    }

    function css(c) {
        if (c.length < 4) return 'rgb(' + c[0] + ',' + c[1] + ',' + c[2] + ')';
        return 'rgba(' + c[0] + ',' + c[1] + ',' + c[2] + ',' + (c[3] / 255).toFixed(4) + ')';
    }

    // 颜色的 RGB + 指定 alpha（COL 里 3/4 分量的都有：必须先归一，直接取 c[3] 会得 NaN）
    function rgbA(c, alpha) {
        return [c[0], c[1], c[2], Math.round(alpha)];
    }

    function createRenderer(canvas) {
        var ctx = canvas.getContext('2d');
        var W = 0, H = 0, dpr = 1;

        function resize() {
            dpr = global.devicePixelRatio || 1;
            W = canvas.clientWidth;
            H = canvas.clientHeight;
            canvas.width = Math.max(1, Math.round(W * dpr));
            canvas.height = Math.max(1, Math.round(H * dpr));
        }

        // ---- raylib 绘图原语的等价物 ----
        function circle(x, y, r, col) {
            if (!(r > 0)) return;
            ctx.fillStyle = css(col);
            ctx.beginPath();
            ctx.arc(x, y, r, 0, Math.PI * 2);
            ctx.fill();
        }

        function circleLines(x, y, r, col, lw) {
            if (!(r > 0)) return;
            ctx.strokeStyle = css(col);
            ctx.lineWidth = lw;
            ctx.beginPath();
            ctx.arc(x, y, r, 0, Math.PI * 2);
            ctx.stroke();
        }

        function fillRect(x, y, w, h, col) {
            ctx.fillStyle = css(col);
            ctx.fillRect(x, y, w, h);
        }

        function strokeRect(x, y, w, h, col, lw) {
            ctx.strokeStyle = css(col);
            ctx.lineWidth = lw;
            ctx.strokeRect(x, y, w, h);
        }

        function line(x1, y1, x2, y2, col, lw) {
            ctx.strokeStyle = css(col);
            ctx.lineWidth = lw;
            ctx.beginPath();
            ctx.moveTo(x1, y1);
            ctx.lineTo(x2, y2);
            ctx.stroke();
        }

        // View::fit + View::to_screen 的移植：scale = min(宽适配, 高适配) * 0.96，地图在画布内居中；
        // 镜像视角世界 x 先变成 wx_min+wx_max-x（左右翻转，不是绕原点取负）
        function fitView(cfg, mirror) {
            var worldW = cfg.map_x_max - cfg.map_x_min;
            var worldH = cfg.map_y_max - cfg.map_y_min;
            var T = {
                mirror: !!mirror,
                wx_min: cfg.map_x_min,
                wx_max: cfg.map_x_max,
                wy_max: cfg.map_y_max,
                scale: Math.min(W / worldW, H / worldH) * 0.96,
                sx: 0,
                sy: 0,
            };
            T.sx = (W - worldW * T.scale) * 0.5;
            T.sy = (H - worldH * T.scale) * 0.5;
            T.toScreen = function (wx, wy) {
                var x = this.mirror ? (this.wx_min + this.wx_max - wx) : wx;
                return {
                    x: this.sx + (x - this.wx_min) * this.scale,
                    y: this.sy + (this.wy_max - wy) * this.scale,
                };
            };
            return T;
        }

        // draw_path 的移植：屏幕空间抽稀——与上一落笔点不足 2px 的中间点跳过（长曲线可达
        // 数千点，逐点连线白耗帧率）；跨 2000px 以上的段跳过（断开而不是补出一条线）；
        // 末点必落笔，保证终点位置准确
        function drawPath(T, points, col, thickness) {
            if (!points || points.length < 2) return;
            ctx.strokeStyle = css(col);
            ctx.lineWidth = thickness;
            ctx.beginPath();
            var prev = T.toScreen(points[0].x, points[0].y);
            ctx.moveTo(prev.x, prev.y);
            for (var i = 1; i < points.length; i++) {
                var next = T.toScreen(points[i].x, points[i].y);
                var last = (i === points.length - 1);
                if (!last && Math.abs(next.x - prev.x) < 2 && Math.abs(next.y - prev.y) < 2) {
                    continue;
                }
                if (Math.abs(next.x - prev.x) < 2000 && Math.abs(next.y - prev.y) < 2000) {
                    ctx.lineTo(next.x, next.y);
                } else {
                    ctx.moveTo(next.x, next.y);
                }
                prev = next;
            }
            ctx.stroke();
        }

        // 画折线到弧长 s 为止（含段内插值点）：炮弹"已飞过"的一段加亮用它。
        // 已飞过段通常不长，不做抽稀（抽稀是给整条长轨迹用的）
        function drawPathUpTo(T, path, s, col, thickness) {
            var pts = path.points;
            if (!pts || pts.length < 2) return;
            var seg = path.segmentAt(s);
            ctx.strokeStyle = css(col);
            ctx.lineWidth = thickness;
            ctx.beginPath();
            var prev = T.toScreen(pts[0].x, pts[0].y);
            ctx.moveTo(prev.x, prev.y);
            for (var i = 1; i <= seg[0]; i++) {
                var n = T.toScreen(pts[i].x, pts[i].y);
                ctx.lineTo(n.x, n.y);
            }
            var a = pts[seg[0]];
            var b = pts[Math.min(seg[0] + 1, pts.length - 1)];
            var e = T.toScreen(a.x + (b.x - a.x) * seg[1], a.y + (b.y - a.y) * seg[1]);
            ctx.lineTo(e.x, e.y);
            ctx.stroke();
        }

        // 流动虚线（C++ draw_path_dashed 的移植）：按屏幕弧长切 dash/gap、相位随时间滚动，
        // 让预览曲线"活"起来并指示炮弹飞行方向。抽稀语义与 drawPath 一致（跳过点不推进
        // 落笔点）；线条相位用负的 lineDashOffset 表达——虚线沿路径前进方向流动
        function drawDashedPath(T, points, col, thickness, dash, gap, phase) {
            if (!points || points.length < 2) return;
            var period = dash + gap;
            ctx.save();
            ctx.strokeStyle = css(col);
            ctx.lineWidth = thickness;
            ctx.setLineDash([dash, gap]);
            ctx.lineDashOffset = -(phase % period);
            ctx.beginPath();
            var prev = T.toScreen(points[0].x, points[0].y);
            ctx.moveTo(prev.x, prev.y);
            for (var i = 1; i < points.length; i++) {
                var next = T.toScreen(points[i].x, points[i].y);
                var last = (i === points.length - 1);
                if (!last && Math.abs(next.x - prev.x) < 2 && Math.abs(next.y - prev.y) < 2) {
                    continue;
                }
                if (Math.abs(next.x - prev.x) < 2000 && Math.abs(next.y - prev.y) < 2000) {
                    ctx.lineTo(next.x, next.y);
                } else {
                    ctx.moveTo(next.x, next.y);
                }
                prev = next;
            }
            ctx.stroke();
            ctx.restore();
        }

        // draw_grid 的移植：每 1 单位细线（玩家要数格子读坐标）、每 5 中线、每 10 主线，轴 2px 最亮
        // 某档网格线的颜色与线宽（与 C++ 的 grid_line_style 一致）
        function gridLineStyle(v) {
            if (v === 0) return [COL.axis, 2];
            if (v % 10 === 0) return [COL.grid, 1];
            if (v % 5 === 0) return [COL.mid, 1];
            return [COL.fine, 1];
        }

        function drawGrid(T, cfg) {
            for (var x = Math.ceil(cfg.map_x_min); x <= cfg.map_x_max; x++) {
                var sx = T.toScreen(x, 0).x;
                var y0 = T.toScreen(x, cfg.map_y_min).y;
                var y1 = T.toScreen(x, cfg.map_y_max).y;
                var st = gridLineStyle(x);
                line(sx, y0, sx, y1, st[0], st[1]);
            }
            for (var y = Math.ceil(cfg.map_y_min); y <= cfg.map_y_max; y++) {
                var sy = T.toScreen(0, y).y;
                var x0 = T.toScreen(cfg.map_x_min, y).x;
                var x1 = T.toScreen(cfg.map_x_max, y).x;
                var st2 = gridLineStyle(y);
                line(x0, sy, x1, sy, st2[0], st2[1]);
            }
        }

        // 点是否在障碍物形状内（画弹坑"炸裂截面"弧段时用；不看坑本身）
        function shapeContains(ob, x, y) {
            if (ob.is_circle) {
                var dx = x - ob.center[0], dy = y - ob.center[1];
                return dx * dx + dy * dy <= ob.radius * ob.radius;
            }
            return Math.abs(x - ob.center[0]) <= ob.half_w && Math.abs(y - ob.center[1]) <= ob.half_h;
        }

        // 该点是否已被"别的坑"炸成空地（重叠坑的断面弧不该画到空地上；
        // 本坑自己不参与判断——圆周点到本坑中心距离恰为半径，含等号会整圈都不画）
        function insideOtherCrater(craters, skip, x, y, r) {
            for (var i = 0; i < craters.length; i++) {
                if (i === skip) continue;
                var dx = x - craters[i][0], dy = y - craters[i][1];
                if (dx * dx + dy * dy < r * r) return true;
            }
            return false;
        }

        // 与 C++ obstacle_blocks 同款：点在形状内且不在该障碍任何坑里，算被实体覆盖
        function blocksPoint(ob, x, y, craterRadius) {
            var inside;
            if (ob.is_circle) {
                var bdx = x - ob.center[0], bdy = y - ob.center[1];
                inside = bdx * bdx + bdy * bdy <= ob.radius * ob.radius;
            } else {
                inside = Math.abs(x - ob.center[0]) <= ob.half_w &&
                         Math.abs(y - ob.center[1]) <= ob.half_h;
            }
            if (!inside) return false;
            var cr = ob.craters || [];
            for (var i = 0; i < cr.length; i++) {
                var hdx = x - cr[i][0], hdy = y - cr[i][1];
                if (hdx * hdx + hdy * hdy <= craterRadius * craterRadius) return false;
            }
            return true;
        }

        // 点是否落在这些坑的洞里（"洞"的判定与物理一致：到坑心距离不超过坑半径）
        function inCraterHole(craters, x, y, r) {
            for (var i = 0; i < craters.length; i++) {
                var dx = x - craters[i][0], dy = y - craters[i][1];
                if (dx * dx + dy * dy <= r * r) return true;
            }
            return false;
        }

        // 点是否被"别的障碍"的实体覆盖（重叠的障碍连成一体：藏在别的石头下的线不画）
        function coveredByOther(obstacles, self, x, y, craterRadius) {
            for (var i = 0; i < obstacles.length; i++) {
                if (i === self) continue;
                if (blocksPoint(obstacles[i], x, y, craterRadius)) return true;
            }
            return false;
        }

        // 形状"带洞"填充：形状先画到复用的离屏层上，再逐坑 destination-out 抠掉，
        // 整层贴回主画布。洞 = 坑的并集（重叠处多抠一遍自然无害）；贴回时透明区露出
        // 下层内容——别的障碍与网格都在下面，"A 的洞区显示 B 的石头"这类层序关系
        // 天然成立。不用"主画布直接抠"（会连先前画好的别的障碍一起抠掉），也不用
        // 反向子路径的 nonzero 技巧（坑伸出形状外的部分是负环绕数、重叠坑的交集是
        // -1，都会被规则当"非零"填回来——凭空多出石头圆盘/残片）
        var holeLayer = null;
        function fillShapeWithHoles(T, ob, craterRadius) {
            // 屏幕坐标包围盒（留 2px 余量接抗锯齿边）
            var minx, maxx, miny, maxy;
            if (ob.is_circle) {
                var c = T.toScreen(ob.center[0], ob.center[1]);
                var r = ob.radius * T.scale;
                minx = c.x - r; maxx = c.x + r;
                miny = c.y - r; maxy = c.y + r;
            } else {
                var q = [
                    T.toScreen(ob.center[0] - ob.half_w, ob.center[1] + ob.half_h),
                    T.toScreen(ob.center[0] + ob.half_w, ob.center[1] + ob.half_h),
                    T.toScreen(ob.center[0] + ob.half_w, ob.center[1] - ob.half_h),
                    T.toScreen(ob.center[0] - ob.half_w, ob.center[1] - ob.half_h),
                ];
                minx = Math.min(q[0].x, q[1].x, q[2].x, q[3].x);
                maxx = Math.max(q[0].x, q[1].x, q[2].x, q[3].x);
                miny = Math.min(q[0].y, q[1].y, q[2].y, q[3].y);
                maxy = Math.max(q[0].y, q[1].y, q[2].y, q[3].y);
            }
            var bx = Math.floor(minx) - 2, by = Math.floor(miny) - 2;
            var bw = Math.ceil(maxx) + 2 - bx, bh = Math.ceil(maxy) + 2 - by;
            if (!holeLayer) holeLayer = document.createElement('canvas');
            var lw = Math.ceil(bw * dpr), lh = Math.ceil(bh * dpr);
            if (holeLayer.width !== lw) holeLayer.width = lw;   // 改尺寸即重置，正好当清屏
            if (holeLayer.height !== lh) holeLayer.height = lh;
            var hc = holeLayer.getContext('2d');
            hc.setTransform(1, 0, 0, 1, 0, 0);
            hc.clearRect(0, 0, lw, lh);
            // 与主画布同比例，只把原点挪到包围盒左上（层内坐标 + 偏移 = 主画布坐标）
            hc.setTransform(dpr, 0, 0, dpr, -bx * dpr, -by * dpr);
            hc.beginPath();
            if (ob.is_circle) {
                hc.arc(c.x, c.y, r, 0, Math.PI * 2);
            } else {
                hc.moveTo(q[0].x, q[0].y);
                hc.lineTo(q[1].x, q[1].y);
                hc.lineTo(q[2].x, q[2].y);
                hc.lineTo(q[3].x, q[3].y);
                hc.closePath();
            }
            hc.fillStyle = css(COL.obstacle);
            hc.fill();
            hc.globalCompositeOperation = 'destination-out';
            var craters = ob.craters || [];
            for (var k = 0; k < craters.length; k++) {
                var cp = T.toScreen(craters[k][0], craters[k][1]);
                hc.beginPath();
                hc.arc(cp.x, cp.y, craterRadius * T.scale, 0, Math.PI * 2);
                hc.fill();
            }
            hc.globalCompositeOperation = 'source-over';
            ctx.drawImage(holeLayer, 0, 0, lw, lh, bx, by, bw, bh);
        }

        // 形状轮廓：沿边缘分段画线，跳过两类段——① 被自己洞咬掉的；② 外侧被别的障碍
        // 盖住的（重叠的障碍算连成一体，中间不画边界线）。洞缘断口线由 strokeCraterRims 画
        function strokeShapeOutline(T, obstacles, idx, craterRadius) {
            var ob = obstacles[idx];
            var keep = function (mx, my, ox, oy) {
                if (inCraterHole(ob.craters || [], mx, my, craterRadius)) return false;
                // 探针取外侧一点：另一边还是石头，说明这条线是重叠区里的内部边界
                return !coveredByOther(obstacles, idx, mx + ox * 0.15, my + oy * 0.15, craterRadius);
            };
            var cx = ob.center[0], cy = ob.center[1];
            if (ob.is_circle) {
                var seg = 120;
                for (var i = 0; i < seg; i++) {
                    var a0 = 2 * Math.PI * i / seg;
                    var a1 = 2 * Math.PI * (i + 1) / seg;
                    var am = (a0 + a1) / 2;
                    if (!keep(cx + Math.cos(am) * ob.radius, cy + Math.sin(am) * ob.radius,
                              Math.cos(am), Math.sin(am))) continue;
                    var p0 = T.toScreen(cx + Math.cos(a0) * ob.radius,
                                        cy + Math.sin(a0) * ob.radius);
                    var p1 = T.toScreen(cx + Math.cos(a1) * ob.radius,
                                        cy + Math.sin(a1) * ob.radius);
                    line(p0.x, p0.y, p1.x, p1.y, COL.obstacleEdge, 1);
                }
                return;
            }
            var corners = [
                [cx - ob.half_w, cy + ob.half_h],
                [cx + ob.half_w, cy + ob.half_h],
                [cx + ob.half_w, cy - ob.half_h],
                [cx - ob.half_w, cy - ob.half_h],
            ];
            for (var e = 0; e < 4; e++) {
                var v0 = corners[e], v1 = corners[(e + 1) % 4];
                var dxe = v1[0] - v0[0], dye = v1[1] - v0[1];
                var len = Math.hypot(dxe, dye);
                if (len < 1e-12) continue;
                // 外法向：两个候选里取离中心更远的那个（与顶点绕向无关）
                var nx = -dye / len, ny = dxe / len;
                var mx0 = (v0[0] + v1[0]) / 2 - cx;
                var my0 = (v0[1] + v1[1]) / 2 - cy;
                if (Math.hypot(mx0 + nx, my0 + ny) < Math.hypot(mx0 - nx, my0 - ny)) {
                    nx = -nx;
                    ny = -ny;
                }
                var nseg = Math.max(1, Math.ceil(len / 0.1));
                for (var s = 0; s < nseg; s++) {
                    var x0 = v0[0] + dxe * s / nseg, y0 = v0[1] + dye * s / nseg;
                    var x1 = v0[0] + dxe * (s + 1) / nseg, y1 = v0[1] + dye * (s + 1) / nseg;
                    if (!keep((x0 + x1) / 2, (y0 + y1) / 2, nx, ny)) continue;
                    var q0 = T.toScreen(x0, y0), q1 = T.toScreen(x1, y1);
                    line(q0.x, q0.y, q1.x, q1.y, COL.obstacleEdge, 1.5);
                }
            }
        }

        // 弹坑洞缘的"炸裂截面"线：坑圆周落在障碍形状内的弧段（伸到空地的不画），再
        // 排除落在本障碍别的坑里的（重叠洞）和盖在别的障碍实体下的（被后画的石头挡住）
        function strokeCraterRims(T, obstacles, idx, craterRadius) {
            var ob = obstacles[idx];
            var craters = ob.craters || [];
            var seg = 48;
            for (var k = 0; k < craters.length; k++) {
                var wx = craters[k][0], wy = craters[k][1];
                for (var s = 0; s < seg; s++) {
                    var a0 = 2 * Math.PI * s / seg;
                    var a1 = 2 * Math.PI * (s + 1) / seg;
                    var am = (a0 + a1) / 2;
                    var ax = wx + craterRadius * Math.cos(am);
                    var ay = wy + craterRadius * Math.sin(am);
                    if (!shapeContains(ob, ax, ay)) continue;
                    if (insideOtherCrater(craters, k, ax, ay, craterRadius)) continue;
                    if (coveredByOther(obstacles, idx, ax, ay, craterRadius)) continue;
                    var p0 = T.toScreen(wx + craterRadius * Math.cos(a0),
                                        wy + craterRadius * Math.sin(a0));
                    var p1 = T.toScreen(wx + craterRadius * Math.cos(a1),
                                        wy + craterRadius * Math.sin(a1));
                    line(p0.x, p0.y, p1.x, p1.y, COL.obstacleEdge, 1);
                }
            }
        }

        // draw_obstacles 的移植：障碍"天生带洞"——画的时候就跳过被炸掉的部分（洞），
        // 网格自然透出；轮廓跳过与其他障碍的重叠段（重叠的石头连成一体）。旧做法是
        // 整块画完再贴底色圆、补网格，会连带盖掉相邻障碍，处处要打配合
        function drawObstacles(T, obstacles, cfg) {
            var craterRadius = cfg.crater_radius;
            for (var i = 0; i < obstacles.length; i++) {
                var ob = obstacles[i];
                // 填充：没被炸过的整块画（快路径）；带坑的"带洞画"
                if ((ob.craters || []).length === 0) {
                    if (ob.is_circle) {
                        var c = T.toScreen(ob.center[0], ob.center[1]);
                        circle(c.x, c.y, ob.radius * T.scale, COL.obstacle);
                    } else {
                        // 镜像时矩形的两个角在屏幕上左右互换：统一取屏幕坐标的 min 当左上角
                        var c1 = T.toScreen(ob.center[0] - ob.half_w, ob.center[1] + ob.half_h);
                        var c2 = T.toScreen(ob.center[0] + ob.half_w, ob.center[1] - ob.half_h);
                        fillRect(Math.min(c1.x, c2.x), Math.min(c1.y, c2.y),
                                 Math.abs(c2.x - c1.x), Math.abs(c2.y - c1.y), COL.obstacle);
                    }
                } else {
                    fillShapeWithHoles(T, ob, craterRadius);
                }
                strokeShapeOutline(T, obstacles, i, craterRadius);
                strokeCraterRims(T, obstacles, i, craterRadius);
            }
        }

        // draw_ticks 的移植：x 值标在 x 轴下方（居中，含 0——原点下方的 "0" 是定位锚点），
        // y 值标在 y 轴左侧（右对齐，跳过 0 免得与 x 的 0 叠一起），每 5 单位一个，带淡底牌。
        // 镜像视角下刻度显示的是取负的世界坐标。数字是"世界坐标的展示"，不是屏幕摆放的
        // 坐标换算：屏幕摆放由 to_screen 另行翻转，两者不必相等——玩家读"自己与目标的数字差"
        // 即得到表达式的相对坐标，地图范围是否关于 0 对称都不影响这套读法（照 C++ 的设计）
        function drawTicks(T, cfg, mirror) {
            var fs = 13;
            ctx.font = fs + 'px ' + FONT;
            ctx.textBaseline = 'top';
            ctx.textAlign = 'left';
            var origin = T.toScreen(0, 0);
            function label(text, cx, cy, centered) {
                var mw = ctx.measureText(text).width;
                var x = centered ? cx - mw * 0.5 : cx - mw;
                var y = cy - fs * 0.5;
                ctx.fillStyle = css(COL.tickPlate);
                ctx.beginPath();
                ctx.roundRect(x - 4, y - 2, mw + 8, fs + 4, 4);
                ctx.fill();
                ctx.fillStyle = css(COL.tick);
                ctx.fillText(text, x, y);
            }
            for (var x = Math.ceil(cfg.map_x_min / 5) * 5; x <= cfg.map_x_max; x += 5) {
                label(String(mirror ? -x : x), T.toScreen(x, 0).x, origin.y + 17, true);
            }
            for (var y = Math.ceil(cfg.map_y_min / 5) * 5; y <= cfg.map_y_max; y += 5) {
                if (Math.abs(y) < 1e-9) continue;
                label(String(y), origin.x - 8, T.toScreen(0, y).y, false);
            }
        }

        // draw_player 的移植：方块半边长与命中判定同源（cfg.player_half_size）；
        // 名字只画一次，挂在第一个存活士兵头上（全灭后挂在第一个士兵的残骸处）
        function drawPlayer(T, halfWorld, p, isMe, isCurrent, curSoldier,
                            isNext, nextSoldier, lastFn, time) {
            var half = T.scale * halfWorld;
            var anchor = null;
            for (var i = 0; i < p.soldiers.length; i++) {
                if (p.soldiers[i].alive) { anchor = p.soldiers[i]; break; }
            }
            if (anchor === null && p.soldiers.length) anchor = p.soldiers[0];
            for (var si = 0; si < p.soldiers.length; si++) {
                var s = p.soldiers[si];
                var c = T.toScreen(s.pos[0], s.pos[1]);
                var body = !s.alive ? COL.dead : (p.team === 'A' ? COL.teamA : COL.teamB);
                var cur = isCurrent && si === curSoldier;
                // 当前出手的士兵：呼吸光圈（一眼看出轮到谁）
                if (cur && s.alive) {
                    var pulse = 0.5 + 0.5 * Math.sin(time * 4);
                    circle(c.x, c.y, half * 2 + pulse * 5,
                           rgbA(COL.preview, 255 * (0.16 + 0.20 * pulse)));
                }
                if (s.alive) {
                    // 轻微投影：方块上移 1px 后从底边露出这一条黑色
                    fillRect(c.x - half, c.y - half, half * 2, half * 2, COL.shadow);
                }
                var faceX = c.x - half, faceY = c.y - half - 1, faceW = half * 2, faceH = half * 2;
                fillRect(faceX, faceY, faceW, faceH, body);
                if (isMe) {
                    strokeRect(faceX, faceY, faceW, faceH, COL.projectile, 2);  // 白框 = 你
                } else if (cur) {
                    strokeRect(faceX, faceY, faceW, faceH, COL.preview, 2);     // 金框 = 当前出手
                }
                // 下一个出手的士兵：细环提示（提前知道下一发是谁打）
                if (isNext && s.alive && si === nextSoldier) {
                    circleLines(c.x, c.y, half * 1.9, COL.nextMark, 1);
                }
                if (s.max_hp > 1 && s.alive) {
                    // 血条：固定 30px 宽、方块下方，与半边长无关（C++ 同）
                    var w = 30;
                    var frac = Math.max(0, Math.min(1, s.hp / s.max_hp));  // 防脏数据画出框外
                    var bx = c.x - w / 2, by = c.y + half + 4;
                    ctx.fillStyle = css(COL.hpBack);
                    ctx.beginPath();
                    ctx.roundRect(bx, by, w, 5, 1.5);
                    ctx.fill();
                    if (frac > 0) {
                        ctx.fillStyle = css(frac > 0.5 ? COL.hpHigh : COL.hpLow);
                        ctx.beginPath();
                        ctx.roundRect(bx, by, w * frac, 5, 1.5);
                        ctx.fill();
                    }
                }
            }
            if (anchor !== null) {
                // 名字（带半透明底牌）+ 最近用过的函数（名字下方一行小字）
                var ac = T.toScreen(anchor.pos[0], anchor.pos[1]);
                var fs = 14, fnFs = 12;
                ctx.font = fs + 'px ' + FONT;
                var mw = ctx.measureText(p.name).width;
                var hasFn = !!lastFn;
                var nx = ac.x - mw * 0.5;
                var ny = ac.y - half - fs - 9 - (hasFn ? fnFs + 3 : 0);
                if (anchor.alive) {
                    var bh = fs + 5 + (hasFn ? fnFs + 4 : 0);
                    ctx.fillStyle = css(COL.namePlate);
                    ctx.beginPath();
                    ctx.roundRect(nx - 5, ny - 2, mw + 10, bh, 5);
                    ctx.fill();
                }
                ctx.fillStyle = css(anchor.alive ? COL.nameAlive : COL.nameDead);
                ctx.fillText(p.name, nx, ny);
                if (hasFn) {
                    ctx.font = fnFs + 'px ' + FONT;
                    var fw = ctx.measureText(lastFn).width;
                    ctx.fillStyle = css(COL.lastFunc);
                    ctx.fillText(lastFn, ac.x - fw * 0.5, ny + fs + 2);
                }
            }
        }

        // 战斗特效（命中冲击环 / 爆炸 / 阵亡碎裂）：条目由 app.js 在消息事件处生成
        //（对齐 C++ fx.cpp 的 spawn_*），这里现算现画——环形半径按 easeOut(t) 扩散、
        // 透明度随寿命衰减；粒子位置用解析式 pos + vel*(1-e^(-3t))/3（与 C++ 逐帧阻尼
        // 积分等价但不依赖帧率），方块自旋随寿命推进
        function drawFx(T, list, time) {
            for (var i = 0; i < list.length; i++) {
                var f = list[i];
                var age = time - f.t0;
                if (!(age >= 0) || age >= f.life) continue;
                var fade = 1 - age / f.life;
                // 颜色三种写法：null = 主题高光色（COL.projectile）；字符串 = COL 里的
                // 主题色键（如 'spark'，随主题切换）；数组 = 固定色
                var c = f.color;
                if (typeof c === 'string') c = COL[c] || COL.projectile;
                else if (!c) c = COL.projectile;
                var ca = (c.length === 4) ? c[3] : 255;  // COL 的色可能没 alpha 分量
                var col = rgbA(c, ca * fade);
                if (f.kind === 'ring') {
                    var et = 1 - (1 - age / f.life) * (1 - age / f.life);  // easeOut
                    var r = Math.max(0.01, (f.r0 + (f.r1 - f.r0) * et) * T.scale);
                    var p = T.toScreen(f.pos[0], f.pos[1]);
                    ctx.strokeStyle = css(col);
                    ctx.lineWidth = f.width;
                    ctx.beginPath();
                    ctx.arc(p.x, p.y, r, 0, Math.PI * 2);
                    ctx.stroke();
                } else {
                    var k = (1 - Math.exp(-3 * age)) / 3;
                    var sp = T.toScreen(f.pos[0] + f.vel[0] * k, f.pos[1] + f.vel[1] * k);
                    var px = Math.max(1.5, f.size * T.scale);
                    ctx.save();
                    ctx.translate(sp.x, sp.y);
                    ctx.rotate(f.rotv * age);
                    ctx.fillStyle = css(col);
                    ctx.fillRect(-px * 0.5, -px * 0.5, px, px);
                    ctx.restore();
                }
            }
        }

        function draw(scene) {
            if (!scene || !scene.cfg) return;
            if (canvas.clientWidth !== W || canvas.clientHeight !== H) resize();
            ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
            // 文字锚点统一为左上（raylib 的 DrawTextEx 语义）；逐处设置会在
            // "先画玩家名字、后画刻度"的顺序下让先画的那批吃上一帧的基线
            ctx.textBaseline = 'top';
            ctx.textAlign = 'left';
            var cfg = scene.cfg;
            var T = fitView(cfg, scene.mirror);

            // 地图底色：与 C++ 一致用纯色 kBackground——弹坑也用同一色覆盖，接缝不可见
            ctx.fillStyle = css(COL.background);
            ctx.fillRect(0, 0, W, H);

            // 屏幕震动：场景内容整层随机偏移绘制（HUD 是 DOM 天然不抖；回合横幅是
            // 信息层、画在震屏层之外也不抖）
            ctx.save();
            if (scene.shake && scene.shake > 0.15) {
                ctx.translate((Math.random() * 2 - 1) * scene.shake,
                              (Math.random() * 2 - 1) * scene.shake);
            }

            // 绘制顺序照 draw_scene 逐行对应：
            // 网格 → 障碍 → 炮弹轨迹（压网格/障碍之上，不被切割）→ 预览 → 玩家 → 特效 → 炮弹 → 刻度
            drawGrid(T, cfg);
            drawObstacles(T, scene.obstacles || [], cfg);
            if (scene.projectilePath && scene.projectilePath.valid()) {
                drawPath(T, scene.projectilePath.points, COL.pathFaint, 1);
                // 已飞过的一段加亮（暖金色，与 C++ draw_path_flown 同色）：弹头停住时亮段也停在撞击处
                if (scene.projectileS > 0) {
                    drawPathUpTo(T, scene.projectilePath, scene.projectileS, COL.trail, 2.4);
                }
            }
            if (scene.previewPath && scene.previewPath.valid()) {
                // 宽而淡的辉光打底 + 流动虚线细线（相位随时间滚动，兼作飞行方向指示）
                drawPath(T, scene.previewPath.points, rgbA(COL.preview, 46), 7);
                drawDashedPath(T, scene.previewPath.points, COL.preview, 2.4, 7, 5,
                               (scene.time * 60) % 12);
            }
            var players = scene.players || [];
            var turn = scene.turn;
            var half = cfg.player_half_size;
            for (var pi = 0; pi < players.length; pi++) {
                var p = players[pi];
                var fn = scene.lastFunc ? scene.lastFunc[p.id] : null;
                drawPlayer(T, half, p, p.id === scene.myId,
                           turn !== null && p.id === turn.playerId,
                           turn !== null ? turn.soldier : -1,
                           turn !== null && p.id === turn.nextPlayer,
                           turn !== null ? turn.nextSoldier : -1,
                           fn || null, scene.time);
            }
            // 战斗特效（命中冲击环/爆炸/阵亡碎裂）：压在玩家之上、炮弹之下
            drawFx(T, scene.fx || [], scene.time);
            // 炮弹：外圈辉光 + 亮核（不分队，与 C++ 同）
            if (scene.projectile) {
                var pc = T.toScreen(scene.projectile.x, scene.projectile.y);
                circle(pc.x, pc.y, 11, rgbA(COL.spark, 46));
                circle(pc.x, pc.y, 6.5, rgbA(COL.projectile, 140));
                circle(pc.x, pc.y, 3.5, COL.projectile);
            }
            drawTicks(T, cfg, scene.mirror);
            ctx.restore();

            // 回合横幅：回合开始瞬间滑入、短暂停留后淡出（与 C++ 的中央横幅同款；
            // 画在震屏层之外——它是信息层，不该跟着抖）；时间从 t0 起算 1.6 秒
            if (scene.banner) {
                var bt = scene.time - scene.banner.t0;
                if (bt >= 0 && bt < 1.6) {
                    var inT = Math.min(1, bt / 0.18);                       // 滑入
                    var outT = Math.max(0, Math.min(1, (1.6 - bt) / 0.5));  // 尾段淡出
                    var alpha = Math.min(inT, outT);
                    ctx.font = '600 34px ' + FONT;
                    var bmw = ctx.measureText(scene.banner.text).width;
                    var bcx = W * 0.5;
                    var bcy = H * 0.22 - (1 - inT) * 16;
                    ctx.fillStyle = css(rgbA(COL.plateBg, COL.plateBg[3] * alpha));
                    ctx.beginPath();
                    ctx.roundRect(bcx - bmw * 0.5 - 30, bcy - 12, bmw + 60, 62, 16);
                    ctx.fill();
                    var bc = scene.banner.gold ? COL.preview : COL.nameAlive;
                    ctx.fillStyle = css([bc[0], bc[1], bc[2], Math.round(255 * alpha)]);
                    ctx.fillText(scene.banner.text, bcx - bmw * 0.5, bcy);
                }
            }
        }

        return { draw: draw, resize: resize, setTheme: setTheme };
    }

    var FGRender = { create: createRenderer };
    if (typeof module !== 'undefined' && module.exports) module.exports = FGRender;
    global.FGRender = FGRender;
})(typeof window !== 'undefined' ? window : globalThis);
