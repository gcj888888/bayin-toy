#pragma once

/*
 * ★ 台架自测开关（作业交付前置 0 或整块删除，即可"不留调试后门"）。
 *
 * BAYIN_ENABLE_DIAG = 1 时启用：
 *   - 诊断命令 `rec_inject`（把合成乐谱注入回放池）与 `pb_play`（直接起播回放池第 N 槽）
 *   - 音频层每起一个音符采样时的 `snd` 事件日志（{"t":"log","tag":"snd","msg":"note=N"}）
 *
 * 关闭方式：把下面默认值改为 0，或编译时 -DBAYIN_ENABLE_DIAG=0。
 */
#ifndef BAYIN_ENABLE_DIAG
#define BAYIN_ENABLE_DIAG 1
#endif
