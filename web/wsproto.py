"""WebSocket 帧编解码与握手辅助（网关与自测客户端共用）。

只实现 RFC 6455 里本项目需要的子集：文本/二进制帧、分片、close/ping/pong；
不协商任何扩展（不回应 permessage-deflate，浏览器就会退回无压缩模式）。

帧上限与游戏服务器 TCP 帧上限（8 MiB）保持一致：一条链路放行、另一条断开
会让超长消息的表现变得扑朔迷离。
"""
import asyncio
import base64
import hashlib
import json
import os
import struct

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_FRAME = 8 * 1024 * 1024


class WsError(Exception):
    """WebSocket/握手层的协议错误；调用方关闭连接即可。"""


def accept_key(sec_key: str) -> str:
    """服务端握手应答：sha1(key + GUID) 的 base64（RFC 6455 规定的固定算法）。"""
    digest = hashlib.sha1((sec_key + GUID).encode("ascii")).digest()
    return base64.b64encode(digest).decode("ascii")


def _xor(data: bytes, mask: bytes) -> bytes:
    return bytes(b ^ mask[i & 3] for i, b in enumerate(data))


def encode_frame(payload: bytes, opcode: int = 0x1, masked: bool = False) -> bytes:
    """组一帧。masked=True 时随机生成掩码键（客户端方向必须掩码，服务端禁止）。"""
    header = bytearray([0x80 | opcode])
    n = len(payload)
    mbit = 0x80 if masked else 0
    if n < 126:
        header.append(mbit | n)
    elif n < 65536:
        header.append(mbit | 126)
        header += struct.pack(">H", n)
    else:
        header.append(mbit | 127)
        header += struct.pack(">Q", n)
    if not masked:
        return bytes(header) + payload
    mask = os.urandom(4)
    return bytes(header) + mask + _xor(payload, mask)


class FrameReader:
    """从 asyncio 流里读出完整 WS 消息（分片自动拼接；控制帧原样返回）。

    initial 用于把"读 HTTP 头时顺带多读到的字节"喂进来——TCP 会把响应头和
    紧随其后的帧粘在一个包里，丢掉这部分等于丢掉开局消息。
    """

    def __init__(self, reader: asyncio.StreamReader, initial: bytes = b""):
        self._reader = reader
        self._buf = bytearray(initial)
        self._opcode = None
        self._parts = []
        self._size = 0

    async def _take(self, n: int) -> bytes:
        while len(self._buf) < n:
            chunk = await self._reader.read(65536)
            if not chunk:
                raise asyncio.IncompleteReadError(bytes(self._buf), n)
            self._buf += chunk
        out = bytes(self._buf[:n])
        del self._buf[:n]
        return out

    async def read_message(self):
        """返回 (opcode, payload)；分片消息收齐后整体返回，控制帧随时返回。"""
        while True:
            hdr = await self._take(2)
            fin = bool(hdr[0] & 0x80)
            opcode = hdr[0] & 0x0F
            masked = bool(hdr[1] & 0x80)
            length = hdr[1] & 0x7F
            if length == 126:
                length = struct.unpack(">H", await self._take(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", await self._take(8))[0]
            # 长度校验必须在读载荷之前：length 可达 2^64-1，先读再查会让网关
            # 为一个恶意帧头一路缓冲到 OOM（累计检查拦的是分片总和，拦不住单帧）
            if length > MAX_FRAME:
                raise WsError("帧声明长度超过 8 MiB 上限")
            mask = await self._take(4) if masked else None
            data = await self._take(length) if length else b""
            if mask:
                data = _xor(data, mask)
            if opcode in (0x8, 0x9, 0xA):
                return opcode, data  # 控制帧：不参与分片拼接
            if opcode in (0x1, 0x2):
                if self._opcode is not None:
                    raise WsError("上一条消息未结束就开了新消息")
                self._opcode, self._parts, self._size = opcode, [], 0
            elif opcode == 0x0:
                if self._opcode is None:
                    raise WsError("孤立的续帧")
            else:
                raise WsError(f"不支持的帧类型 0x{opcode:x}")
            self._parts.append(data)
            self._size += len(data)
            if self._size > MAX_FRAME:
                raise WsError("消息超过 8 MiB 上限")
            if fin:
                opcode, payload = self._opcode, b"".join(self._parts)
                self._opcode, self._parts, self._size = None, [], 0
                return opcode, payload


async def read_http_head(reader: asyncio.StreamReader):
    """读一个 HTTP 请求头（读到空行为止）。

    返回 (方法, 路径, 头部字典[小写键], 多读到的剩余字节)。
    """
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = await reader.read(4096)
        if not chunk:
            raise WsError("握手前连接已关闭")
        data += chunk
        if len(data) > 64 * 1024:
            raise WsError("请求头过大")
    head, _, rest = data.partition(b"\r\n\r\n")
    lines = head.decode("latin-1").split("\r\n")
    parts = lines[0].split()
    if len(parts) < 2:
        raise WsError("请求行不合法")
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
    return parts[0], parts[1], headers, rest


async def connect_ws(host: str, port: int, path: str = "/", timeout: float = 10.0):
    """客户端方向：完成 WS 握手，返回 (FrameReader, writer)。"""
    reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    req = (
        f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
    )
    writer.write(req.encode("ascii"))
    await writer.drain()
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = await asyncio.wait_for(reader.read(4096), timeout)
        if not chunk:
            raise WsError("握手中连接被关闭")
        head += chunk
        if len(head) > 64 * 1024:
            raise WsError("握手响应头过大")
    text = head.decode("latin-1")
    status = text.split("\r\n", 1)[0]
    if "101" not in status:
        raise WsError("网关拒绝升级：" + status)
    if accept_key(key).lower() not in text.lower():
        raise WsError("Sec-WebSocket-Accept 校验失败")
    _, _, rest = head.partition(b"\r\n\r\n")
    return FrameReader(reader, initial=rest), writer


def encode_game_frame(obj) -> bytes:
    """游戏协议帧：4 字节小端长度 + UTF-8 JSON（见 PROTOCOL.md）。"""
    data = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return len(data).to_bytes(4, "little") + data
