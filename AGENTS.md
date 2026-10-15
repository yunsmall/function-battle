# AGENTS.md — 给 AI 助手的项目速览

给接手本项目的 AI 的最小必要信息；人类向文档看 `README.md`，协议看 `PROTOCOL.md`，
历史决策见各代码头注释与 `review/` 下的审查报告（该目录属开发过程材料、不进版本库，
只有开发机上才有）。

## 一句话与关键布局

函数对战：坐标系上的回合制数学射击（输入曲线 → 炮弹沿线飞行 → 命中/穿透/友伤/炸障）。

- `src/core/`：共享核心（曲线采样、世界模拟、AI、房间、协议）——服务器与客户端**共用**，
  改这里会同时影响所有端；AI 在 `ai.cpp`（规划主链/择优）+ `ai_curves.cpp`（骨架生成）
- `src/server/` 权威服务器；`src/client/` raylib 客户端；`web/static/` 网页客户端
- `src/tests/` gtest（fb_tests）；`src/tools/` fb_ai_bench（AI 对打评测）、fb_play
  （文件信箱式客户端，供 AI 亲自实玩检验）；`scripts/tune_ai.py` 自动调参
- 构建：clang + Ninja + RelWithDebInfo(-O2) + lld-link + LTO（CMake 已配好）

## 构建与测试

（首次克隆后按 `README.md` 的 vcpkg + cmake 步骤配置一次，之后：）

```bash
cmake --build build                # 改任何代码后重新编译（-j 按核数；别同时跑多份重编译）
build/src/tests/fb_tests.exe       # 全部单测，改 AI/物理/曲线必跑
```

- 压测直接给测试二进制传 `--gtest_repeat=N --gtest_break_on_failure --gtest_brief=1`，不走 ctest
- 链接报 "unable to remove file: permission denied" = 有 exe 正在运行（服务器/客户端），先停掉

## AI 的口径与验证流程（动 AI 前必读）

**设计口径**（2026-10 定稿，改动前想清楚是不是在违背它）：
- 强度 = 相邻难度档胜率 ≈70%（调参只惩罚低于 72% 的档）
- 择优"好线优先"：干净（全程不碰障碍）硬过滤 > 命中数 > 友伤 > 长度；
  "命中数优先于友伤"是**刻意设计**（宁可擦自己人也多打一个敌人）
- **一发多杀是难度实力的核心载体**（简单/普通上限 1 人/2 人不变，困难 ≤4，大神不限）
- 评测固定种子完全可复现；改行为后与 baseline 同种子对比（`--games 300 --json`）

**改 AI 的标准流程**：单测 → `fb_ai_bench --games 300 --json`（看"每发打中人数分布"
与胜率矩阵）→ 需要诊断时 `FG_AI_TRACE=1`（trace 行 `multi_clean=` 池里干净多杀候选数、
`degraded=` 被限流否掉）→ 参数变化跑调参：`python scripts/tune_ai.py --games 150
--parallel 4 --threads <核数> --firing-hit-target 0.25`（`--start-from <历史结果.json>`
可从上次继续）（样本 <150 局噪声大；物理一改必须重跑）→ 最优参数**落地到
`ai_profile()` 默认值**并注释日期。

**实玩检验**（AI 亲自打一局看体感）：

```bash
build/src/server/fb_server.exe --input-seconds 600   # 必须 600；默认 120 会频繁超时
build/src/tools/fb_play.exe --difficulty 3           # 我=A 队 vs B 队大神 AI
```

读 `build/src/tools/session/state.txt`（ASCII 战场 + 回合时限倒计时），把一条表达式
（**不带 "y=" 前缀**，服务器字符白名单拒收 '='）写进 `session/shot.txt` 即自动提交。
坐标以出手士兵为原点、x 向右、f(0) 归零；多杀 = step 折线过多个目标点，**陡升/陡降段
离障碍角留 >0.2 余量**（贴角必撞）。

**性能**：评测与决策耗时已经四轮收敛（有语义理由的开销不要盲目再削）；profiling 用
Linux perf 一类采样器即可，历史轮次与教训见 `review/2026-10-15_perf_round*.md`。

## 外部审查工作流（可选）

本项目历史上用外部 AI（网页版）做过多轮批量代码审查——性能、AI 多杀链等批次的报告
都在 `review/`。再次发起这类审查时沿用这些经验：

- **报告与提示词都归 `review/`**：报告放 `review/*.md`，提示词放 `review/prompts/`
- 提示词必须给"充分检查后没有真问题就直说没找到"留出口，否则模型会凑数
- 报告整份读进来**逐条甄别**：误报就地补一句"为什么这么写是对的"的注释（下轮不会再报）；
  真问题实施后跑完整验证（单测 + 同种子 bench）；同一主题多轮审到只剩细枝末节为止

## 代码约定

- 注释全中文、只写"为什么"（不向读者描述代码历史变化）；4 空格缩进（跟现有文件）
- 服务器返回的错误必须 `code + message` 两字段（多语言/网页端预留）
- 协议改动必须同步 `PROTOCOL.md`
