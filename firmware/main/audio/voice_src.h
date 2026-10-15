#pragma once

#include <stdbool.h>

/*
 * 混音声道"源选择"规则（纯逻辑，宿主测试覆盖；audio_click.c 引用它做判定）。
 *
 * ★ 历史 bug（真机"语音反复播报"根因）：
 *   混音循环曾用 "ext != NULL" 判定声道是否走外部 PCM 缓冲，而 start_voice /
 *   start_synth_voice 复用已归还的声道（曾为 VOICE_PCM）时**不清 ext**。
 *   于是这些音符声道的残留 ext 指向上一段语音缓冲 → 被误当 PCM 播放 →
 *   曲名/模式提示等被音符反复重放。
 *
 * 铁律：只有 kind==VOICE_PCM 的声道才读外部缓冲；采样/合成音一律查表。
 * 宿主测试 voice_src_tests.c 断言 (is_pcm=false, has_ext=true) 必须为 false——
 * 任何人把它改回 "只看 ext"，该用例立即变红。
 */
static inline bool voice_src_uses_external(bool is_pcm, bool has_ext)
{
    return is_pcm && has_ext;
}
