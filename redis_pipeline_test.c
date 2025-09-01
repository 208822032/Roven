#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <hiredis/hiredis.h>

#define TIME_SUB_MS(tv1, tv2)  ((tv1.tv_sec - tv2.tv_sec) * 1000 + (tv1.tv_usec - tv2.tv_usec) / 1000)

//redis pipeline测试用例
void redis_pipeline_testcase(redisContext *conn){
    redisReply *reply;

    // 使用 Redis 协议格式的命令
    // 注意：MOD、SORT、RANGE 不是标准 Redis 命令，这里使用类似的标准命令替代
    redisAppendCommand(conn, "SET PipeKey1 Value1");
    redisAppendCommand(conn, "GET PipeKey1");
    redisAppendCommand(conn, "SET PipeKey1 NewValue1"); // 替代 MOD
    redisAppendCommand(conn, "GET PipeKey1");
    redisAppendCommand(conn, "DEL PipeKey1");
    redisAppendCommand(conn, "GET PipeKey1");
    redisAppendCommand(conn, "SET PipeKey2 Value2");
    redisAppendCommand(conn, "SET PipeKey3 Value3");
    redisAppendCommand(conn, "SET PipeKey4 Value4");
    redisAppendCommand(conn, "SORT high"); // 假设 high 是一个列表或集合
    redisAppendCommand(conn, "SORT low");  // 假设 low 是一个列表或集合
    redisAppendCommand(conn, "LRANGE PipeKey2 0 -1"); // 替代 RANGE，假设是列表
    redisAppendCommand(conn, "DEL PipeKey2");
    redisAppendCommand(conn, "DEL PipeKey3");
    redisAppendCommand(conn, "DEL PipeKey4");

    for(int i = 0;i < 15; i++){
        redisGetReply(conn, (void**)&reply);
        freeReplyObject(reply);
    }
}

//redis pipeline qps测试函数
void test_redis_pipeline(redisContext *conn){
    int iterations = 100000;
    int commands_per_iteration = 15;
    int i = 0;

    struct timeval tv_begin;
    gettimeofday(&tv_begin, NULL);

    for(i = 0;i < iterations;i++){
        redis_pipeline_testcase(conn);
    }

    struct timeval tv_end;
    gettimeofday(&tv_end, NULL);

    int time_used = TIME_SUB_MS(tv_end, tv_begin); // ms
    int total_commands = iterations * commands_per_iteration;

    float commands_per_second = (total_commands * 1000.0) / time_used;

    printf("Redis pipeline testcase --> time_used: %d ms, commands: %d, qps: %.2f\n", 
           time_used, total_commands, commands_per_second);

}

// 初始化测试数据
void init_test_data(redisContext *conn) {
    // 创建用于 SORT 测试的列表
    redisReply *reply;
    
    // 创建 high 列表
    reply = redisCommand(conn, "DEL high");
    freeReplyObject(reply);
    
    for (int i = 0; i < 10; i++) {
        reply = redisCommand(conn, "LPUSH high %d", 100 - i);
        freeReplyObject(reply);
    }
    
    // 创建 low 列表
    reply = redisCommand(conn, "DEL low");
    freeReplyObject(reply);
    
    for (int i = 0; i < 10; i++) {
        reply = redisCommand(conn, "LPUSH low %d", i);
        freeReplyObject(reply);
    }
}


// 主函数示例
int main() {
    // 连接到 Redis
    redisContext *conn = redisConnect("127.0.0.1", 6379);
    if (conn == NULL || conn->err) {
        if (conn) {
            printf("Connection error: %s\n", conn->errstr);
            redisFree(conn);
        } else {
            printf("Connection error: can't allocate redis context\n");
        }
        exit(1);
    }
    
    // 初始化测试数据
    init_test_data(conn);
    
    // 测试 Redis Pipeline
    test_redis_pipeline(conn);
    
    // 清理测试数据
    redisReply *reply = redisCommand(conn, "DEL high low");
    freeReplyObject(reply);
    
    // 断开连接
    redisFree(conn);
    return 0;
}