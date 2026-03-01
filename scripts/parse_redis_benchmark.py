#!/usr/bin/env python3
import argparse
import json
import re
from typing import Optional


def find_float(pattern: str, text: str) -> Optional[float]:
    m = re.search(pattern, text, re.MULTILINE)
    if not m:
        return None
    try:
        return float(m.group(1))
    except Exception:
        return None


def parse_latency_summary(text: str):
    # Newer format:
    # latency summary (msec):
    #      avg       min       p50       p95       p99       max
    #    0.530     0.136     0.399     1.079     3.583    40.287
    m = re.search(
        r"latency summary \(msec\):\s*\n\s*avg\s+min\s+p50\s+p95\s+p99\s+max\s*\n\s*([0-9.]+)\s+[0-9.]+\s+([0-9.]+)\s+([0-9.]+)\s+([0-9.]+)",
        text,
        re.IGNORECASE | re.MULTILINE,
    )
    if not m:
        return None
    return {
        "avg_ms": float(m.group(1)),
        "p50_ms": float(m.group(2)),
        "p95_ms": float(m.group(3)),
        "p99_ms": float(m.group(4)),
    }


def parse_percentile_lines(text: str):
    # Fallback for older output lines like:
    # 95.00% <= 1 milliseconds
    # 99.00% <= 2 milliseconds
    pts = []
    for m in re.finditer(r"([0-9]+(?:\.[0-9]+)?)%\s*<=\s*([0-9]+(?:\.[0-9]+)?)\s*(?:milliseconds|ms)", text, re.IGNORECASE):
        pct = float(m.group(1))
        lat = float(m.group(2))
        pts.append((pct, lat))
    pts.sort(key=lambda x: x[0])

    def pick(target: float) -> Optional[float]:
        for pct, lat in pts:
            if pct >= target:
                return lat
        return pts[-1][1] if pts else None

    return {
        "avg_ms": None,
        "p50_ms": pick(50.0),
        "p95_ms": pick(95.0),
        "p99_ms": pick(99.0),
    }


def main() -> None:
    ap = argparse.ArgumentParser(description="Parse redis-benchmark stdout into JSON metrics")
    ap.add_argument("--file", required=True)
    args = ap.parse_args()

    with open(args.file, "r", encoding="utf-8", errors="ignore") as f:
        text = f.read()

    qps = find_float(r"throughput summary:\s*([0-9.]+)\s*requests per second", text)
    if qps is None:
        qps = find_float(r"([0-9.]+)\s*requests per second", text)

    lat = parse_latency_summary(text)
    if lat is None:
        lat = parse_percentile_lines(text)

    out = {
        "qps": qps,
        "avg_ms": lat["avg_ms"],
        "p50_ms": lat["p50_ms"],
        "p95_ms": lat["p95_ms"],
        "p99_ms": lat["p99_ms"],
    }
    print(json.dumps(out, ensure_ascii=False))


if __name__ == "__main__":
    main()
