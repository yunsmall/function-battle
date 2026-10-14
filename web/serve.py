"""函数对战 · 网页版网关（零依赖，仅标准库）。

一个端口同时干两件事：
  - 带 `Upgrade: websocket` 的请求：升级为 WebSocket，并与游戏服务器
    （TCP，协议见 PROTOCOL.md）连一条线，在 WS 文本帧与 TCP 长度前缀帧之间双向搬运；
  - 其余 GET 请求：从 web/static/ 出静态文件。

前端与网关同源，因此没有跨域问题，整个网页版零第三方依赖、零构建。
WebSocket 之后会折进 C++ 服务器（单进程部署），届时这个进程退休、前端不动——
所以这里刻意不做任何业务逻辑，只做搬运。

用法：
    python web/serve.py                     # 监听 127.0.0.1:8000，桥到 127.0.0.1:9527
    python web/serve.py --port 8080 --game-port 9527
"""
import argparse
import asyncio
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # 让 wsproto 可导入（脚本内直接运行）

from wsproto import (  # noqa: E402
    FrameReader,
    WsError,
    accept_key,
    encode_frame,
    read_http_head,
)

MAX_GAME_FRAME = 8 * 1024 * 1024

CONTENT_TYPES = {
    ".html": "text/html; charset=utf-8",
    ".js": "text/javascript; charset=utf-8",
    ".css": "text/css; charset=utf-8",
    ".json": "application/json; charset=utf-8",
    ".svg": "image/svg+xml",
    ".png": "image/png",
    ".ico": "image/x-icon",
}


def now_str() -> str:
    return time.strftime("%H:%M:%S")


class Gateway:
    def __init__(self, host, port, game_host, game_port, static_dir):
        self.host = host
        self.port = port
        self.game_host = game_host
        self.game_port = game_port
        self.static_root = Path(static_dir).resolve()

    def log(self, msg: str):
        print(f"[{now_str()}] {msg}", flush=True)

    async def handle(self, reader, writer):
        peer = writer.get_extra_info("peername")
        peer = f"{peer[0]}:{peer[1]}" if peer else "?"
        try:
            method, path, headers, rest = await read_http_head(reader)
        except (WsError, asyncio.IncompleteReadError, ConnectionError):
            writer.close()
            return
        try:
            if headers.get("upgrade", "").lower() == "websocket":
                await self.handle_ws(reader, writer, headers, rest, peer)
            elif method in ("GET", "HEAD"):
                await self.handle_static(writer, method, path)
            else:
                await self.respond(writer, 405, b"Method Not Allowed", "text/plain; charset=utf-8")
        except (WsError, asyncio.IncompleteReadError, ConnectionResetError, BrokenPipeError):
            pass  # 客户端中途断开是日常，不值得打日志刷屏
        finally:
            try:
                writer.close()
            except Exception:
                pass

    async def respond(self, writer, status, body, ctype, head_len=None):
        n = len(body) if head_len is None else head_len
        # no-cache 是给开发留的：改完 JS 刷新即生效，避免旧缓存误导排查
        head = (
            f"HTTP/1.1 {status}\r\nContent-Type: {ctype}\r\nContent-Length: {n}\r\n"
            f"Cache-Control: no-cache\r\nConnection: close\r\n\r\n"
        )
        writer.write(head.encode("latin-1") + body)
        await writer.drain()

    async def handle_static(self, writer, method, path):
        rel = path.split("?", 1)[0].split("#", 1)[0].lstrip("/") or "index.html"
        target = (self.static_root / rel).resolve()
        # 目录穿越防护：解析后的真实路径必须仍在 static 根里
        inside = target == self.static_root or str(target).startswith(str(self.static_root) + os.sep)
        if not inside:
            await self.respond(writer, 403, b"403 Forbidden", "text/plain; charset=utf-8")
            return
        if target.is_dir():
            target = target / "index.html"
        if not target.is_file():
            await self.respond(writer, 404, b"404 Not Found", "text/plain; charset=utf-8")
            return
        body = await asyncio.to_thread(target.read_bytes)
        ctype = CONTENT_TYPES.get(target.suffix.lower(), "application/octet-stream")
        if method == "HEAD":
            await self.respond(writer, 200, b"", ctype, head_len=len(body))
        else:
            await self.respond(writer, 200, body, ctype)

    async def handle_ws(self, reader, writer, headers, rest, peer):
        key = headers.get("sec-websocket-key")
        if not key:
            await self.respond(writer, 400, b"Missing Sec-WebSocket-Key", "text/plain; charset=utf-8")
            return
        # 先连游戏服务器：连不上就明确回 503（前端能显示"游戏服务器未启动"），
        # 而不是升级成功后再无声断开——那样前端只能看到"连接失败"四个字
        try:
            tcp_reader, tcp_writer = await asyncio.wait_for(
                asyncio.open_connection(self.game_host, self.game_port), 5.0
            )
        except (OSError, asyncio.TimeoutError) as exc:
            self.log(f"WS {peer} 拒绝升级：游戏服务器不可达（{exc}）")
            await self.respond(
                writer, 503, "游戏服务器未启动（fg_server）。".encode("utf-8"),
                "text/plain; charset=utf-8",
            )
            return
        resp = (
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Accept: " + accept_key(key) + "\r\n\r\n"
        )
        try:
            writer.write(resp.encode("ascii"))
            await writer.drain()
            self.log(f"桥接开启 {peer} → {self.game_host}:{self.game_port}")

            frames = FrameReader(reader, initial=rest)
            up = down = 0

            async def ws_to_tcp():
                nonlocal up
                while True:
                    op, data = await frames.read_message()
                    if op == 0x8:
                        return
                    if op == 0x9:  # ping → pong（载荷原样回）
                        writer.write(encode_frame(data, 0xA))
                        await writer.drain()
                        continue
                    if op == 0xA:
                        continue
                    if op not in (0x1, 0x2):
                        return
                    tcp_writer.write(len(data).to_bytes(4, "little") + data)
                    await tcp_writer.drain()
                    up += len(data)

            async def tcp_to_ws():
                nonlocal down
                while True:
                    hdr = await tcp_reader.readexactly(4)
                    n = int.from_bytes(hdr, "little")
                    if n > MAX_GAME_FRAME:
                        return
                    body = await tcp_reader.readexactly(n)
                    writer.write(encode_frame(body, 0x1))
                    await writer.drain()
                    down += len(body)

            tasks = [asyncio.create_task(ws_to_tcp()), asyncio.create_task(tcp_to_ws())]
            try:
                await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            finally:
                for t in tasks:
                    t.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
                try:
                    writer.write(encode_frame(b"", 0x8))  # 让浏览器立刻触发 onclose，而不是等 TCP 超时
                    await writer.drain()
                except Exception:
                    pass
                self.log(f"桥接关闭 {peer} 上行 {up}B 下行 {down}B")
        finally:
            # 从 TCP 连上到桥接结束之间的任何一步失败（含握手应答写不出去——
            # 客户端在升级响应回来前刷新页面就会这样），游戏服务器那头的连接都必须关，
            # 否则它要悬到 GC 才释放，白占服务器一个连接槽
            try:
                tcp_writer.close()
            except Exception:
                pass

    async def run(self):
        server = await asyncio.start_server(self.handle, self.host, self.port)
        shown = "127.0.0.1" if self.host in ("0.0.0.0", "") else self.host
        print(f"网页网关已启动：http://{shown}:{self.port}")
        print(f"  游戏服务器：{self.game_host}:{self.game_port}")
        print(f"  静态目录：{self.static_root}")
        print("按 Ctrl+C 停止")
        async with server:
            await server.serve_forever()


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description="函数对战 · 网页版网关（静态文件服务 + WebSocket↔TCP 桥）")
    ap.add_argument("--host", default="127.0.0.1", help="监听地址（默认 127.0.0.1，仅本机）")
    ap.add_argument("--port", type=int, default=8000, help="监听端口（默认 8000）")
    ap.add_argument("--game-host", default="127.0.0.1", help="游戏服务器地址（默认 127.0.0.1）")
    ap.add_argument("--game-port", type=int, default=9527, help="游戏服务器端口（默认 9527）")
    ap.add_argument("--static-dir", default=None, help="静态目录（默认 <脚本目录>/static）")
    return ap.parse_args(argv)


def main():
    args = parse_args()
    static_dir = args.static_dir or (Path(__file__).resolve().parent / "static")
    gw = Gateway(args.host, args.port, args.game_host, args.game_port, static_dir)
    try:
        asyncio.run(gw.run())
    except KeyboardInterrupt:
        print("\n网关已停止")


if __name__ == "__main__":
    main()
