### 架构设计
![image](https://disk.0voice.com/p/py)

## 增加kvstore数据持久化

## 增加sort、range功能

## 引入内存池

​	使用kvs_pref.c测试系统malloc与内存池kvs_malloc。

​	编译命令：

```bash
gcc -o kvs_perf kvs_perf.c kvs_array.c kvs_hash.c kvs_rbtree.c kvstore.c ntyco.c proactor.c reactor.c kvs_mempool.c kvs_skiptable.c -I ./NtyCo/core/ -L ./NtyCo/ -lntyco -lpthread -luring -ldl
```

​	kvstore.c中关闭主函数，执行：

```bash
./kvs_perf
```

​	本机测试结果：

```bash
zhengpan@zhengpan:~/share/Linux/chapter_9/8.10-kvstore$ ./kvs_perf
===== TESTING WITH MEMORY POOL =====
MEMPOOL SET 100000 ops: 4.86 ms (20580.37 ops/ms)
MEMPOOL GET 100000 ops: 552.19 ms (181.10 ops/ms)

===== TESTING WITH MALLOC/FREE =====
MALLOC SET 100000 ops: 5.12 ms (19538.88 ops/ms)
MALLOC GET 100000 ops: 571.86 ms (174.87 ops/ms)
```
## 主从分布式架构


性能参数 使用说明 编译命令



