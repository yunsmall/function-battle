// 网页版主逻辑：连接 → 建房/加入 → 房间 → 对战 → 结算。
// 服务器是唯一权威：回合、命中、血量、胜负全由服务器事件驱动；本地只做
// 输入收集、界面渲染，以及按 shot.trail 把弹道画出来（移动位置用 20Hz 的
// projectiles 校准）。不解析表达式、不判碰撞。
(function () {
    'use strict';

    var t = FGStrings.t;
    var $ = function (id) { return document.getElementById(id); };
    var net = new FGNet();
    var renderer = null;

    // ---------- 模式表（与服务端 7 种曲线模式对应；只描述界面） ----------
    var MODES = [
        { id: 'y_of_x', label: 'mode_y_of_x', a: 'label_a_y', b: null, range: false,
          dir: ['dir_right', 'dir_left'], angle: false, hint: 'hint_y_of_x', ph: '0.5*x^2 - 1.2*x' },
        { id: 'x_of_y', label: 'mode_x_of_y', a: 'label_a_x', b: null, range: false,
          dir: ['dir_up', 'dir_down'], angle: false, hint: 'hint_x_of_y', ph: '0.5*y^2 - 4' },
        { id: 'polar', label: 'mode_polar', a: 'label_a_r', b: null, range: true,
          dir: null, angle: false, hint: 'hint_polar', ph: '8*sin(3*t)' },
        { id: 'parametric', label: 'mode_parametric', a: 'label_a_xt', b: 'label_b_yt', range: true,
          dir: null, angle: false, hint: 'hint_parametric', ph: '8*cos(t)', ph2: '6*sin(t)' },
        { id: 'ode1', label: 'mode_ode1', a: 'label_a_dydx', b: null, range: false,
          dir: ['dir_posx', 'dir_negx'], angle: false, hint: 'hint_ode1', ph: '-0.1*y' },
        { id: 'ode2', label: 'mode_ode2', a: 'label_a_xp', b: 'label_b_yp', range: false,
          dir: null, angle: false, hint: 'hint_ode2', ph: '6*cos(0.5*t)', ph2: '6*sin(0.5*t)' },
        { id: 'ypp', label: 'mode_ypp', a: 'label_a_ypp', b: null, range: false,
          dir: ['dir_posx', 'dir_negx'], angle: true, hint: 'hint_ypp', ph: '-3' },
    ];

    var MAX_ATTEMPTS = 16;

    // ---------- 全局状态 ----------
    var S = {
        screen: 'menu',
        myId: null,
        myName: '玩家',
        host: null,
        members: [],
        inGame: false,
        cfg: null,            // "下一把"配置（room_state 的 next_match / game_start 的 match）
        roomCfg: null,        // 房间的属性（room_state / game_start 的 room 对象：士兵数/坐标系）
        worldCfg: null,       // 世界常量（world 对象：地图范围与物理参数，渲染与采样用）
        vote: { voted: [], total: 0 },
        battle: null,
        modeIdx: 0,
        dirNeg: false,
        submitted: false,
        tooMany: false,
        attempts: 0,
        lastPreviewAt: 0,
        lastPreviewText: '',    // 上次广播的"正在写"文本（内容不变不重发，与 C++ 的 sent_preview_text_ 一致）
        spectateMirror: false,  // 观战者在 mirror_view 房间的视角选择（仅观战有效）
        leavingSpectate: false, // 观战者"离开房间"已发出、等回应（对齐 C++ 的 leaving_spectate，防连点）
        restartSent: false,     // "再来一局"已发出 start、等待服务器开局（防连点）
        createOpts: { soldiers: 3, obstacles: 15, firstTeam: -1, coord: 'soldier' },
    };

    function esc(s) {
        return String(s).replace(/[&<>"']/g, function (c) {
            return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
        });
    }

    function num(v, dflt) {
        return (typeof v === 'number' && isFinite(v)) ? v : dflt;
    }

    // 种子策略：配置的 seed 为 null 表示"每把随机"（协议语义，与 C++ 的 optional 一致）
    function seedIsRandom() {
        return !S.cfg || S.cfg.seed === null || S.cfg.seed === undefined;
    }

    // 严格数字解析：整串都必须是数字才接受（parseFloat 会把 "1.2.3" 吃成 1.2，
    // 与 C++ 客户端"整串合法才接受"的输入行为不一致）；空串返回 null。
    // 使用方（参数范围/发射角框）另有 filterNumInput 做源头字符过滤（与 C++ 的
    // digits_only 同款）：两侧可达字符集一致后，Number 与 C++ 的 strtod 等效
    function strictNum(text) {
        var s = String(text).trim();
        if (s === '') return null;
        var v = Number(s);
        return isFinite(v) ? v : null;
    }

    // "最近用过的函数"的截断规则与 C++ 客户端一致：超过 16 个 UTF-8 字节就截断加省略号，
    // 截断点回退到字符边界（一个中文字符 3 字节，从中间切开会画出乱码）
    function truncateUtf8(s, maxBytes) {
        var out = '', bytes = 0, i = 0;
        while (i < s.length) {
            var cp = s.codePointAt(i);
            var len = cp < 0x80 ? 1 : (cp < 0x800 ? 2 : (cp < 0x10000 ? 3 : 4));
            if (bytes + len > maxBytes) return out + '…';
            out += String.fromCodePoint(cp);
            bytes += len;
            i += cp > 0xFFFF ? 2 : 1;
        }
        return s;
    }

    function toast(msg) {
        var el = $('toast');
        el.textContent = msg;
        el.classList.remove('hidden');
        clearTimeout(el._timer);
        el._timer = setTimeout(function () { el.classList.add('hidden'); }, 3000);
    }

    function log(html) {
        var box = $('event-log');
        var div = document.createElement('div');
        div.className = 'entry';
        div.innerHTML = html;
        box.appendChild(div);
        while (box.children.length > 120) box.removeChild(box.firstChild);
        box.scrollTop = box.scrollHeight;
    }

    function nameHtml(id) {
        var p = playerById(id);
        var cls = p ? (p.team === 'A' ? 'team-a' : 'team-b') : 'dim';
        return '<span class="' + cls + '">' + esc(p ? p.name : '#' + id) + '</span>';
    }

    function playerById(id) {
        if (!S.battle) return null;
        for (var i = 0; i < S.battle.players.length; i++) {
            if (S.battle.players[i].id === id) return S.battle.players[i];
        }
        return null;
    }

    // ---------- 界面切换 ----------
    function showScreen(name) {
        S.screen = name;
        ['menu', 'room', 'battle'].forEach(function (s) {
            $('screen-' + s).classList.toggle('hidden', s !== name);
        });
        // 帮助面板只属于对局屏（C++ 的 help_modal_active 也只在 Battle 屏为真）：切屏即收起，
        // 否则断线/被踢后它残留在主菜单上挡住界面
        $('help-overlay').classList.add('hidden');
    }

    function applyStaticI18n() {
        document.querySelectorAll('[data-i18n]').forEach(function (el) {
            el.textContent = t(el.getAttribute('data-i18n'));
        });
        document.querySelectorAll('[data-i18n-ph]').forEach(function (el) {
            el.placeholder = t(el.getAttribute('data-i18n-ph'));
        });
    }

    // ---------- 主菜单 ----------
    function buildMenuOptions() {
        function row(containerId, values, label, fmt, cb, current) {
            var box = $(containerId);
            box.innerHTML = '';
            values.forEach(function (v) {
                var b = document.createElement('button');
                b.type = 'button';
                b.textContent = fmt ? fmt(v) : String(v);
                b.dataset.v = v;
                if (v === current) b.classList.add('active');
                b.addEventListener('click', function () {
                    box.querySelectorAll('button').forEach(function (x) { x.classList.remove('active'); });
                    b.classList.add('active');
                    cb(v);
                });
                box.appendChild(b);
            });
        }
        row('opt-soldiers', [1, 2, 3, 4], null, null, function (v) { S.createOpts.soldiers = v; }, S.createOpts.soldiers);
        row('opt-coord', ['soldier', 'field'], null, function (v) {
            return t(v === 'field' ? 'coord_field' : 'coord_soldier');
        }, function (v) {
            S.createOpts.coord = v;
            // 选项下方的说明随选择切换（与 C++ 创建界面/单机设置同文案）
            $('coord-hint').textContent = t(v === 'field' ? 'coord_note_field' : 'coord_note_soldier');
        }, S.createOpts.coord);
        row('opt-obstacles', [0, 5, 10, 15, 20, 30], null, null, function (v) { S.createOpts.obstacles = v; }, S.createOpts.obstacles);
        row('opt-first-team', [-1, 0, 1], null, function (v) {
            return v === -1 ? t('seed_random') : (v === 0 ? t('first_team_a') : t('first_team_b'));
        }, function (v) { S.createOpts.firstTeam = v; }, S.createOpts.firstTeam);
    }

    function doCreate() {
        var name = $('menu-name').value.trim() || '玩家';
        S.myName = name;
        // 创建请求分两个对象（与 C++ 的创建界面一致）：
        // room = 房间的属性（士兵数/坐标系，唯一一次设置机会）；
        // config = 下一把配置的初值（遮挡物/先手/种子）；血量、时限、停顿等进房间后再调
        var room = {
            soldiers_per_player: S.createOpts.soldiers,
            coord_origin: S.createOpts.coord,
        };
        var match = {
            obstacle_count: S.createOpts.obstacles,
            first_team: S.createOpts.firstTeam,
        };
        var seedText = $('menu-seed').value.trim();
        if (seedText !== '') {
            // 只接受十进制数字（Number('0x10') 会得到 16，与 C++ 数字框的行为不一致）
            if (!/^\d+$/.test(seedText)) {
                toast(t('bad_seed'));
                return;
            }
            match.seed = Number(seedText);
        } else {
            match.seed = null;  // 留空 = 每把随机（显式表态，与 C++ 创建界面一致）
        }
        // trail:true —— 本连接要求 shot 携带轨迹折线（网页端不共享 C++ core）
        var cmsg = { type: 'create_room', name: name, room: room, match: match, trail: true };
        // 房间密码：本地算 SHA-256 再上行（传输中不出现明文）；哈希=口令，不放 room 对象
        var cpw = $('menu-password').value;
        if (cpw !== '') cmsg.password_hash = sha256Hex(cpw);
        net.send(cmsg);
    }

    function doJoin(spectate) {
        // 严格整串十进制（C++ 的房间号框是 digits_only，粘贴 "12abc" 不会静默变成 12）
        var ridText = $('join-id').value.trim();
        if (!/^\d+$/.test(ridText)) { toast(t('bad_room_id')); return; }
        var rid = Number(ridText);
        var name = $('menu-name').value.trim() || '玩家';
        S.myName = name;
        var msg = { type: 'join_room', room_id: rid, name: name, trail: true };
        var jpw = $('join-password').value;
        if (jpw !== '') msg.password_hash = sha256Hex(jpw);
        if (spectate) msg.spectate = true;  // 观战身份进房（进行中的局也能看，与 C++ 的"观战"按钮一致）
        net.send(msg);
    }

    // ---------- 房间 ----------
    // 离开/被踢时清空房间种子输入框的残留：旧房间的种子不能被带进新房间
    //（对齐 C++ 在 kLeftRoom/kKicked 里的 room_seed_box_.text.clear()）
    function clearRoomSeedBox() {
        var el = $('room-seed');
        if (el) el.value = '';
    }

    // 房主在房间界面改规则：选项、键名与 C++ 客户端的房间配置面板一致；
    // 改动即时 set_match_config，服务器校验后以 room_state 回显（非房主整块隐藏）。
    // roomCfg = 房间的属性回显（只读展示）、cfg = 下一把配置回显（高亮与补丁基线）
    function buildRoomRules(roomCfg, cfg) {
        var box = $('room-rules');
        var amHost = S.host === S.myId;
        box.classList.toggle('hidden', !amHost);
        // 重建前保留种子输入框里的内容（成员进出会反复触发 room_state → renderRoom）
        var prevSeed = '';
        var oldSeed = $('room-seed');
        if (oldSeed) prevSeed = oldSeed.value;
        box.innerHTML = '';
        if (!amHost) return;
        var title = document.createElement('h3');
        title.className = 'dim';
        title.textContent = t('room_rules_title');
        box.appendChild(title);
        // 士兵数与坐标系是"房间的属性"：创建时定死，房间里只展示（服务器也拒绝中途修改）
        var solLine = document.createElement('div');
        solLine.className = 'opt-label dim';
        solLine.textContent = t('soldiers_locked', { n: num(roomCfg.soldiers_per_player, 3) });
        box.appendChild(solLine);
        var coordLine = document.createElement('div');
        coordLine.className = 'opt-label dim';
        coordLine.textContent = t('coord_locked', {
            v: t(roomCfg.coord_origin === 'field' ? 'coord_field' : 'coord_soldier')
        });
        box.appendChild(coordLine);

        function pick(label, values, labelOf, current, key) {
            var group = document.createElement('div');
            group.className = 'opt-group';
            var lab = document.createElement('div');
            lab.className = 'opt-label';
            lab.textContent = label;
            group.appendChild(lab);
            var rowBox = document.createElement('div');
            rowBox.className = 'opt-row';
            values.forEach(function (v) {
                var b = document.createElement('button');
                b.type = 'button';
                b.textContent = labelOf(v);
                if (v === current) b.classList.add('active');
                b.addEventListener('click', function () {
                    // 单键补丁不会误改种子策略：服务器只对含 seed 字段的补丁更新
                    // 随机/固定策略（room.cpp 的 apply_config），所以无需像 C++ 那样
                    // 发全量配置再显式补 seed:null 表态
                    var patch = {};
                    patch[key] = v;
                    net.send({ type: 'set_match_config', match: patch });
                });
                rowBox.appendChild(b);
            });
            group.appendChild(rowBox);
            box.appendChild(group);
        }

        pick(t('cfg_hp'), [1, 2, 3, 5], String, num(cfg.hp, 1), 'hp');
        pick(t('cfg_seconds'), [15, 30, 60, 120], function (v) { return t('sec_n', { n: v }); },
            num(cfg.input_seconds, 120), 'input_seconds');
        pick(t('cfg_obstacles'), [0, 5, 10, 15, 20, 30], String, num(cfg.obstacle_count, 0), 'obstacle_count');
        pick(t('delay_label'), [0, 1, 3, 5], function (v) { return t('sec_n', { n: v }); },
            num(cfg.turn_delay, 3), 'turn_delay');
        pick(t('first_team_label'), [-1, 0, 1], function (v) {
            return v === -1 ? t('seed_random') : (v === 0 ? t('team_a') : t('team_b'));
        }, num(cfg.first_team, -1), 'first_team');

        // 种子策略：每把随机 / 固定（固定时用旁边的输入框填种子，点"应用"生效）
        var seedGroup = document.createElement('div');
        seedGroup.className = 'opt-group';
        var seedLab = document.createElement('div');
        seedLab.className = 'opt-label';
        seedLab.textContent = t('cfg_seed');
        seedGroup.appendChild(seedLab);
        var seedRow = document.createElement('div');
        seedRow.className = 'opt-row';
        var btnRand = document.createElement('button');
        btnRand.type = 'button';
        btnRand.textContent = t('seed_each');
        btnRand.classList.toggle('active', seedIsRandom());
        btnRand.addEventListener('click', function () {
            net.send({ type: 'set_match_config', match: { seed: null } });
        });
        seedRow.appendChild(btnRand);
        var seedInput = document.createElement('input');
        seedInput.id = 'room-seed';
        seedInput.inputMode = 'numeric';
        seedInput.placeholder = t('seed_fixed_ph');
        // 没手动输入过就回填服务器当前的固定种子（与 C++ 客户端 room_state 的回填一致）
        seedInput.value = prevSeed !== '' ? prevSeed
            : (!seedIsRandom() && cfg.seed !== null && cfg.seed !== undefined)
                ? String(num(cfg.seed, 0)) : '';
        seedRow.appendChild(seedInput);
        var btnFix = document.createElement('button');
        btnFix.type = 'button';
        btnFix.textContent = t('apply');
        btnFix.classList.toggle('active', !seedIsRandom());
        btnFix.addEventListener('click', function () {
            // 空输入静默不动（与 C++ 的应用按钮一致）；只接受十进制数字
            var s = seedInput.value.trim();
            if (s === '') return;
            if (!/^\d+$/.test(s)) {
                toast(t('bad_seed'));
                return;
            }
            net.send({ type: 'set_match_config', match: { seed: Number(s) } });
        });
        seedRow.appendChild(btnFix);
        seedGroup.appendChild(seedRow);
        box.appendChild(seedGroup);
    }

    function renderRoom() {
        $('room-title').textContent = t('room_title', { id: S.cfgRoomId !== undefined ? S.cfgRoomId : '?' });
        var teams = { A: $('team-a'), B: $('team-b'), S: $('team-s') };
        for (var k in teams) teams[k].innerHTML = '';
        var amHost = S.host === S.myId;
        S.members.forEach(function (m) {
            var li = document.createElement('li');
            var html = esc(m.name);
            if (m.is_ai) html += ' <span class="tag ai">' + t('ai_tag', { d: t('diff_' + num(m.difficulty, 0)) }) + '</span>';
            if (m.id === S.host) html += ' <span class="tag host">' + t('badge_host') + '</span>';
            if (m.id === S.myId) html += ' <span class="tag you">' + t('badge_you') + '</span>';
            if (amHost && m.id !== S.myId) {
                if (m.role === 'player') {
                    var teamBtn = document.createElement('button');
                    teamBtn.type = 'button';
                    teamBtn.className = 'mini';
                    teamBtn.textContent = m.team === 'A' ? t('to_team_b') : t('to_team_a');
                    teamBtn.addEventListener('click', function () {
                        net.send({ type: 'set_team', member_id: m.id, team: m.team === 'A' ? 'B' : 'A' });
                    });
                    li.appendChild(teamBtn);
                }
                var kickBtn = document.createElement('button');
                kickBtn.type = 'button';
                kickBtn.className = 'mini danger';
                kickBtn.textContent = t('kick');
                kickBtn.addEventListener('click', function () {
                    net.send({ type: 'kick', member_id: m.id });
                });
                li.appendChild(kickBtn);
            }
            if (m.id === S.myId) {
                // 自己的身份切换（与 C++ 客户端一致：仅等待/局间可用，对局中禁用）
                var specBtn = document.createElement('button');
                specBtn.type = 'button';
                specBtn.className = 'mini';
                specBtn.textContent = m.role === 'player' ? t('to_spectate') : t('to_play');
                specBtn.disabled = S.inGame;
                specBtn.addEventListener('click', function () {
                    net.send({ type: 'set_spectate', spectate: m.role === 'player' });
                });
                li.appendChild(specBtn);
            }
            var span = document.createElement('span');
            span.className = 'member-name';
            span.innerHTML = html;
            li.insertBefore(span, li.firstChild);
            var slot = m.role === 'spectator' ? teams.S : (m.team === 'B' ? teams.B : teams.A);
            slot.appendChild(li);
        });
        var roomCfg = S.roomCfg || {};
        var cfg = S.cfg || {};  // S.cfg = room_state 的 config（下一把配置）
        // 回显下一把配置（next_match，与入参补丁同键名）；种子按 seed 是否为 null 显示随机/固定；
        // 士兵数/坐标系来自房间的属性（room）
        $('room-cfg').textContent = t('room_cfg', {
            hp: num(cfg.hp, 1), sp: num(roomCfg.soldiers_per_player, 3), obs: num(cfg.obstacle_count, 0),
            sec: num(cfg.input_seconds, 120),
            coord: t(roomCfg.coord_origin === 'field' ? 'coord_field' : 'coord_soldier'),
            seed: seedIsRandom() ? t('seed_random') : num(cfg.seed, t('seed_random')),
        });
        $('host-tools').classList.toggle('hidden', !amHost);
        $('btn-start').classList.toggle('hidden', !amHost);
        $('room-hint').textContent = amHost ? t('host_hint') : t('wait_host');
        buildRoomRules(roomCfg, cfg);
    }

    // ---------- 对战 ----------
    function normalizePlayer(p) {
        var soldiers;
        if (Array.isArray(p.soldiers) && p.soldiers.length) {
            soldiers = p.soldiers.map(function (s) {
                return { pos: [s.pos[0], s.pos[1]], hp: num(s.hp, 1), max_hp: num(s.max_hp, num(s.hp, 1)), alive: s.alive !== false };
            });
        } else {
            soldiers = [{ pos: [p.pos[0], p.pos[1]], hp: num(p.hp, 1), max_hp: num(p.max_hp, num(p.hp, 1)), alive: p.alive !== false }];
        }
        return { id: p.id, name: p.name || ('#' + p.id), team: p.team, is_ai: !!p.is_ai, soldiers: soldiers };
    }

    function buildBattle(m) {
        // 本局的配置以 game_start 为准并写回（帮助面板与观战视角按钮都读它，
        // 与 C++ 的 battle_.cfg = config_from_json(room, world, match) 对齐；房间刚改过
        // 规则时避免用到旧值）：room = 房间属性、world = 世界常量、cfg = 本局实际配置
        var room = m.room || S.roomCfg || {};
        var world = m.world || S.worldCfg || {};
        var cfg = m.match || S.cfg || {};
        S.roomCfg = room;
        S.worldCfg = world;
        S.cfg = cfg;
        var players = (m.players || []).map(normalizePlayer);
        var me = null;
        for (var i = 0; i < players.length; i++) if (players[i].id === S.myId) me = players[i];
        var obstacles = (m.obstacles || []).map(function (o) {
            return {
                is_circle: o.is_circle !== false,
                center: [o.center[0], o.center[1]],
                radius: num(o.radius, 0),
                half_w: num(o.half_w, 0),
                half_h: num(o.half_h, 0),
                craters: (o.craters || []).map(function (c) { return [c[0], c[1]]; }),
            };
        });
        var rejoin = !!S.battle;
        S.battle = {
            players: players,
            obstacles: obstacles,
            renderCfg: {
                map_x_min: num(world.map_x_min, -24), map_x_max: num(world.map_x_max, 24),
                map_y_min: num(world.map_y_min, -15), map_y_max: num(world.map_y_max, 15),
                // 渲染尺寸取服务器回显（与命中判定同源）；兜底值等于 core 默认值
                player_half_size: num(world.player_half_size, 0.45),
                crater_radius: num(world.crater_radius, 1.2),
            },
            myTeam: me ? me.team : null,
            // 镜像视角：房间开了 mirror_view 且我是 B 队人类时，屏幕左右翻转；
            // 观战者（不在阵容里）可以用顶部的"视角"按钮自由切换（默认常规）
            viewMirror: cfg.mirror_view === true &&
                (me !== null ? me.team === 'B' : S.spectateMirror),
            shot: null,       // 当前一发炮弹：{path, speed}（完整轨迹淡线 + 弹头逐帧推进）
            projectile: null, // 弹头位置 {x,y}；服务器销毁炮弹后停住不动，等下回合清除
            projectileS: 0,   // 弹头当前弧长（把"已飞过"的一段画亮用；纯观感）
            projNetTravel: 0, // 服务器广播来的最新 travel（停滞检测的基准）
            projStopped: false, // 服务器 travel 不再增长=炮弹已停（销毁），本地停止推进
            projMsgAt: 0,     // 最近一次收到炮弹广播的时刻（收不到广播也要停推进）
            frameAt: 0,       // 上一渲染帧时刻（弹头推进的 dt 基准）
            flashes: [],      // 命中/阵亡/炸坑的扩散圈（纯视觉反馈，网页版自己的发挥）
            lastFunc: {},     // 各玩家"最近用过的函数"文本（shot 时记录，新开局清空）
            preview: null,   // 服务器代算的输入预览（curve_preview 返回的轨迹）
            previewAt: 0,    // 上次请求预览的时刻（客户端节流，与服务端 1 秒限频对齐）
            previewSentAt: 0, // 最近一次真正发出预览请求的时刻（识别并丢弃作废/迟到的响应）
            turn: null,
            round: num(m.round, 0),  // 缺省 0：首回合由 turn_begin 自增到 1（与 C++ 的 json_int 默认一致）
            over: null,
        };
        S.submitted = false;
        S.tooMany = false;
        S.attempts = 0;
        // 新对局复位观战视角（与 C++ 的 kGameStart 一致：从常规视角起步）；
        // "退出观战已发出"的防连点标志同样复位（C++ 在 kGameStart 里清 leaving_spectate）
        S.spectateMirror = false;
        S.leavingSpectate = false;
        // 预览广播状态归零（对齐 C++ 的 kGameStart）：否则上一局末尾的发送时刻/文本
        // 会拦掉新局第一条"正在写"
        S.lastPreviewAt = 0;
        S.lastPreviewText = '';
        // "再来一局"的等待状态复位（上一把点过 start 又被拒的情况）
        S.restartSent = false;
        var againBtn = $('btn-again');
        againBtn.disabled = false;
        againBtn.textContent = t('again');
        S.vote = { voted: [], total: 0 };
        updateVoteButton();
        updateRoundLabel();
        updateViewButton();
        $('over-overlay').classList.add('hidden');
        $('input-bar').classList.toggle('hidden', me === null);
        $('turn-banner').textContent = me === null ? t('spectating') : t('waiting_move');
        $('countdown').textContent = '';
        $('typing').textContent = '';
        updateInputState();
        if (rejoin) log('<span class="dim">' + esc(t('ev_new_game')) + '</span>');
        showScreen('battle');
    }

    // shot 到来：先用 request 记录"最近用过的函数"（截断规则与 C++ 同），再用 trail
    // 重建本发炮弹的完整轨迹（C++ 客户端在这同一位置本地采样；网页端不共享 core，
    // 轨迹由服务器下发）。弹头初始位置藏在士兵方块里（跳过半个边长再显示）——纯视觉，与命中判定无关
    function addShot(m) {
        var b = S.battle;
        if (!b) return;
        if (m.request && typeof m.request.expr_a === 'string' && m.request.expr_a !== '') {
            b.lastFunc[m.owner] = truncateUtf8(m.request.expr_a, 16);
        }
        b.shot = null;
        b.projectile = null;
        var path = FGPath.build(m.trail);
        if (!path.valid()) {
            console.warn('shot 没有可用轨迹（trail 缺失或过短）', m);
            return;
        }
        b.shot = { path: path, speed: num(m.speed, 0) };
        var half = b.renderCfg.player_half_size;
        b.projectileS = Math.min(half, path.length());
        b.projectile = path.at(b.projectileS);
        // 推进状态复位：同批到达的 projectiles 会设 projMsgAt，这里先给个安全初值
        b.projNetTravel = 0;
        b.projStopped = false;
        b.projMsgAt = performance.now();
    }

    // 炮弹位置：服务器约 20Hz 的降频广播只做两件事——温和校正弹头弧长（小偏差=
    // 网络时延的领先量，不动；大偏差=命中展示停顿/丢包，按比例拉近）与"停滞检测"。
    // 弹头逐帧位置由 frame() 按 shot.speed 本地推进；服务器 travel 不再增长说明
    // 炮弹已销毁（撞坑/撞人爆炸），必须停止推进——纯外推会画出"炸坑后还在飞"的假象
    function updateProjectiles(m) {
        var b = S.battle;
        if (!b) return;
        var list = m.list || [];
        if (!list.length) return;
        if (!b.shot) {
            // 本地没有轨迹（trail 缺失或重建失败）时才退回服务器坐标
            if (list[0].pos) b.projectile = { x: num(list[0].pos[0], 0), y: num(list[0].pos[1], 0) };
            return;
        }
        var travel = num(list[0].travel, 0);
        b.projMsgAt = performance.now();
        if (travel <= b.projNetTravel + 1e-9) {
            b.projStopped = true;  // 服务器位置不再前进：炮弹已停，本地也别再推进
            return;
        }
        b.projStopped = false;
        b.projNetTravel = travel;
        // 不在这里改弹头位置：位置由 frame() 连续平滑地追"外推目标"，见那里的说明
    }

    function onTurnBegin(m) {
        var b = S.battle;
        if (!b) return;
        b.turn = {
            playerId: m.player_id,
            soldier: m.soldier,
            deadline: performance.now() + num(m.input_seconds, 120) * 1000,
            nextPlayer: num(m.next_player, -1),
            nextSoldier: num(m.next_soldier, 0),
        };
        // 与 C++ 一致：服务器给了正数就用它；字段缺失（异常路径）按自增走
        var r = num(m.round, 0);
        b.round = (r > 0) ? r : (b.round + 1);
        updateRoundLabel();
        // 新一轮开始：清掉上一轮的炮弹与弹道痕迹（与 C++ 的 turn_begin 处理一致）。
        // 结算遮罩下没有 turn_begin，最后一发的完整弹道会保留在画面上
        b.shot = null;
        b.projectile = null;
        b.projectileS = 0;
        b.preview = null;      // 上一回合的预览线不残留
        b.previewSentAt = 0;   // 上一回合还在飞的预览响应就此作废（见 curve_preview 处理）
        $('typing').textContent = '';  // 上一轮的"正在写"不残留下回合（对齐 C++ turn_begin 的清空）
        S.lastPreviewText = '';        // 允许新回合用相同文本时重新广播（对齐 C++ 的清空）
        log(t('ev_turn', { name: nameHtml(m.player_id), s: m.soldier }));
        // "已提交"随新回合无条件结束（对齐 C++：turn_begin 里无条件复位 battle_.submitted）；
        // 只在 mine 分支清会残留到对手回合，按钮一直显示"已提交，等待飞行…"
        S.submitted = false;
        var mine = (m.player_id === S.myId);
        if (mine) {
            S.tooMany = false;
            S.attempts = 0;
            $('turn-banner').innerHTML = t('banner_your_turn', { s: m.soldier });
            var a = $('expr-a');
            if (a) a.focus();
            requestPreview(true);  // 输入框里若还留着上回合的表达式，立刻把预览画出来
        } else {
            var p = playerById(m.player_id);
            var nm = p ? esc(p.name) : '#' + m.player_id;
            $('turn-banner').innerHTML = (p && p.is_ai)
                ? '<span class="dim">' + esc(t('banner_thinking', { name: p.name })) + '</span>'
                : t('banner_turn', { name: nm, s: m.soldier });
        }
        updateInputState();
    }

    function onHit(m) {
        var p = playerById(m.target);
        if (p && p.soldiers[m.soldier]) {
            p.soldiers[m.soldier].hp = num(m.hp_left, p.soldiers[m.soldier].hp);
        }
        S.battle.flashes.push({ pos: m.pos, kind: 'hit', t0: performance.now() / 1000 });
        log(t('ev_hit', { name: nameHtml(m.target), s: m.soldier, hp: num(m.hp_left, 0) }));
    }

    function onDeath(m) {
        var p = playerById(m.player_id);
        if (p && p.soldiers[m.soldier]) p.soldiers[m.soldier].alive = false;
        var pos = p && p.soldiers[m.soldier] ? p.soldiers[m.soldier].pos : [0, 0];
        S.battle.flashes.push({ pos: pos, kind: 'death', t0: performance.now() / 1000 });
        log(t('ev_death', { name: nameHtml(m.player_id), s: m.soldier }));
    }

    function onCrater(m) {
        var o = S.battle.obstacles[m.obstacle];
        if (o) o.craters.push(m.pos);
        S.battle.flashes.push({ pos: m.pos, kind: 'crater', t0: performance.now() / 1000 });
        log(t('ev_crater'));
    }

    function onGameOver(m) {
        var b = S.battle;
        if (!b) return;
        b.over = m.winner;
        // 与 C++ 一致：结算时弹头隐藏、完整弹道保留（遮罩下仍能看到最后一发的轨迹）。
        // turn 有意不清——C++ 的结算画面同样保留最后出手者的金框/呼吸圈
        b.projectile = null;
        var text;
        if (m.winner === 2) text = t('over_draw');
        else if (b.myTeam === null) text = (m.winner === 0) ? t('over_a') : t('over_b');
        else {
            var myWin = (m.winner === 0 && b.myTeam === 'A') || (m.winner === 1 && b.myTeam === 'B');
            text = myWin ? t('over_win') : t('over_lose');
        }
        // 观战者拿到的 text 本身就是"A 队胜利/平局"，再拼括号会显示"A 队胜利（A 队胜利）"；
        // 只有参赛者（"你赢了/你输了"）才补一句胜方信息
        var suffix = b.myTeam !== null
            ? '（' + (m.winner === 0 ? t('over_a') : m.winner === 1 ? t('over_b') : t('over_draw')) + '）'
            : '';
        $('over-text').textContent = text + suffix;
        // 非房主也显示"再来一局"位：置灰并转"等待房主"（与 C++ 结算卡片一致，看得见在等谁）
        var againEl = $('btn-again');
        var againHost = (S.host === S.myId);
        againEl.classList.remove('hidden');
        againEl.disabled = !againHost;
        againEl.textContent = againHost ? t('again') : t('waiting_host');
        $('over-overlay').classList.remove('hidden');
        updateVoteButton();  // 结算后隐藏投票按钮（与 C++ 一致）
        log('<b>' + esc(text) + '</b>');
        $('turn-banner').textContent = '';
        $('countdown').textContent = '';
    }

    // ---------- 函数帮助面板 ----------
    // 清单与说明照抄 C++ 客户端（ui_battle.cpp 的 kExprFuncGroups 与规则表）。其中
    // "坐标系/平移"几行是写函数瞄准的语义：以你（发射点）为原点、曲线整体平移使起点落在
    // 你的位置——网页版与 C++ 完全相同（预览线与实弹都是服务器按这套规则采样的）
    var HELP_GROUPS = [
        ['基本运算', '+  -  *  /  ^（乘方）和括号 ( )', ''],
        ['三角', 'sin  cos  tan  asin  acos  atan', ''],
        ['双曲', 'sinh  cosh  tanh  asinh  acosh  atanh', ''],
        ['指数对数', 'exp  log  log10  log2  sqrt  pow', 'exp 即 e^x，pow(a,b) 为 a 的 b 次方'],
        ['取整符号', 'floor  ceil  round  trunc  abs  sgn', 'sgn 取正负号（-1/0/1）'],
        ['多参数', 'min  max  hypot  atan2', '如 max(x,3)、hypot(a,b)、atan2(y,x)'],
        ['误差', 'erf  erfc', ''],
        ['阶跃', 'step  heaviside', 'x<0 得 0，x≥0 得 1；step 是别名'],
    ];
    // 规则说明按房间的曲线参考系（coord_origin）切换两套文案，
    // 与 C++ 客户端 ui_battle.cpp 帮助面板的 rules_soldier / rules_field 逐行一致
    var HELP_RULES_SOLDIER = [
        ['常量', 'pi（圆周率）    e（自然常数）'],
        ['变量', 'y=f(x)→x     x=f(y)→y    r=f(t)→theta'],
        ['', "参数方程→t    dy/dx→x,y    (x',y')→t,x,y    y''→x,y,dy"],
        ['坐标系', '只影响 y=f(x) 与 x=f(y)：本房间以你为原点（x=0、y=0 就在你身上）'],
        ['', 'y=f(x)：曲线只做上下平移，使 x=0 处的点正好落在你身上'],
        ['', 'x=f(y)：曲线只做左右平移，使 y=0 处的点正好落在你身上'],
        ['', '其他模式不受影响：极坐标 / 参数方程从你出发；微分方程本来就按地图坐标'],
        ['', "y''：dy 是当前斜率，初值由发射角决定（度，正数朝发射方向上方偏）"],
        ['平移', '曲线会被整体平移，让出发点正好落在你身上；加任意常数（如 +5）不影响弹道'],
        ['示例', '2*sin(0.5*x) + 0.2*x'],
        ['', '5*step(x-10)（前进 10 后抬升）'],
    ];
    var HELP_RULES_FIELD = [
        ['常量', 'pi（圆周率）    e（自然常数）'],
        ['变量', 'y=f(x)→x     x=f(y)→y    r=f(t)→theta'],
        ['', "参数方程→t    dy/dx→x,y    (x',y')→t,x,y    y''→x,y,dy"],
        ['坐标系', '只影响 y=f(x) 与 x=f(y)：本房间按地图坐标（函数里的数字就是地图上的）'],
        ['', 'y=f(x)：只做上下平移——往上平移 y0-f(x0) 后压到你身上'],
        ['', 'x=f(y)：只做左右平移——往右平移 x0-f(y0) 后压到你身上。(x0,y0) 是你的位置'],
        ['', '其他模式不受影响：极坐标 / 参数方程从你出发；微分方程本来就按地图坐标'],
        ['', "y''：dy 是当前斜率，初值由发射角决定（度，正数朝发射方向上方偏）"],
        ['平移', '曲线会被整体平移，让出发点正好落在你身上；加任意常数（如 +5）不影响弹道'],
        ['示例', '2*sin(0.5*x) + 0.2*x'],
        ['', '5*step(x-10)（前进 10 后抬升）'],
    ];

    function renderHelp() {
        var box = $('help-body');
        // 规则文案随房间的曲线参考系切换，房间配置可能在两局之间改过，
        // 所以每次打开都重建（节点不多，不值得为它做缓存失效）
        box.innerHTML = '';
        function row(key, main, note) {
            var div = document.createElement('div');
            div.className = 'help-row';
            var k = document.createElement('span');
            k.className = 'help-key';
            k.textContent = key;
            var m = document.createElement('span');
            m.className = 'help-main';
            m.textContent = main;
            var n = document.createElement('span');
            n.className = 'help-note';
            n.textContent = note;
            div.appendChild(k);
            div.appendChild(m);
            div.appendChild(n);
            box.appendChild(div);
        }
        HELP_GROUPS.forEach(function (g) { row(g[0], g[1], g[2]); });
        var sep = document.createElement('div');
        sep.className = 'help-sep';
        box.appendChild(sep);
        var rules = (S.roomCfg && S.roomCfg.coord_origin === 'field') ? HELP_RULES_FIELD : HELP_RULES_SOLDIER;
        rules.forEach(function (r) { row(r[0], r[1], ''); });
    }

    // 顶部"第 N 回合"（与 C++ 客户端顶部左侧一致）
    function updateRoundLabel() {
        // 下限钳到 1（与 C++ 的 std::max(1, battle_.round) 一致）：中途观战快照可能带 0
        $('round-n').textContent = S.battle ? t('round_n', { n: Math.max(1, S.battle.round) }) : '';
    }

    // 观战者的视角切换按钮：只在"镜像是房间规则 + 我是观战者"时出现
    //（参赛者的视角由队伍决定，照 C++ 客户端一致：观战者默认常规视角）
    function updateViewButton() {
        var b = S.battle;
        var spect = b && b.myTeam === null && S.cfg && S.cfg.mirror_view === true;
        var btn = $('btn-view');
        btn.classList.toggle('hidden', !spect);
        btn.textContent = spect ? (b.viewMirror ? t('view_mirror') : t('view_normal')) : '';
    }

    function updateVoteButton() {
        var btn = $('btn-vote');
        var b = S.battle;
        // 与 C++ 一致：只有"在局参战者、对局未结束"才显示投票按钮
        //（观战者与结算后不显示——他们没有投票资格）
        var show = !!(b && b.myTeam !== null && b.over === null && S.myId !== null);
        btn.classList.toggle('hidden', !show);
        if (!show) return;
        var voted = S.vote.voted.indexOf(S.myId) >= 0;
        var label = voted ? t('vote_voted') : t('vote_skip');
        if (S.vote.total > 0) label += ' ' + t('voting', { n: S.vote.voted.length, total: S.vote.total });
        btn.textContent = label;
        btn.classList.toggle('active', voted);
    }

    // ---------- 输入面板 ----------
    function currentMode() { return MODES[S.modeIdx]; }

    function myTurnActive() {
        var b = S.battle;
        return !!(b && b.turn && b.myTeam !== null && !b.over && b.turn.playerId === S.myId);
    }

    function updateInputState() {
        var active = myTurnActive() && !S.submitted && !S.tooMany;
        var btn = $('btn-submit');
        btn.disabled = !active;
        btn.textContent = S.submitted ? t('submitted') : t('submit');
        // 模式标签与提交按钮同权限（与 C++ 的 can_submit 一致）：非本回合/已提交/结算时不可切换
        var tabs = $('mode-tabs').children;
        for (var k = 0; k < tabs.length; k++) tabs[k].disabled = !active;
        $('attempts').textContent = t('attempts_fmt', { used: S.attempts, max: MAX_ATTEMPTS });
    }

    function buildModeTabs() {
        var box = $('mode-tabs');
        box.innerHTML = '';
        MODES.forEach(function (m, i) {
            var b = document.createElement('button');
            b.type = 'button';
            b.textContent = t(m.label);
            b.addEventListener('click', function () { selectMode(i); });
            box.appendChild(b);
        });
        selectMode(0);
    }

    function selectMode(i) {
        // 对局中与提交按钮同权限（初始化界面时 S.battle 还没建，不受限）
        if (S.battle && !(myTurnActive() && !S.submitted && !S.tooMany)) return;
        if (S.battle) $('expr-a').focus();  // 焦点交回表达式框（C++ 切模式后同样重聚焦，否则键盘打不进表达式）
        // 方向选择跨模式保留（与 C++ 客户端的 dir_index_ 一致）；初始为正向
        S.modeIdx = i;
        var m = currentMode();
        var tabs = $('mode-tabs').children;
        for (var k = 0; k < tabs.length; k++) tabs[k].classList.toggle('active', k === i);
        $('label-a').textContent = t(m.a);
        $('field-b').classList.toggle('hidden', !m.b);
        if (m.b) { $('label-b').textContent = t(m.b); $('expr-b').placeholder = m.ph2 || ''; }
        $('field-range').classList.toggle('hidden', !m.range);
        $('p-start').placeholder = m.id === 'polar' ? '0' : (m.id === 'parametric' ? '0' : '');
        $('p-end').placeholder = m.id === 'polar' ? '6.2832' : (m.id === 'parametric' ? '1000' : '');
        $('field-angle').classList.toggle('hidden', !m.angle);
        var dirBox = $('dir-buttons');
        dirBox.classList.toggle('hidden', !m.dir);
        if (m.dir) {
            dirBox.innerHTML = '';
            [false, true].forEach(function (neg, idx) {
                var b = document.createElement('button');
                b.type = 'button';
                b.textContent = t(m.dir[idx]);
                b.dataset.neg = neg ? '1' : '0';
                if (neg === S.dirNeg) b.classList.add('active');
                b.addEventListener('click', function () {
                    S.dirNeg = neg;
                    dirBox.querySelectorAll('button').forEach(function (x) { x.classList.remove('active'); });
                    b.classList.add('active');
                    // 方向是低频离散动作：立即重算预览，不被 1050ms 节流拖住（C++ 的预览随变化即时刷新）
                    requestPreview(true);
                });
                dirBox.appendChild(b);
            });
        }
        $('expr-a').placeholder = m.ph || '';
        $('hint').textContent = t(m.hint);
        requestPreview(true);  // 切模式立即重算预览：否则 1050ms 内会残留旧模式的曲线
    }

    function buildRequest() {
        var m = currentMode();
        var req = { mode: m.id, expr_a: $('expr-a').value.trim(), dir: S.dirNeg ? 'neg' : 'pos' };
        if (m.b) req.expr_b = $('expr-b').value.trim();
        if (m.range) {
            var a = strictNum($('p-start').value);
            var e = strictNum($('p-end').value);
            if (a !== null) req.p_start = a;
            if (e !== null) req.p_end = e;
        }
        if (m.angle) {
            var d = strictNum($('angle').value);
            if (d !== null) {
                // 客户端与服务端的约定：角度输入按度、clamp 到 ±85，再换算成弧度
                d = Math.max(-85, Math.min(85, d));
                req.angle = d * Math.PI / 180;
            }
        }
        var b = S.battle;
        if (b && b.viewMirror && b.myTeam === 'B') req.mirror = true;
        return req;
    }

    // 数字输入框（参数范围/发射角）的字符过滤：与 C++ TextBox 的 digits_only 同款——
    // 只放行数字、小数点、正负号（参数范围框另允许小写 e 的科学计数法，角度框不允许）。
    // 靠源头过滤对齐：能进框的字符集一致后，0x/空格/大写 E 这些 C++ 打不进的内容
    // 在网页端也进不来，Number 与 strtod 的解析差异就无从触发
    var NUM_OK = /[0-9.+-]/;
    function filterNumInput(el, allowE) {
        var before = el.value;
        var pos = el.selectionStart;
        var removed = 0;
        var filtered = '';
        for (var i = 0; i < before.length; i++) {
            var c = before[i];
            if (NUM_OK.test(c) || (allowE && c === 'e')) filtered += c;
            else if (i < pos) removed++;
        }
        if (filtered !== before) {
            el.value = filtered;
            var np = Math.max(0, pos - removed);
            el.setSelectionRange(np, np);
        }
    }

    // 表达式输入的字符白名单（与 C++ 客户端 TextBox 的 expr_chars_only 一致）：
    // 只放行数字、字母、_ + - * / ^ ( ) . , 和空格；非法字符当场剔除，
    // 光标按删掉的字符数回退，输入位置不跳
    var EXPR_OK = /[0-9a-zA-Z_+\-*/^()., ]/;
    function filterExprInput(el) {
        var before = el.value;
        var pos = el.selectionStart;
        var removed = 0;
        var filtered = '';
        for (var i = 0; i < before.length; i++) {
            if (EXPR_OK.test(before[i])) filtered += before[i];
            else if (i < pos) removed++;
        }
        if (filtered !== before) {
            el.value = filtered;
            var np = Math.max(0, pos - removed);
            el.setSelectionRange(np, np);
        }
    }

    function doSubmit() {
        if (!myTurnActive() || S.submitted || S.tooMany) return;
        var req = buildRequest();
        if (!req.expr_a) { toast(t('submit_fail', { msg: '表达式为空' })); return; }
        if (!net.send({ type: 'submit', request: req })) return;
        S.submitted = true;
        S.attempts++;
        S.battle.preview = null;  // 炮弹马上会盖过预览线
        updateInputState();
    }

    // "正在写"的广播内容与 C++ 客户端一致：模式标签 + 主表达式（参数方程/一阶方程组
    // 再加第二式），200ms 节流——服务器另有 100ms 限频与"只转发当前回合玩家"的把关
    function sendPreview() {
        if (!myTurnActive() || S.submitted) return;
        if ($('expr-a').value === '') return;  // 空表达式不广播（C++ 同）
        var m = currentMode();
        var text = t(m.a) + ' ' + $('expr-a').value;
        if (m.b && $('expr-b').value !== '') text += '，' + $('expr-b').value;
        text = text.slice(0, 400);
        if (text === S.lastPreviewText) return;  // 内容没变不重发（与 C++ 的 sent_preview_text_ 一致）
        var now = performance.now();
        if (now - S.lastPreviewAt < 200) return;
        S.lastPreviewAt = now;
        S.lastPreviewText = text;
        net.send({ type: 'function_preview', preview: text });
    }

    // 曲线预览：网页端不共享 core、本地算不了，请服务器采样（preview_curve）。
    // 客户端节流放在服务端 1 秒窗口之上留一点余量（1050ms），常态下不会撞限频错误
    function requestPreview(force) {
        var b = S.battle;
        if (!b || !myTurnActive() || S.submitted) return;
        var now = performance.now();
        if (!force && now - b.previewAt < 1050) return;
        var req = buildRequest();
        if (!req.expr_a) {
            // 输入框清空：预览线清掉，在飞的响应一并作废（防止迟到的响应把旧线画回来）
            b.preview = null;
            b.previewSentAt = 0;
            return;
        }
        b.previewAt = now;
        b.previewSentAt = now;
        net.send({ type: 'preview_curve', request: req });
    }

    // ---------- 每帧 ----------
    function updateHud(now) {
        var b = S.battle;
        if (!b) return;
        if (b.turn && !b.over) {
            var rem = (b.turn.deadline - now) / 1000;
            $('countdown').textContent = rem > 0 ? t('remaining', { n: rem.toFixed(1) }) : t('waiting_move');
        }
        if (b.flashes.length) {
            b.flashes = b.flashes.filter(function (f) { return now / 1000 - f.t0 < 0.6; });
        }
    }

    function frame() {
        requestAnimationFrame(frame);
        var now = performance.now();
        if (S.screen !== 'battle' || !S.battle) return;
        updateHud(now);
        var b = S.battle;
        // 弹头本地推进：服务器 20Hz 广播到达之间的逐帧位置由"外推 + 平滑"得到——
        // 目标 = 最新 travel 按流逝时间外推出的"服务器此刻应在的位置"（消息到达时
        // 天然连续，不会瞬移）；弹头每帧按比值（dt×12）向目标收敛，抹掉 RTT 抖动。
        // 停滞（projStopped）或超过 0.25s 收不到广播（炮弹已销毁）即停止推进
        var dt = b.frameAt ? Math.min((now - b.frameAt) / 1000, 0.1) : 0;
        b.frameAt = now;
        if (b.shot && b.projectile && b.projectileS > 0 && !b.projStopped &&
            now - b.projMsgAt < 250) {
            var target = b.projNetTravel + num(b.shot.speed, 0) * ((now - b.projMsgAt) / 1000);
            b.projectileS += (target - b.projectileS) * Math.min(1, dt * 12);
            b.projectile = b.shot.path.at(b.projectileS);
        }
        var turn = b.turn;
        renderer.draw({
            cfg: b.renderCfg,
            players: b.players,
            obstacles: b.obstacles,
            myId: S.myId,
            turn: turn ? {
                playerId: turn.playerId, soldier: turn.soldier,
                nextPlayer: turn.nextPlayer, nextSoldier: turn.nextSoldier,
            } : null,
            lastFunc: b.lastFunc,
            previewPath: b.preview ? b.preview.path : null,
            projectilePath: b.shot ? b.shot.path : null,
            projectile: b.projectile,
            projectileS: b.projectileS,
            flashes: b.flashes,
            mirror: b.viewMirror,
            time: now / 1000,
        });
    }

    // ---------- 服务器消息 ----------
    function wireNet() {
        // 未连接时禁用创建/加入/观战/刷新按钮（对齐 C++ 大厅的 conn 门控）：
        // 连接不上时点这些按钮只会静默失败，玩家会以为界面卡死
        function updateConnButtons() {
            var ok = net.connected;
            ['btn-create', 'btn-join', 'btn-join-spec', 'btn-refresh'].forEach(function (id) {
                $(id).disabled = !ok;
            });
        }
        updateConnButtons();  // 初始（未连接）即置灰，连上后由 __open 恢复
        net.on('__open', function () {
            $('conn-status').textContent = t('status_online');
            $('conn-status').classList.remove('off');
            updateConnButtons();
        });
        net.on('__close', function () {
            // 断线即清会话、回主菜单（与 C++ 客户端的断线处理一致）：
            // 不清的话界面会冻结在房间里/对局里，所有操作静默失败，只能刷新页面
            S.myId = null; S.host = null; S.members = []; S.cfg = null;
            S.roomCfg = null; S.worldCfg = null;
            S.battle = null; S.inGame = false; S.vote = { voted: [], total: 0 };
            S.submitted = false; S.tooMany = false; S.restartSent = false;
            S.spectateMirror = false; S.leavingSpectate = false;
            $('over-overlay').classList.add('hidden');
            showScreen('menu');
            $('conn-status').textContent = t('status_offline');
            $('conn-status').classList.add('off');
            updateConnButtons();
            toast(t('conn_lost') + ' · ' + t('conn_fail_hint'));
        });

        net.on('hello', function () { /* 连接建立；握手信息无需展示 */ });

        net.on('joined', function (m) {
            S.myId = m.member_id;
            S.host = m.host;
            S.cfgRoomId = m.room_id;
        });

        net.on('room_state', function (m) {
            S.members = m.members || [];
            S.host = m.host;
            S.inGame = !!m.in_game;
            S.roomCfg = m.room || S.roomCfg;
            S.worldCfg = m.world || S.worldCfg;
            S.cfg = m.next_match || S.cfg;
            if (S.screen === 'menu' && S.myId !== null) showScreen('room');
            if (S.screen === 'room') renderRoom();
        });

        net.on('left_room', function () {
            S.myId = null; S.host = null; S.members = []; S.battle = null; S.inGame = false;
            clearRoomSeedBox();
            $('conn-status').textContent = t('status_online');
            showScreen('menu');
        });

        net.on('kicked', function () {
            toast(t('ev_you_kicked'));
            log('<span class="dim">' + esc(t('ev_you_kicked')) + '</span>');
            // 与 left_room 相同的一次性清理（host 也要清，防迟到的 room_state 按旧身份解析）
            S.myId = null; S.host = null; S.members = []; S.battle = null; S.inGame = false;
            clearRoomSeedBox();
            showScreen('menu');
        });

        net.on('room_list', function (m) {
            var box = $('room-list');
            box.innerHTML = '';
            var rooms = m.rooms || [];
            if (!rooms.length) {
                box.innerHTML = '<div class="dim">' + esc(t('no_rooms')) + '</div>';
                return;
            }
            rooms.forEach(function (r) {
                // 每个房间两个入口，与 C++ 客户端的房间列表一致：
                // "加入"仅未开局时出现，"观战"任何时候都有（进行中的局以观战身份进）
                var item = document.createElement('div');
                item.className = 'room-item';
                var label = document.createElement('span');
                label.innerHTML = esc(t('room_line', { id: r.id, players: r.players, spec: r.spectators })) +
                    ' <span class="tag">' + esc(r.in_game ? t('room_in_game') : t('room_waiting')) + '</span>' +
                    (r.has_password ? ' <span class="tag">' + esc(t('room_has_password')) + '</span>' : '');
                item.appendChild(label);
                function join(spectate) {
                    var name = $('menu-name').value.trim() || '玩家';
                    S.myName = name;
                    var msg = { type: 'join_room', room_id: r.id, name: name, trail: true };
                    if (r.has_password) {
                        // 列表里已标了"有密码"：就地问一次，取消则中止（不空手撞服务器）
                        var pw = prompt(t('password_prompt'));
                        if (pw === null) return;
                        if (pw !== '') msg.password_hash = sha256Hex(pw);
                    }
                    if (spectate) msg.spectate = true;
                    net.send(msg);
                }
                if (!r.in_game) {
                    var jb = document.createElement('button');
                    jb.type = 'button';
                    jb.className = 'mini';
                    jb.textContent = t('join');
                    jb.addEventListener('click', function () { join(false); });
                    item.appendChild(jb);
                }
                var sb = document.createElement('button');
                sb.type = 'button';
                sb.className = 'mini';
                sb.textContent = t('spectate_join');
                sb.addEventListener('click', function () { join(true); });
                item.appendChild(sb);
                box.appendChild(item);
            });
        });

        net.on('game_start', function (m) { buildBattle(m); });

        net.on('turn_begin', function (m) { onTurnBegin(m); });

        net.on('turn_skipped', function (m) {
            log(t('ev_skip', { name: nameHtml(m.player_id) }));
            if (m.player_id === S.myId && myTurnActive()) {
                S.submitted = true; // 本回合已被服务器跳过，禁止再提交
                updateInputState();
            }
        });

        net.on('shot', function (m) {
            if (!S.battle) return;
            addShot(m);
            log(t('ev_shot', { name: nameHtml(m.owner) }));
        });

        net.on('projectiles', function (m) { if (S.battle) updateProjectiles(m); });
        net.on('hit', function (m) { if (S.battle) onHit(m); });
        net.on('death', function (m) { if (S.battle) onDeath(m); });
        net.on('crater', function (m) { if (S.battle) onCrater(m); });

        net.on('skip_vote', function (m) {
            S.vote = { voted: m.voted || [], total: num(m.total, 0) };
            updateVoteButton();
        });

        net.on('function_preview', function (m) {
            // 只认当前回合玩家的"正在写"（服务器已过滤，这里防消息在途换回合）；
            // 自己的不显示（表达式就在输入框里，C++ 同）
            if (m.member_id === S.myId) return;
            if (!S.battle || !S.battle.turn || m.member_id !== S.battle.turn.playerId) return;
            var p = playerById(m.member_id);
            var name = p ? p.name : '#' + m.member_id;
            // 显示保留到回合结束（turn_begin 时清空，与 C++ 一致），不设超时：
            // 对手停手思考时清掉会让等待方误以为对手已提交
            $('typing').textContent = t('typing_fmt', { name: name }) + m.preview;
        });

        net.on('curve_preview', function (m) {
            var b = S.battle;
            // 预览只对"我的回合且还没提交"有意义：回合换人、或已提交（响应可能晚于
            // doSubmit 清预览的那一下到达）时都直接丢弃，不然飞行期间预览线会重新冒出来
            if (!b || !myTurnActive() || S.submitted) return;
            // 还必须是"本回合刚发出的请求"的响应：回合开始、清空输入框都会把
            // previewSentAt 归零，弱网下跨回合迟到的旧响应会被这里挡住；
            // 同回合内连续请求的响应按 TCP 顺序到达，后到的自然覆盖先到的
            var now = performance.now();
            if (!b.previewSentAt || now - b.previewSentAt > 5000) return;
            b.preview = { path: FGPath.build(m.trail) };
        });

        net.on('game_over', function (m) { onGameOver(m); });

        net.on('error', function (m) {
            if (m.code === 'room_closed') {
                // 房间已销毁（房主离开后空房被回收等）：清干净回主菜单，
                // 否则界面会抱着一个不存在的房间（与 C++ 客户端的处理一致）
                S.myId = null; S.host = null; S.members = []; S.battle = null; S.inGame = false;
                S.leavingSpectate = false;  // "退出观战"的 leave_room 被拒（房间已没了）：允许重来
                showScreen('menu');
                toast(m.message);
                return;
            }
            // "退出观战"的 leave_room 被其它原因拒绝：复位防连点标志（与 C++ 同，否则按钮永久哑住）
            if (m.context === 'leave') S.leavingSpectate = false;
            if (S.restartSent) {
                // "再来一局"的 start 被拒：恢复按钮让房主能重试（结算层一直没收起）
                S.restartSent = false;
                $('btn-again').disabled = false;
                $('btn-again').textContent = t('again');
            }
            if (m.context === 'submit') {
                // 提交被拒：复位"已提交"，允许重提；次数用尽是服务器规则
                S.submitted = false;
                if (m.code === 'submit_too_many') S.tooMany = true;
                updateInputState();
                toast(t('submit_fail', { msg: m.message }));
                log('<span class="danger">' + esc(t('submit_fail', { msg: m.message })) + '</span>');
            } else if (m.context === 'preview') {
                // 打字中途语法不完整是常态：静默清掉旧预览线即可，不弹提示；
                // 限频错误说明手上的预览还新鲜，保留不动作
                if (m.code !== 'preview_too_frequent' && S.battle) S.battle.preview = null;
            } else if (m.context === 'leave') {
                toast(m.message);
            } else {
                toast(m.message);
                log('<span class="danger">' + esc(m.message) + '</span>');
            }
        });
    }

    // ---------- 事件绑定 ----------
    function bindEvents() {
        $('btn-create').addEventListener('click', doCreate);
        // click 事件对象会被当参数传进来，用闭包包一层固定传参（否则 event 是 truthy）
        $('btn-join').addEventListener('click', function () { doJoin(false); });
        $('btn-join-spec').addEventListener('click', function () { doJoin(true); });
        $('btn-refresh').addEventListener('click', function () { net.send({ type: 'list_rooms' }); });

        $('btn-add-ai').addEventListener('click', function () {
            net.send({
                type: 'add_ai',
                difficulty: parseInt($('ai-diff').value, 10),
                team: $('ai-team').value,
            });
        });
        $('btn-start').addEventListener('click', function () { net.send({ type: 'start' }); });
        $('btn-leave').addEventListener('click', function () { net.send({ type: 'leave_room' }); });

        $('btn-submit').addEventListener('click', doSubmit);
        $('btn-preview').addEventListener('click', function () { requestPreview(true); });
        ['expr-a', 'expr-b'].forEach(function (id) {
            $(id).addEventListener('input', function () {
                filterExprInput(this);
                sendPreview();
                requestPreview(false);
            });
        });
        // 参数范围框允许科学计数法 e；角度框不让 e 进入（C++ 的 allow_e=false：
        // 输入 1e2 会被读成 100 再夹到 85，误导玩家）
        ['p-start', 'p-end'].forEach(function (id) {
            $(id).addEventListener('input', function () {
                filterNumInput(this, true);
                requestPreview(false);
            });
        });
        $('angle').addEventListener('input', function () {
            filterNumInput(this, false);
            requestPreview(false);
        });
        ['expr-a', 'expr-b'].forEach(function (id) {
            $(id).addEventListener('keydown', function (ev) {
                if (ev.key === 'Enter') { ev.preventDefault(); doSubmit(); }
            });
        });

        $('btn-vote').addEventListener('click', function () {
            var voted = S.vote.voted.indexOf(S.myId) >= 0;
            net.send({ type: 'vote_skip', vote: !voted });
        });

        $('btn-view').addEventListener('click', function () {
            // 观战视角切换：只改本地看到的翻转方向，对服务器与世界无任何影响
            if (!S.battle || S.battle.myTeam !== null) return;
            S.spectateMirror = !S.spectateMirror;
            S.battle.viewMirror = S.spectateMirror;
            updateViewButton();
        });
        $('btn-help').addEventListener('click', function () {
            renderHelp();
            $('help-overlay').classList.remove('hidden');
        });
        $('btn-help-close').addEventListener('click', function () {
            $('help-overlay').classList.add('hidden');
        });
        $('help-overlay').addEventListener('click', function (ev) {
            // 点卡片外的遮罩同样关闭（模态的常规操作）
            if (ev.target === $('help-overlay')) $('help-overlay').classList.add('hidden');
        });

        $('btn-leave-battle').addEventListener('click', function () {
            // 对局中参战者不能离开（服务器规则）：本地直接提示，省一次往返；
            // 观战者随时可走，打完之后也放行
            var b = S.battle;
            if (b && b.myTeam !== null && b.over === null) {
                toast(t('leave_ingame_title'));
                return;
            }
            if (S.leavingSpectate) return;  // 已发出、等回应（防连点，与 C++ 的 leaving_spectate 一致）
            S.leavingSpectate = true;
            net.send({ type: 'leave_room' });
        });

        $('btn-again').addEventListener('click', function () {
            // 不立刻收起结算层：start 可能被服务器拒绝（人数不足等），收起后就没有
            // 重试入口了。按钮转为"等待开始"，成功时由 buildBattle 收起、失败时由
            // error 处理恢复（与 C++ 客户端的 restart_sent 机制一致）。
            // 只发 start：换不换地图由房主设的种子策略决定
            if (S.restartSent) return;
            S.restartSent = true;
            this.disabled = true;
            this.textContent = t('waiting_start');
            net.send({ type: 'start' });
        });
        $('btn-to-room').addEventListener('click', function () {
            $('over-overlay').classList.add('hidden');
            showScreen('room');
            renderRoom();
        });
    }

    // ---------- 启动 ----------
    function init() {
        applyStaticI18n();
        buildMenuOptions();
        buildModeTabs();
        bindEvents();
        wireNet();
        renderer = FGRender.create($('battle-canvas'));
        requestAnimationFrame(frame);
        $('conn-status').textContent = t('status_connecting');
        net.connect();
    }

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', init);
    } else {
        init();
    }
})();
