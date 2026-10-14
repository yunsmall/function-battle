"""函数对战 · 端到端自测客户端（零依赖，仅标准库）。

不打开浏览器也能把整条链路走一遍：连网关（或直连游戏服务器）→ 创建房间 →
加人机 → 开局 → 按广播自动提交曲线 → 收集事件 → 打印 PASS/FAIL 与消息统计。
用来在改网关/前端时快速验证协议链路没被改坏。

用法：
    python web/probe.py                        # 经网关（默认 127.0.0.1:8000）
    python web/probe.py --direct               # 直连游戏服务器（默认 127.0.0.1:9527）
    python web/probe.py --difficulty 3 --seed 42
"""
import argparse
import asyncio
import sys
import time
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from wsproto import connect_ws, encode_frame, encode_game_frame  # noqa: E402


class Link:
    """统一的收发层：WS 模式走帧编解码，TCP 模式走长度前缀帧。"""

    def __init__(self, reader, writer, frames=None):
        self._reader = reader
        self._writer = writer
        self._frames = frames  # 非 None 表示 WS 模式

    async def send(self, obj):
        if self._frames is not None:
            self._writer.write(encode_frame(encode_game_frame(obj)[4:], 0x1, masked=True))
        else:
            self._writer.write(encode_game_frame(obj))
        await self._writer.drain()

    async def recv(self, timeout):
        if self._frames is not None:
            op, data = await asyncio.wait_for(self._frames.read_message(), timeout)
            if op == 0x8:
                raise ConnectionError("服务器关闭了连接")
            if op not in (0x1, 0x2):
                return {"type": "__frame", "op": op}
            return _decode(data)
        hdr = await asyncio.wait_for(self._reader.readexactly(4), timeout)
        n = int.from_bytes(hdr, "little")
        body = await asyncio.wait_for(self._reader.readexactly(n), timeout)
        return _decode(body)

    def close(self):
        try:
            self._writer.close()
        except Exception:
            pass


def _decode(data: bytes):
    import json

    return json.loads(data.decode("utf-8"))


async def connect(args) -> Link:
    if args.direct:
        reader, writer = await asyncio.open_connection(args.game_host, args.game_port)
        print(f"[连接] 直连 TCP {args.game_host}:{args.game_port}")
        return Link(reader, writer)
    frames, writer = await connect_ws(args.gateway_host, args.gateway_port)
    print(f"[连接] 经网关 WS {args.gateway_host}:{args.gateway_port}")
    return Link(None, writer, frames=frames)


class Probe:
    def __init__(self, args):
        self.args = args
        self.my_id = None
        self.stats = Counter()
        self.travel_last = {}
        self.travel_ok = True
        self.my_shot = False
        self.ai_shot = False
        self.my_proj_seen = False
        self.game_start_ok = False
        self.ai_in_room = False
        self.got_over = False
        self.winner = None
        self.err_expr_empty = False
        self.err_not_your_turn = False
        self.sent_bad_submit = False
        self.sent_bad_turn = False
        self.sent_good_submit = False
        self.sent_create = False
        self.sent_add_ai = False
        self.sent_start = False
        self.done = False
        self.ai_modes = []
        self.trail_ok = True
        self.soldier_pos = {}  # (玩家id, 士兵序号) → 出生位置，用于核对 trail 起点
        self.sent_obstacles = 8  # create_room 实际发送的障碍数，game_start 回显要与它一致
        self.preview_done = False
        self.preview_ok = True        # curve_preview 返回且 trail 起点=我
        self.preview_err_ok = False   # 坏表达式回 context=preview 的错误
        self.preview_rate_err_ok = False  # 限频重发回 preview_too_frequent
        self.preview_times = []       # 收到 curve_preview 的时刻（验证 1 秒限频）

    # ---- 各消息的处理 ----

    async def on_message(self, link, msg):
        t = msg.get("type", "?")
        self.stats[t] += 1
        handler = getattr(self, "h_" + t, None)
        if handler:
            await handler(link, msg)
        elif t not in ("projectiles",):
            print(f"[消息] {t}: {msg}")

    async def h_hello(self, link, msg):
        print(f"[hello] conn={msg.get('conn')}")
        if not self.sent_create:
            self.sent_create = True
            # 房间属性与下一把配置分两个对象（协议现行版）：
            # room = 创建时定死的（士兵数/坐标系）；match = 下一把模板
            room = {
                "soldiers_per_player": 1,
                "coord_origin": "field",  # 顺带验证"地图坐标"参考系的整条链路
            }
            match = {
                "seed": self.args.seed,
                "hp": 1,
                "obstacle_count": 8,
                "input_seconds": 30,
                "turn_delay": 0.5,
            }
            self.sent_obstacles = match["obstacle_count"]
            await link.send({"type": "create_room", "name": "自测探针", "room": room, "match": match,
                             "trail": self.args.trail})
            print(f"[发送] create_room room={room} match={match} trail={self.args.trail}")

    async def h_joined(self, link, msg):
        self.my_id = msg["member_id"]
        print(f"[joined] 房间 {msg['room_id']}，我的 id={self.my_id}，房主={msg['host']}")

    async def h_room_state(self, link, msg):
        members = msg.get("members", [])
        desc = ", ".join(f"{m['name']}({'AI' if m['is_ai'] else '人'}/{m['team']})" for m in members)
        print(f"[room_state] in_game={msg['in_game']} 成员: {desc}")
        if any(m["is_ai"] for m in members):
            self.ai_in_room = True
            if not self.sent_start:
                self.sent_start = True
                await link.send({"type": "start"})
                print("[发送] start")
        elif not self.sent_add_ai:
            # room_state 会因人员变化被反复推送，这里只发一次 add_ai
            self.sent_add_ai = True
            await link.send({"type": "add_ai", "difficulty": self.args.difficulty, "team": "B"})
            print(f"[发送] add_ai difficulty={self.args.difficulty} team=B")

    async def h_game_start(self, link, msg):
        players = msg.get("players", [])
        obstacles = msg.get("obstacles", [])
        room = msg.get("room", {})
        match = msg.get("match", {})
        ok_players = len(players) >= 2 and all(
            isinstance(p.get("soldiers"), list) and p["soldiers"] for p in players
        )
        # 回显与入参同键名；比对实际发送的数量，别写死数字
        ok_obstacles = (len(obstacles) == self.sent_obstacles
                        and match.get("obstacle_count") == self.sent_obstacles)
        ok_coord = (room.get("coord_origin") == "field")
        self.game_start_ok = ok_players and ok_obstacles and ok_coord
        print(
            f"[game_start] 玩家 {len(players)} 人，障碍 {len(obstacles)} 个"
            f"（配置 {match.get('obstacle_count')}），seed={match.get('seed')}，"
            f"coord={room.get('coord_origin')}，"
            f"校验{'通过' if self.game_start_ok else '失败'}"
        )
        for p in players:
            s0 = p["soldiers"][0]["pos"]
            print(f"    id={p['id']} {p['name']} 队伍{p['team']} 出生({s0[0]:.1f},{s0[1]:.1f})")
            for i, sd in enumerate(p["soldiers"]):
                self.soldier_pos[(p["id"], i)] = sd["pos"]

    async def h_turn_begin(self, link, msg):
        who = "我" if msg["player_id"] == self.my_id else f"玩家{msg['player_id']}"
        if msg["player_id"] == self.my_id:
            if not self.preview_done:
                self.preview_done = True
                await self.run_preview_checks(link)
            if not self.sent_bad_submit:
                # 空表达式：应收到 expr_empty 且 context=submit（错误码契约的第一条检查）
                self.sent_bad_submit = True
                await link.send({"type": "submit", "request": {"mode": "y_of_x", "expr_a": "", "dir": "pos"}})
                print("[发送] submit（空表达式，试探错误码）")
            elif not self.sent_good_submit:
                self.sent_good_submit = True
                req = {"mode": "y_of_x", "expr_a": "0.1*x + 2", "dir": "pos"}
                await link.send({"type": "submit", "request": req})
                print(f"[发送] submit {req}")
        else:
            if not self.sent_bad_turn:
                # 别人的回合提交：应收到 not_your_turn
                self.sent_bad_turn = True
                await link.send({"type": "submit", "request": {"mode": "y_of_x", "expr_a": "x", "dir": "pos"}})
                print("[发送] submit（非我回合，试探 not_your_turn）")

    async def run_preview_checks(self, link):
        """曲线预览契约：有效请求得 curve_preview（trail 起点=我）；紧接着重发被
        1 秒限频回 preview_too_frequent；超过窗口后发坏表达式回 context=preview 的错误"""
        req = {"mode": "y_of_x", "expr_a": "0.2*x + 1", "dir": "pos"}
        await link.send({"type": "preview_curve", "request": req})
        await link.send({"type": "preview_curve", "request": req})  # 立刻重发：应被限频拒
        print("[发送] preview_curve ×2（第二条应回 preview_too_frequent）")
        await asyncio.sleep(1.1)  # 睡过限频窗口；此时到来的游戏消息在 TCP 里排队等收
        await link.send({"type": "preview_curve",
                         "request": {"mode": "y_of_x", "expr_a": "", "dir": "pos"}})
        print("[发送] preview_curve（空表达式，试探 context=preview 错误）")

    async def h_curve_preview(self, link, msg):
        self.preview_times.append(time.monotonic())
        ok, why = self.check_trail({"owner": self.my_id, "soldier": 0}, msg.get("trail"))
        if not ok:
            self.preview_ok = False
            print(f"[警告] curve_preview 的 trail 不合法：{why}")
        else:
            print(f"[curve_preview] trail {len(msg['trail'])} 点，起点与我的位置一致")

    async def h_error(self, link, msg):
        code = msg.get("code")
        print(f"[错误] code={code} context={msg.get('context')} msg={msg.get('message')}")
        if code == "expr_empty" and msg.get("context") == "submit":
            self.err_expr_empty = True
        if code == "expr_empty" and msg.get("context") == "preview":
            self.preview_err_ok = True
        if code == "preview_too_frequent" and msg.get("context") == "preview":
            self.preview_rate_err_ok = True
        if code == "not_your_turn":
            self.err_not_your_turn = True

    def check_trail(self, msg, trail):
        """trail 契约：≥2 个 [x,y] 数字点，首点=发射者位置（服务器坐标按 4 位小数取整）"""
        if not isinstance(trail, list) or len(trail) < 2:
            return False, "缺失或点数不足"
        for pt in trail:
            if not (isinstance(pt, list) and len(pt) == 2
                    and all(isinstance(v, (int, float)) for v in pt)):
                return False, "元素不是 [x,y] 数字对"
        pos = self.soldier_pos.get((msg["owner"], msg["soldier"]))
        if pos is not None and (abs(trail[0][0] - pos[0]) > 1e-3 or abs(trail[0][1] - pos[1]) > 1e-3):
            return False, f"起点 {trail[0]} 与发射者位置 {pos} 不符"
        return True, ""

    async def h_shot(self, link, msg):
        req = msg.get("request", {})
        who = "我" if msg["owner"] == self.my_id else f"玩家{msg['owner']}"
        print(f"[shot] #{msg['id']} {who} 士兵{msg['soldier']} speed={msg['speed']} "
              f"mode={req.get('mode')} expr_a={req.get('expr_a')!r} dir={req.get('dir')}")
        trail = msg.get("trail")
        if self.args.trail:
            ok, why = self.check_trail(msg, trail)
            if not ok:
                self.trail_ok = False
                print(f"[警告] trail 不合法：{why}")
            else:
                print(f"        trail {len(trail)} 点，起点与发射位置一致")
        elif "trail" in msg:
            self.trail_ok = False
            print("[警告] 未声明 trail 的连接却收到了 trail 字段")
        assert isinstance(msg.get("id"), int) and msg["speed"] > 0, "shot 字段不合法"
        if msg["owner"] == self.my_id:
            self.my_shot = True
            assert req.get("mode") == "y_of_x", "回显的 request.mode 不对"
        else:
            self.ai_shot = True
            self.ai_modes.append(req.get("mode"))
        if req.get("mode") not in (
            "y_of_x", "x_of_y", "polar", "parametric", "ode1", "ode2", "ypp"
        ):
            print(f"[警告] 未知曲线模式：{req.get('mode')}")

    async def h_projectiles(self, link, msg):
        for it in msg.get("list", []):
            pid, travel = it["id"], it["travel"]
            pos = it.get("pos", [None, None])
            if not (isinstance(pos[0], (int, float)) and isinstance(pos[1], (int, float))):
                self.travel_ok = False
            if pid in self.travel_last and travel < self.travel_last[pid] - 1e-9:
                self.travel_ok = False
                print(f"[警告] 炮弹 #{pid} travel 回退：{self.travel_last[pid]} → {travel}")
            self.travel_last[pid] = travel
            if self.my_shot and pid in getattr(self, "_my_ids", set()):
                self.my_proj_seen = True

    async def h_hit(self, link, msg):
        print(f"[hit] {msg['by']} 命中 {msg['target']} 士兵{msg['soldier']}，剩 {msg['hp_left']} 血")

    async def h_death(self, link, msg):
        print(f"[death] 玩家 {msg['player_id']} 士兵 {msg['soldier']} 阵亡（by {msg['by']}）")

    async def h_crater(self, link, msg):
        print(f"[crater] 障碍 {msg['obstacle']} 被炸出坑 @ {msg['pos']}")

    async def h_turn_skipped(self, link, msg):
        print(f"[turn_skipped] 玩家 {msg['player_id']} 超时")

    async def h_skip_vote(self, link, msg):
        print(f"[skip_vote] {msg['voted']} / {msg['total']}")

    async def h_game_over(self, link, msg):
        self.got_over = True
        self.winner = msg.get("winner")
        names = {0: "A 队胜", 1: "B 队胜", 2: "平局"}
        print(f"[game_over] {names.get(self.winner, self.winner)}")
        assert self.winner in (0, 1, 2), "winner 字段不合法"
        await link.send({"type": "leave_room"})
        print("[发送] leave_room")

    async def h_left_room(self, link, msg):
        print("[left_room] 已离开房间")
        self.done = True

    # ---- 主循环 ----

    async def run(self, link):
        deadline = time.monotonic() + self.args.timeout
        pending_my_ids = set()
        while time.monotonic() < deadline and not self.done:
            try:
                msg = await link.recv(deadline - time.monotonic())
            except asyncio.TimeoutError:
                print(f"[超时] {self.args.timeout} 秒内没有等到 game_over，按已收到的事件判定")
                break
            if msg.get("type") == "shot" and msg.get("owner") == self.my_id:
                pending_my_ids.add(msg["id"])
                self._my_ids = pending_my_ids  # h_projectiles 里按它判断我方炮弹进过列表
            await self.on_message(link, msg)

    def report(self):
        checks = [
            ("hello / 创建房间", self.my_id is not None),
            ("房间出现人机并开局", self.ai_in_room),
            ("game_start 快照合法（球员/障碍数）", self.game_start_ok),
            ("错误码 expr_empty（context=submit）", self.err_expr_empty),
            ("错误码 not_your_turn", self.err_not_your_turn),
            ("我方提交被受理并广播 shot", self.my_shot),
            ("我方炮弹出现在 projectiles", self.my_proj_seen),
            ("shot 轨迹字段符合声明（trail 起点=发射位置）" if self.args.trail
             else "未声明 trail 时 shot 不带 trail 字段", self.trail_ok),
            ("curve_preview 采样（trail 起点=我）", self.preview_ok and len(self.preview_times) >= 1),
            ("预览 1 秒限频（重发回 preview_too_frequent）", self.preview_rate_err_ok),
            ("坏表达式回 context=preview 错误", self.preview_err_ok),
            ("AI 发射过炮弹", self.ai_shot),
            ("projectiles 的 travel 单调、坐标有限", self.travel_ok),
        ]
        print("\n===== 自测结果 =====")
        ok = True
        for name, passed in checks:
            print(f"[{'PASS' if passed else 'FAIL'}] {name}")
            ok = ok and passed
        print(f"[{'PASS' if self.got_over else '信息'}] 对局打到最后："
              f"{'是（winner=' + str(self.winner) + '）' if self.got_over else f'{self.args.timeout} 秒内未结束，截断'}")
        print(f"AI 用过的曲线模式：{Counter(self.ai_modes)}")
        print("消息统计：" + ", ".join(f"{k}={v}" for k, v in sorted(self.stats.items())))
        return ok


async def run(args) -> bool:
    link = await connect(args)
    probe = Probe(args)
    try:
        await probe.run(link)
    finally:
        link.close()
    return probe.report()


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description="函数对战 · 端到端自测客户端")
    ap.add_argument("--gateway-host", default="127.0.0.1", help="网关地址（默认 127.0.0.1）")
    ap.add_argument("--gateway-port", type=int, default=8000, help="网关端口（默认 8000）")
    ap.add_argument("--game-host", default="127.0.0.1", help="游戏服务器地址（默认 127.0.0.1）")
    ap.add_argument("--game-port", type=int, default=9527, help="游戏服务器端口（默认 9527）")
    ap.add_argument("--direct", action="store_true", help="绕过网关直连游戏服务器（对照用）")
    ap.add_argument("--no-trail", dest="trail", action="store_false",
                    help="不声明 trail（模拟 C++ 客户端），验证 shot 里不带轨迹字段")
    ap.add_argument("--difficulty", type=int, default=1, choices=range(4), help="人机难度（默认 1）")
    ap.add_argument("--seed", type=int, default=4242, help="地图种子（默认 4242）")
    ap.add_argument("--timeout", type=float, default=120.0, help="总时限秒数（默认 120）")
    return ap.parse_args(argv)


def main():
    args = parse_args()
    ok = asyncio.run(run(args))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
