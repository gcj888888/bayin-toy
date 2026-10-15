#!/usr/bin/env python3
"""
八音琴 8 音音色 A/B 备选生成器（离线合成，纯 Python + numpy）。

产出三个可人耳对比的版本，8 个音：哆 唻 咪 发 嗦 啦 唏 高音哆（C 大调 + 高八度 do）：
  1. fw_live —— 复刻固件"现场合成"音色（三角波 + 15% 二次泛音 + 指数衰减），
                用于在没有板子的情况下试听"板子上实际会是什么声"。
  2. cur_sample —— 复刻现有离线采样音色（tools/gen_note_samples.py：6 个泛音加法合成）。
  3. alt_musicbox —— 新备选音色：更强的 2/4/6/8 次泛音、高次泛音更快衰减，
                   做出更典型"八音盒金属齿/音簧"的清脆盒子感。

另附每个版本的整条音阶预览 wav（scale_*.wav），方便连续 A/B 试听。
输出目录：tools/note_samples/（同时写 .wav 便于试听、.raw 便于将来嵌入固件）。

参数可微调：改下面 VARIANTS 里的系数即可。
"""
import os
import struct

import numpy as np
import soundfile as sf

SR = 32000                 # 与固件 I2S 一致
DURATION = 0.85            # 音长（秒），与固件 AUDIO_NOTE_DEFAULT_MS=850ms 对齐
N = int(SR * DURATION)

# 8 个音：哆 唻 咪 发 嗦 啦 唏 高音哆
FREQS = [261.63, 293.66, 329.63, 349.23, 392.00, 440.00, 493.88, 523.25]
NAMES = ["do", "re", "mi", "fa", "sol", "la", "si", "high_do"]

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, "note_samples")

ATTACK_S = 0.005
RELEASE_S = 0.12

# 固件现场合成常数（见 firmware/main/audio/audio_click.c）
FW_OVERTONE = 0.15
FW_DECAY = 4.5

# 现有离线采样常数（见 tools/gen_note_samples.py）
CUR_HARMONICS = [
    (1.0, 1.00, 1.0), (2.0, 0.50, 1.0), (3.0, 0.20, 1.0),
    (4.0, 0.28, 1.0), (5.0, 0.10, 1.0), (6.0, 0.08, 1.0),
]
CUR_DECAY = 4.0

# 新备选：八音盒音簧风。基础音干净、2/4/6/8 次泛音突出，且高次泛音衰减更快，
# 形成"清脆敲击 → 余音纯净"的盒子感；3 次泛音压低减少"木琴/方波"味。
ALT_HARMONICS = [
    (1.0, 1.00, 1.0), (2.0, 0.45, 1.6), (3.0, 0.08, 2.4),
    (4.0, 0.32, 2.6), (6.0, 0.14, 3.2), (8.0, 0.06, 3.8),
]
ALT_DECAY = 3.2


def cosine_env(i):
    """起音 5ms + 释放 120ms 的余弦包络，两端导数为 0（无 click）。"""
    attack_n = max(1, int(ATTACK_S * SR))
    release_n = max(1, int(RELEASE_S * SR))
    release_start = N - release_n
    if i < attack_n:
        a = 0.5 * (1.0 - np.cos(np.pi * i / attack_n))
    else:
        a = 1.0
    if i >= release_start:
        x = (N - i) / release_n
        a *= 0.5 * (1.0 - np.cos(np.pi * x))
    return a


def synth_fw(freq):
    """复刻固件现场合成：三角波*(1-0.15) + sin(2ω)*0.15，再乘攻击/指数衰减/释放。"""
    t = np.arange(N) / SR
    phase = (freq * t) % 1.0
    tri = np.where(phase < 0.25, 4 * phase,
                   np.where(phase < 0.75, 2 - 4 * phase, 4 * phase - 4))
    overtone = np.sin(2.0 * (2.0 * np.pi * freq * t))
    wave = tri * (1.0 - FW_OVERTONE) + overtone * FW_OVERTONE
    env = np.array([cosine_env(i) for i in range(N)]) * np.exp(-t * FW_DECAY)
    return wave * env


def synth_additive(freq, harmonics, base_decay):
    """加法合成：多个泛音叠加、各自独立衰减。"""
    t = np.arange(N) / SR
    v = np.zeros(N)
    for mult, amp, dm in harmonics:
        f = freq * mult
        if f >= SR / 2:
            continue
        v += amp * np.sin(2.0 * np.pi * f * t) * np.exp(-t * base_decay * dm)
    env = np.array([cosine_env(i) for i in range(N)])
    return v * env


VARIANTS = {
    "fw_live": lambda f: synth_fw(f),
    "cur_sample": lambda f: synth_additive(f, CUR_HARMONICS, CUR_DECAY),
    "alt_musicbox": lambda f: synth_additive(f, ALT_HARMONICS, ALT_DECAY),
}


def to_int16(x, peak=0.85 * 32767.0):
    x = np.asarray(x, dtype=np.float64)
    m = np.max(np.abs(x)) or 1.0
    return np.clip(x / m * peak, -32768, 32767).astype(np.int16)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    for tag, fn in VARIANTS.items():
        notes = []
        for name, freq in zip(NAMES, FREQS):
            s = to_int16(fn(freq))
            notes.append(s)
            sf.write(os.path.join(OUT_DIR, f"{tag}_{name}.wav"), s, SR, subtype="PCM_16")
            with open(os.path.join(OUT_DIR, f"{tag}_{name}.raw"), "wb") as f:
                f.write(b"".join(struct.pack("<h", int(v)) for v in s))
        gap = np.zeros(int(SR * 0.15), dtype=np.int16)
        cat = []
        for i, s in enumerate(notes):
            cat.append(s)
            if i != len(notes) - 1:
                cat.append(gap)
        sf.write(os.path.join(OUT_DIR, f"scale_{tag}.wav"),
                 np.concatenate(cat), SR, subtype="PCM_16")
        print(f"[{tag}] 生成 8 音 + scale_{tag}.wav（{len(NAMES)} 音，每音 {N} 帧）")
    print(f"输出目录：{OUT_DIR}")


if __name__ == "__main__":
    main()
