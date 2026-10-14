# -*- coding: utf-8 -*-
# Claude 对局客户端：连 fg_server 打一局（我 = A 队先手 3 士兵 vs 人机 AI 3 士兵）。
# 用法：轮到我出手时它把战场快照写进 session/state.txt；把一条 y=f(x) 表达式写入
# session/shot.txt 即自动提交。纯文件信箱，无需 stdin/界面。
# 运行目录（state/shot/log/done）固定在脚本旁的 session/ 里。
import argparse
import json
import os
import socket
import struct
import sys
import time

HOST, PORT = "127.0.0.1", 9527
DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "session")
STATE = os.path.join(DIR, "state.txt")
SHOT = os.path.join(DIR, "shot.txt")
LOG = os.path.join(DIR, "log.txt")
DONE = os.path.join(DIR, "done.txt")

MY_NAME = "Claude"
DIFF_NAMES = {0: "简单", 1: "普通", 2: "困难", 3: "大神"}


def parse_args():
    ap = argparse.ArgumentParser(description="连 fg_server 打一局：我（A 队）vs 人机 AI（B 队）")
    ap.add_argument("seed", nargs="?", default=None,
                    help="固定种子（整数，用于复盘同局）；不传或 - / random 表示每把随机")
    ap.add_argument("difficulty", nargs="?", type=int, default=2, choices=range(4),
                    help="AI 难度：0 简单 / 1 普通 / 2 困难 / 3 大神（默认 2）")
    args = ap.parse_args()
    if args.seed in (None, "", "-", "random", "r"):
        args.seed = None
    else:
        try:
            args.seed = int(args.seed)
        except ValueError:
            ap.error(f"种子必须是整数或 '-'（随机），收到：{args.seed!r}")
    return args


CFG = {"soldiers_per_player": 3, "obstacles": 15, "hp": 1, "first_team": 0,
       "input_seconds": 600.0, "turn_delay": 1.0}
# 命令行参数在 main() 里解析（模块级解析会让 import 本文件也去解析调用方的 argv）
AI_DIFF = 2

# ---- 收发（帧 = 4 字节小端长度 + JSON；带缓冲防止超时截断半包）----
recvbuf = b""

def send(s, obj):
    data = json.dumps(obj).encode()
    s.sendall(struct.pack("<I", len(data)) + data)

def recv_one(s, timeout):
    """收一帧；超时返回 None（已读进缓冲的字节不丢）"""
    global recvbuf
    deadline = time.time() + timeout
    while True:
        if len(recvbuf) >= 4:
            n = struct.unpack("<I", recvbuf[:4])[0]
            if len(recvbuf) >= 4 + n:
                body = recvbuf[4:4 + n]
                recvbuf = recvbuf[4 + n:]
                return json.loads(body.decode())
        left = deadline - time.time()
        if left <= 0:
            return None
        s.settimeout(left)
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            return None
        if not chunk:
            raise ConnectionError("服务器关闭了连接")
        recvbuf += chunk

# ---- 状态 ----
players = {}      # id -> {name, team, is_ai, soldiers:[{x,y,hp,max_hp,alive}]}
obstacles = []    # 每项：原始 obstacle dict + craters 列表
cfg = {}          # 服务器回显的 GameConfig
my_id = None
my_team = "A"
round_no = 0
last_turn = {}    # 最近一次 turn_begin（含 soldier）
events = []       # 滚动事件文本
my_history = []   # 我提交过的表达式
ai_shots = []     # AI 最近出手（名字, 表达式）
pending_reject = ""  # 最近一次提交被拒的原因（写进下一次 state）
winner = None

def log(line):
    stamp = time.strftime("%H:%M:%S")
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(f"[{stamp}] {line}\n")
    print(f"[{stamp}] {line}", flush=True)

def vx(v):
    """Vec2 在协议里是 [x, y] 数组；这里兼容两种形式"""
    return float(v[0] if isinstance(v, (list, tuple)) else v.get("x", 0.0))

def vy(v):
    return float(v[1] if isinstance(v, (list, tuple)) else v.get("y", 0.0))

def name_of(pid):
    return players.get(pid, {}).get("name", f"#{pid}")

def ai_name():
    for pid, p in players.items():
        if p.get("is_ai"):
            return p["name"]
    return "AI"

def push_event(text):
    events.append(text)
    del events[:-18]

# ---- 消息处理（主循环与等待回合共用）----
def handle(m):
    global round_no, pending_reject, winner
    t = m.get("type")
    if t == "game_start":
        players.clear()
        for p in m.get("players", []):
            players[p["id"]] = {
                "name": p["name"], "team": p["team"], "is_ai": p["is_ai"],
                "soldiers": [{"x": vx(s["pos"]), "y": vy(s["pos"]), "hp": s["hp"],
                              "max_hp": s["max_hp"], "alive": s["alive"]}
                             for s in p["soldiers"]]}
        obstacles.clear()
        for ob in m.get("obstacles", []):
            ob = dict(ob)
            # 统一成 {"x":..,"y":..} 形式，渲染层直接用
            ob["center"] = {"x": vx(ob["center"]), "y": vy(ob["center"])}
            ob["craters"] = [{"x": vx(c), "y": vy(c)} for c in ob.get("craters", [])]
            obstacles.append(ob)
        cfg.update(m.get("config", {}))
        log(f"开局：种子 {cfg.get('seed')}，地图 [{cfg.get('map_x_min')},{cfg.get('map_x_max')}]x"
            f"[{cfg.get('map_y_min')},{cfg.get('map_y_max')}]，障碍 {len(obstacles)}")
        for pid, p in players.items():
            tag = "我" if pid == my_id else "AI"
            coords = " ".join(f"({s['x']:.2f},{s['y']:.2f})" for s in p["soldiers"])
            log(f"  [{tag}] {p['name']}({p['team']}队) 士兵: {coords}")
    elif t == "turn_begin":
        round_no = m.get("round", round_no)
        last_turn.clear(); last_turn.update(m)
        who = "我" if m.get("player_id") == my_id else name_of(m.get("player_id"))
        log(f"第 {round_no} 回合轮到 {who}（兵#{m.get('soldier', 0) + 1}）")
    elif t == "shot":
        req = m.get("request", {})
        expr = req.get("expr_a", "")
        if m.get("owner") != my_id:
            ai_shots.append((name_of(m.get("owner")), expr))
            del ai_shots[:-3]
            push_event(f"{name_of(m.get('owner'))} 出手：{expr}")
        log(f"出手：{name_of(m.get('owner'))} 兵#{m.get('soldier', 0) + 1}  {expr}")
    elif t == "hit":
        push_event(f"{name_of(m.get('by'))} 命中 {name_of(m.get('target'))}"
                   f"兵#{m.get('soldier', 0) + 1}（剩 {m.get('hp_left')} 血）"
                   f"@({vx(m.get('pos', [])):.1f},{vy(m.get('pos', [])):.1f})")
        tgt = players.get(m.get("target"))
        if tgt and m.get("soldier", 0) < len(tgt["soldiers"]):
            tgt["soldiers"][m["soldier"]]["hp"] = m.get("hp_left", 0)
        log(f"命中：{name_of(m.get('by'))} → {name_of(m.get('target'))} 兵#{m.get('soldier', 0) + 1}"
            f"（剩 {m.get('hp_left')} 血）")
    elif t == "death":
        tgt = players.get(m.get("player_id"))
        if tgt and m.get("soldier", 0) < len(tgt["soldiers"]):
            tgt["soldiers"][m["soldier"]]["alive"] = False
            tgt["soldiers"][m["soldier"]]["hp"] = 0
        push_event(f"{name_of(m.get('player_id'))} 士兵#{m.get('soldier', 0) + 1} 阵亡")
        log(f"阵亡：{name_of(m.get('player_id'))} 兵#{m.get('soldier', 0) + 1}")
    elif t == "crater":
        idx = m.get("obstacle")
        pos = m.get("pos", [])
        if idx is not None and idx < len(obstacles):
            obstacles[idx]["craters"].append({"x": vx(pos), "y": vy(pos)})
        push_event(f"障碍#{idx} 被打出坑 @({vx(pos):.1f},{vy(pos):.1f})")
    elif t == "turn_skipped":
        who = "我" if m.get("player_id") == my_id else name_of(m.get("player_id"))
        push_event(f"{who} 超时被跳过")
        log(f"超时跳过：{who}")
    elif t == "game_over":
        winner = m.get("winner")
        push_event(f"对局结束 winner={winner}")
        log(f"对局结束：winner={winner}（0=我(A)胜，1=AI 胜，2=平局）")
    elif t == "projectiles":
        pass
    elif t == "error":
        code = m.get("code", "")
        msg = m.get("message", "")
        pending_reject = f"上次提交被拒：[{code}] {msg}"
        log(f"服务器错误：[{code}] {msg}")
    else:
        log(f"未处理消息：{t} {json.dumps(m, ensure_ascii=False)[:120]}")

# ---- 战场渲染 ----
X0, Y0, Y1 = -24.0, -15.0, 15.0
COLS, ROWS = 96, 30  # 每单位 2 列 x 方向、1 行 y 方向

def render_map():
    grid = [["."] * COLS for _ in range(ROWS)]
    crater_r = cfg.get("crater_radius", 1.2)

    for ob in obstacles:
        cx, cy = ob["center"]["x"], ob["center"]["y"]
        for r in range(ROWS):
            for c in range(COLS):
                if grid[r][c] != ".":
                    continue
                x = X0 + (c + 0.5) / 2.0
                y = Y1 - (r + 0.5)
                if ob["is_circle"]:
                    inside = (x - cx) ** 2 + (y - cy) ** 2 <= ob["radius"] ** 2
                else:
                    inside = abs(x - cx) <= ob["half_w"] and abs(y - cy) <= ob["half_h"]
                if not inside:
                    continue
                in_crater = any((x - ct["x"]) ** 2 + (y - ct["y"]) ** 2 <= crater_r ** 2
                                for ct in ob["craters"])
                grid[r][c] = "o" if in_crater else "#"

    for pid, p in players.items():
        mine = (p["team"] == my_team)
        for si, s in enumerate(p["soldiers"]):
            c = int((s["x"] - X0) * 2)
            r = int(Y1 - s["y"])
            if not (0 <= c < COLS and 0 <= r < ROWS):
                continue
            if not s["alive"]:
                grid[r][c] = "x"
            else:
                grid[r][c] = str(si + 1) if mine else chr(ord("a") + si)

    lines = []
    for r in range(ROWS):
        lines.append(f"{int(Y1 - r):4d} " + "".join(grid[r]))
    ruler = [" "] * COLS
    for xv in range(-20, 21, 5):
        p = int((xv - X0) * 2)
        for i, ch in enumerate(str(xv)):
            if 0 <= p + i < COLS:
                ruler[p + i] = ch
    lines.append("     " + "".join(ruler))
    return lines

def render_state(reject_note=""):
    L = []
    L.append(f"===== 函数对战：我（A 队 {MY_NAME}） vs {ai_name()}"
             f"（B 队，{DIFF_NAMES.get(AI_DIFF, '?')}） =====")
    L.append(f"第 {round_no} 回合 | 轮到：我 | 当前出手士兵：#{last_turn.get('soldier', 0) + 1}")
    L.append("图例：1/2/3=我的兵  a/b/c=敌方兵  #=遮挡物  o=被打穿的坑  x=阵亡残骸")
    L.extend(render_map())
    L.append("")
    L.append("--- 士兵坐标（精确）---")
    for pid, p in players.items():
        tag = "（我）" if pid == my_id else f"（{DIFF_NAMES.get(AI_DIFF, '?')}AI）"
        L.append(f"{p['name']} {p['team']} 队{tag}：")
        for si, s in enumerate(p["soldiers"]):
            mark = str(si + 1) if p["team"] == my_team else chr(ord("a") + si)
            star = "  ★ 本回合出手" if pid == my_id and si == last_turn.get("soldier") else ""
            state = f"hp{s['hp']}/{s['max_hp']}" + ("" if s["alive"] else " 已阵亡")
            L.append(f"  [{mark}] ({s['x']:7.2f}, {s['y']:7.2f})  {state}{star}")
    L.append("")
    L.append("--- 遮挡物 ---")
    for i, ob in enumerate(obstacles):
        if ob["is_circle"]:
            shape = f"圆形 中心({ob['center']['x']:.2f},{ob['center']['y']:.2f}) 半径{ob['radius']:.2f}"
        else:
            shape = (f"矩形 中心({ob['center']['x']:.2f},{ob['center']['y']:.2f}) "
                     f"半宽{ob['half_w']:.2f} 半高{ob['half_h']:.2f}")
        cr = f"  坑×{len(ob['craters'])}" if ob["craters"] else ""
        L.append(f"  #{i} {shape}{cr}")
    L.append("")
    L.append("--- 最近事件（新→旧）---")
    for e in reversed(events[-12:]):
        L.append("  " + e)
    if my_history:
        L.append("")
        L.append("--- 我已出手 ---")
        for i, h in enumerate(my_history):
            L.append(f"  第{i + 1}发: {h}")
    if ai_shots:
        L.append("--- AI 最近出手 ---")
        for nm, ex in ai_shots:
            L.append(f"  {nm}: {ex}")
    L.append("")
    if reject_note:
        L.append(f"！！ {reject_note}")
    L.append("================= 等待你的表达式 =================")
    L.append(f"把一条 y=f(x) 表达式写入 {SHOT}（一行，写入后自动提交）")
    L.append("坐标语义：以你当前出手士兵为原点，x 向右为正；常数对弹道无影响（f(0) 归零）")
    with open(STATE, "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")

# ---- 回合等待：读写文件 + 继续收消息 ----
def wait_for_shot(s):
    global pending_reject
    wrote_reject = False
    while True:
        if os.path.exists(SHOT):
            with open(SHOT, encoding="utf-8") as f:
                txt = f.read().strip()
            os.remove(SHOT)
            line = ""
            for ln in txt.splitlines():
                ln = ln.strip()
                if ln and not ln.startswith("#"):
                    line = ln
                    break
            if not line:
                continue
            send(s, {"type": "submit",
                     "request": {"mode": "y_of_x", "expr_a": line, "dir": "pos"}})
            my_history.append(line)
            pending_reject = ""
            log(f"[我] 提交：{line}")
            # 不在此返回：等自己的 shot 回声（或回合翻页）确认本回合结束
            continue
        m = recv_one(s, 0.25)
        if m is None:
            continue
        handle(m)
        if m.get("type") == "error" and not wrote_reject:
            render_state(pending_reject)
            wrote_reject = True
        if m.get("type") == "game_over":
            return "over"
        if m.get("type") == "shot" and m.get("owner") == my_id:
            return None
        if m.get("type") == "turn_skipped" and m.get("player_id") == my_id:
            return "skipped"
        if m.get("type") == "turn_begin" and m.get("player_id") != my_id:
            return None  # 回合已翻页（理论上 shot 回声先到，这里兜底）

def main():
    global my_id, winner, AI_DIFF
    args = parse_args()
    AI_DIFF = args.difficulty
    if args.seed is not None:
        CFG["seed"] = args.seed
    os.makedirs(DIR, exist_ok=True)
    for f in (SHOT, DONE):
        if os.path.exists(f):
            os.remove(f)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write("---- 新对局 ----\n")

    s = socket.create_connection((HOST, PORT), timeout=5)
    hello = recv_one(s, 3)
    log(f"hello: {hello}")

    send(s, {"type": "create_room", "name": MY_NAME, "config": CFG})
    joined = None
    for _ in range(60):
        m = recv_one(s, 3)
        if m is None:
            break
        if m.get("type") == "joined":
            joined = m
            break
        if m.get("type") == "error":
            log(f"建房被拒：{m}"); return 1
    if not joined:
        log("建房失败"); return 1
    my_id = joined["member_id"]
    log(f"已建房：member_id={my_id} room_id={joined.get('room_id')}")

    send(s, {"type": "add_ai", "difficulty": AI_DIFF, "team": "B"})
    time.sleep(0.3)
    send(s, {"type": "start"})
    log(f"已发 add_ai(难度 {AI_DIFF}) 与 start")

    while True:
        m = recv_one(s, 1.0)
        if m is None:
            continue
        t = m.get("type")
        handle(m)
        if t == "turn_begin" and m.get("player_id") == my_id:
            render_state()
            r = wait_for_shot(s)
            if r == "over":
                break
        elif t == "game_over":
            break

    if os.path.exists(SHOT):
        os.remove(SHOT)
    verdict = {0: "我（A 队）胜", 1: f"{ai_name()}（B 队）胜", 2: "平局"}.get(winner, f"未知({winner})")
    with open(DONE, "w", encoding="utf-8") as f:
        f.write(f"对局结束：{verdict}\n")
        f.write("我的出手：\n")
        for i, h in enumerate(my_history):
            f.write(f"  第{i + 1}发: {h}\n")
    log(f"对局结束：{verdict}；我一共出手 {len(my_history)} 次")
    return 0

if __name__ == "__main__":
    sys.exit(main())
