#pragma once


typedef struct {
	int (*set)(void*, char*, char*);
	char* (*get)(void*, char*);
	int (*del)(void*, char*);
	int (*mod)(void*, char*, char*);
	int (*exist)(void*, char*);
	int (*sort)(void*, int, int, char*, int);
	int (*range)(void*, char*, char*, char*, int);
} kvs_operations;

extern kvs_operations array_ops;
extern kvs_operations rbtree_ops;
extern kvs_operations hash_ops;
extern kvs_operations skiplist_ops;

