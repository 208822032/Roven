#!/bin/bash

# 测试参数
TOTAL_REQUESTS=100000
CONCURRENT=1
PIPELINE_SIZE=20

echo "Testing mixed operations in pipeline mode..."
echo "Total requests: $TOTAL_REQUESTS, Concurrent: $CONCURRENT, Pipeline: $PIPELINE_SIZE"
echo ""

# 清理旧数据
redis-cli FLUSHALL > /dev/null

# 准备测试数据
echo "Preparing test data..."
for i in {1..1000}; do
    redis-cli SET "key:$i" "value_$i" > /dev/null
    redis-cli SET "counter:$i" "100" > /dev/null
    redis-cli LPUSH "list:$i" "item1" "item2" "item3" "item4" "item5" > /dev/null
    redis-cli SADD "sortset:$i" "m1" "m2" "m3" > /dev/null
done

# 运行性能测试
start_time=$(date +%s%N)

# 使用多次 redis-cli 调用来模拟混合操作
for ((i=0; i<TOTAL_REQUESTS/PIPELINE_SIZE; i++)); do
    {
        # 构建一个包含多种命令的管道（6个命令）
        for ((j=0; j<PIPELINE_SIZE; j++)); do
            rand_key=$((RANDOM % 1000 + 1))
            rand_num=$((RANDOM % 50 + 1))
            
            echo "SET key:mod:$rand_key modified_value_$RANDOM"  # 修改值
            echo "INCR counter:$rand_key"                       # 计数器操作
            echo "APPEND key:$rand_key _appended"               # 追加内容
            echo "GET key:$rand_key"                           # 读取值
            echo "DEL key:$((RANDOM % 1000 + 1))"              # 删除键
            echo "LRANGE list:$rand_key 0 2"                   # 列表操作
        done
    } | redis-cli --pipe > /dev/null 2>&1
done &

# 等待所有后台进程完成
wait

end_time=$(date +%s%N)
elapsed_time=$(( (end_time - start_time) / 1000000 ))  # 毫秒

# 计算 QPS (每个请求包含6个命令)
total_commands=$((TOTAL_REQUESTS * 6))
qps=$(( (total_commands * 1000) / elapsed_time ))

echo "Elapsed time: ${elapsed_time}ms"
echo "Total commands executed: $total_commands"
echo "Approximate QPS: $qps commands/second"
echo "Operations: SET(modify), INCR, APPEND, GET, DEL, LRANGE"