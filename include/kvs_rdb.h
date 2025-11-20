#pragma once
#include <stdio.h>

#define RDB_MAGIC   "KVSRDB"
#define RDB_VERSION 1

#define RDB_OK  0
#define RDB_ERR 1

// RDB 接口（具体保存/加载细节在实现文件中）
int kvs_rdb_save(const char *filename);
int kvs_rdb_load(const char *filename);

