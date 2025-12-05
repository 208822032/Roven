#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "kvs_config.h"

static void trim(char *s) {
    if (!s) return;
    // 去掉前后空白
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) end--;
    *end = '\0';
    if (start != s) memmove(s, start, end - start + 1);
}

void kvs_config_init_default(kvs_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));

    cfg->port = 8080;
    cfg->role = KVS_ROLE_MASTER;

    cfg->master_ip[0] = '\0';
    cfg->master_port = 0;

    cfg->sync_port = 0; // 对于 master 稍后设为 port+1000

    cfg->enable_aof = ENABLE_AOF;
    cfg->aof_fsync_mode = KVS_AOF_FSYNC_EVERYSEC;

    snprintf(cfg->aof_filename, sizeof(cfg->aof_filename), "%s", "data/kvstore.aof");
    snprintf(cfg->rdb_filename, sizeof(cfg->rdb_filename), "%s", "data/kvstore.rdb");
    snprintf(cfg->dump_filename, sizeof(cfg->dump_filename), "%s", "data/kvstore.dat");

    cfg->network_mode = NETWORK_SELECT;

    // 默认使用自定义内存池
    cfg->mem_backend = KVS_MEM_BACKEND_POOL;

    // 默认允许较大 Value，可通过配置覆盖或关闭限制
    cfg->max_value_size = 512 * 1024; // 512KB

    //初始化 同步模式默认选择TCP
    cfg->sync_mode = KVS_SYNC_TCP;
    snprintf(cfg->ebpf_conf, sizeof(cfg->ebpf_conf), "%s", "config/ebpf_sync.conf");
}

static int parse_bool(const char *v, int def) {
    if (!v) return def;
    if (strcasecmp(v, "yes") == 0 || strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0) return 1;
    if (strcasecmp(v, "no") == 0 || strcasecmp(v, "false") == 0 || strcmp(v, "0") == 0) return 0;
    return def;
}

static kvs_aof_fsync_mode_t parse_aof_mode(const char *v, kvs_aof_fsync_mode_t def) {
    if (!v) return def;
    if (strcasecmp(v, "no") == 0) return KVS_AOF_FSYNC_NO;
    if (strcasecmp(v, "always") == 0) return KVS_AOF_FSYNC_ALWAYS;
    if (strcasecmp(v, "everysec") == 0) return KVS_AOF_FSYNC_EVERYSEC;
    return def;
}

static kvs_mem_backend_t parse_mem_backend(const char *v, kvs_mem_backend_t def) {
    if (!v) return def;
    if (strcasecmp(v, "pool") == 0) return KVS_MEM_BACKEND_POOL;
    if (strcasecmp(v, "system") == 0 || strcasecmp(v, "malloc") == 0)
        return KVS_MEM_BACKEND_SYSTEM;
    if (strcasecmp(v, "jemalloc") == 0) return KVS_MEM_BACKEND_JEMALLOC;
    return def;
}

int kvs_config_load_file(kvs_config_t *cfg, const char *path) {
    if (!cfg || !path) return -1;

    FILE *fp = fopen(path, "r");
    if (!fp) {
        // 配置文件不存在或无法打开，视为非致命错误，保留默认值
        return -1;
    }

    char line[KVS_MAX_LINE];
    while (fgets(line, sizeof(line), fp)) {
        // 去掉注释
        char *p = strchr(line, '#');
        if (p) *p = '\0';

        trim(line);
        if (line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (strcasecmp(key, "port") == 0) {
            cfg->port = atoi(val);
        } else if (strcasecmp(key, "role") == 0) {
            if (strcasecmp(val, "master") == 0) cfg->role = KVS_ROLE_MASTER;
            else if (strcasecmp(val, "slave") == 0) cfg->role = KVS_ROLE_SLAVE;
        } else if (strcasecmp(key, "master_ip") == 0) {
            snprintf(cfg->master_ip, sizeof(cfg->master_ip), "%s", val);
        } else if (strcasecmp(key, "master_port") == 0) {
            cfg->master_port = atoi(val);
        } else if (strcasecmp(key, "sync_port") == 0) {
            cfg->sync_port = atoi(val);
        } else if (strcasecmp(key, "enable_aof") == 0) {
            cfg->enable_aof = parse_bool(val, cfg->enable_aof);
        } else if (strcasecmp(key, "aof_fsync_mode") == 0) {
            cfg->aof_fsync_mode = parse_aof_mode(val, cfg->aof_fsync_mode);
        } else if (strcasecmp(key, "aof_filename") == 0) {
            snprintf(cfg->aof_filename, sizeof(cfg->aof_filename), "%s", val);
        } else if (strcasecmp(key, "rdb_filename") == 0) {
            snprintf(cfg->rdb_filename, sizeof(cfg->rdb_filename), "%s", val);
        } else if (strcasecmp(key, "dump_filename") == 0) {
            snprintf(cfg->dump_filename, sizeof(cfg->dump_filename), "%s", val);
        } else if (strcasecmp(key, "network_mode") == 0) {
            if (strcasecmp(val, "reactor") == 0) cfg->network_mode = NETWORK_REACTOR;
            else if (strcasecmp(val, "proactor") == 0) cfg->network_mode = NETWORK_PROACTOR;
            else if (strcasecmp(val, "ntyco") == 0) cfg->network_mode = NETWORK_NTYCO;
        } else if (strcasecmp(key, "mem_backend") == 0) {
            cfg->mem_backend = parse_mem_backend(val, cfg->mem_backend);
        } else if (strcasecmp(key, "max_value_size") == 0) {
            long long v = atoll(val);
            if (v < 0) v = 0; // 负数按 0 处理，表示不限制
            cfg->max_value_size = (size_t)v;
        } else if (strcasecmp(key, "sync_mode") == 0) {
            if (strcasecmp(val, "tcp") == 0) cfg->sync_mode = KVS_SYNC_TCP;
            else if (strcasecmp(val, "ebpf") == 0) cfg->sync_mode = KVS_SYNC_EBPF;
        } else if (strcasecmp(key, "ebpf_conf") == 0) {
            snprintf(cfg->ebpf_conf, sizeof(cfg->ebpf_conf), "%s", val);
        }
    }

    fclose(fp);
    return 0;
}

void kvs_config_apply_cmdline(kvs_config_t *cfg, int argc, char *argv[], const char **config_path_out) {
    if (!cfg) return;
    int i;
    if (config_path_out) *config_path_out = NULL;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 < argc) {
                if (config_path_out) *config_path_out = argv[++i];
            }
        } else if (strcmp(argv[i], "--slave") == 0) {
            cfg->role = KVS_ROLE_SLAVE;
        } else if (strcmp(argv[i], "--master") == 0) {
            if (i + 3 < argc) {
                snprintf(cfg->master_ip, sizeof(cfg->master_ip), "%s", argv[++i]);
                cfg->master_port = atoi(argv[++i]);
                cfg->port = atoi(argv[++i]);
                cfg->role = KVS_ROLE_SLAVE; // 作为从节点
            }
        } else if (strcmp(argv[i], "--aof_fsync") == 0) {
            if (i + 1 < argc) {
                cfg->aof_fsync_mode = parse_aof_mode(argv[i + 1], cfg->aof_fsync_mode);
                i++;
            }
        } else if (strcmp(argv[i], "--aof_filename") == 0) {
            if (i + 1 < argc) {
                snprintf(cfg->aof_filename, sizeof(cfg->aof_filename), "%s", argv[++i]);
            }
        } /*else if (argv[i][0] != '-') {
            // 第一个非选项参数，如果仍是 master，视为 port
            if (cfg->role == KVS_ROLE_MASTER) {
                cfg->port = atoi(argv[i]);
            }
        }*/
    }

    // 根据最终 role/port 填充 sync_port（仅当未显式指定）
    if (cfg->sync_port == 0) {
        if (cfg->role == KVS_ROLE_MASTER) {
            cfg->sync_port = cfg->port + 1000;
        } else if (cfg->role == KVS_ROLE_SLAVE && cfg->master_port > 0) {
            cfg->sync_port = cfg->master_port + 1000; // 主节点的同步端口
        }
    }
}
