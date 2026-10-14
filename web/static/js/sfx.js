// 合成音效（WebAudio）：与 C++ 客户端 src/client/sfx.cpp 同一设计——无任何音频
// 资源文件，全部运行时合成；音色参数照抄那边（发射扫频 / 命中短打 / 爆炸噪声 /
// 阵亡下坠 / 回合提示）。浏览器的自动播放策略要求用户手势后才能出声：首次
// 点击/按键时解锁 AudioContext，未解锁时所有播放静默跳过。
(function (global) {
    'use strict';

    var ctx = null;
    var master = null;
    var unavailable = false;  // 无 WebAudio 或构造失败：只判定一次，之后所有播放静默

    function unlock() {
        if (unavailable) return;
        if (!ctx) {
            var AC = global.AudioContext || global.webkitAudioContext;
            if (!AC) {
                unavailable = true;  // 老旧浏览器：无 WebAudio
                return;
            }
            // 构造可能抛异常（隐私模式、音频上下文数量超限、嵌入式 WebView）：
            // 失败即永久静默，别让每次点击都重复抛错
            try {
                ctx = new AC();
                master = ctx.createGain();
                master.gain.value = 0.9;
                master.connect(ctx.destination);
            } catch (e) {
                unavailable = true;
                ctx = null;
                return;
            }
        }
        if (ctx.state === 'suspended') {
            var p = ctx.resume();
            if (p && typeof p.catch === 'function') p.catch(function () { /* 恢复被拒：保持静默 */ });
        }
    }

    // 捕获阶段监听：不等业务处理器，第一次交互就解锁
    document.addEventListener('pointerdown', unlock, true);
    document.addEventListener('keydown', unlock, true);

    function ready() {
        return ctx !== null && ctx.state === 'running';
    }

    // 振荡器音：频率从 f0 滑到 f1（相同则恒定），音量指数衰减到静音
    function tone(type, f0, f1, dur, vol) {
        var t0 = ctx.currentTime;
        var o = ctx.createOscillator();
        var g = ctx.createGain();
        o.type = type;
        o.frequency.setValueAtTime(Math.max(1, f0), t0);
        if (f1 !== f0) o.frequency.exponentialRampToValueAtTime(Math.max(1, f1), t0 + dur);
        g.gain.setValueAtTime(vol, t0);
        g.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
        o.connect(g);
        g.connect(master);
        o.start(t0);
        o.stop(t0 + dur + 0.02);
    }

    // 噪声爆：白噪声经低通（cutoff 越低越"闷"），音量指数衰减
    function noise(dur, vol, cutoff) {
        var t0 = ctx.currentTime;
        var n = Math.max(1, Math.floor(ctx.sampleRate * dur));
        var buf = ctx.createBuffer(1, n, ctx.sampleRate);
        var d = buf.getChannelData(0);
        for (var i = 0; i < n; i++) d[i] = Math.random() * 2 - 1;
        var src = ctx.createBufferSource();
        src.buffer = buf;
        var f = ctx.createBiquadFilter();
        f.type = 'lowpass';
        f.frequency.value = cutoff;
        var g = ctx.createGain();
        g.gain.setValueAtTime(vol, t0);
        g.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
        src.connect(f);
        f.connect(g);
        g.connect(master);
        src.start(t0);
    }

    var FGSfx = {
        shot: function () {        // 发射："咻"——正弦 260→900Hz 扫频
            if (!ready()) return;
            tone('sine', 260, 900, 0.16, 0.28);
        },
        hit: function () {         // 命中："啪"——方波短打 + 极短噪声毛刺
            if (!ready()) return;
            tone('square', 210, 210, 0.09, 0.26);
            noise(0.03, 0.18, 2500);
        },
        death: function () {       // 阵亡："咚"——180→45Hz 下坠（方波打底 + 正弦补低频）
            if (!ready()) return;
            tone('square', 180, 45, 0.45, 0.24);
            tone('sine', 180, 45, 0.45, 0.18);
        },
        explode: function () {     // 炸坑："轰"——低通白噪声爆
            if (!ready()) return;
            noise(0.40, 0.5, 800);
        },
        turn: function () {        // 轮到我："叮"——880Hz + 五度泛音
            if (!ready()) return;
            tone('sine', 880, 880, 0.15, 0.20);
            tone('sine', 1320, 1320, 0.15, 0.09);
        },
    };

    global.FGSfx = FGSfx;
})(typeof window !== 'undefined' ? window : globalThis);
