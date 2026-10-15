#include "json_field.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool extract_quoted_type(const char *line, char *out, size_t out_len)
{
    const char *t = strstr(line, "\"t\"");
    if (t == NULL) {
        /* 八音琴曲库命令用 {"v":2,"cmd":"song_add",...}，回退到 "cmd" */
        t = strstr(line, "\"cmd\"");
    }
    if (t == NULL) {
        return false;
    }
    const char *colon = strchr(t, ':');
    if (colon == NULL) {
        return false;
    }
    const char *q1 = strchr(colon, '"');
    if (q1 == NULL) {
        return false;
    }
    const char *q2 = strchr(q1 + 1, '"');
    if (q2 == NULL || (size_t)(q2 - (q1 + 1)) >= out_len) {
        return false;
    }
    memcpy(out, q1 + 1, (size_t)(q2 - (q1 + 1)));
    out[q2 - (q1 + 1)] = '\0';
    return true;
}

bool extract_int_field(const char *line, const char *key, long *out)
{
    char pattern[32];
    /* Require `"key":` so short keys like "n" never match inside "note". */
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(line, pattern);
    if (p == NULL) {
        return false;
    }
    const char *value = p + strlen(pattern);
    /*
     * 容忍 JSON 字符串形式的数字，例如网页滑块发来的 {"v":"80"}。
     * 若值以 '"' 开头，跳过引号再解析（结尾的 '"' 会被 strtol 自然停在它前面）。
     * 这样 {\"v\":\"80\"} 与 {\"v\":80} 都能正确解析，
     * 否则裸 strtol 从 '"' 起步会失败，导致回调恒用默认值（网页滑块失效）。
     */
    if (*value == '"') {
        value++;
    }
    char *end = NULL;
    const long parsed = strtol(value, &end, 10);
    if (end == value) {
        return false;
    }
    *out = parsed;
    return true;
}

int extract_notes_array(const char *line, song_ev_t *out, int max)
{
    const char *p = strstr(line, "\"notes\"");
    if (p == NULL) {
        return 0;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return 0;
    }
    p++;                /* 跳过 ':' */
    while (*p == ' ') { /* 允许 ':' 后空格 */
        p++;
    }
    if (*p != '[') {
        return 0;
    }
    p++; /* 跳过外层 '[' */

    int n = 0;
    while (*p != '\0' && n < max) {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') {
            p++;
        }
        if (*p != '[') {
            break; /* 外层数组结束或格式不符 */
        }
        p++; /* 跳过内层 '[' */
        char *end = NULL;
        long note = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        p = end;
        while (*p == ' ') {
            p++;
        }
        if (*p != ',') {
            break;
        }
        p++;
        long ms = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        p = end;
        while (*p == ' ') {
            p++;
        }
        if (*p != ']') {
            break; /* 缺内层 ']' */
        }
        p++;

        if (note < 0) {
            note = 0;
        }
        if (note > 7) {
            note = 7;
        }
        if (ms < 0) {
            ms = 0;
        }
        if (ms > 65535) {
            ms = 65535;
        }
        out[n].note = (uint8_t)note;
        out[n].delta_ms = (uint16_t)ms;
        n++;
    }
    return n;
}

bool extract_quoted_field(const char *line, const char *key, char *out, size_t out_len)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(line, pattern);
    if (p == NULL) {
        return false;
    }
    const char *colon = strchr(p, ':');
    if (colon == NULL) {
        return false;
    }
    const char *q1 = strchr(colon, '"');
    if (q1 == NULL) {
        return false;
    }
    const char *q2 = strchr(q1 + 1, '"');
    if (q2 == NULL || (size_t)(q2 - (q1 + 1)) >= out_len) {
        return false;
    }
    memcpy(out, q1 + 1, (size_t)(q2 - (q1 + 1)));
    out[q2 - (q1 + 1)] = '\0';
    return true;
}
