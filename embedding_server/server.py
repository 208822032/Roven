#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import argparse
import logging
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

    这个类对应 embedding.proto 里的：

        service EmbeddingService {
          rpc Embed(EmbedRequest) returns (EmbedResponse);
          rpc Health(HealthRequest) returns (HealthResponse);
        }

    Python 端通过继承 EmbeddingServiceServicer 来实现服务端逻辑。
    """

    def __init__(self, model_name: str, normalize: bool):
        self.model_name = model_name
        self.normalize = normalize
        self.provider = "sentence-transformers"

        logging.info("loading embedding model: %s", model_name)

        #
        # 这里会加载 sentence-transformers 模型。
        #
        # 第一次运行时，如果本机没有缓存模型，会自动下载。
        # 如果网络不方便，可以把 model_name 换成本地模型目录。
        #
        self.model = SentenceTransformer(model_name)

        #
        # 用一条测试文本推理一次，拿到模型输出维度。
        #
        probe = self.model.encode(
            ["health check"],
            normalize_embeddings=normalize,
            convert_to_numpy=True,
        )

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
            vectors = self.model.encode(
                texts,
                normalize_embeddings=normalize,
                convert_to_numpy=True,
            )
        except Exception as exc:
            logging.exception("embedding failed")
            context.set_code(grpc.StatusCode.INTERNAL)
            context.set_details(str(exc))
            return embedding_pb2.EmbedResponse()

        vectors = np.asarray(vectors, dtype=np.float32)

        response = embedding_pb2.EmbedResponse()
        response.model = self.model_name
        response.dimension = int(vectors.shape[1])

        for row in vectors:
            item = response.embeddings.add()
            item.dimension = int(vectors.shape[1])
            item.values.extend(row.astype(np.float32).tolist())

        return response

    def Health(self, request, context):
        """
        健康检查接口。

        主要给 KVLite 启动时或运行时检查 embedding server 是否可用。
        """

        return embedding_pb2.HealthResponse(
            ok=True,
            model=self.model_name,
            dimension=self.dimension,
            provider=self.provider,
        )


def serve(host: str, port: int, model: str, normalize: bool, workers: int):
    """
    启动 gRPC server。

    grpc.server:
        创建一个 gRPC 服务端。

    ThreadPoolExecutor:
        用线程池处理并发请求。

    add_EmbeddingServiceServicer_to_server:
        把我们实现的 EmbeddingService 注册到 gRPC server。
    """

    logging.basicConfig(
        level=logging.INFO,
        format="[%(asctime)s] [%(levelname)s] %(message)s",
    )

    grpc_server = grpc.server(futures.ThreadPoolExecutor(max_workers=workers))

    service = EmbeddingService(
        model_name=model,
        normalize=normalize,
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

    args = parser.parse_args()

    serve(
        host=args.host,
        port=args.port,
        model=args.model,
        normalize=args.normalize,
        workers=args.workers,
    )


if __name__ == "__main__":
    main()