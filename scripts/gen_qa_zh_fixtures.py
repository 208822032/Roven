#!/usr/bin/env python3
from pathlib import Path


TOPICS = [
    ("Redis AOF", "Redis AOF 是追加日志持久化机制，通过顺序写记录写命令。"),
    ("Redis RDB", "Redis RDB 通过周期性快照把内存数据序列化到磁盘。"),
    ("TTL 过期", "TTL 机制通过过期时间控制键的生命周期。"),
    ("KV 存储", "KV 存储以键值对为核心抽象，强调简单读写路径。"),
    ("跳表", "跳表通过多层索引在有序集合中提供接近平衡树的查找性能。"),
    ("HNSW", "HNSW 通过分层近邻图在高维空间中近似搜索最近邻。"),
    ("efSearch", "efSearch 控制 HNSW 查询阶段保留的候选宽度。"),
    ("efConstruction", "efConstruction 控制 HNSW 建图阶段的候选扩展宽度。"),
    ("M 参数", "HNSW 的 M 参数决定每个节点允许保留的邻居上限。"),
    ("向量数据库", "向量数据库通常基于 embedding 和 ANN 索引完成相似检索。"),
    ("embedding", "embedding 把文本映射到连续向量空间以表达语义相近性。"),
    ("ONNX Runtime", "ONNX Runtime 是执行 ONNX 模型的高性能推理运行时。"),
    ("BGE 模型", "bge-small-zh-v1.5 是面向中文语义检索的轻量 embedding 模型。"),
    ("L2 归一化", "L2 归一化让向量长度变为 1，便于用内积近似 cosine。"),
    ("Inner Product", "归一化后的 inner product 可以直接表示 cosine 相似度。"),
    ("Flat 检索", "Flat 检索会遍历全部向量，结果精确但规模增大后成本线性上升。"),
    ("Faiss Flat", "Faiss IndexFlatIP 提供精确内积检索，常用作 ground truth。"),
    ("Faiss HNSW", "Faiss IndexHNSWFlat 提供 ANN 检索并支持调节 efSearch。"),
    ("倒排索引", "倒排索引适合关键词检索，不直接解决向量近邻问题。"),
    ("语义缓存", "语义缓存通过查询 embedding 命中相近问题，减少重复计算。"),
    ("批量 embedding", "批量 embedding 可以摊薄 tokenizer 和推理框架开销。"),
    ("embedding cache", "embedding cache 可以避免重复文本反复做推理。"),
    ("query cache", "query cache 适合高重复率查询，直接复用命中结果。"),
    ("线程过订阅", "线程过订阅会导致上下文切换上升并拖慢整体吞吐。"),
    ("intra_op 线程", "ONNX Runtime intra_op 线程控制单次算子内部并行度。"),
    ("inter_op 线程", "ONNX Runtime inter_op 线程控制图中算子级并行调度。"),
    ("reactor 模型", "reactor 模型通过事件循环分发 I/O 就绪事件。"),
    ("executor", "executor 负责把解析后的命令分发到实际执行上下文。"),
    ("AOF rewrite", "AOF rewrite 会生成更紧凑的新日志文件以缩减体积。"),
    ("RDB 恢复", "RDB 恢复通过加载快照快速重建内存数据。"),
    ("RESP 协议", "RESP 是 Redis 风格文本二进制混合协议，便于客户端实现。"),
    ("record lookup", "record lookup 根据索引返回的 record_id 找到问题与答案。"),
    ("topK heap", "topK heap 用固定容量堆维护当前最优候选集合。"),
    ("SIMD 距离", "SIMD 距离计算通过向量指令提升点积或 L2 计算吞吐。"),
    ("visited bitmap", "visited bitmap 用于 HNSW 搜索时避免重复访问节点。"),
    ("memory layout", "连续内存布局通常比离散指针结构更有利于缓存命中。"),
    ("vector fast path", "vector fast path 目标是减少搜索后回表与响应构造开销。"),
    ("scope filter", "scope filter 在命中候选后按业务域过滤结果。"),
    ("近似召回", "ANN 检索需要在速度与 recall 之间做权衡。"),
    ("召回率", "recall@k 衡量 ANN 结果与精确 topK 重合的比例。"),
    ("量化模型", "量化模型可以降低推理成本，但可能带来精度损失。"),
    ("小模型", "更小的 embedding 模型通常延迟更低但语义能力可能下降。"),
    ("多线程 worker", "embedding worker 数量增加后可能提升吞吐，也可能放大锁竞争。"),
    ("共享 ORT Env", "共享 ORT Env 可以减少每线程重复创建环境的成本。"),
    ("每线程 Session", "每线程 Session 可以降低全局互斥，但会增加内存占用。"),
    ("候选回表", "候选回表负责从 record 索引读取问题、答案和作用域信息。"),
    ("归并输出", "响应构造会把候选结果转成最终的业务返回格式。"),
    ("KVLite Flat", "KVLite Flat 适合小规模数据下的精确向量检索基线。"),
    ("KVLite HNSW", "KVLite HNSW 是当前语义缓存主路径的 ANN 索引实现。"),
    ("数据规模阈值", "小规模数据下 Flat 可能比 HNSW 更稳定且实现更简单。"),
]

QUESTION_PATTERNS = [
    "什么是{topic}？",
    "{topic} 的核心作用是什么？",
    "{topic} 主要解决什么问题？",
    "{topic} 在系统里承担什么职责？",
    "为什么需要{topic}？",
    "{topic} 的典型用途是什么？",
    "{topic} 的关键点是什么？",
    "{topic} 在性能分析里为什么重要？",
    "{topic} 和语义检索有什么关系？",
    "怎样理解{topic}？",
    "{topic} 在 KVLite 场景里有什么意义？",
    "做向量检索时为什么要关注{topic}？",
    "{topic} 对延迟有什么影响？",
    "{topic} 对吞吐有什么影响？",
    "{topic} 的调优方向通常有哪些？",
    "{topic} 和 HNSW/Flat 选择有什么联系？",
    "{topic} 会不会成为瓶颈？",
    "{topic} 在中文问答检索链路里处于哪一段？",
    "分析 VSET/VSEARCH 时为什么要拆看{topic}？",
    "{topic} 的工程优化优先级怎么判断？",
]

QUERY_PATTERNS = [
    "请解释一下{topic}",
    "{topic} 是干什么用的",
    "{topic} 为什么会影响检索性能",
    "如果做中文语义检索，{topic} 该怎么理解",
]


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    fixture_dir = root / "tests" / "fixtures"
    fixture_dir.mkdir(parents=True, exist_ok=True)

    qa_path = fixture_dir / "qa_zh_1000.tsv"
    query_path = fixture_dir / "qa_zh_queries.tsv"

    qa_lines = []
    for topic, answer in TOPICS:
        for pattern in QUESTION_PATTERNS:
            qa_lines.append(f"{pattern.format(topic=topic)}\t{answer}")

    query_lines = []
    for topic, answer in TOPICS:
        for pattern in QUERY_PATTERNS:
            query_lines.append(f"{pattern.format(topic=topic)}\t{answer}")

    qa_path.write_text("\n".join(qa_lines) + "\n", encoding="utf-8")
    query_path.write_text("\n".join(query_lines) + "\n", encoding="utf-8")

    print(f"wrote {qa_path} ({len(qa_lines)} rows)")
    print(f"wrote {query_path} ({len(query_lines)} rows)")


if __name__ == "__main__":
    main()
