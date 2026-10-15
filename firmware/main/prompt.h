#pragma once

#include <stdbool.h>

#include "prompt_format.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 固件侧语音提示播放（甲方案）。
 *
 * prompt_init()：定位 rec 分区，读 VPMT 头 + 索引表；失败/为空则静默禁用（不崩）。
 * prompt_play(id)：从 rec 分区读该句 PCM（优先 SPIRAM 缓冲），做 2× 上采样
 *   （16kHz→32kHz，每样点重复一次），复用 audio_click_play_pcm 播放。
 *   非阻塞（只把指针交给音频引擎）；播放前先让位（停掉旧 PCM 声道）。
 *   返回播放的 32kHz 帧数（0 = 无该句/资源未就绪）。
 */

void prompt_init(void);
bool prompt_ready(void);
int prompt_count(void);
int prompt_play(int id);
/* 该句播放的 32kHz 帧数（0=无该句/未就绪）。用于选曲播报后给首音留延时。 */
int prompt_duration_frames(int id);

#ifdef __cplusplus
}
#endif
