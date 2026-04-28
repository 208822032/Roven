#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import sys

import grpc

import embedding_pb2
import embedding_pb2_grpc


def main():
    parser = argparse.ArgumentParser(
        description="KVLite embedding gRPC client test"
    )

    parser.add_argument(
        "--addr",
        default="127.0.0.1:50051",
        help="embedding server address",
    )

    parser.add_argument(
        "--timeout",
        type=float,
        default=3.0,
        help="gRPC request timeout seconds",
    )

    parser.add_argument(
        "--texts",
        nargs="*",
        default=["你好 世界", "hello world", "向量检索测试"],
        help="texts to embed",
    )

    args = parser.parse_args()

    #
    # 创建 gRPC channel。
    #
    # insecure_channel:
    #   不使用 TLS，适合本地开发和内网测试。
    #
    channel = grpc.insecure_channel(args.addr)

    #
    # 通过生成出来的 Stub 调用远程服务。
    #
    stub = embedding_pb2_grpc.EmbeddingServiceStub(channel)

    #
    # 先做健康检查。
    #
    health = stub.Health(
        embedding_pb2.HealthRequest(),
        timeout=args.timeout,
    )

    print("health:")
    print(f"  ok={health.ok}")
    print(f"  provider={health.provider}")
    print(f"  model={health.model}")
    print(f"  dimension={health.dimension}")

    #
    # 再测试批量 embedding。
    #
    response = stub.Embed(
        embedding_pb2.EmbedRequest(
            texts=args.texts,
            model=health.model,
            normalize=True,
        ),
        timeout=args.timeout,
    )

    print("embed:")
    print(f"  model={response.model}")
    print(f"  dimension={response.dimension}")
    print(f"  count={len(response.embeddings)}")

    if len(response.embeddings) != len(args.texts):
        print("embedding count mismatch", file=sys.stderr)
        return 1

    for idx, emb in enumerate(response.embeddings):
        print(
            f"  [{idx}] text={args.texts[idx]!r} "
            f"dimension={emb.dimension} "
            f"first5={list(emb.values[:5])}"
        )

        if emb.dimension <= 0:
            print("invalid dimension", file=sys.stderr)
            return 1

        if len(emb.values) != emb.dimension:
            print("values length mismatch", file=sys.stderr)
            return 1

    print("client test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())