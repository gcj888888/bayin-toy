#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "song_pool.h"

/*
 * 纯 JSON 行解析工具（无 ESP-IDF 依赖，便于在电脑上做宿主测试）。
 *
 * 只支持本项目“一行一个 JSON 命令”协议用到的取值：
 *   - 整数：  "v":123  或   "slot":0
 *   - 引号串： "d":"..." 或  "name":"..."
 *   - notes 数组： "notes":[[note,ms],...]
 *
 * 这些函数原本是 host_link.c 里的 static 函数，抽出来后才能在 PC 上
 * 直接单元测试（覆盖所有命令的解析）。
 */

/* 取出命令类型：优先 "t":"..."，其次 "cmd":"..."（八音琴曲库命令用 cmd）。 */
bool extract_quoted_type(const char *line, char *out, size_t out_len);

/* 取出 "key":整数 的值（要求 "key" 后紧跟冒号，避免 "n" 误匹配 "note"） */
bool extract_int_field(const char *line, const char *key, long *out);

/* 取出 "key":"..." 里的引号字符串 */
bool extract_quoted_field(const char *line, const char *key, char *out, size_t out_len);

/*
 * 解析 "notes":[[note,dely_ms],...] 到 ev 数组。
 * 返回解析到的条数（0 = 未找到 / 空数组）；超过 max 的部分忽略。
 */
int extract_notes_array(const char *line, song_ev_t *out, int max);
