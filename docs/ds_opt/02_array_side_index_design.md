# 第 2 步：array 专属 dense array + side index 设计方案

## 1. 本步目标

本步只针对 array 后端做专属方案设计，不改任何代码。目标是在不破坏现有命令层语义的前提下，为 10 万级数据量场景下的查找、删除、TTL 清理提供一条更低成本的实现路径。

第 1 步已经确认，array 当前的主要瓶颈不是单一函数，而是线性查找、删除 memmove、TTL 反复按 key 删除、range/sort 的整表复制 + qsort 叠加造成的放大效应。因此本步只围绕 array 设计 dense array + side index 方案，其他三种结构只保留共性元数据优化思路，不在本步展开。

## 2. 当前 array 的核心瓶颈分析

### 2.1 线性查找

当前 array 的查找路径集中在 [src/storage/kvs_array.c](src/storage/kvs_array.c) 的 kvs_array_find_index、kvs_array_get、kvs_array_set、kvs_array_mod 中，都是从 0 到 len 顺序扫表，并通过 strcmp 比较 key。

问题是：

- GET、EXIST、MOD、SET 覆盖写都会重复线性扫描。
- TTL 命令在 [src/core/kvs_operation.c](src/core/kvs_operation.c) 的 ttl_lookup_array 中同样线性扫一遍。
- 后台过期线程在 [src/core/kvs_ttl_background.c](src/core/kvs_ttl_background.c) 也会再扫一遍。

这意味着一个热点 key 的访问，会在多个路径上反复付出 O(n) 字符串比较成本。

### 2.2 删除 memmove

当前 kvs_array_del 的删除路径是：

1. 先线性查找 key；
2. 释放 key 和 value；
3. 如果不是尾部元素，就把后续元素整体 memmove 前移；
4. 再清空尾槽。

这个方案的问题有两个：

- 删除一次就会搬移后面一段元素，代价是 O(n)。
- 搬移后所有受影响元素的物理位置变了，但当前实现没有独立索引，不需要修复额外结构；这也是它慢但简单的原因。

### 2.3 TTL 过期清理反复扫描/删除

当前 array 的 TTL 清理路径至少有三层：

- 读路径中的 TTL 命中检查，会先查找，再判断过期，再删除。
- kvs_array_reap_expired 会按 len 线性扫描，并对每个过期 key 再调用 kvs_array_del。
- 后台 TTL 扫描在 [src/core/kvs_ttl_background.c](src/core/kvs_ttl_background.c) 中也是逐项扫描。

最重的点是 reap_expired 这种模式：

- 先扫到过期项；
- 再按 key 重新查找一次；
- 再 memmove 删除。

这会把一次过期清理放大成“扫描 + 二次查找 + 删除搬移”。

### 2.4 range / sort 的整表复制 + qsort

当前 kvs_array_sort 和 kvs_array_range 的实现都先做一次全量收集，再 qsort，再输出。

这对 10 万级数据量的影响是：

- 每次 sort/range 都要分配临时 items 数组；
- 每次都把有效项逐个复制一遍；
- 再对复制后的数组做 qsort；
- range 还要对排序后的结果再做 start/end 过滤。

因此，array 的排序和范围查询本质上都在做“全量快照 + 排序 + 输出”，而不是利用数据结构本身的访问优势。

## 3. dense array + side index 设计方案

### 3.1 设计原则

本方案坚持三个原则：

1. 主存储尽量保持连续内存，便于顺序扫描和缓存友好访问。
2. 查找入口由 side index 接管，避免每次都线性扫主数组。
3. 删除采用尾元素覆盖，避免 memmove 整段搬移。

这里的关键不是把 array 改成别的数据结构，而是把它拆成“密集主数组 + 独立索引层”。

### 3.2 主存储结构

建议主存储继续沿用 dense array 的思路，即一段连续数组保存真实数据槽位，每个槽位保留：

- key 指针；
- value 指针；
- expire_at_ms；
- 建议补充 key_len、value_len，用于减少重复 strlen；
- 可选保留 key_hash 作为辅助字段，但不是必须放在主槽里。

主存储仍然负责：

- 真正承载 key/value；
- 顺序遍历；
- full dump；
- range/sort 的候选集来源；
- TTL 扫描的实体数据访问。

也就是说，主数组保留“密集、连续、可遍历”的特点，不承担查找职责。

### 3.3 side index 的 key/value 设计

side index 建议单独维护一层索引表，存储从 key 到 dense 位置的映射。

建议索引项至少包含：

- key_hash：用于快速定位桶位和初筛；
- key_len：用于减少无效比较；
- key_ptr：指向主数组中的 key；
- dense_idx：主数组中的物理位置；
- state：空槽、使用中、墓碑或重建中状态。

索引项设计的重点是：

- 索引层不重复存储 value；
- 索引层只保存定位信息；
- 真正的数据仍然只有一份，放在主数组中。

这样做的好处是：

- GET / MOD / DEL / EXISTS 可以先通过 side index 直接定位 dense_idx；
- 过期扫描如果拿到 dense_idx，也可以直接处理，不必再次 key 查找；
- 删除尾元素时，只需要修复被搬到尾部位置的那一项索引。

### 3.4 扩容策略

建议主数组和 side index 分开扩容，但使用同一套增长原则。

#### 主数组扩容

- 仍然以连续内存为主，使用倍增策略更合适，不要每次只扩一个槽。
- 当前实现是按 capacity + 1 扩容，这会导致频繁 realloc，10 万级数据量下成本很高。
- 建议改成按倍增或至少按批量扩容，以减少扩容次数。

#### side index 扩容

- side index 的容量应始终保持明显大于元素数量。
- 建议维持较低负载因子，例如 0.7 左右，避免线性探测过长。
- side index 扩容后需要重建或重新探测所有索引项，但这类成本只在扩容时发生，不会落到每次 GET 上。

#### 两者的关系

- 主数组扩容主要影响物理存储。
- side index 扩容主要影响查找性能。
- 两者可以独立增长，但插入时建议先保证 side index 有足够空间，再写主数组，避免出现主数组写入成功但索引插入失败的半完成状态。

### 3.5 索引冲突处理策略

side index 不建议用链表挂接在桶上，优先采用开放定址更贴近当前风格，也更容易做内存控制。

建议策略如下：

- 以 key_hash 定位初始桶位；
- 发生冲突后按固定探测策略继续找空位；
- 命中时再用 key_len + memcmp 做最终确认；
- 删除时使用墓碑状态，避免中途断链影响后续查找；
- 当墓碑过多时触发局部重建或整表重hash。

这里必须强调：

- side index 的 key_hash 只用于定位和剪枝；
- array 的对外行为不依赖哈希顺序；
- 这只是一个索引层，不是把 array 语义改成 hash 语义。

## 4. 核心操作如何实现

### 4.1 SET

建议 SET 分成“命中覆盖”和“新插入”两条路径。

#### 命中覆盖

1. 通过 side index 定位 dense_idx。
2. 直接覆盖 value。
3. 如有必要，更新 value_len 和 expire_at_ms。
4. 不改主数组物理位置。

#### 新插入

1. 先检查 side index 是否需要扩容。
2. 再检查主数组是否需要扩容。
3. 在主数组末尾写入新槽位。
4. 将 key 的索引写入 side index。
5. len 加一。

这条路径的重点是：

- 新插入不做线性查找；
- 覆盖写不做重复扫描；
- 主数组只负责 append，新键位置稳定直到删除。

### 4.2 GET

GET 的实现应当只走 side index：

1. 计算 key_hash 和 key_len；
2. 在 side index 中定位；
3. 找到 dense_idx 后直接返回 value；
4. 如启用 TTL，再先判断 expire_at_ms，过期则删除后返回不存在。

这样 GET 不再依赖主数组线性扫描。

### 4.3 MOD

MOD 与 SET 覆盖路径相似，但语义保持当前风格：

- key 存在则只替换 value；
- key 不存在则返回不存在或当前约定的非 0 状态，不在本步改语义。

实现上仍然通过 side index 定位 dense_idx，然后直接更新 value。

### 4.4 DEL

DEL 是 dense array + side index 的核心收益点。

建议删除流程改为：

1. 通过 side index 定位待删项 dense_idx；
2. 释放该槽位的 key/value；
3. 用尾部元素覆盖当前槽位；
4. 更新被搬来的尾部元素在 side index 中对应的 dense_idx；
5. 删除原 key 在 side index 中的索引项；
6. len 减一。

这意味着：

- 删除从 O(n) memmove 降到 O(1) 尾覆盖加索引修复；
- 主数组保持密集，不留下大块空洞；
- side index 负责维护位置正确性。

### 4.5 EXISTS

EXISTS 只需要复用 GET 的定位路径，但不取 value：

- 找到则返回存在；
- 没找到则返回不存在；
- 命中 TTL 过期时按当前规则先删再返回不存在。

这比当前线性扫描更直接，也更适合 10 万级数据量。

## 5. 删除与索引修复策略

### 5.1 是否采用尾元素覆盖

建议采用尾元素覆盖。

理由是：

- 这是把 array 删除成本从 O(n) 降下来的关键步骤；
- array 没有对外承诺稳定物理顺序；
- 当前 range/sort 已经是基于快照排序，不依赖主数组物理顺序。

### 5.2 覆盖后如何修复 side index

尾覆盖后要做的唯一额外工作是修复被搬来的尾元素索引：

- 找到尾元素原来的索引项；
- 把它的 dense_idx 改成被覆盖位置；
- 如索引项还携带额外校验信息，也要一并同步。

然后再删除被删 key 的索引项。

### 5.3 尾覆盖是否影响现有语义

从当前仓库的实现看，array 的外部语义并不要求保留物理插入顺序。

- GET / SET / MOD / DEL / EXISTS 的语义不受影响。
- RANGE / SORT 本来就是排序后输出，外部不依赖主数组物理顺序。
- FULLDUMP 当前是遍历数组槽位逐条输出，输出顺序可能会因为尾覆盖而变化，但输出内容应保持一致。

因此尾覆盖不会破坏当前核心语义，但需要明确：

- 如果后续测试对 full dump 顺序有严格要求，要么调整测试接受“集合语义”，要么额外维护顺序字段；
- 以最小侵入为前提，本步更推荐接受 full dump 的顺序不变性不作为硬约束。

## 6. TTL 兼容方案

### 6.1 惰性删除

TTL 语义建议继续采用惰性删除：

- GET / EXISTS / TTL 命中时先判断 expire_at_ms；
- 如果过期，先删再返回不存在；
- 删除动作通过 side index 直接定位，不再按 key 线性查找。

这样可以保持当前语义不变，同时把过期处理成本降下来。

### 6.2 后台扫描

后台扫描建议从“按 key 删除”改为“按 dense_idx 直接删除”。

更具体地说：

- 扫描器按 dense array 的物理位置顺序走；
- 对过期项直接调用内部按索引删除路径；
- 删除后如果发生尾覆盖，要回退当前扫描位置或重新检查搬来的槽位，避免跳过元素。

这比当前 reap_expired 中先扫一遍再按 key 二次查找更适合 dense array。

### 6.3 如何避免 reap_expired 中反复按 key 删除

这是本方案里最需要落地的点之一。

建议把 array 内部删除拆成两层：

- 对外的 kvs_array_del：仍然保留按 key 删除接口，兼容命令层和公共调用；
- 对内的 kvs_array_del_at 或等价内部 helper：直接接收 dense_idx，跳过二次查找。

这样 kvs_array_reap_expired、后台 TTL 扫描、某些内部清理路径都可以直接走索引删除，避免“扫描后再按 key 搜一次”。

## 7. range / sort / full dump 语义保持方案

### 7.1 哪些语义不变

- RANGE 仍然返回 key 在区间内的记录；
- SORT 仍然按 key 字典序排序输出；
- GET / SET / MOD / DEL / EXISTS 的命令语义保持不变；
- TTL 过期后返回不存在或过期状态的语义保持不变。

### 7.2 哪些实现成本会变化

- range / sort 不再需要先把整表线性快照成一份“包含全部槽位拷贝”的大临时数组，至少可以把候选收集和排序对象控制得更细；
- 如果侧重最小侵入，可以先保留现有 sort/range 逻辑，只把底层 lookup、delete 和 TTL 扫描提速；
- 后续若再做进一步优化，可以考虑直接基于 side index 收集候选项，减少无效项扫描。

### 7.3 当前阶段的推荐做法

为了降低风险，本步建议：

- 先不改 range / sort 的对外格式；
- 优先保证其结果正确；
- 允许它们继续通过主数组或索引层收集候选，但不允许再回到全量线性查找的热点路径。

也就是说，range / sort 是保语义，先不追求本步全部重写。

## 8. 与当前公共 TTL / RDB / AOF / full dump 链路的兼容性分析

### 8.1 TTL 链路

当前 TTL 入口在 [src/core/kvs_operation.c](src/core/kvs_operation.c) 中统一分发。array 引入 side index 后，TTL 只需要把 lookup / delete 换成索引路径即可，不需要改变命令层协议。

兼容点：

- expire_at_ms 语义不变；
- master / slave 的同步语义不变；
- TTL 已过期后的删除动作仍然可以同步触发后续链路。

### 8.2 RDB 链路

RDB 保存 / 加载在 [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c) 中是按结构遍历与回放。

引入 dense array + side index 后，RDB 的兼容性重点不是文件格式，而是：

- 保存阶段仍需遍历主数组的有效槽；
- 加载阶段仍可复用 SET 回放；
- 加载完成后需要同步重建 side index；
- 如果未来补 key_len/value_len，RDB 回放可以直接利用这些元数据减少重复 strlen。

也就是说，RDB 不需要先改协议，先保证结构重建正确即可。

### 8.3 AOF 链路

AOF rewrite 在 [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c) 中对 array 的快照逻辑仍然可以工作，但需要注意：

- 如果 array 采用尾覆盖，AOF rewrite 不应假设物理顺序稳定；
- AOF 输出的是有效命令序列，不是数组物理布局；
- 只要 key/value 和 expire_at_ms 正确，AOF 的语义就不变。

### 8.4 full dump 链路

full dump 在 [src/core/kvs_operation.c](src/core/kvs_operation.c) 中直接遍历各结构导出。array 采用 side index 后，full dump 有两个可选方案：

1. 继续遍历主数组导出，保持实现最简单；
2. 通过 side index 迭代有效项导出，减少空洞影响。

当前阶段更推荐第一种，因为它最容易复用已有测试，且不要求新增迭代器接口。

## 9. 风险点

- 语义风险：尾元素覆盖会改变主数组的物理顺序，如果有隐含顺序依赖的测试，需要提前确认。
- 索引一致性风险：主数组和 side index 是双写结构，任何一次插入、删除、搬移、扩容如果不同步，就会出现脏索引。
- TTL 逻辑风险：如果后台扫描仍然按 key 删除，会把索引收益吃掉，必须落实内部按索引删除路径。
- 扩容风险：主数组和 side index 同时扩容时要保证失败回滚明确，否则容易出现半完成状态。
- 调试复杂度风险：双结构比当前单数组更难排错，需要清楚的校验函数和一致性检查入口。
- 迁移风险：RDB/AOF 加载后必须重建索引，否则恢复出来的数据不可查。

## 10. 分步落地建议

为了保证最小侵入、容易做对、便于复用测试，本步建议拆成三层落地：

1. 第一步只引入 side index 的数据模型和查找语义，不动 range / sort / full dump。
2. 第二步把 DEL 改成尾元素覆盖，并补好索引修复。
3. 第三步把 TTL 清理切到按 dense_idx 的内部删除路径，消掉 reap_expired 的二次查找。

这样能把风险控制在最小范围内，也方便每一步都用现有测试回归语义。

## 11. 本步结论

array 的核心优化方向已经很明确：后续优先走 side index，而不是继续在主数组上做线性扫描修补。

- 对 10 万级数据量，最值得先砍掉的是 GET / EXISTS / MOD / DEL / TTL 的 O(n) 查找成本。
- 删除应当从 memmove 转向尾元素覆盖，并同步修复 side index。
- TTL 清理必须避免 reap_expired 中反复按 key 删除，内部按索引删除是关键。
- range / sort / full dump 的对外语义尽量保持，先保正确性，再考虑进一步减少候选收集开销。
- rbtree / skiptable 不应改成按 hash 排序；它们只保留共性元数据优化思路，不在本步展开。

## 12. 下一步建议

如果进入第 3 步，最推荐修改的文件列表是：

- [include/kvs_array.h](include/kvs_array.h)
- [src/storage/kvs_array.c](src/storage/kvs_array.c)
- [src/core/kvs_ttl_background.c](src/core/kvs_ttl_background.c)
- [src/core/kvs_operation.c](src/core/kvs_operation.c)
- [src/persistence/kvs_rdb.c](src/persistence/kvs_rdb.c)
- [src/persistence/kvs_aof_rewrite.c](src/persistence/kvs_aof_rewrite.c)

其中真正优先动手的是前两份，其他文件后续用于补 TTL、持久化和导出链路兼容。
