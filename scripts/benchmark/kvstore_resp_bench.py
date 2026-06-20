#!/usr/bin/env python3
import argparse
import asyncio
import json
import random
import shlex
import time
from typing import Any, Dict, List, Optional, Tuple


def encode_resp(tokens: List[str]) -> bytes:
    parts = [f"*{len(tokens)}\r\n".encode()]
    for token in tokens:
        b = token.encode()
        parts.append(f"${len(b)}\r\n".encode())
        parts.append(b)
        parts.append(b"\r\n")
    return b"".join(parts)


async def read_line(reader: asyncio.StreamReader) -> bytes:
    data = await reader.readuntil(b"\r\n")
    return data[:-2]


async def read_resp(reader: asyncio.StreamReader) -> Tuple[str, Any]:
    prefix = await reader.readexactly(1)
    t = prefix.decode(errors="ignore")

    if t == "+":
        return "simple", (await read_line(reader)).decode(errors="ignore")
    if t == "-":
        return "error", (await read_line(reader)).decode(errors="ignore")
    if t == ":":
        return "integer", int((await read_line(reader)).decode(errors="ignore"))
    if t == "$":
        n = int((await read_line(reader)).decode(errors="ignore"))
        if n == -1:
            return "bulk", None
        payload = await reader.readexactly(n + 2)
        return "bulk", payload[:-2].decode(errors="ignore")
    if t == "*":
        n = int((await read_line(reader)).decode(errors="ignore"))
        if n == -1:
            return "array", None
        arr = []
        for _ in range(n):
            arr.append(await read_resp(reader))
        return "array", arr

    raise ValueError(f"Unknown RESP type byte: {prefix!r}")


def percentile(values: List[float], p: float) -> Optional[float]:
    if not values:
        return None
    if len(values) == 1:
        return values[0]
    values_sorted = sorted(values)
    pos = (p / 100.0) * (len(values_sorted) - 1)
    low = int(pos)
    high = min(low + 1, len(values_sorted) - 1)
    if low == high:
        return values_sorted[low]
    frac = pos - low
    return values_sorted[low] * (1 - frac) + values_sorted[high] * frac


def resolve_template_tokens(
    token_tpl: List[str],
    keyspace: int,
    value_blob: str,
    start_key: str,
    end_key: str,
    limit: int,
    rnd: random.Random,
) -> List[str]:
    rid = rnd.randrange(max(1, keyspace))
    key = f"key:{rid:08d}"

    out = []
    for t in token_tpl:
        v = t.replace("__rand_int__", str(rid))
        v = v.replace("__key__", key)
        v = v.replace("__value__", value_blob)
        v = v.replace("__start_key__", start_key)
        v = v.replace("__end_key__", end_key)
        v = v.replace("__limit__", str(limit))
        out.append(v)
    return out


async def worker(
    worker_id: int,
    host: str,
    port: int,
    requests: int,
    pipeline: int,
    token_tpl: List[str],
    keyspace: int,
    value_blob: str,
    start_key: str,
    end_key: str,
    limit: int,
    timeout: float,
    validate_ratio: float,
    expect_type: str,
    seed: int,
) -> Dict[str, Any]:
    latencies_ms: List[float] = []
    completed = 0
    errors = 0

    rnd = random.Random(seed + worker_id)

    reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout=timeout)

    try:
        sent = 0
        while sent < requests:
            batch = min(pipeline, requests - sent)
            payloads = []
            send_ts = []

            for _ in range(batch):
                tokens = resolve_template_tokens(
                    token_tpl=token_tpl,
                    keyspace=keyspace,
                    value_blob=value_blob,
                    start_key=start_key,
                    end_key=end_key,
                    limit=limit,
                    rnd=rnd,
                )
                payloads.append(encode_resp(tokens))
                send_ts.append(time.perf_counter())

            writer.write(b"".join(payloads))
            await writer.drain()

            for i in range(batch):
                try:
                    rtype, _ = await asyncio.wait_for(read_resp(reader), timeout=timeout)
                    now = time.perf_counter()
                    latencies_ms.append((now - send_ts[i]) * 1000.0)

                    if validate_ratio > 0 and rnd.random() < validate_ratio:
                        if expect_type != "any" and rtype != expect_type:
                            errors += 1
                    completed += 1
                except Exception:
                    errors += 1

            sent += batch
    finally:
        writer.close()
        await writer.wait_closed()

    return {
        "completed": completed,
        "errors": errors,
        "latencies_ms": latencies_ms,
    }


async def main_async(args: argparse.Namespace) -> Dict[str, Any]:
    token_tpl = shlex.split(args.command)
    if not token_tpl:
        raise ValueError("Empty command template")

    value_blob = "v" * args.value_size

    per_client = args.requests // args.clients
    rem = args.requests % args.clients

    start = time.perf_counter()
    tasks = []
    for i in range(args.clients):
        req_i = per_client + (1 if i < rem else 0)
        if req_i <= 0:
            continue
        tasks.append(
            worker(
                worker_id=i,
                host=args.host,
                port=args.port,
                requests=req_i,
                pipeline=max(1, args.pipeline),
                token_tpl=token_tpl,
                keyspace=max(1, args.keyspace),
                value_blob=value_blob,
                start_key=args.start_key,
                end_key=args.end_key,
                limit=args.limit,
                timeout=args.timeout,
                validate_ratio=max(0.0, min(1.0, args.validate_ratio)),
                expect_type=args.expect_type,
                seed=args.seed,
            )
        )

    results = await asyncio.gather(*tasks, return_exceptions=True)
    elapsed = time.perf_counter() - start

    all_lat = []
    completed = 0
    errors = 0
    worker_failures = 0

    for r in results:
        if isinstance(r, Exception):
            worker_failures += 1
            continue
        completed += r["completed"]
        errors += r["errors"]
        all_lat.extend(r["latencies_ms"])

    avg = (sum(all_lat) / len(all_lat)) if all_lat else None

    metrics = {
        "system": args.system,
        "ds": args.ds,
        "op": args.op,
        "host": args.host,
        "port": args.port,
        "clients": args.clients,
        "pipeline": args.pipeline,
        "requests": args.requests,
        "value_size": args.value_size,
        "keyspace": args.keyspace,
        "completed": completed,
        "errors": errors,
        "worker_failures": worker_failures,
        "elapsed_sec": elapsed,
        "qps": (completed / elapsed) if elapsed > 0 else 0.0,
        "avg_ms": avg,
        "p50_ms": percentile(all_lat, 50),
        "p95_ms": percentile(all_lat, 95),
        "p99_ms": percentile(all_lat, 99),
    }
    return metrics


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Generic RESP benchmark client with concurrency + pipeline")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, required=True)
    p.add_argument("--clients", type=int, default=50)
    p.add_argument("--requests", type=int, default=100000)
    p.add_argument("--pipeline", type=int, default=1)
    p.add_argument("--command", required=True, help="Command template, e.g. 'RRANGE __start_key__ __end_key__ LIMIT __limit__'")
    p.add_argument("--value-size", type=int, default=32)
    p.add_argument("--keyspace", type=int, default=100000)
    p.add_argument("--start-key", default="key:00000000")
    p.add_argument("--end-key", default="key:99999999")
    p.add_argument("--limit", type=int, default=100)
    p.add_argument("--timeout", type=float, default=10.0)
    p.add_argument("--validate-ratio", type=float, default=0.0, help="Sample ratio [0,1] for response type validation")
    p.add_argument("--expect-type", default="any", choices=["any", "simple", "bulk", "integer", "error", "array"])
    p.add_argument("--seed", type=int, default=20260301)
    p.add_argument("--system", default="kvstore")
    p.add_argument("--ds", default="array")
    p.add_argument("--op", default="range")
    return p.parse_args()


def main() -> None:
    args = parse_args()
    metrics = asyncio.run(main_async(args))
    print(json.dumps(metrics, ensure_ascii=False))


if __name__ == "__main__":
    main()
