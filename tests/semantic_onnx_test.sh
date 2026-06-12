#!/usr/bin/env bash
set -e

HOST="${1:-127.0.0.1}"
PORT="${2:-28080}"

python3 - "$HOST" "$PORT" <<'PY'
import socket
import sys

host = sys.argv[1]
port = int(sys.argv[2])
scope = "onnx_demo"


class RespClient:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=30)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()

    def send(self, tokens):
        chunks = [f"*{len(tokens)}\r\n".encode()]
        for token in tokens:
            data = str(token).encode("utf-8")
            chunks.append(f"${len(data)}\r\n".encode())
            chunks.append(data)
            chunks.append(b"\r\n")
        self.sock.sendall(b"".join(chunks))
        return self.read()

    def _fill(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(8192)
            if not chunk:
                raise RuntimeError("connection closed")
            self.buf += chunk

    def _read_line(self):
        while b"\r\n" not in self.buf:
            chunk = self.sock.recv(8192)
            if not chunk:
                raise RuntimeError("connection closed")
            self.buf += chunk
        idx = self.buf.index(b"\r\n")
        line = self.buf[:idx]
        self.buf = self.buf[idx + 2:]
        return line

    def _read_exact(self, n):
        self._fill(n)
        data = self.buf[:n]
        self.buf = self.buf[n:]
        return data

    def read(self):
        self._fill(1)
        typ = self._read_exact(1)
        if typ == b"+":
            return ("simple", self._read_line().decode("utf-8", "replace"))
        if typ == b"-":
            return ("error", self._read_line().decode("utf-8", "replace"))
        if typ == b":":
            return ("integer", int(self._read_line()))
        if typ == b"$":
            n = int(self._read_line())
            if n < 0:
                return ("null", None)
            data = self._read_exact(n + 2)
            return ("bulk", data[:n].decode("utf-8", "replace"))
        if typ == b"*":
            n = int(self._read_line())
            if n < 0:
                return ("null", None)
            return ("array", [self.read() for _ in range(n)])
        raise RuntimeError(f"unknown RESP type: {typ!r}")


def flatten(resp):
    typ, val = resp
    if typ == "array":
        out = []
        for item in val:
            out.extend(flatten(item))
        return out
    if val is None:
        return []
    return [str(val)]


def format_resp(resp):
    return " | ".join(flatten(resp)) or repr(resp)


def require(cond, msg):
    if not cond:
        raise AssertionError(msg)


def scores(resp):
    values = []
    for item in flatten(resp):
        marker = "score:"
        if marker in item:
            tail = item.split(marker, 1)[1].split("|", 1)[0].split()[0].strip()
            try:
                values.append(float(tail))
            except ValueError:
                pass
    return values


samples = [
    ("怎么重置密码", "可以在登录页点击忘记密码", "忘记密码怎么办"),
    ("如何修改手机号", "可以在账号安全页面更换绑定手机", "怎么更换绑定手机"),
    ("如何开具发票", "可以在订单详情页申请发票", "发票在哪里申请"),
]
negative_query = "今天北京天气怎么样"
thresholds = ["0.0", "0.70", "0.75", "0.80", "0.85"]

client = RespClient(host, port)
try:
    print(f"[semantic-onnx] target={host}:{port} scope={scope}")

    for question, _, _ in samples:
        client.send(["VDEL", question, scope])
    client.send(["VINDEX", "REBUILD"])

    info = client.send(["VINFO"])
    info_text = format_resp(info)
    print(f"[semantic-onnx] VINFO {info_text}")
    require("dimension:512" in info_text, "expected VINFO to contain dimension:512")

    for question, answer, _ in samples:
        resp = client.send(["VSET", question, answer, scope])
        print(f"[semantic-onnx] VSET {question}: {format_resp(resp)}")
        require(resp == ("simple", "OK"), f"VSET failed for {question}: {resp}")

    first = client.send(["VGET", samples[0][0], scope])
    print(f"[semantic-onnx] VGET {samples[0][0]}: {format_resp(first)}")
    require(first == ("bulk", samples[0][1]), "VGET did not return expected answer")

    all_positive_scores = []
    for _, _, query in samples:
        print(f"\n[semantic-onnx] query={query}")
        for threshold in thresholds:
            resp = client.send(["VSEARCH", query, scope, "5", threshold])
            found_scores = scores(resp)
            all_positive_scores.extend(found_scores)
            score_text = ",".join(f"{s:.4f}" for s in found_scores) or "-"
            print(f"  threshold={threshold} scores={score_text} result={format_resp(resp)}")

    print(f"\n[semantic-onnx] negative_query={negative_query}")
    for threshold in thresholds:
        resp = client.send(["VSEARCH", negative_query, scope, "5", threshold])
        found_scores = scores(resp)
        score_text = ",".join(f"{s:.4f}" for s in found_scores) or "-"
        print(f"  threshold={threshold} scores={score_text} result={format_resp(resp)}")

    require(all_positive_scores, "positive semantic queries returned no score at any threshold")
    print("\n[semantic-onnx] done")
finally:
    client.close()
PY
