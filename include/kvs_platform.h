#pragma once
/*
 * 平台与编译器相关的轻量级适配层。
 * 保持最小化：避免在公共头中引入系统级头文件。
 */

#if defined(_WIN32) || defined(_WIN64)
#define KVS_PLATFORM_WINDOWS 1
#else
#define KVS_PLATFORM_WINDOWS 0
#endif

