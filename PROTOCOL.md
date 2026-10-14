# 通信协议（function_game）

客户端与服务器之间的通信协议。

**代码中的唯一事实来源是 `src/core/protocol.h`**：所有消息类型名集中在 `fg::msg_type`，
所有下行消息的字段名只出现在 `fg::msg` 的构造函数里，客户端与服务器都通过同一套
常量与构造器拼装/解析，本文件是与之对应的说明文档。改协议时请同步修改这两处。

## 传输层（两个端口）

服务器同时监听两个端口，**消息层完全共用**（同一份 JSON 协议、同一套房间与对局逻辑）：

| 端口 | 默认 | 传输 | 用途 |
|---|---|---|---|
| 游戏端口 | 9527 | 原生 TCP（`--port`） | C++ 客户端 |
| 网页端口 | 8000 | WebSocket 文本帧 + HTTP 静态页（`--web-port`） | 网页版 |

- 网页端口上一个 **WebSocket 文本帧 = 一条 JSON 消息**（不含长度前缀），双向相同；
  页面从哪来就连哪（`net.js` 用同源地址），没有独立的网关进程
- 同一端口还直接服务 `web/static/` 下的静态文件（GET/HEAD，带路径穿越防护）；
  静态目录用 `--static-dir` 指定
- 两个端口的单条消息上限同为 8 MiB，超限断开

## 帧格式（游戏端口）

TCP 之上的帧格式：

```
4 字节小端长度前缀（JSON 文本的字节数） + UTF-8 JSON 文本
```

- 单帧上限 8 MiB，超过视为恶意/损坏连接，服务器直接断开
- 每条消息都是一个 JSON 对象，必有 `"type"` 字段
- 编解码函数：`fg::encode_frame` / `fg::decode_frame`

## 连接与房间流程

1. 连接建立 → 服务器发 `hello`
2. 在大厅：`list_rooms` / `create_room` / `join_room`
3. 在房间（等待或局间）：房主用 `add_ai`、`set_team`、`kick` 管理人员，用 `set_match_config`
   调整下一把的规则，`start` 开局
4. 对局中：轮到谁输入，谁发 `submit`；其余消息全部由服务器广播
5. 对局结束（`game_over`）后房间保留，可调整人员再来一局
6. 观战：`join_room` 带 `spectate: true` 直接以观战者身份加入（进行中的对局也能进），
   或在房间里 `set_spectate` 切换身份。观战者不进对局阵容、不能提交，但收到与
   在场者相同的广播；中途进入时会先收到一份对局快照（`game_start` + 当前回合的
   `turn_begin` 或 `shot` + `projectiles`），客户端按正常开局路径渲染。
   纯 AI 对局靠它实现：房主自己转为观战，两边都加人机再开局

## 客户端 → 服务器

| type | 字段 | 说明 |
|---|---|---|
| `create_room` | `name?` 玩家名（≤64 字节，超出按 UTF-8 边界截断）；`room?` 房间的属性（见下）；`match?` 下一把配置的初值（见下）；`password_hash?` 房间密码；`trail?` bool | 创建房间并加入；`password_hash` 是密码的 SHA-256 哈希（64 位小写十六进制，明文由客户端本地哈希、传输中不出现；不填 = 无密码）；`trail: true` 声明本连接要在 `shot` 里收到轨迹折线（见"炮弹轨迹"节，默认 false） |
| `join_room` | `room_id` 房间号；`name?`（同上限）；`spectate?`；`password_hash?`；`trail?` bool | 用房间号加入房间；`spectate: true` 以观战者身份（对局进行中也能进）；有密码的房间必须带匹配的 `password_hash`（观战同样要过），不匹配回 `bad_room_password`；`trail` 含义同 `create_room` |
| `list_rooms` | 无 | 索取房间列表 |
| `leave_room` | 无 | 离开房间（对局进行中不允许） |
| `set_spectate` | `spectate` | 切换自己的身份：参战 / 观战（仅等待/局间） |
| `add_ai` | `difficulty` 0/1/2/3；`team?` `"A"`/`"B"`/`"auto"` | 房主添加人机（0=简单 1=普通 2=困难 3=大神；大神档多杀不限流） |
| `set_team` | `member_id`；`team` `"A"`/`"B"` | 房主调整成员队伍 |
| `kick` | `member_id` | 房主把成员移出房间 |
| `start` | 无 | 房主开始对局 |
| `submit` | `request` 曲线请求对象 | 当前回合玩家提交曲线 |
| `set_match_config` | `match` 配置对象 | 房主修改"下一把"的规则（仅等待/局间；合并补丁，`seed` 传 null = 每把随机）。补丁里出现任何房间属性或世界常量字段（`soldiers_per_player`、`coord_origin`、`map_*` 等）一律被 `cannot_change_room` 拒绝——这些字段没有修改入口；白名单之外的键回 `unknown_match_field` |
| `vote_skip` | `vote?`（缺省 true） | 投票跳过当前地图（false = 撤回）。参战人类一人一票，全员同意后立即换一张**随机**新图——即使房间种子是固定值（跳过地图的含义就是换图；只影响这一把，下一把开局仍按固定种子） |
| `function_preview` | `preview` 字符串（≤400 字节） | 实时函数预览：只转发"当前回合玩家"发的，约 100ms 限频/人 |
| `preview_curve` | `request` 曲线请求对象 | 请求服务器采样曲线用于本地预览（仅当前回合玩家；每连接约 1 秒 1 次，超出回 `preview_too_frequent`）。只返回起点起约 12 单位（`kPreviewTravel`）的一段——预览用于确认起步形态，整条弹道提前全画出来等于剧透能否命中。不推进对局、不计入 submit 的尝试上限 |

### room（房间的属性：创建房间时唯一一次设置机会，之后不可更改）

`create_room` 的 `room` 对象只接受下面两个字段，出现其它字段（如 `map_x_max`）回 `bad_room_field`：

| 字段 | 类型 | 范围 | 含义 |
|---|---|---|---|
| `soldiers_per_player` | int | 1~4 | 每人士兵数（全场统一，不填默认 3） |
| `coord_origin` | string | `"soldier"`（默认）/ `"field"` | 曲线参考系（只影响 y=f(x)、x=f(y)）：`soldier` = 以出手士兵为原点（自变量从 0 起）；`field` = 自变量是地图坐标（照刻度数字写），曲线整体平移使起点落在士兵位置（y=f(x) 竖直平移 y0-f(x0)，x=f(y) 水平平移 x0-f(y0)）；镜像房间里取屏幕读到的负值 |

### world（世界常量：只下行，客户端渲染与本地预览采样用）

地图范围与物理参数由代码固定（`src/core/config.h` 的 `kDefaultWorld`），协议不提供
任何上行入口。`room_state` 与 `game_start` 都带完整的 `world` 对象：

| 字段 | 含义 |
|---|---|
| `map_x_min` / `map_x_max` / `map_y_min` / `map_y_max` | 地图范围（默认 48×30） |
| `player_half_size` | 士兵碰撞半边长 |
| `projectile_speed` | 炮弹速度 |
| `max_travel` | 轨迹最大采样长度（全局默认；参数方程模式不用它——那个模式的行程预算按地图算：斜对角线 × 2） |
| `crater_radius` | 炮弹炸出的坑半径 |

### match（下一把配置：`create_room` 的初值 / `set_match_config` 的补丁，全部可选，带范围校验）

| 字段 | 类型 | 范围 | 含义 |
|---|---|---|---|
| `seed` | uint64 或 null | | 地图种子（同种子生成同地图）；`null` = 每把随机 |
| `hp` | int | 1~999 | 每个士兵的血量（1 = 一击毙命） |
| `obstacle_count` | int | 0~300 且 ≤ 地图面积/32 | 遮挡物数量（与地图大小交叉校验，过密会没有合法出生点；默认地图上限 45） |
| `input_seconds` | double | 3~600 | 每回合输入时限（秒） |
| `turn_delay` | double | 0~10 | 命中后的展示停顿（炮弹消失后等这么久再轮到下一位，默认 3） |
| `mirror_view` | bool | | 镜像视角房间：B 队人类玩家的屏幕左右翻转、坐标显示为 -x（见 request 的 `mirror` 字段） |
| `first_team` | int | -1/0/1 | 先手队伍：-1 = 由地图种子随机决定，0 = A 队，1 = B 队 |

服务器回显的下一把配置与入参同键名、包含全部字段：`room_state` 的 `next_match` 是模板，
`seed` 为 null 表示每把随机；`game_start` 的 `match` 是本局实际配置——`seed` 在这里恒为
实值（"每把随机"在开局那一刻才真正定下种子，本局重放/复现都用它）。

### request（提交的曲线）

| 字段 | 类型 | 含义 |
|---|---|---|
| `mode` | string | `y_of_x` / `x_of_y` / `polar` / `parametric` / `ode1` / `ode2` / `ypp` |
| `expr_a` | string | 主表达式（如 `0.5*x^2 - 1.2*x`、`theta/2`、`x+y`；ypp 时是 y'' 的右端） |
| `expr_b` | string | 参数方程的 y(t)，或 ode2 的 y'(t,x,y) |
| `dir` | string | `pos` = 正方向 / `neg` = 负方向（只允许单向） |
| `p_start` / `p_end` | double? | 极坐标/参数方程的参数范围（省略用默认：极坐标 0→2π，参数方程 0→1000——实际长度按"斜对角线 × 2"的行程预算截断） |
| `mirror` | bool?（仅 true 时出现） | 镜像提交：B 队人类在 mirror_view 房间里提交时置 true，服务器在镜像空间里采样（`-x`），"所见即所写" |
| `angle` | double? | 仅 ypp：初斜率角（弧度，`y'(0) = tan(angle)`，相对行进方向的仰角；\|angle\| < 1.5 即 ±85° 内），省略 = 0（沿轴出发） |

## 服务器 → 客户端

| type | 字段 | 说明 |
|---|---|---|
| `hello` | `conn` | 连接建立，`conn` 为服务器分配的连接 id |
| `error` | `code`, `message`, `context`(可选) | 请求失败：`code` 为稳定的机器可读标识，`message` 为中文说明（见"错误码"节）；`context` 标出来源域（当前有 `submit`，见下） |
| `room_list` | `rooms: [{id, players, spectators, in_game, has_password}]` | 房间列表（参战/观战分开报数；`has_password` 只报"有没有密码"，哈希本身不下发） |
| `joined` | `room_id`, `member_id`, `host` | 加入成功；`member_id` 即世界内玩家 id |
| `left_room` / `kicked` | 无 | 已离开 / 被移出房间 |
| `room_state` | `room`, `world`, `next_match`, `host`, `in_game`, `has_password`, `members: [{id, name, is_ai, difficulty, team, role}]` | 房间与人员状态（人员变化时自动推送；`role` 为 `"player"` / `"spectator"`；`next_match.seed` 为 null 表示每把随机；`has_password` 只报"有没有密码"，哈希本身绝不下发） |
| `game_start` | `room`, `world`, `match`, `players: [...]`, `obstacles: [...]`, `round`(仅快照) | 开局地图快照（客户端据此渲染；`match` 是本局实际配置，`seed` 恒为实值）；中途观战快照会额外带当前回合号 |
| `turn_begin` | `player_id`, `soldier`, `input_seconds`, `round`(可选), `next_player?`, `next_soldier?` | 轮到某玩家的某个士兵输入；`next_*` = 下一位出手者（提前高亮，`<0` 表示不带） |
| `turn_skipped` | `player_id` | 该玩家超时被跳过 |
| `shot` | `id`, `owner`, `soldier`, `request`, `speed`, `trail?` | 炮弹发射（`soldier` = 出手士兵序号）；`trail` 为服务器采样好的轨迹折线 `[[x,y],…]`，仅对声明过 `trail: true` 的连接携带 |
| `projectiles` | `list: [{id, pos, travel, length}]` | 飞行中的炮弹位置（约 20Hz） |
| `hit` | `target`, `soldier`, `by`, `hp_left`, `pos` | 命中某个士兵扣血（穿透后继续飞） |
| `death` | `player_id`, `soldier`, `by` | 某个士兵阵亡 |
| `crater` | `obstacle`, `pos` | 遮挡物被炮弹炸出坑（坑内不再阻挡） |
| `skip_vote` | `voted: [member_id]`, `total` | 跳过当前地图的投票状态（`total` = 参战人类数；任何一票变化都广播） |
| `function_preview` | `member_id`, `preview` | 当前回合玩家的实时函数预览（打字时 ≈200ms 节流上行转播） |
| `curve_preview` | `trail` | 预览采样结果：轨迹折线（编码同 `shot.trail`），只回给请求者 |
| `game_over` | `winner`：0=A 队胜 1=B 队胜 2=平局 | 对局结束 |

## 错误码

`error` 消息形如：

```json
{"type": "error", "code": "not_your_turn", "message": "还没轮到你"}
```

- `code`：稳定的机器可读标识（小写下划线）。客户端按码做程序化分支（重试、回退界面等），
  未来的多语言客户端按码查文案表。**码一经发布含义不再改变**（改文案不改码）
- `message`：中文说明，供日志、调试与当前界面的默认展示
- `context`（可选）：错误的来源域。当前有 `"submit"`（提交被拒，客户端据此复位本回合的
  "已提交"状态）、`"leave"`（离开房间被拒，客户端据此恢复"退出观战"按钮）与 `"preview"`
  （预览请求被拒：打字中途语法不完整是常态，客户端静默清掉预览即可，不必弹窗）；不带该
  字段的错误客户端不做对应状态处理，这样将来新增的对局内错误不会干扰已接受、炮弹在飞的提交

全部错误码（按域分组；代码中的产生点见 `src/core/error.h` 的说明）：

| 域 | code | 触发场景 |
|---|---|---|
| 协议 | `must_join_room` | 尚未加入房间就发房间内指令 |
| | `unknown_message_type` | 未知的消息类型（message 附具体类型名） |
| | `preview_too_frequent` | 曲线预览请求过于频繁（每连接约 1 秒 1 次） |
| | `missing_request` | 提交缺少 request 字段 |
| | `bad_request_format` | 提交的 request 不是 JSON 对象 |
| | `unknown_curve_mode` | 未知的曲线模式 |
| | `unknown_fire_dir` | 未知的发射方向 |
| | `server_error` | 服务器内部异常的兜底 |
| 房间 | `room_not_found` | 房间号不存在 |
| | `bad_room_password` | 房间密码错误（有密码房间的 `password_hash` 缺失或不匹配） |
| | `bad_password_hash` | 密码哈希格式不对（需 64 位小写十六进制） |
| | `room_closed` | 房间已销毁 |
| | `join_failed` | 加入失败（满员或对局中） |
| | `room_full` | 参赛席位已满（添加人机、或观战者转为参战时） |
| | `not_host_start` / `not_host_add_ai` / `not_host_kick` / `not_host_team_change` / `not_host_config` | 非房主执行对应操作 |
| | `already_started` | 对局已在进行（start） |
| | `in_game_add_ai` / `in_game_kick` / `in_game_team_change` / `in_game_config` / `in_game_leave` | 对局进行中的对应操作被拒 |
| | `cannot_kick_self` | 不能移除自己 |
| | `member_not_found` | 成员不存在 |
| | `in_game_spectate` | 对局进行中不能切换参战/观战身份 |
| | `spectator_full` | 观战席位已满（转为观战时） |
| | `member_is_spectator` | 观战者没有队伍（换队操作被拒） |
| | `spectator_cannot_submit` | 观战者不能提交曲线 |
| | `need_two_players` | 开局至少需要两名参与者 |
| | `need_both_teams` | 两队都需要至少一名参与者 |
| | `need_equal_teams` | 两队人数需要相等（多打少不公平；换队调整后可再开局） |
| | `cannot_change_room` | set_match_config 补丁里出现房间属性或世界常量字段（士兵数/坐标系/地图范围/物理参数等，创建时定死，没有任何修改入口） |
| 配置 | `config_not_object` | config 不是 JSON 对象 |
| | `bad_room_field` | create_room 的 room 对象出现不支持的上行字段（只能带 soldiers_per_player / coord_origin） |
| | `unknown_match_field` | 下一把配置出现白名单之外的键（键名拼错不会被静默忽略） |
| | `bad_hp` / `bad_hp_range` | hp 类型 / 范围非法（1~999） |
| | `bad_soldiers_per_player` / `bad_soldiers_per_player_range` | soldiers_per_player 类型 / 范围非法（1~4） |
| | `bad_coord_origin` | coord_origin 需要是 "soldier" 或 "field" |
| | `bad_turn_delay` / `bad_turn_delay_range` | turn_delay 类型 / 范围非法（0~10 秒） |
| | `bad_mirror_view` | mirror_view 需要是布尔值 |
| | `bad_obstacles` / `bad_obstacles_range` | obstacle_count 类型 / 范围非法（0~300） |
| | `obstacles_too_dense` | 遮挡物对当前地图过密（上限约为地图面积/32） |
| | `bad_input_seconds` / `bad_input_seconds_range` | input_seconds 类型 / 范围非法（3~600） |
| | `bad_first_team` / `bad_first_team_range` | first_team 类型 / 范围非法（-1/0/1） |
| | `bad_seed` | seed 需要是非负整数或 null |
| 表达式 | `expr_empty` | 表达式为空 |
| | `expr_too_long` | 表达式过长（超过 500 字符） |
| | `expr_bad_char` | 表达式包含不允许的字符 |
| | `expr_forbidden_function` | 使用了白名单之外的函数 |
| | `expr_unknown_name` | 使用了本模式不允许的变量名 |
| | `expr_compile_failed` | 表达式语法错误（message 附 ExprTk 原文） |
| 曲线 | `curve_bad_f0` | f(0) 不是有限数值，无法确定起始点 |
| | `curve_bad_anchor_r` / `curve_bad_anchor_xy` | 起点处 r / 坐标不是有限数值 |
| | `curve_no_trail` | 曲线没有形成有效轨迹 |
| | `curve_param_empty` | 参数范围为空 |
| | `curve_param_too_wide` | 参数范围过大（最多 1000，仅极坐标；参数方程对 t 无上限） |
| | `curve_param_start_too_large` | 参数起点过大（仅极坐标） |
| | `curve_bad_angle` | ypp 的发射角过于竖直（需在 ±85 度即 1.5 弧度内） |
| 回合 | `not_input_phase` | 当前不是输入阶段 |
| | `not_your_turn` | 还没轮到该玩家 |
| | `player_invalid` | 玩家不存在或已阵亡 |
| | `game_not_started` | 游戏尚未开始 |
| | `submit_too_many` | 本输入阶段的提交尝试次数过多（上限 16，含被拒的） |

## 复合对象字段

`players` 数组元素（`game_start` / 后续同步）。每个玩家的士兵在 `soldiers` 数组里，
元素序号即协议中各消息的 `soldier` 字段（`turn_begin` 的出手士兵、`hit`/`death`
的受击士兵）：

```json
{"id": 0, "seat": 1, "team": "A", "name": "张三",
 "soldiers": [{"pos": [12.5, -3.2], "hp": 1, "max_hp": 1, "alive": true},
              {"pos": [14.1, 2.8], "hp": 1, "max_hp": 1, "alive": true}],
 "is_ai": false, "ai_difficulty": 1}
```

旧格式（顶层 `pos`/`hp`/`max_hp`/`alive`、无 `soldiers`）仍可解析：按单士兵补齐。

`obstacles` 数组元素：

```json
{"is_circle": true, "center": [30.1, -8.4], "radius": 5.0,
 "half_w": 0.0, "half_h": 0.0, "craters": [[29.8, -8.0]]}
```

`pos`、`center`、`craters` 均为 `[x, y]` 二元数组。

## 炮弹轨迹：本地重采样 vs 服务器下发

`shot` 携带 `request`（曲线描述）作为弹道的唯一事实来源，是否附带采样好的
轨迹折线由连接自己选：

- **默认（C++ 客户端）**：不下发轨迹。客户端与服务器共用同一份 core 代码，
  各自用 `sample_curve` 在本地重建**完全相同**的轨迹（规则确定性），省带宽。
- **声明 `trail: true` 的连接（网页端）**：`shot` 附带 `trail` 字段——服务器
  已采样好的轨迹折线 `[[x,y],…]`（抽稀到 ≤4000 点、坐标保留 4 位小数）。
  不共享 C++ core 的客户端无法本地重采样，直接画这条折线，显示与判定天然一致。

偏好按连接在 `create_room` / `join_room` 里声明。两种模式下实际飞行位置都由
服务器以约 20Hz 的 `projectiles` 消息校准——`trail` 是整条理论轨迹，实际飞到
哪里（撞上遮挡物就停在坑的位置）以 `travel` 为准。

同一套折线编码也用于 `curve_preview`：网页端连输入时的曲线预览也没法本地算，
改由 `preview_curve` 请服务器代采样（C++ 客户端共享 core、本地自己算，不用这条）。

## 示例

创建房间并开局（客户端视角）：

```json
→ {"type": "create_room", "name": "张三",
   "room": {"soldiers_per_player": 2, "coord_origin": "field"},
   "match": {"hp": 3, "obstacle_count": 12, "seed": 42}}
← {"type": "joined", "room_id": 1, "member_id": 0, "host": 0}
← {"type": "room_state", "room": {...}, "world": {...}, "next_match": {...}, "host": 0,
   "in_game": false, "members": [{"id": 0, "name": "张三", "is_ai": false, "difficulty": 1, "team": "A"}]}
→ {"type": "set_match_config", "match": {"seed": null}}
→ {"type": "add_ai", "difficulty": 2, "team": "B"}
→ {"type": "start"}
← {"type": "game_start", "room": {...}, "world": {...}, "match": {...}, "players": [...], "obstacles": [...]}
← {"type": "turn_begin", "player_id": 0, "soldier": 0, "input_seconds": 30,
   "next_player": 1, "next_soldier": 0}
```

提交曲线：

```json
→ {"type": "submit", "request": {"mode": "y_of_x", "expr_a": "0.1*x^2 - 0.5*x", "dir": "pos"}}
← {"type": "shot", "id": 1, "owner": 0, "soldier": 0, "request": {...}, "speed": 25.0}
← {"type": "projectiles", "list": [{"id": 1, "pos": [12.3, 4.1], "travel": 9.7, "length": 128.4}]}
← {"type": "hit", "target": 3, "soldier": 1, "by": 0, "hp_left": 0, "pos": [40.2, 12.0]}
← {"type": "death", "player_id": 3, "soldier": 1, "by": 0}
← {"type": "crater", "obstacle": 2, "pos": [55.0, 18.3]}
← {"type": "game_over", "winner": 0}
```

请求被拒绝（错误码 + 中文说明）：

```json
→ {"type": "submit", "request": {"mode": "y_of_x", "expr_a": "avg(x,1)"}}
← {"type": "error", "code": "expr_forbidden_function", "message": "不允许的函数：avg"}
```
