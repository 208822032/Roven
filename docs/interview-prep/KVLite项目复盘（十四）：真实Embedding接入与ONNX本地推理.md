# 第十四章 真实 Embedding 接入与 ONNX 本地推理：从 hash 占位到可运行推理闭环

## 14.1 上一阶段为什么还不能叫真实语义缓存

第九章做完以后，KVLite 已经有了 VSET/VGET/VSEARCH/VDEL/VINFO/VINDEX REBUILD，也有 flat 和 HNSW 两套索引。那一阶段解决的是：传统 KV 只能精确 key 命中，AI Chat 场景需要用问题文本去召回语义相近的历史问答。

但第九章当时还有一个很重要的边界：embedding 仍然是内置确定性 hash。

也就是说，链路上已经可以做到：

```text
文本问题
→ 生成一段 float embedding
→ 写入 vector record
→ 插入 flat / HNSW index
→ VSEARCH 查询相似向量
→ 返回 answer 和 score
```

但是这个 embedding 本身不来自真实语义模型。hash provider 的价值是验证工程链路，而不是证明语义质量。第十一章压测里也明确把 hash embedding 的边界写了出来：它可以验证索引流程和结果一致性，但不能代表真实语义质量。

所以第十四章要解决的问题不是“再加一个模型配置”这么简单，而是要回答几件更底层的问题：

```text
真实模型怎么接进 C 项目？
模型依赖是否要进入默认构建？
provider 输出的向量如何和 HNSW 的 score 语义对齐？
推理失败、维度不匹配、模型加载失败时怎么清理资源？
测试到底证明了本地推理闭环，还是只证明了命令能返回？
```

这类问题同时属于正确性、依赖边界、生命周期、性能可观测性和测试边界。它不是第九章的重复，而是把“语义缓存”里最不真实的 hash 占位替换成可运行的本地推理闭环。

本章对应的提交范围是：

```text
9c410c6^..b43ec18
```

核心提交不是“一次提交一篇复盘”，而是一组连续提交共同完成同一个工程问题：

```text
9c410c6  统一 provider 输出后的 L2 归一化、动态维度和 threshold 语义
3a737dd  增加 opaque C ABI 和默认 hash mock runtime
c99679c  可选 ONNX 构建和 session 加载，但 encode 明确未实现
a5d669c  最小 tokenizer、Tensor、OrtRun、模型/setup/config
f7c74cb  semantic smoke 和 benchmark semantic mode
b43ec18  vocab hash、ORT 参数和分段 profiling
```

从工程问题看，它的真实顺序是：

```text
hash provider 的 score 语义不够稳定
→ 先统一 L2 normalize + inner product
→ 用 C ABI 隔离模型运行时
→ 默认构建仍然保留无依赖 mock
→ ONNX 先做到能加载 session
→ 再补 tokenizer / tensor / OrtRun / output 校验
→ 增加 smoke、benchmark 入口和 profiling 观察点
```

## 14.2 一次真实的转折：远程 gRPC 接入为什么被移除

在进入这组提交之前，Git 历史里有一个很重要的转折：`3ac0c5e` 删除了之前的 gRPC embedding provider。

这个提交的 diff 很明确：删除了 `embedding_server/` 下的 Python gRPC 服务、删除了 `src/vector_grpc/kvs_embedding_grpc_client.cc` 这类 C++ bridge、删除了 `include/kvs_embedding_grpc_client.h`，也从 Makefile 和配置里移除了 protobuf / grpc++ 相关构建依赖。

这件事不能被讲成“gRPC 性能很差，所以我测出来后删掉了”。当前能从仓库证明的是：远程 gRPC 接入确实被撤回，系统回到了单进程内置向量检索和 hash provider；但这个阶段没有保留 gRPC 延迟、吞吐或故障率结果文件。

当时真正的工程取舍更像这样：

```text
远程 gRPC provider
优点：模型服务可以独立部署，语言和进程边界更自由。
代价：引入 Python 服务、protobuf 生成、C++ client bridge、RPC 失败语义、部署复杂度。

本地 C ABI / ONNX provider
优点：主进程内直接调用，默认构建可以不带模型依赖，C 模块边界更清楚。
代价：模型内存和 CPU 会和 KVLite 进程竞争，推理生命周期要由 C 代码自己兜底。
```

`3ac0c5e` 还带有 `Co-Authored-By: Claude Opus 4.7`。这个事实只能说明该提交有 Agent 协作痕迹，不能反推出本章候选提交里每一行代码是谁写的，也不能把 commit message 当成设计依据。面试时更稳妥的讲法是：我能解释为什么撤掉远程依赖、为什么保留默认无依赖构建、为什么后面选择 C ABI，而不是声称“某段实现完全由我或 Agent 写成”。

## 14.3 先统一数学语义：L2 normalize + inner product

真实 provider 接入前，必须先把 score 语义统一掉。

第九章的语义缓存已经支持 HNSW 和 score threshold，但如果不同 provider 输出的向量尺度不一致，threshold 就没有可比性。比如一个 provider 输出模长很大的向量，inner product 会天然偏大；另一个 provider 输出模长很小，分数会偏小。这样 `VSEARCH threshold=0.75` 到底代表什么就不稳定。

`9c410c6` 做的关键事情是把不变量放到 adapter 层：

```text
provider raw output
→ kvs_vector_embedding_l2_normalize()
→ HNSW inner product
→ cosine-like score
→ VSEARCH threshold 过滤
```

相关代码在 `src/vector/kvs_vector_embedding.c`：

- `kvs_vector_embedding_l2_normalize()` 负责单条向量归一化。
- `kvs_vector_embedding_l2_normalize_batch()` 负责批量入口后的统一归一化。
- `kvs_vector_embedding_generate()` 在 hash 或 local_c provider 生成后统一调用 L2 normalize。
- `kvs_vector_embedding_generate_batch()` 在批量生成后统一调用 batch normalize。

这一步的重点不是“用了 L2 公式”，而是把归一化从 provider 内部收回到向量模块。这样后续无论 provider 是 hash、mock C runtime 还是 ONNX runtime，进入 HNSW 的向量都满足同一个语义：非零向量和自己的 inner product 接近 1，normalized inner product 可以按 cosine-like score 理解。

这也解释了为什么 threshold 语义要跟这一阶段一起修。此前已有文档 `docs/shutdown_segfault_fix_report.md` 记录过 `VSEARCH-threshold-keep-exact` 这类测试问题。`tests/vector_testcase.c` 后续覆盖了默认 threshold、显式 `0.0`、`0.9`、`1.1` 等路径，证明阈值过滤不应该因为 provider 尺度变化而变成随机行为。

这里的取舍是：

```text
把 normalize 放在 provider 内部
→ provider 可以自行决定输出语义，但 threshold 跨 provider 不稳定。

把 normalize 放在 adapter 层
→ 每个 provider 少一点自由度，但 VSEARCH score 和 threshold 更可解释。
```

我选择后者，因为 KVLite 的 VSEARCH 是用户可见命令，score 语义必须比 provider 内部实现更稳定。

## 14.4 用 C ABI 把模型运行时隔离在向量模块之外

`3a737dd` 引入了 `include/embedding_c_api.h`，这一步是本章的边界设计。

当时的 C ABI 很小，核心就是五个接口：

```c
embedding_create
embedding_encode
embedding_dimension
embedding_last_error
embedding_destroy
```

`EmbeddingHandle` 是 opaque pointer，调用方不知道里面是 hash mock、ONNX Runtime session，还是别的模型运行时。`embedding_encode()` 的 output buffer 由调用方传入，维度由 `embedding_dimension()` 返回，错误通过 `EmbeddingErrorCode` 和 `embedding_last_error()` 暴露。

这样做有几个实际好处。

第一，KVLite 主体仍然是 C 项目。向量模块只看到 C ABI，不直接依赖 ONNX Runtime 的类型，也不把 C++ bridge 或 Python 服务带进主链路。

第二，默认构建仍然轻。`Makefile` 在没有 `ENABLE_ONNX_EMBEDDING=1` 时链接的是 `src/embedding/embedding_runtime.c`，它只是确定性 hash mock runtime。只有显式 `make embedding` 或设置 `ENABLE_ONNX_EMBEDDING=1` 时，才链接 `src/embedding/embedding_runtime_onnx.c` 和 ONNX Runtime。

第三，生命周期边界更清楚。`kvs_vector_embedding_provider_init()` 负责构造 `EmbeddingConfig`，调用 `embedding_create()`，再用 `embedding_dimension()` 校验 runtime 返回的维度；失败时销毁 handle 并返回错误。`kvs_vector_embedding_provider_destroy()` 只需要调用 `embedding_destroy()`，不需要知道 ORT session、vocab、memory info 的内部释放顺序。

这一步也避免了两个替代方案的问题：

```text
直接在 src/vector/ 里写 ONNX Runtime 调用
→ 向量模块会被 ORT 类型、session options、tensor 创建细节污染。

重新引入 C++ / gRPC bridge
→ 又回到 toolchain、RPC、协议生成和进程边界问题。
```

所以 C ABI 的价值不是接口数量少，而是它把“文本如何变成 embedding”从“embedding 如何进入 KVLite record / HNSW index”里拆开了。

## 14.5 第一版 ONNX 只会加载 Session，还不会推理

`c99679c` 是本章里最应该诚实讲的一次中间态。

这个提交已经加入了可选 ONNX 构建，也能创建 ONNX Runtime env、session options、session 和 memory info。也就是说，它开始验证这些问题：

```text
ONNX Runtime 头文件和动态库能不能被找到？
模型路径能不能加载？
session 创建失败时能不能报错并清理？
默认构建和 ONNX 构建能不能分开？
```

但它还不能生成真实 embedding。

`src/embedding/embedding_runtime_onnx.c` 在这个提交里明确写着：

```text
ONNX tokenizer/inference is not implemented yet
```

然后 `embedding_encode()` 返回 `EMBEDDING_ERR_INFERENCE_FAILED`。

这说明当时的状态只能叫“ONNX session 加载闭环”，不能叫“真实 embedding 接入完成”。如果面试里把这一段说成“我一上来就完成了 ONNX 推理”，就是把计划和实现混在一起了。

这个中间态其实很有工程价值。它把风险拆开了：先确认依赖、构建、加载、销毁，再补 tokenizer、tensor 和 OrtRun。C 项目接入外部 runtime 时，如果一次性把所有逻辑都塞进去，失败时很难判断是动态库、模型文件、输入 shape，还是输出解析的问题。

## 14.6 从文本到 512 维输出的最终调用链

真正把文本推理闭环补齐的是 `a5d669c`，后续 `b43ec18` 又补了 vocab hash、ORT 参数和 profiling。

当时的 VSET 路径可以这样看：

```text
cmd write lock
→ handle_vset_command
→ kvs_vector_set
→ kvs_vector_record_set_fields
→ kvs_vector_embedding_generate
→ kvs_vector_local_c_embedding_generate
→ embedding_encode
→ bert_tokenize
→ OrtCreateTensorWithDataAsOrtValue
→ OrtRun
→ embedding_copy_sentence_embedding
→ adapter L2 normalize
→ current index insert
```

VSEARCH 路径类似，只是进入搜索：

```text
cmd read lock
→ handle_vsearch_command
→ kvs_vector_search
→ kvs_vector_embedding_generate
→ embedding_encode
→ adapter L2 normalize
→ current index search
→ threshold / scope 过滤
```

ONNX runtime 的输入输出契约在 `src/embedding/embedding_runtime_onnx.c` 里写死了：

```text
inputs:
  input_ids
  attention_mask
  token_type_ids

shape:
  [1, seq_len], int64

output:
  sentence_embedding

requirements:
  float32, rank 2, [1, configured_dimension]
```

`config/kvstore-embedding.conf` 把 provider 配成 `local_c`，模型路径指向 `models/bge-small-zh-v1.5/model.onnx`，维度配成 `512`。但要注意：`local_c + dimension:512` 本身不能证明已经链接了 ONNX runtime，因为默认 mock runtime 也可以按配置返回维度。真正能说明 ONNX 参与构建的是 `ENABLE_ONNX_EMBEDDING=1` 对应的 Makefile 路径，以及运行时确实加载 `embedding_runtime_onnx.c`。

失败路径也在这一阶段变得重要。

`embedding_encode()` 需要处理这些情况：

- handle、text、output 参数为空。
- text 长度为 0。
- `output_dim` 和 runtime 维度不一致。
- 文本 byte length 超过 `embedding_max_text_len`。
- 三个输入数组分配失败。
- tokenizer 失败或超出 token 上限。
- 三个 OrtValue 创建失败。
- `OrtRun` 失败。
- 输出缺失、输出不是 float tensor、rank 不是 2、shape 不是 `[1, embedding_dim]`。

每个失败点都必须释放已经创建的 `OrtValue` 和 input buffer。`embedding_create()` 也有多级失败清理：env、session options、session、memory info、vocab、model path、vocab path 都需要在错误路径上按已初始化状态释放。`embedding_destroy()` 则负责逆序释放 session、session options、memory info、env / shared env ref、vocab、路径字符串和 profile mutex。

这就是 C 接外部 runtime 最容易被低估的地方：成功路径只是 `OrtRun`，真正决定稳定性的，是半初始化状态和失败清理。

## 14.7 手写最小 WordPiece：为什么可用，但不能说完全兼容

ONNX 模型需要 token id，而 KVLite 是 C 项目，所以这一阶段实现了一个最小 tokenizer。

它的行为大致是：

```text
加载 vocab.txt
检查 [PAD] / [UNK] / [CLS] / [SEP]
按空白分段
ASCII 字母做 lowercase
ASCII 字母数字走最小 WordPiece
非 ASCII 按 UTF-8 字符查 vocab
前后加 [CLS] 和 [SEP]
不足 max_tokens 的位置补 [PAD]
attention_mask 标记真实 token
token_type_ids 全部置 0
```

这套实现足够支撑 bge-small-zh-v1.5 的基本中文短文本 smoke，但不能说它完全兼容 Hugging Face tokenizer。完整 tokenizer 里还有 Unicode normalization、control character 处理、accent stripping、标点切分、特殊 token 规则等细节。当前实现只证明了“本项目的最小中文短文本样例可以跑通”，不是证明“任意输入和官方 tokenizer 完全一致”。

`b43ec18` 对 vocab 查找做了一个结构优化：从线性查找改成 hash table + open addressing。代码里通过 FNV 风格的字符串 hash 构建 `EmbeddingVocabHashEntry`，再在 `embedding_vocab_lookup()` 里做开放寻址查询。

这里也要注意表达边界。这个提交没有保存 profile 输出或 benchmark 结果，所以不能写“vocab hash 带来多少百分比提升”。能写的是：它把 tokenizer 里的明显线性查找热点改成了哈希查找，并提供了 profiling 入口，后续可以用结果验证收益。

## 14.8 可选构建、配置和资产准备

这一阶段的 Makefile 设计有一个明确目标：默认构建不能被 ONNX Runtime 绑架。

相关规则大致是：

```text
默认 make
→ 链接 src/embedding/embedding_runtime.c
→ 不要求 ONNX Runtime、model.onnx、vocab.txt

make embedding 或 ENABLE_ONNX_EMBEDDING=1
→ 链接 src/embedding/embedding_runtime_onnx.c
→ 检查 ONNXRUNTIME_ROOT
→ 检查 model.onnx / model.onnx_data / vocab.txt
→ 链接 libonnxruntime 并设置 rpath
```

`config/kvstore-embedding.conf` 是本地 ONNX 推理的示例配置，`tools/setup_onnx_embedding.sh` 负责准备 ONNX Runtime 和 bge-small-zh-v1.5 模型资产。

这个设计的取舍很实际：

```text
把 ONNX 变成默认依赖
→ 使用真实模型更直接，但所有开发、测试、CI 都要承担模型和动态库成本。

让 ONNX 显式 opt-in
→ 默认链路仍然轻量，代价是用户必须明确执行 setup / make embedding / embedding config。
```

我更倾向第二种，因为 KVLite 是一个 C 后端教学项目，不应该让普通 SET/GET、TTL、AOF/RDB、HNSW hash 测试都依赖体积较大的模型资产。

但这里也留下了债务。`tools/setup_onnx_embedding.sh` 当时没有把 Hugging Face revision 固定到不可变提交，也没有把下载文件 checksum 写进阶段结果。模型输入输出名硬编码为 `input_ids`、`attention_mask`、`token_type_ids` 和 `sentence_embedding`，模型不兼容时可能要到第一次 `OrtRun` 或输出解析阶段才暴露。

## 14.9 性能转折：先可观测，再谈优化

`b43ec18` 增加了几类性能相关入口：

- vocab hash，减少 tokenizer 查 vocab 的线性扫描。
- ONNX Runtime 的 intra-op / inter-op thread 配置。
- graph optimization level 配置。
- execution mode 配置。
- `KVLITE_EMBEDDING_PROFILE` 分段统计 total、tokenize、tensor、OrtRun、output 时间。

这些改动说明当时已经意识到真实 embedding 的成本不只在 HNSW。VSET/VSEARCH 的端到端耗时会混合：

```text
socket / RESP
→ 命令分发和锁
→ tokenizer
→ tensor 创建
→ OrtRun
→ 输出拷贝和 normalize
→ HNSW search / insert
→ threshold 过滤和 RESP 响应
```

但是这一阶段没有结果文件保存 profile 输出，也没有留下 ONNX 路径的 QPS、P95、P99 或优化百分比。所以本章只能说“加入了可观测性和调参入口”，不能写“性能提升了多少”。

还有一个容易被误解的点：当时的 batch 不是模型级 batch。

`kvs_vector_embedding_generate_batch()` 对 local_c provider 的处理仍然是循环调用 `embedding_encode()`，然后统一做 L2 normalize。`kvs_vector_embedding_generate_batch_parallel()` 在这个阶段也只是回到 batch 入口，并没有真正使用 `batch_size` 和 `max_inflight` 去做并发或模型级 batching。

所以这阶段的结论是：真实 embedding 接入后，性能瓶颈已经有了观察入口，但还没有完成吞吐优化。

## 14.10 测试与 benchmark 到底证明了什么

这一阶段最重要的测试是 `tests/semantic_onnx_test.sh`。当前仓库后续把它移动到了 `tests/vector/semantic_onnx_test.sh`，但在 `b43ec18` 这个阶段，它仍然是 `tests/semantic_onnx_test.sh`。

它做了几件事：

- 连接一个已经启动的 KVLite 服务。
- 通过 `VINFO` 检查输出里包含 `dimension:512`。
- 写入三条中文样例 VSET。
- 用 VGET 检查第一条问题能返回预期 answer。
- 对三条正向 query 跑多个 threshold 的 VSEARCH。
- 对一个 negative query 也跑多个 threshold 并打印结果。
- 最后只要求正向 query 在所有 threshold 的结果里至少出现过一个 score。

它能证明的东西有限，但很有用：命令层、配置、向量 record、embedding 生成、索引插入、搜索和响应格式形成了一个小型端到端 smoke。

它不能证明的东西同样要讲清楚：

| 证据 | 类型 | 能证明 | 不能证明 |
| --- | --- | --- | --- |
| `tests/semantic_onnx_test.sh` | smoke / end-to-end | 512 维配置下，少量 VSET/VGET/VSEARCH 命令能跑通 | 不能单独证明实际链接的是 ONNX runtime，不能证明 top1 正确、每个 query 都命中预期 answer、negative query 被过滤、recall@K、并发或持久化 |
| `tests/vector_testcase.c` 的 threshold 相关用例 | correctness | threshold 基本语义和显式阈值路径 | 不能证明真实模型语义质量 |
| `tests/vector_benchmark.c` 的 `--text-mode semantic` | benchmark harness | benchmark 客户端可以发送中文短文本和 threshold | 不是结果文件，不证明 QPS、P95、P99，也不证明语义召回 |
| `scripts/bench_vector_rw.sh` 的 threshold / DIM 参数 | benchmark harness | 脚本具备按维度和阈值跑 VSEARCH 的入口 | 不能把旧 hash embedding 结果外推为 ONNX 结果 |
| `KVLITE_EMBEDDING_PROFILE` | profiling hook | 可以把 encode 拆成 tokenize / tensor / OrtRun / output 阶段观察 | 没有持久化输出就不能写具体耗时结论 |

`tests/vector_benchmark.c` 的 semantic mode 还要特别小心。它只是循环一组中文模板，让压测命令更接近语义文本；VSEARCH 只要返回 RESP array，就可能被 benchmark 当成一次成功请求，即使 array 是空的。endpoint benchmark 也会混合 socket、RESP、embedding、index、过滤和响应成本，不能直接等价成“模型推理耗时”。

所以本章不能使用旧 `results/` 里的 100K、64 维 hash embedding 数据作为 ONNX 性能数字，也不能把 smoke test 写成 recall 结论。

## 14.11 阶段结论：按证据强度说话

这一阶段已经证明的内容：

- 代码层面完成了 C ABI，把 embedding runtime 隔离成 opaque handle。
- 默认构建仍然可以使用无外部依赖的 hash mock runtime。
- 显式 ONNX 构建可以进入 `embedding_runtime_onnx.c`。
- ONNX 路径补齐了 tokenizer、三输入 tensor、`OrtRun`、`sentence_embedding` 输出校验。
- provider 输出会在 adapter 层统一 L2 normalize，再进入 HNSW inner product。
- 失败路径上有错误码、last_error 和资源释放逻辑。
- small semantic smoke 能跑通少量 VSET/VGET/VSEARCH。

基本证明但需要谨慎表述的内容：

- `config/kvstore-embedding.conf` 的 512 维配置和 `VINFO dimension:512` 说明配置链路有效，但单独不能排除 mock runtime。
- smoke 说明端到端路径可运行，但不是 recall 或规模验证。
- profiling hook 说明阶段耗时可以拆开观察，但没有结果文件就不能给数字。

暂时观察到、但没有阶段结果支撑的内容：

- tokenizer 查 vocab 可能是热点，所以改成了 hash lookup。
- ORT 线程和 graph optimization 可能影响延迟和吞吐，所以暴露成配置。
- 真实 embedding 会明显增加 VSET/VSEARCH 成本，但本阶段没有保存端到端性能数据。

尚未证明的内容：

- 真实数据集上的语义相关性。
- recall@K。
- negative query 的稳定过滤效果。
- 100K / 1M 规模下的 ONNX VSEARCH 延迟。
- 多线程并发下 shared handle、last_error、ORT session 的稳定性。
- 长时间运行、ASan、leak、sanitizer 结果。
- 生产 SLA。

这类分层表达在面试里很重要。它比直接说“我接入了真实 embedding，效果很好”更可信。

## 14.12 当前缺陷与重新设计条件

这个阶段完成的是“真实 embedding 本地推理闭环”，不是生产化 embedding 服务。它留下了不少明确缺陷。

第一，batch 还是伪 batch。`kvs_vector_embedding_generate_batch()` 对每条文本循环调用 `embedding_encode()`，没有模型级 batch tensor，也没有真正把多条文本一起送入 `OrtRun`。`kvs_vector_embedding_generate_batch_parallel()` 当时也没有使用 `batch_size` 和 `max_inflight` 做实际并发控制。

第二，每次 encode 都有不少临时分配。三个 int64 input array、token buffer、OrtValue 都是按请求创建和释放。短文本低并发下能接受，但如果 `OrtRun` 之外的分配和 tensor 构造变成热点，就需要 buffer 复用或真正的 batch API。

第三，tokenizer 不是 Hugging Face 完整等价实现。它对中文短文本可用，但没有官方 tokenizer parity 测试，不能保证 Unicode、标点、特殊字符和长文本行为完全一致。

第四，模型契约硬编码。输入名、输出名、输出 rank 和维度都按当前模型假设写死。模型换成别的结构时，可能要改代码，而不是只改配置。

第五，错误状态不是严格并发安全。`last_error` 是 handle 内的共享 buffer，profile counters 有 mutex 或原子保护，但 last_error 本身没有按请求隔离。如果未来把 embedding 从全局锁内移到并发 worker，就要重新审视这个状态。

第六，未知 provider 字符串会回退到 hash。这对开发体验友好，但对生产配置不够严格。用户把 `embedding_provider` 写错时，系统可能还能启动，只是实际不走真实模型。

第七，ONNX runtime 直接读取全局 `g_cfg`，削弱了 C ABI 的隔离性。更理想的方式是所有 runtime 参数都通过 `EmbeddingConfig` 传入，而不是在 runtime 内部依赖全局配置。

第八，数据演进没有闭环。RDB 保存的是 record 里已有的 float embedding；`VINDEX REBUILD` 只是用已有 embedding 重建索引，不会重新跑模型。如果模型、维度、tokenizer 或 normalize 语义变化，历史 record 需要重新 embedding，而不是只 rebuild HNSW。

重新设计的触发条件也很清楚：

```text
如果 OrtRun 或 tensor 创建主导延迟
→ 需要模型级 batch、buffer 复用、worker pool 或 cache。

如果要支持多模型 / 模型热更新
→ 需要模型版本、checksum、record embedding version 和 re-embedding migration。

如果要做真实召回评估
→ 需要 ground truth、flat exact baseline、recall@K 和固定数据集。

如果要线上长期运行
→ 需要并发压测、ASan/leak、长稳测试和模型资产校验。
```

## 14.13 Agent 辅助开发边界

这一章尤其适合在面试里说明 Agent 辅助开发的边界。

Git 能直接证明的只有：`3ac0c5e` 的 gRPC 删除提交带有 `Co-Authored-By: Claude Opus 4.7`。本章六个候选提交不能仅凭提交速度、代码风格或 commit message 判断逐行作者。

更合理的表达是：Agent 可以辅助生成脚手架、补测试、整理 diff、提示边界，但我必须能独立解释这些关键决策：

- 为什么撤掉 gRPC / Python / C++ bridge 的远程方案。
- 为什么默认构建继续保持 dependency-free。
- 为什么用 opaque C ABI，而不是把 ORT 直接写进 vector 模块。
- 为什么 L2 normalize 放在 adapter 层，HNSW 继续用 inner product。
- 为什么 `c99679c` 只能叫 session load，不算真实推理完成。
- `embedding_create()` 和 `embedding_encode()` 的失败路径怎么释放资源。
- semantic smoke 到底证明了什么，没证明什么。
- 为什么当前 batch、tokenizer、provider fallback 和模型版本都是债务。

面试官不会只关心“你有没有用 Agent”。他们更关心：如果没有 Agent，你还能不能维护这段代码。我的回答重点应该放在源码理解、设计取舍、验证边界和缺陷识别上，而不是把文档写成 AI 使用说明。

## 14.14 可以关联哪些八股知识点

这一章可以关联的八股点比较集中。

**C ABI 和 opaque pointer：**

`EmbeddingHandle` 对调用方不透明，向量模块只通过 create / encode / dimension / last_error / destroy 操作。这是 C 语言里做模块边界和 ABI 稳定性的常见方式。

**资源所有权和失败清理：**

ONNX runtime 里有 env、session options、session、memory info、vocab、路径字符串、OrtValue 和 input buffers。成功路径容易写，难点是中途失败时只释放已经初始化的资源。

**动态链接和可选依赖：**

默认构建链接 mock runtime，ONNX 构建才链接 libonnxruntime 并设置 rpath。这可以拿来讲“可选能力不要污染基础构建”。

**向量相似度：**

L2-normalized embedding + inner product 可以按 cosine-like score 理解。这个不变量让 threshold 在不同 provider 下更稳定。

**Tokenizer 和 tensor shape：**

模型推理不只是调用 `OrtRun`，还要把文本变成符合模型契约的 `input_ids`、`attention_mask`、`token_type_ids`，并严格校验输出 tensor type、rank 和 shape。

**Benchmark 分层：**

smoke test、correctness test、benchmark harness、profile hook、scale benchmark、recall benchmark 证明的东西不同。不能把“命令能跑”说成“召回很好”，也不能把“有 benchmark 程序”说成“有性能结果”。

## 14.15 面试中怎么讲真实 Embedding 接入

### 1 分钟版本

“第九章我已经做了 VSET/VSEARCH 和 HNSW，但当时 embedding 是确定性 hash，只能验证链路，不能代表真实语义。后来我把这块补成了本地 ONNX 推理闭环。

我先做了一个关键前置：把 provider 输出统一做 L2 normalize，然后用 HNSW inner product，这样 threshold 语义不会因为 provider 尺度不同而漂。之后我设计了一个很小的 C ABI，向量模块只依赖 `embedding_create/encode/dimension/last_error/destroy`，默认构建仍然链接 hash mock，只有 `ENABLE_ONNX_EMBEDDING=1` 才链接 ONNX Runtime。

中间有一个真实转折：第一版 ONNX 只完成 session 加载，`embedding_encode` 明确返回未实现；后面才补了最小 WordPiece tokenizer、三个 int64 input tensor、`OrtRun` 和 `sentence_embedding` 输出校验。最后用 semantic smoke 跑通了少量中文 VSET/VGET/VSEARCH。

我不会把这阶段夸大成生产级语义检索。它证明的是本地推理链路能跑通，不证明 recall@K、100K 规模性能、并发稳定性或 tokenizer 完全等价。”

### 3～5 分钟版本

“真实 embedding 这块我不是直接把 ONNX Runtime 塞进业务代码。前面其实有过一次 gRPC provider 的尝试，后来 `3ac0c5e` 把 Python gRPC 服务、C++ bridge、protobuf 依赖都删掉了。能从 Git 证明的是这套远程方案被撤回，不能编造说它有多少性能问题。

撤回之后我选择本地 C ABI。原因是 KVLite 主体是 C 项目，默认构建不能强依赖模型和 ORT 动态库。C ABI 让 vector 模块只看到一个 opaque handle 和几个函数：create、encode、dimension、last_error、destroy。默认 make 链接 `embedding_runtime.c` 的 mock，ONNX 版本显式 opt-in。

在接 provider 之前，我先统一了数学语义。provider 输出 raw vector 后，adapter 层统一做 L2 normalize，然后 HNSW 用 inner product。这样非零向量自相似接近 1，threshold 更接近 cosine 语义。这个不变量不能放给各 provider 自己决定，否则 VSEARCH 的 score 在 hash、mock、ONNX 之间不可比较。

ONNX 接入分两步。`c99679c` 先做 session load 和可选构建，但 encode 明确未实现，所以这一步只能叫加载闭环。后面 `a5d669c` 才补 tokenizer、input tensor、OrtRun 和输出校验。输入固定是 `input_ids/attention_mask/token_type_ids`，shape 是 `[1, seq_len] int64`；输出固定取 `sentence_embedding`，要求 float32、rank 2、shape `[1, 512]`。

验证上我做了 semantic smoke：启动 embedding config 后，脚本检查 VINFO 有 512 维，写三条中文问答，VGET 一条，再用几个 threshold 跑 VSEARCH。但这个测试只能证明小型端到端链路可用，不能证明实际召回质量，也不能证明 negative query 一定过滤，更不能给性能结论。

当前缺陷我会主动讲：batch 只是循环单条 encode，不是真 batch；tokenizer 是最小实现，不保证和 Hugging Face 完全一致；模型 I/O 名硬编码；每次 encode 都有分配；模型变更后历史 record 需要重新 embedding，VINDEX REBUILD 不会重新跑模型。”

### 面试官追问方向

**为什么不用 gRPC？**

gRPC 的好处是模型服务可以独立部署，语言自由。但当时它引入 Python 服务、protobuf、C++ bridge、RPC 失败语义和部署复杂度。KVLite 是 C 后端项目，这一阶段优先要一个可控的本地推理闭环，所以撤回远程方案，改用 C ABI + 可选 ONNX。

**为什么不直接在 vector 模块里调用 ORT？**

直接调用会让 `src/vector/` 被 ORT 类型、session options、tensor 创建和模型路径细节污染。C ABI 把 runtime 隔离起来，vector 模块只关心“给文本，拿 float 数组和维度”。

**为什么 L2 normalize 放在 adapter 层？**

因为 VSEARCH score 和 threshold 是用户可见语义。不同 provider raw vector 的尺度不同，如果各 provider 自己决定是否归一化，threshold 就不可比较。adapter 统一 L2 后，inner product 才更接近 cosine-like score。

**ONNX session 加载成功是否等于真实 embedding 接入完成？**

不是。`c99679c` 就是反例：它能加载 session，但 `embedding_encode` 明确返回未实现。真实闭环必须包括 tokenizer、输入 tensor、OrtRun、输出 tensor 类型/shape 校验和错误清理。

**semantic smoke 能不能证明 recall？**

不能。它只证明少量命令跑通，正向 query 至少出现过 score。recall 需要固定数据集、ground truth、flat exact baseline 或标注答案，再按 recall@K 统计。

**换模型后 VINDEX REBUILD 是否足够？**

不够。RDB 里保存的是旧 embedding。VINDEX REBUILD 只是用已有 embedding 重建索引，不会重新调用模型。换模型、维度或 tokenizer 后需要 re-embedding 历史 record。

## 14.16 如果继续优化，可以怎么做

如果继续做这一阶段的后续演进，我会按下面顺序补证据和能力：

1. 做 tokenizer parity 测试，把 KVLite tokenizer 输出和官方 tokenizer 对齐，至少覆盖中文、英文、数字、标点、未知词、长文本和特殊字符。
2. 做真正的模型级 batch ABI，让多条文本组成 `[batch, seq_len]` 输入，而不是循环单条 `embedding_encode()`。
3. 复用 input buffer 和 OrtValue，减少每请求分配。
4. 建立真实数据集 ground truth，用 flat exact 或标注数据计算 recall@K。
5. 分层 benchmark：单独测 tokenizer、OrtRun、HNSW search、端到端 RESP，结果文件记录 provider、model revision、checksum、ORT 版本、线程参数和 text mode。
6. 给模型资产加 revision pin 和 checksum，避免同一个配置在不同机器拉到不同模型。
7. 给 record 增加 embedding model version，支持模型变更后的 re-embedding migration。
8. 做并发、ASan、leak 和长稳测试，确认 shared handle、last_error、session 和 shutdown 路径安全。

这些方向在本章只能作为后续优化，不能写成已经实现的能力。

## 14.17 本章小结

这一章的核心不是“我接了一个 ONNX 模型”，而是把第九章里的 hash 占位推进成了一个可运行的本地推理闭环，同时守住了几个工程边界。

第一，score 语义先统一。provider raw output 不直接进 HNSW，而是经过 adapter L2 normalize，再用 inner product 做 cosine-like score。

第二，依赖边界被控制住。默认构建仍然是无 ONNX 依赖的 mock runtime，真实模型通过 `ENABLE_ONNX_EMBEDDING=1` 显式进入。

第三，C ABI 把模型运行时从向量模块里隔离出来。向量模块只关心 create、encode、dimension、last_error、destroy，不关心 ORT session 的内部结构。

第四，Git 历史里有真实转折。gRPC 远程 provider 被移除，ONNX 第一版也只是 session load，后面才补齐 tokenizer、tensor、OrtRun 和输出校验。

第五，验证边界必须诚实。semantic smoke 证明小型端到端链路能跑，但不证明 recall、规模性能、并发稳定性或生产 SLA。

所以这一阶段的结论应该是：KVLite 从 hash 占位走到了可选 ONNX 本地推理闭环；它已经具备真实 embedding 接入的工程骨架，但还没有完成语义质量评估和性能生产化。
