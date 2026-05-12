#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import logging
import time
from concurrent import futures
from typing import List

import grpc
import numpy as np
from sentence_transformers import SentenceTransformer

import embedding_pb2
import embedding_pb2_grpc


class EmbeddingService(embedding_pb2_grpc.EmbeddingServiceServicer):
    """
    KVLite 嵌入向量 gRPC 服务实现。

    对应 embedding.proto:

        service EmbeddingService {
          rpc Embed(EmbedRequest) returns (EmbedResponse);
          rpc Health(HealthRequest) returns (HealthResponse);
        }

    Embed:
        批量把文本转换成 float 向量。

    Health:
        返回服务健康状态、模型名、向量维度、provider 信息。
    """

    def __init__(self, model_name: str, normalize: bool, log_requests: bool):
        self.model_name = model_name
        self.normalize = normalize
        self.provider = "sentence-transformers"
        self.log_requests = log_requests

        logging.info("loading embedding model: %s", model_name)

        self.model = SentenceTransformer(model_name)

        probe = self.model.encode(
            ["health check"],
            normalize_embeddings=normalize,
            convert_to_numpy=True,
            show_progress_bar=False,
        )

        probe = np.asarray(probe, dtype=np.float32)

        if probe.ndim != 2 or probe.shape[0] <= 0 or probe.shape[1] <= 0:
            raise RuntimeError("invalid embedding probe shape")

        self.dimension = int(probe.shape[1])

        logging.info(
            "embedding service ready: model=%s dimension=%d normalize=%s",
            self.model_name,
            self.dimension,
            self.normalize,
        )

    def Embed(self, request, context):
        """
        批量文本向量化接口。

        request.texts:
            repeated string，待向量化文本列表。

        返回：
            EmbedResponse，其中 embeddings 与 request.texts 一一对应。
        """

        total_start = time.perf_counter()

        texts: List[str] = list(request.texts)

        if not texts:
            context.set_code(grpc.StatusCode.INVALID_ARGUMENT)
            context.set_details("texts must not be empty")
            return embedding_pb2.EmbedResponse()

        for text in texts:
            if text is None or len(text) == 0:
                context.set_code(grpc.StatusCode.INVALID_ARGUMENT)
                context.set_details("text must not be empty")
                return embedding_pb2.EmbedResponse()

        normalize = self.normalize
        if request.normalize:
            normalize = True

        try:
            encode_start = time.perf_counter()
            vectors = self.model.encode(
                texts,
                normalize_embeddings=normalize,
                convert_to_numpy=True,
                show_progress_bar=False,
            )
            encode_ms = (time.perf_counter() - encode_start) * 1000.0
        except Exception as exc:
            logging.exception("embedding failed")
            context.set_code(grpc.StatusCode.INTERNAL)
            context.set_details(str(exc))
            return embedding_pb2.EmbedResponse()

        convert_start = time.perf_counter()
        vectors = np.asarray(vectors, dtype=np.float32)
        convert_ms = (time.perf_counter() - convert_start) * 1000.0

        if vectors.ndim != 2 or vectors.shape[0] != len(texts) or vectors.shape[1] <= 0:
            context.set_code(grpc.StatusCode.INTERNAL)
            context.set_details("invalid embedding shape")
            logging.error(
                "invalid embedding shape count=%d shape=%s",
                len(texts),
                getattr(vectors, "shape", None),
            )
            return embedding_pb2.EmbedResponse()

        response_start = time.perf_counter()

        dimension = int(vectors.shape[1])

        response = embedding_pb2.EmbedResponse()
        response.model = self.model_name
        response.dimension = dimension

        for row in vectors:
            item = response.embeddings.add()
            item.dimension = dimension
            item.values.extend(row.astype(np.float32).tolist())

        response_ms = (time.perf_counter() - response_start) * 1000.0
        total_ms = (time.perf_counter() - total_start) * 1000.0
        avg_ms = total_ms / max(len(texts), 1)

        if self.log_requests:
            logging.info(
                "embed request count=%d dimension=%d encode_ms=%.2f convert_ms=%.2f response_ms=%.2f total_ms=%.2f avg_ms=%.2f",
                len(texts),
                dimension,
                encode_ms,
                convert_ms,
                response_ms,
                total_ms,
                avg_ms,
            )

        return response

    def Health(self, request, context):
        """
        健康检查接口。

        KVLite 启动时通过 Health 获取：
        1. 服务是否可用；
        2. 模型名称；
        3. 向量维度；
        4. provider 类型。
        """

        return embedding_pb2.HealthResponse(
            ok=True,
            model=self.model_name,
            dimension=self.dimension,
            provider=self.provider,
        )


def serve(host: str, port: int, model: str, normalize: bool, workers: int, log_requests: bool):
    """
    启动 gRPC server。

    workers:
        gRPC worker 线程数。

    注意：
        sentence-transformers / PyTorch 自身可能也会使用多线程。
        workers 不一定越大越快，建议从 1、2、4 逐步压测。
    """

    logging.basicConfig(
        level=logging.INFO,
        format="[%(asctime)s] [%(levelname)s] %(message)s",
    )

    if workers <= 0:
        workers = 1

    logging.info(
        "starting embedding server host=%s port=%d model=%s normalize=%s workers=%d",
        host,
        port,
        model,
        normalize,
        workers,
    )

    grpc_server = grpc.server(
        futures.ThreadPoolExecutor(max_workers=workers)
    )

    service = EmbeddingService(
        model_name=model,
        normalize=normalize,
        log_requests=log_requests,
    )

    embedding_pb2_grpc.add_EmbeddingServiceServicer_to_server(
        service,
        grpc_server,
    )

    addr = f"{host}:{port}"
    grpc_server.add_insecure_port(addr)
    grpc_server.start()

    logging.info("embedding gRPC server listening on %s", addr)

    try:
        grpc_server.wait_for_termination()
    except KeyboardInterrupt:
        logging.info("shutting down embedding server")
        grpc_server.stop(grace=2)


def main():
    parser = argparse.ArgumentParser(
        description="KVLite embedding gRPC server"
    )

    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="listen host",
    )

    parser.add_argument(
        "--port",
        type=int,
        default=50051,
        help="listen port",
    )

    parser.add_argument(
        "--model",
        default="BAAI/bge-small-zh-v1.5",
        help="sentence-transformers model name or local path",
    )

    parser.add_argument(
        "--normalize",
        action="store_true",
        default=True,
        help="normalize returned embeddings",
    )

    parser.add_argument(
        "--workers",
        type=int,
        default=4,
        help="gRPC worker thread count",
    )

    parser.add_argument(
        "--log-requests",
        action="store_true",
        default=False,
        help="log every embedding request latency",
    )

    args = parser.parse_args()

    serve(
            host=args.host,
            port=args.port,
            model=args.model,
            normalize=args.normalize,
            workers=args.workers,
            log_requests=args.log_requests,
        )


if __name__ == "__main__":
    main()