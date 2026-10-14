// 折线 + 弧长参数化：炮弹按服务器下发的 travel（弧长）沿弹道前进，
// 渲染时需要"按弧长取点"和"画到某个弧长为止"——两件小事都在这里。
// 轨迹点来自 shot.trail（服务器采样下发），本文件不做任何曲线计算。
(function (global) {
    'use strict';

    function build(trail) {
        var pts = [];
        var src = trail || [];
        for (var i = 0; i < src.length; i++) {
            var p = src[i];
            if (Array.isArray(p) && p.length >= 2 && isFinite(p[0]) && isFinite(p[1])) {
                pts.push({ x: p[0], y: p[1] });
            }
        }
        var cum = new Array(pts.length);
        if (pts.length) cum[0] = 0;
        for (var k = 1; k < pts.length; k++) {
            cum[k] = cum[k - 1] + Math.sqrt(
                (pts[k].x - pts[k - 1].x) * (pts[k].x - pts[k - 1].x) +
                (pts[k].y - pts[k - 1].y) * (pts[k].y - pts[k - 1].y));
        }
        return {
            points: pts,
            cum: cum,
            length: function () { return cum.length ? cum[cum.length - 1] : 0; },
            valid: function () { return pts.length >= 2; },
            // 二分找 s 所在的段：返回 [下标, 段内比例]
            segmentAt: function (s) {
                if (pts.length < 2) return [0, 0];
                if (s <= cum[0]) return [0, 0];
                if (s >= cum[cum.length - 1]) return [pts.length - 1, 0];
                var lo = 0, hi = cum.length - 1;
                while (hi - lo > 1) {
                    var mid = (lo + hi) >> 1;
                    if (cum[mid] <= s) lo = mid; else hi = mid;
                }
                var s0 = cum[lo], s1 = cum[hi];
                return [lo, s1 > s0 ? (s - s0) / (s1 - s0) : 0];
            },
            at: function (s) {
                var seg = this.segmentAt(s);
                var a = pts[seg[0]];
                var b = pts[Math.min(seg[0] + 1, pts.length - 1)];
                return { x: a.x + (b.x - a.x) * seg[1], y: a.y + (b.y - a.y) * seg[1] };
            },
        };
    }

    var FGPath = { build: build };
    if (typeof module !== 'undefined' && module.exports) module.exports = FGPath;
    global.FGPath = FGPath;
})(typeof window !== 'undefined' ? window : globalThis);
