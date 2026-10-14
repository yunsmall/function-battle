// 界面文案键值表：所有可见文案走 t(key, params)（占位符 {x}）。
// 现在只有中文；将来加语言在这里补一份字典、setLang 切默认即可。
(function (global) {
    'use strict';

    var DICTS = {
        zh: {
            // 连接状态
            status_connecting: '连接中…',
            status_online: '已连接',
            status_offline: '未连接',
            conn_lost: '与服务器的连接断开了',
            conn_fail_hint: '请确认游戏服务器（fb_server）已启动，然后刷新页面',

            // 主菜单 / 建房
            create_room: '创建对局',
            your_name: '你的名字',
            name_ph: '玩家',
            cfg_soldiers: '每人士兵数',
            soldier_locked_short: '创建后不可更改',
            first_team_a: 'A 队先手',
            first_team_b: 'B 队先手',
            cfg_hp: '每人血量',
            cfg_obstacles: '遮挡物数量',
            cfg_seed: '地图种子',
            seed_ph: '留空 = 每把随机',
            cfg_password: '房间密码（可选）',
            password_ph: '留空 = 无密码',
            cfg_seconds: '输入时限',
            sec_n: '{n} 秒',
            cfg_coord: '坐标系',
            coord_soldier: '以你为原点',
            coord_field: '地图坐标',
            coord_note_soldier: '以你为原点：函数从你写起，0 就在你身上（创建后不可更改）',
            coord_note_field: '按地图坐标写：曲线整体平移到你身上（y=f(x) 上下、x=f(y) 左右，创建后不可更改）',
            join_ph: '房间号',
            join_password_ph: '密码（如有）',
            join: '加入',
            spectate_join: '观战',
            waiting_start: '等待开始…',
            room_list_title: '房间列表',
            refresh: '刷新',
            no_rooms: '暂无房间',
            room_line: '{id} 号房 · 参战 {players} · 观战 {spec}',
            room_in_game: '进行中',
            room_has_password: '有密码',
            password_prompt: '房间有密码，请输入：',
            room_waiting: '等待中',
            // 主题切换（顶栏按钮循环三套预设；与 C++ 客户端的主题同名同性格）
            theme_btn_title: '切换配色主题',
            theme_dark: '主题：深空',
            theme_bright: '主题：明亮',
            theme_terminal: '主题：终端绿',

            // 房间
            room_title: '房间 #{id}',
            team_a: 'A 队',
            team_b: 'B 队',
            team_spectators: '观战',
            badge_host: '房主',
            badge_you: '你',
            ai_tag: '人机·{d}',
            kick: '踢出',
            to_team_a: '换到 A 队',
            to_team_b: '换到 B 队',
            add_ai: '添加人机',
            team_pick: '队伍',
            team_auto: '自动',
            start_game: '开始对局',
            leave_room: '离开房间',
            wait_host: '等待房主开始对局…',
            host_hint: '你是房主：添加人机或等对手就位后开始',
            room_rules_title: '房间规则（房主可改，下一把生效）',
            soldiers_locked: '每人士兵数 {n}（创建时定，不可更改）',
            coord_locked: '坐标系：{v}（创建时定，不可更改）',
            first_team_label: '先手队伍',
            delay_label: '展示停顿',
            seed_each: '每把随机',
            seed_fixed_ph: '固定种子',
            apply: '应用',
            room_cfg: '规则：血量 {hp} ｜ 士兵 {sp} ｜ 障碍 {obs} ｜ 时限 {sec} 秒 ｜ 坐标系 {coord} ｜ 种子 {seed}',
            seed_random: '随机',
            diff_0: '简单',
            diff_1: '普通',
            diff_2: '困难',
            diff_3: '大神',

            // 对战 HUD
            banner_your_turn: '你的回合！士兵 #{s}',
            banner_turn: '轮到 {name} 的士兵 #{s}',
            banner_thinking: '{name} 思考中…',
            round_n: '第 {n} 回合',
            remaining: '剩余 {n} 秒',
            waiting_move: '等待中…',
            vote_skip: '跳过地图',
            vote_voted: '已投跳过',
            voting: '{n}/{total}',
            typing_fmt: '{name} 正在写：',
            spectating: '观战中（只看不打）',
            leave_ingame_title: '对局进行中不能离开，打完这把再走',
            view_normal: '视角：常规',
            view_mirror: '视角：镜像',
            to_spectate: '转为观战',
            to_play: '转为参战',

            // 模式与输入
            mode_y_of_x: 'y = f(x)',
            mode_x_of_y: 'x = f(y)',
            mode_polar: 'r = f(t)',
            mode_parametric: '参数方程',
            mode_ode1: 'dy/dx = f(x,y)',
            mode_ode2: "(x',y') = (f,g)",
            mode_ypp: "y'' = f(x,y,dy)",
            label_a_y: 'y =',
            label_a_x: 'x =',
            label_a_r: 'r =',
            label_a_xt: 'x(t) =',
            label_a_dydx: 'dy/dx =',
            label_a_xp: "x'(t,x,y) =",
            label_a_ypp: "y'' =",
            label_b_yt: 'y(t) =',
            label_b_yp: "y'(t,x,y) =",
            dir_right: '向右',
            dir_left: '向左',
            dir_up: '向上',
            dir_down: '向下',
            dir_posx: '沿 +x',
            dir_negx: '沿 -x',
            range_label: '参数',
            range_ph_lo: '起',
            range_ph_hi: '止',
            angle_label: '发射角(度)',
            submit: '提交函数',
            preview: '预览',
            submitted: '已提交，等待飞行…',
            attempts_fmt: '提交 {used}/{max}',
            hint_y_of_x: '变量 x ｜ 支持 + - * / ^ sin cos tan exp log sqrt pow pi e 等',
            hint_x_of_y: '变量 y ｜ 支持 + - * / ^ sin cos tan exp log sqrt pow pi e 等',
            hint_polar: '变量 t（角度）｜ 曲线从你出发',
            hint_parametric: '变量 t ｜ 曲线从你出发（常数项无影响）',
            hint_ode1: '变量 x, y ｜ 从你的位置开始积分',
            hint_ode2: '变量 t, x, y ｜ 从你的位置开始积分',
            hint_ypp: '变量 x, y, dy ｜ 从你的位置开始积分，初速由发射角决定',

            // 事件流
            ev_turn: '轮到 {name} 的士兵 #{s}',
            ev_shot: '{name} 发射炮弹',
            ev_hit: '命中 {name} 的士兵 #{s}（剩 {hp} 血）',
            ev_death: '{name} 的士兵 #{s} 阵亡',
            ev_crater: '遮挡物被炸出坑',
            ev_skip: '{name} 超时跳过',
            ev_new_game: '—— 新对局 ——',
            ev_join: '{name} 进入房间',
            ev_leave: '{name} 离开房间',
            ev_you_kicked: '你被移出了房间',
            ev_your_timeout: '超时未提交，等待服务器处理',

            // 结算
            over_a: 'A 队胜利',
            over_b: 'B 队胜利',
            over_draw: '平局',
            over_win: '你赢了！',
            over_lose: '你输了',
            again: '再来一局',
            waiting_host: '等待房主',
            to_room: '返回房间',

            // 杂项
            submit_fail: '提交失败：{msg}',
            error_fmt: '{msg}',
            bad_seed: '种子需要是非负整数',
            bad_room_id: '房间号需要是非负整数',
            events_title: '事件',

            // 函数帮助（清单本身在 app.js 的 HELP_GROUPS / HELP_RULES，与 C++ 客户端一致）
            help_btn: '函数帮助',
            help_sub: '表达式里可用的全部内容（与 sin、cos 等随意嵌套组合）',
            help_close: '关闭',
        },
    };

    var LANG = 'zh';

    function t(key, params) {
        var s = (DICTS[LANG] && DICTS[LANG][key] !== undefined) ? DICTS[LANG][key] : key;
        if (params) {
            for (var k in params) {
                s = s.split('{' + k + '}').join(String(params[k]));
            }
        }
        return s;
    }

    var FGStrings = {
        t: t,
        setLang: function (lang) { if (DICTS[lang]) LANG = lang; },
    };
    if (typeof module !== 'undefined' && module.exports) module.exports = FGStrings;
    global.FGStrings = FGStrings;
})(typeof window !== 'undefined' ? window : globalThis);
