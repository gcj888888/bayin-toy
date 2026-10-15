#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * 开机语音存储。
 *
 * 默认语音（人声“欢迎来到AI音乐世界”）直接内嵌在固件里（audio/samples/voice_default.raw），
 * 开机一定有声音、且断电不丢。
 *
 * 用户可从网页导入自己的语音（≤10 秒，任意格式）：板子收完后会写进
 * voice 数据分区（Flash），下次开机优先播“导入的”；想恢复默认就清除导入。
 */

/* 10 秒 @ 32kHz 单声道 16bit 的上限字节数 */
#define VOICE_MAX_BYTES (32000u * 2u * 10u)

/* 开机播放语音（导入的优先；没有导入则播内嵌默认） */
void voice_store_init(void);
void voice_store_play_greeting(void);

/* 网页导入：分块 base64 流式接收 */
esp_err_t voice_store_upload_begin(uint32_t total_bytes);
esp_err_t voice_store_upload_append(const char *b64, uint32_t b64_len);
esp_err_t voice_store_upload_end(void);
bool voice_store_has_import(void);

/* 清除导入，恢复内嵌默认语音 */
esp_err_t voice_store_clear_import(void);
