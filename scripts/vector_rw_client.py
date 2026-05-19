#!/usr/bin/env python3
"""
vector_rw_client.py — Minimal RESP socket client for KVLite V-family benchmark.
Supports: write (VSET), get (VGET), search (VSEARCH), del (VDEL), info (VINFO).

Each op is independent — no forced VSET before read.
Output format: key=value, parseable by shell scripts.
"""

import argparse
import socket
import sys
import time


class RespClient:
    """Buffered RESP protocol client over a TCP socket."""

    def __init__(self, host: str, port: int, timeout: float = 30.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def close(self) -> None:
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()

    # ── low-level I/O ──────────────────────────────────────────────

    def _fill(self, n: int) -> None:
        """Ensure at least n bytes are in the buffer."""
        while len(self.buf) < n:
            chunk = self.sock.recv(8192)
            if not chunk:
                raise ConnectionError("connection closed by peer")
            self.buf += chunk

    def _read_line(self) -> bytes:
        """Read until \\r\\n (exclusive), return line without CRLF."""
        while b"\r\n" not in self.buf:
            chunk = self.sock.recv(8192)
            if not chunk:
                raise ConnectionError("connection closed by peer")
            self.buf += chunk
        idx = self.buf.index(b"\r\n")
        line = self.buf[:idx]
        self.buf = self.buf[idx + 2 :]
        return line

    def _read_exact(self, n: int) -> bytes:
        self._fill(n)
        data = self.buf[:n]
        self.buf = self.buf[n:]
        return data

    # ── RESP protocol ──────────────────────────────────────────────

    @staticmethod
    def encode_cmd(tokens: list) -> bytes:
        parts = [f"*{len(tokens)}\r\n".encode()]
        for t in tokens:
            b = t.encode()
            parts.append(f"${len(b)}\r\n".encode())
            parts.append(b)
            parts.append(b"\r\n")
        return b"".join(parts)

    def send_cmd(self, tokens: list) -> None:
        raw = self.encode_cmd(tokens)
        self.sock.sendall(raw)

    def read_resp(self):
        """Read one complete RESP value. Returns (type_str, value)."""
        prefix = self._read_exact(1)
        t = prefix.decode(errors="ignore")

        if t == "+":
            return "simple", self._read_line().decode(errors="ignore")
        if t == "-":
            return "error", self._read_line().decode(errors="ignore")
        if t == ":":
            return "integer", int(self._read_line())
        if t == "$":
            nbytes = int(self._read_line())
            if nbytes == -1:
                return "null", None
            data = self._read_exact(nbytes + 2)
            return "bulk", data[:nbytes].decode(errors="ignore")
        if t == "*":
            nitems = int(self._read_line())
            if nitems == -1:
                return "null", None
            arr = [self.read_resp() for _ in range(nitems)]
            return "array", arr

        raise ValueError(f"Unknown RESP type byte: {prefix!r}")

    # ── commands ───────────────────────────────────────────────────

    def vset(self, question: str, answer: str, scope: str):
        tokens = ["VSET", question, answer]
        if scope:
            tokens.append(scope)
        self.send_cmd(tokens)
        return self.read_resp()

    def vget(self, question: str, scope: str):
        tokens = ["VGET", question]
        if scope:
            tokens.append(scope)
        self.send_cmd(tokens)
        return self.read_resp()

    def vsearch(self, query: str, scope: str, topk: int):
        tokens = ["VSEARCH", query]
        if scope:
            tokens.append(scope)
        tokens.append(str(topk))
        self.send_cmd(tokens)
        return self.read_resp()

    def vdel(self, key: str, scope: str):
        tokens = ["VDEL", key]
        if scope:
            tokens.append(scope)
        self.send_cmd(tokens)
        return self.read_resp()

    def vinfo(self):
        self.send_cmd(["VINFO"])
        return self.read_resp()


# ── statistics ─────────────────────────────────────────────────────

def percentile(values: list, p: float) -> float:
    """Linear-interpolated percentile of a pre-sorted list."""
    if not values:
        return 0.0
    n = len(values)
    if n == 1:
        return values[0]
    pos = (p / 100.0) * (n - 1)
    lo = int(pos)
    hi = min(lo + 1, n - 1)
    if lo == hi:
        return values[lo]
    frac = pos - lo
    return values[lo] * (1.0 - frac) + values[hi] * frac


def calc_stats(latencies_ms: list, elapsed_ms: float, success: int):
    """Return a dict of stat fields."""
    if not latencies_ms:
        return {
            "qps": 0.0,
            "avg_ms": 0.0,
            "p50_ms": 0.0,
            "p95_ms": 0.0,
            "p99_ms": 0.0,
        }
    sorted_lat = sorted(latencies_ms)
    sec = elapsed_ms / 1000.0
    if sec <= 0.0:
        sec = 0.001
    return {
        "qps": round(success / sec, 2),
        "avg_ms": round(sum(sorted_lat) / len(sorted_lat), 2),
        "p50_ms": round(percentile(sorted_lat, 50), 2),
        "p95_ms": round(percentile(sorted_lat, 95), 2),
        "p99_ms": round(percentile(sorted_lat, 99), 2),
    }


def print_results(op: str, count: int, success: int, failed: int,
                  elapsed_ms: float, latencies_ms: list) -> dict:
    stats = calc_stats(latencies_ms, elapsed_ms, success)
    print(f"op={op}")
    print(f"count={count}")
    print(f"success={success}")
    print(f"failed={failed}")
    print(f"elapsed_ms={elapsed_ms:.0f}")
    print(f"qps={stats['qps']:.2f}")
    print(f"avg_ms={stats['avg_ms']:.2f}")
    print(f"p50_ms={stats['p50_ms']:.2f}")
    print(f"p95_ms={stats['p95_ms']:.2f}")
    print(f"p99_ms={stats['p99_ms']:.2f}")
    return stats


# ── phases ─────────────────────────────────────────────────────────

def progress_msg(i: int, total: int, success: int, failed: int) -> None:
    pct = (i + 1) * 100 // total
    print(f"progress {i+1}/{total} ({pct}%) success={success} failed={failed}",
          flush=True)


def should_report(i: int, total: int) -> bool:
    if total <= 20:
        return True
    step = max(1, total // 10)
    return (i + 1) % step == 0 or i + 1 == total


def run_write(client: RespClient, count: int, scope: str, prefix: str,
              topk: int) -> dict:
    latencies = []
    success = 0
    failed = 0

    t_start = time.perf_counter()
    for i in range(count):
        question = f"{prefix}_question_{i:08d}"
        answer = f"{prefix}_answer_{i:08d}"

        t0 = time.perf_counter()
        rtype, rval = client.vset(question, answer, scope)
        t1 = time.perf_counter()
        latencies.append((t1 - t0) * 1000.0)

        if rtype == "simple" and rval == "OK":
            success += 1
        else:
            failed += 1
            if failed <= 3:
                print(f"VSET failed i={i}: {rtype}={rval}", file=sys.stderr)

        if should_report(i, count):
            progress_msg(i, count, success, failed)

    elapsed_ms = (time.perf_counter() - t_start) * 1000.0
    return print_results("write", count, success, failed, elapsed_ms, latencies)


def run_get(client: RespClient, count: int, scope: str, prefix: str,
            total_keys: int) -> dict:
    latencies = []
    success = 0
    failed = 0

    t_start = time.perf_counter()
    for i in range(count):
        idx = i % total_keys
        question = f"{prefix}_question_{idx:08d}"

        t0 = time.perf_counter()
        rtype, rval = client.vget(question, scope)
        t1 = time.perf_counter()
        latencies.append((t1 - t0) * 1000.0)

        if rtype == "bulk":
            success += 1
        else:
            failed += 1
            if failed <= 3:
                print(f"VGET failed i={i}: {rtype}={rval}", file=sys.stderr)

        if should_report(i, count):
            progress_msg(i, count, success, failed)

    elapsed_ms = (time.perf_counter() - t_start) * 1000.0
    return print_results("get", count, success, failed, elapsed_ms, latencies)


def run_search(client: RespClient, count: int, scope: str, prefix: str,
               topk: int, total_keys: int) -> dict:
    latencies = []
    success = 0
    failed = 0

    t_start = time.perf_counter()
    for i in range(count):
        idx = i % total_keys
        query = f"{prefix}_question_{idx:08d}"

        t0 = time.perf_counter()
        rtype, rval = client.vsearch(query, scope, topk)
        t1 = time.perf_counter()
        latencies.append((t1 - t0) * 1000.0)

        if rtype == "array":
            success += 1
        else:
            failed += 1
            if failed <= 3:
                print(f"VSEARCH failed i={i}: {rtype}={rval}", file=sys.stderr)

        if should_report(i, count):
            progress_msg(i, count, success, failed)

    elapsed_ms = (time.perf_counter() - t_start) * 1000.0
    return print_results("search", count, success, failed, elapsed_ms, latencies)


def run_del(client: RespClient, count: int, scope: str, prefix: str) -> dict:
    latencies = []
    success = 0
    failed = 0

    t_start = time.perf_counter()
    for i in range(count):
        question = f"{prefix}_question_{i:08d}"

        t0 = time.perf_counter()
        rtype, rval = client.vdel(question, scope)
        t1 = time.perf_counter()
        latencies.append((t1 - t0) * 1000.0)

        # VDEL returns :1 (integer) or +OK on success
        if rtype in ("integer", "simple"):
            success += 1
        else:
            failed += 1
            if failed <= 3:
                print(f"VDEL failed i={i}: {rtype}={rval}", file=sys.stderr)

        if should_report(i, count):
            progress_msg(i, count, success, failed)

    elapsed_ms = (time.perf_counter() - t_start) * 1000.0
    return print_results("del", count, success, failed, elapsed_ms, latencies)


def run_info(client: RespClient) -> None:
    rtype, rval = client.vinfo()
    if rtype == "array":
        # VINFO returns RESP array of bulk strings: key:value pairs
        for item in rval:
            if item[0] == "bulk":
                print(item[1])
    else:
        print(f"VINFO: {rtype}={rval}")


# ── main ───────────────────────────────────────────────────────────

def main() -> None:
    p = argparse.ArgumentParser(
        description="KVLite V-family RESP benchmark client"
    )
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=9300)
    p.add_argument("--op", required=True,
                   choices=["write", "get", "search", "del", "info"])
    p.add_argument("--count", type=int, default=100000)
    p.add_argument("--scope", default="")
    p.add_argument("--prefix", default="rw_data")
    p.add_argument("--topk", type=int, default=10)
    p.add_argument("--timeout", type=float, default=600.0)
    args = p.parse_args()

    client = RespClient(args.host, args.port, timeout=args.timeout)

    try:
        if args.op == "write":
            run_write(client, args.count, args.scope, args.prefix, args.topk)
        elif args.op == "get":
            run_get(client, args.count, args.scope, args.prefix, args.count)
        elif args.op == "search":
            run_search(client, args.count, args.scope, args.prefix,
                        args.topk, args.count)
        elif args.op == "del":
            run_del(client, args.count, args.scope, args.prefix)
        elif args.op == "info":
            run_info(client)
    finally:
        client.close()


if __name__ == "__main__":
    main()
