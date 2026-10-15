#!/usr/bin/env python3
"""AI 参数自动调参：坐标下降搜索各难度档位参数，把对局胜率拉到均衡。

背景：AI 各档的手感参数（瞄准误差、多杀限流……）与游戏物理耦合——炮弹碰撞体、
地图尺寸这类游戏参数一变，档位间胜率梯度就跑偏。与其人工一轮轮试，不如定好
优化目标让这个脚本自动搜（每次评估跑一遍 fb_ai_bench 对打矩阵，固定种子可复现）。

优化目标（损失 = 各缺口平方和，0 = 全部达标）：
  - 相邻档胜率（简单-普通、普通-困难、困难-大神）>= --win-target（默认 0.72，
    比要求的 0.70 留一点采样噪声余量）
  - 简单档">=1 次命中"的对局占比 >= --hit-target（默认 0.60）：哪怕最笨的 AI
    也不能连着好几局一个人都摸不到（闷）

搜索：坐标下降——每轮把"所有维度的 ±一步"作为整批候选**并行评估**（多个
fb_ai_bench 进程、各分一部分核），取损失最低的点作新中心；一轮无改善步长减半。
所有评估共用固定种子（同参数的结果可缓存、跨候选是配对比较），结果可复现。

用法（项目根目录下执行）：
  python scripts/tune_ai.py --games 150            # 标准调参（并行后几十分钟级）
  python scripts/tune_ai.py --rounds 0             # 只评估当前默认参数（冒烟）
  python scripts/tune_ai.py --start-from 报告.json  # 从上次最优参数续跑
"""

import argparse
import json
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

# 可调维度：(档位, 键名, 初值, 下限, 上限)。初值与 ai.cpp 的默认档位表一致；
# 键名与 fb_ai_bench --profile 的键一致（pos/angle/k2/k3/…）。
# 多杀限流（k2/k3）与 jitter 轮数不进搜索空间：它们是"对局节奏/杀手锏"的设计约束，
# 有专门的单测守着，不该被胜率目标推着走
DIMS = [
    (2, "pos", 0.042, 0.005, 0.09),
    (2, "angle", 0.06, 0.005, 0.08),
    (3, "pos", 0.002, 0.0005, 0.03),
    (3, "angle", 0.00225, 0.0005, 0.02),
    (1, "pos", 0.06, 0.01, 0.12),
    (1, "angle", 0.06, 0.01, 0.10),
    (0, "pos", 0.15, 0.03, 0.20),
    (0, "angle", 0.09, 0.01, 0.12),
    # 简单档的瞄准散布封顶：单发命中率（脱靶 50% 目标）的主旋钮——误差封小则
    # 每发更容易正好落在命中半径内，封大则大量放空
    (0, "cap", 1.4, 0.2, 4.0),
]

PAIRS = [(0, 1), (1, 2), (2, 3)]


def default_params() -> dict:
    return {(d, k): v for d, k, v, _, _ in DIMS}


def profile_arg(params: dict) -> str:
    """把参数向量拼成 --profile 字符串（每档发它拥有的全部键）。"""
    by_diff = {}
    for (d, k), v in params.items():
        by_diff.setdefault(d, {})[k] = v
    return ";".join(
        f"{d}:" + ",".join(f"{k}={v:.6g}" for k, v in sorted(kv.items()))
        for d, kv in sorted(by_diff.items())
    )


class Bench:
    """fb_ai_bench 的调用器：同参数结果缓存（并行候选之间常撞参数）。"""

    def __init__(self, args):
        self.args = args
        # 并行 N 个进程、每个分总核数的 1/N（下限 2 线程，避免进程太多细碎调度）
        self.threads = max(2, args.threads // args.parallel)
        self.cache = {}
        self.lock = threading.Lock()
        self.evals = 0

    def evaluate(self, params: dict) -> dict:
        key = profile_arg(params)
        with self.lock:
            if key in self.cache:
                return self.cache[key]
        cmd = [
            str(self.args.bench),
            "--games", str(self.args.games),
            "--threads", str(self.threads),
            "--pairs", ",".join(f"{lo}-{hi}" for lo, hi in PAIRS),
            "--json",
            "--profile", key,
        ]
        t0 = time.time()
        # 显式 utf-8：fb_ai_bench 输出是 UTF-8（含中文），不能靠 locale 解码
        # （中文 Windows 默认 cp936，遇到无效字节会直接抛 UnicodeDecodeError）
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              encoding="utf-8", errors="replace")
        if proc.returncode != 0:
            sys.stderr.write(proc.stderr)
            raise RuntimeError(f"fb_ai_bench 退出码 {proc.returncode}")
        parts = proc.stdout.split("=== JSON ===")
        if len(parts) < 2:
            raise RuntimeError("fb_ai_bench 输出里找不到 === JSON === 标记")
        try:
            report = json.loads(parts[1])
        except json.JSONDecodeError as e:
            raise RuntimeError(
                f"fb_ai_bench 的 JSON 段解析失败：{e}；输出尾部：{parts[-1][-200:]!r}") from e
        report["_elapsed"] = time.time() - t0
        with self.lock:
            self.cache[key] = report
            self.evals += 1
        return report


def pair_winrate(report: dict, lo: int, hi: int) -> float:
    for p in report["pairs"]:
        if p["low"] == lo and p["high"] == hi:
            decisive = p["low_wins"] + p["high_wins"]
            return p["high_wins"] / decisive if decisive > 0 else 0.0
    raise RuntimeError(f"报告里缺少对局 {lo}-{hi}")


def metrics(report: dict) -> dict:
    m = {f"win{lo}{hi}": pair_winrate(report, lo, hi) for lo, hi in PAIRS}
    h = report["hit"].get("0", {})
    m["hit0"] = h.get("hit_games", 0) / max(1, h.get("games", 0))
    # 简单档单发命中率 = 命中事件数 / 出手数（"脱靶率 50%"口径）
    shots0 = sum(report.get("types", {}).get("0", {}).values())
    m["fire0"] = (h.get("hits", 0) / shots0) if shots0 else 0.0
    return m


def loss(report: dict, args) -> float:
    m = metrics(report)
    total = 0.0
    for lo, hi in PAIRS:
        gap = args.win_target - m[f"win{lo}{hi}"]
        if gap > 0:
            total += gap * gap
    gap = args.hit_target - m["hit0"]
    if gap > 0:
        total += gap * gap
    # 简单档单发命中率贴近目标值（双向：太准也是偏离）。权重 4：它是"每发"量级、
    # 相对波动比胜率细，放大后与胜率缺口同量级，两类目标可以同时被推动
    gap = m["fire0"] - args.firing_hit_target
    total += gap * gap * 4.0
    return total


def fmt_metrics(m: dict) -> str:
    return (f"0-1:{m['win01']*100:5.1f}%  1-2:{m['win12']*100:5.1f}%  "
            f"2-3:{m['win23']*100:5.1f}%  hit0:{m['hit0']*100:5.1f}%  "
            f"fire0:{m['fire0']*100:5.1f}%")


def log(tag: str, m: dict, L: float, cost: float):
    print(f"[{tag}] loss={L:.5f} | {fmt_metrics(m)} | {cost:.0f}s", flush=True)


def clamp(v: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, v))


def optimize(bench: Bench, args, params: dict) -> tuple:
    best = dict(params)
    report = bench.evaluate(best)
    best_L = loss(report, args)
    best_m = metrics(report)
    log("起点", best_m, best_L, report["_elapsed"])
    if best_L <= 1e-9 or args.rounds == 0:
        return best, best_L, best_m

    step = 0.25
    for round_no in range(1, args.rounds + 1):
        # 一轮的全部候选：每个维度沿正负方向各走一步
        cands = []  # (标签, 参数)
        for (d, k, _, lo, hi) in DIMS:
            for sign in (+1.0, -1.0):
                cand = dict(best)
                cand[(d, k)] = clamp(best[(d, k)] * (1.0 + sign * step), lo, hi)
                if abs(cand[(d, k)] - best[(d, k)]) < 1e-9:
                    continue  # 撞界或步长太小：该方向没得走
                cands.append((f"r{round_no} d{d}.{k}{'+' if sign > 0 else '-'}", cand))
        # 并行评估整批（各候选独立进程；同种子下互为配对比较）
        results = []
        with ThreadPoolExecutor(max_workers=args.parallel) as ex:
            futs = {ex.submit(bench.evaluate, c): (tag, c) for tag, c in cands}
            for f in as_completed(futs):
                tag, cand = futs[f]
                try:
                    results.append((tag, cand, f.result()))
                except Exception as e:  # noqa: BLE001——单个候选失败不该炸整轮搜索
                    print(f"[!] 候选 {tag} 评估失败，已跳过：{e}", flush=True)
        improved = False
        for tag, cand, r in sorted(results, key=lambda t: loss(t[2], args)):
            L = loss(r, args)
            m = metrics(r)
            log(tag, m, L, r["_elapsed"])
            if L < best_L - 1e-9:
                best, best_L, best_m = cand, L, m
                improved = True
        if best_L <= 1e-9:
            return best, best_L, best_m
        if not improved:
            step *= 0.5
            print(f"  一轮无改善，步长缩到 {step:.3f}", flush=True)
            if step < 0.04:
                break
        else:
            print(f"  本轮采纳新中心（loss={best_L:.5f}）", flush=True)
    return best, best_L, best_m


def main():
    ap = argparse.ArgumentParser(description="AI 参数自动调参（坐标下降，目标=胜率均衡）")
    root = Path(__file__).resolve().parents[1]
    ap.add_argument("--bench", type=Path,
                    default=root / "build" / "src" / "tools" / "fb_ai_bench.exe",
                    help="fb_ai_bench 可执行文件路径")
    ap.add_argument("--games", type=int, default=150, help="每对每方向的对局数（默认 150）")
    ap.add_argument("--threads", type=int, default=16,
                    help="总 CPU 预算（默认 16；按 --parallel 分给各评测进程）")
    ap.add_argument("--parallel", type=int, default=4, help="并行评测的进程数（默认 4）")
    ap.add_argument("--rounds", type=int, default=6, help="坐标下降轮数上限（默认 6）")
    ap.add_argument("--win-target", type=float, default=0.72,
                    help="相邻档胜率目标（默认 0.72，比 0.70 的要求留噪声余量）")
    ap.add_argument("--hit-target", type=float, default=0.60,
                    help="简单档命中局占比目标（默认 0.60）")
    ap.add_argument("--firing-hit-target", type=float, default=0.50,
                    help="简单档单发命中率目标（默认 0.50，即脱靶率 50%%）")
    ap.add_argument("--start-from", type=Path, default=None,
                    help="从上一次的报告 JSON 读取参数作为起点")
    ap.add_argument("--report-out", type=Path, default=None,
                    help="最优结果写入的 JSON 路径（默认 scripts/ai_tune_result.json）")
    args = ap.parse_args()

    if not Path(args.bench).exists():
        ap.error(f"评测工具不存在：{args.bench}（先构建 fb_ai_bench）")

    params = default_params()
    if args.start_from:
        try:
            with open(args.start_from, encoding="utf-8") as f:
                saved = json.load(f)["best_params"]
        except (OSError, json.JSONDecodeError, KeyError) as e:
            ap.error(f"读取 --start-from 失败（{args.start_from}）：{e}")
        for (d, k, _, lo, hi) in DIMS:
            v = saved.get(f"{d}.{k}")
            if v is not None:
                params[(d, k)] = clamp(float(v), lo, hi)
        print(f"从 {args.start_from} 续跑")

    bench = Bench(args)
    t0 = time.time()
    best, best_L, best_m = optimize(bench, args, params)
    print(f"\n共 {bench.evals} 次评估（含去重），耗时 {(time.time()-t0)/60:.1f} 分钟")
    print(f"最优 loss={best_L:.5f} | {fmt_metrics(best_m)}")

    out = {
        "loss": best_L,
        "metrics": best_m,
        "win_target": args.win_target,
        "hit_target": args.hit_target,
        "games": args.games,
        "evals": bench.evals,
        "best_params": {f"{d}.{k}": v for (d, k), v in sorted(best.items())},
        # 方便直接贴回 ai.cpp 的覆盖串
        "profile_arg": profile_arg(best),
    }
    out_path = args.report_out or (root / "scripts" / "ai_tune_result.json")
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=2)
    print(f"报告已写入 {out_path}")
    if best_L > 1e-9:
        print("[!] 目标未完全达成——可加大 --rounds/--games 或放宽某个阈值后重跑")


if __name__ == "__main__":
    main()
